#include "sync_task.h"
#include "s3_interface.h"
#include "s3_utils.h"
#include "cloud_metrics.h"
#include "directory_comparison.h"
#include "crc32_chunks.h"
#include "constants.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <unordered_map>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <algorithm>
#include <set>
#include <random>
#include <cctype>
#include <limits>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#endif

namespace fs = std::filesystem;

#ifndef _WIN32
enum class TempSweepMode {
    RemoveOrphans,
    RefuseOrphans
};
#endif

void apply_sync_profiles(SyncDirection direction,
                         const std::string& source_profile,
                         const std::string& dest_profile,
                         FileSource& s3_source,
                         FileSource& s3_dest) {
    switch (direction) {
        case SyncDirection::S3ToS3:
            if (!source_profile.empty()) s3_source.profile = source_profile;
            if (!dest_profile.empty())   s3_dest.profile   = dest_profile;
            break;
        case SyncDirection::Upload:      // S3 side is the destination
            if (!dest_profile.empty())   s3_source.profile = dest_profile;
            break;
        case SyncDirection::Download:    // S3 side is the source
            if (!source_profile.empty()) s3_source.profile = source_profile;
            break;
        case SyncDirection::LocalToLocal:
            break;  // no S3 side
    }
}

// Threshold for using diff upload (8 MiB)
constexpr int64_t DIFF_UPLOAD_THRESHOLD = 8 * 1024 * 1024;

// Performance tuning
// Connection pool size is set dynamically based on max_threads (see run_sync)
constexpr size_t PRE_READ_QUEUE_SIZE = 512;    // Number of files to pre-read ahead
constexpr int64_t PRE_READ_MAX_FILE_SIZE = 16 * 1024 * 1024;  // Only pre-read files < 16MB
constexpr size_t PRE_READ_MAX_TOTAL_BYTES = 1024 * 1024 * 1024;  // Max 1GB total in cache
constexpr size_t DISK_WRITER_THREADS = 64;    // Limit concurrent disk writes (SSDs handle parallelism well)

// Retry configuration
constexpr int MAX_RETRIES = 3;
constexpr int INITIAL_BACKOFF_MS = 100;  // 100ms, then 200ms, then 400ms

// Add jitter to backoff to prevent thundering herd
// Returns backoff_ms + random(0, backoff_ms/2)
static int add_jitter(int backoff_ms) {
    // Use thread-local random engine for thread safety
    thread_local std::mt19937 rng(std::random_device{}());
    int jitter_range = std::max(1, backoff_ms / 2);
    std::uniform_int_distribution<int> dist(0, jitter_range);
    return backoff_ms + dist(rng);
}

// Get local file mtime as seconds since epoch
// Note: C++17 doesn't provide a portable way to convert file_time to system_clock,
// so we use a workaround based on duration since epoch
// Size and modification time of one side of a comparison.
struct SyncEntryInfo {
    int64_t size = 0;
    int64_t mtime = 0;
};

// True when the destination can be proven no older than the source, so equal
// sizes really do mean equal content for practical purposes.
//
// Equal size alone is not evidence: an in-place edit that preserves length is
// invisible to it. Comparing timestamps catches that, in the direction that
// matters - the destination is written after the source is read, so a correctly
// synced destination is always at least as new. If either timestamp is unknown
// (0) nothing can be proven and the entry is transferred.
static bool destination_is_current(int64_t source_mtime, int64_t dest_mtime) {
    if (source_mtime <= 0 || dest_mtime <= 0) return false;
    // Strictly newer, not "no older". Both timestamps are whole seconds
    // (st_mtime, and S3 LastModified truncated to seconds), so an in-place
    // edit made in the same second as the destination write would compare
    // equal - and, because neither stamp ever changes again, would be skipped
    // forever. That is the bug this predicate exists to fix. Requiring a
    // strictly newer destination costs at most one redundant transfer, after
    // which the destination stamp advances and the entry settles as Skip.
    return dest_mtime > source_mtime;
}


// Enumerate and classify files for sync
static std::vector<SyncFileEntry> classify_files(
    const SyncConfig& config,
    SyncProgress& progress,
    std::shared_ptr<IS3Client> s3_client,
    bool& enumeration_ok
) {
    std::vector<SyncFileEntry> entries;

    // Create a local copy to handle trailing slash
    std::string local_path = config.local_path;
    if (!local_path.empty() && local_path.back() != '/') {
        local_path += '/';
    }

    // Enumerate local files
    bool local_complete = true;
    std::vector<DirectoryEntry> local_entries;
    bool local_alias_omitted = false;
    local_entries = parallel_enumerate_local_directory(
        local_path, true, 64,
        progress.files_scanned_local, progress.cancelled,
        &local_complete, &local_alias_omitted
    );
    if (local_alias_omitted) {
        // Those paths are absent from the source list, so with --delete their
        // destination copies would be deleted as orphans.
        spdlog::error("A symlinked subtree of the source was omitted because its target was "
                      "already enumerated under another name; the source list is incomplete");
        local_complete = false;
    }

    // Enumerate S3 objects
    bool s3_complete = true;
    std::vector<DirectoryEntry> s3_entries;
    s3_entries = parallel_enumerate_s3_prefix(
        config.destination.bucket, config.destination.path,
        true, 64,
        progress.files_scanned_s3, progress.cancelled,
        s3_client, &s3_complete
    );

    // Deletes are "in destination, absent from source", so only an incomplete
    // SOURCE listing is dangerous: it invents orphans. An incomplete
    // destination listing can only under-delete and re-copy, which is wasteful
    // but never destructive.
    enumeration_ok = local_complete;
    if (!s3_complete) {
        spdlog::warn("Destination listing was incomplete; existing objects may be re-uploaded "
                     "and some orphans may not be removed");
    }
    progress.scanning_done = true;

    if (progress.cancelled) {
        return entries;
    }

    // Build maps for lookup
    std::unordered_map<std::string, SyncEntryInfo> local_map;
    std::unordered_map<std::string, SyncEntryInfo> s3_map;

    for (const auto& e : local_entries) {
        local_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }
    for (const auto& e : s3_entries) {
        s3_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    // Classify each file
    // Files in local
    for (const auto& e : local_entries) {
        SyncFileEntry entry;
        entry.relative_path = e.relative_path;
        entry.local_size = e.size;

        // From the enumerator's stat(), which is Unix epoch seconds and so is
        // comparable with an S3 LastModified. get_local_mtime() uses
        // fs::last_write_time, whose epoch is implementation-defined.
        entry.local_mtime = e.mtime;

        auto it = s3_map.find(e.relative_path);
        if (it == s3_map.end()) {
            // New file - upload
            entry.s3_size = -1;
            entry.action = (e.size >= DIFF_UPLOAD_THRESHOLD) ?
                           SyncAction::UploadDiff : SyncAction::Upload;
        } else {
            entry.s3_size = it->second.size;
            const int64_t s3_mtime = it->second.mtime;

            // Check if file changed (size or local newer)
            if (entry.local_size != entry.s3_size) {
                // Size differs - needs upload
                entry.action = (entry.local_size >= DIFF_UPLOAD_THRESHOLD) ?
                               SyncAction::UploadDiff : SyncAction::Upload;
            } else if (destination_is_current(entry.local_mtime, s3_mtime)) {
                entry.action = SyncAction::Skip;
            } else {
                // Equal size but the destination is older, or a timestamp is
                // unknown: the local file may have been edited in place.
                entry.action = (entry.local_size >= DIFF_UPLOAD_THRESHOLD) ?
                               SyncAction::UploadDiff : SyncAction::Upload;
            }
        }

        entries.push_back(std::move(entry));
    }

    // Files only in S3
    if (config.delete_orphans) {
        for (const auto& e : s3_entries) {
            if (local_map.find(e.relative_path) == local_map.end()) {
                SyncFileEntry entry;
                entry.relative_path = e.relative_path;
                entry.local_size = -1;
                entry.s3_size = e.size;
                entry.action = SyncAction::Delete;
                entries.push_back(std::move(entry));
            }
        }
    }

    // Note: files_total is set later to actionable.size() (excludes skipped files)
    return entries;
}

// Enumerate and classify files for download (S3 -> local)
static std::vector<SyncFileEntry> classify_files_download(
    const SyncConfig& config,
    SyncProgress& progress,
    std::shared_ptr<IS3Client> s3_client,
    bool& enumeration_ok
) {
    std::vector<SyncFileEntry> entries;

    std::string local_path = config.local_path;
    if (!local_path.empty() && local_path.back() != '/') {
        local_path += '/';
    }

    // Enumerate S3 objects (source)
    std::vector<DirectoryEntry> s3_entries;
    bool s3_complete = true;
    s3_entries = parallel_enumerate_s3_prefix(
        config.source.bucket, config.source.path,
        true, 64,
        progress.files_scanned_s3, progress.cancelled,
        s3_client, &s3_complete
    );

    // Enumerate local files (destination)
    std::vector<DirectoryEntry> local_entries;
    bool local_complete = true;
    if (fs::exists(local_path)) {
        local_entries = parallel_enumerate_local_directory(
            local_path, true, 64,
            progress.files_scanned_local, progress.cancelled,
            &local_complete
        );
    }

    // Only the source side gates the sync - see classify_files().
    enumeration_ok = s3_complete;
    if (!local_complete) {
        spdlog::warn("Destination listing was incomplete; existing files may be re-downloaded "
                     "and some orphans may not be removed");
    }
    progress.scanning_done = true;

    if (progress.cancelled) {
        return entries;
    }

    // Build lookup maps
    std::unordered_map<std::string, SyncEntryInfo> local_map;
    for (const auto& e : local_entries) {
        local_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    std::unordered_map<std::string, SyncEntryInfo> s3_map;
    for (const auto& e : s3_entries) {
        s3_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    // Classify each S3 file
    for (const auto& e : s3_entries) {
        SyncFileEntry entry;
        entry.relative_path = e.relative_path;
        entry.s3_size = e.size;
        const int64_t s3_mtime = e.mtime;   // source side

        auto it = local_map.find(e.relative_path);
        if (it == local_map.end()) {
            // File not in local - full download (no diff possible without local file)
            entry.local_size = -1;
            entry.action = SyncAction::Download;
        } else {
            entry.local_size = it->second.size;
            entry.local_mtime = it->second.mtime;

            if (entry.local_size != entry.s3_size) {
                // Size differs - needs download
                entry.action = (entry.s3_size >= DIFF_UPLOAD_THRESHOLD) ?
                               SyncAction::DownloadDiff : SyncAction::Download;
            } else if (destination_is_current(s3_mtime, entry.local_mtime)) {
                entry.action = SyncAction::Skip;
            } else {
                entry.action = (entry.s3_size >= DIFF_UPLOAD_THRESHOLD) ?
                               SyncAction::DownloadDiff : SyncAction::Download;
            }
        }

        entries.push_back(std::move(entry));
    }

    // Files only in local (for --delete)
    if (config.delete_orphans) {
        for (const auto& e : local_entries) {
            if (s3_map.find(e.relative_path) == s3_map.end()) {
                SyncFileEntry entry;
                entry.relative_path = e.relative_path;
                entry.local_size = e.size;
                entry.s3_size = -1;
                entry.action = SyncAction::Delete;
                entries.push_back(std::move(entry));
            }
        }
    }

    return entries;
}

// Classify files for S3-to-S3 sync (server-side copying)
static std::vector<SyncFileEntry> classify_files_s3_to_s3(
    const SyncConfig& config,
    SyncProgress& progress,
    std::shared_ptr<IS3Client> source_client,
    std::shared_ptr<IS3Client> dest_client,
    bool& enumeration_ok
) {
    std::vector<SyncFileEntry> entries;

    // Enumerate source and destination S3 objects in parallel
    // Each future owns its own flag; both are read only after .get().
    bool source_complete = true;
    bool dest_complete = true;

    auto source_future = std::async(std::launch::async, [&]() {
        return parallel_enumerate_s3_prefix(
            config.source.bucket, config.source.path,
            true, 64,
            progress.files_scanned_s3, progress.cancelled,
            source_client, &source_complete
        );
    });

    auto dest_future = std::async(std::launch::async, [&]() {
        return parallel_enumerate_s3_prefix(
            config.destination.bucket, config.destination.path,
            true, 64,
            progress.files_scanned_dest, progress.cancelled,
            dest_client, &dest_complete
        );
    });

    // Wait for both enumerations to complete
    std::vector<DirectoryEntry> source_entries = source_future.get();
    std::vector<DirectoryEntry> dest_entries = dest_future.get();
    // Only the source side gates the sync - see classify_files().
    enumeration_ok = source_complete;
    if (!dest_complete) {
        spdlog::warn("Destination listing was incomplete; existing entries may be re-copied "
                     "and some orphans may not be removed");
    }

    progress.scanning_done = true;

    if (progress.cancelled) {
        return entries;
    }

    // Build lookup maps
    std::unordered_map<std::string, SyncEntryInfo> source_map;
    for (const auto& e : source_entries) {
        source_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    std::unordered_map<std::string, SyncEntryInfo> dest_map;
    for (const auto& e : dest_entries) {
        dest_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    // Classify each source file
    for (const auto& e : source_entries) {
        SyncFileEntry entry;
        entry.relative_path = e.relative_path;
        entry.s3_size = e.size;  // Source size
        const int64_t src_mtime = e.mtime;

        auto it = dest_map.find(e.relative_path);
        if (it == dest_map.end()) {
            // File not in dest - copy
            entry.dest_size = -1;
            entry.action = SyncAction::Copy;
        } else {
            entry.dest_size = it->second.size;
            const int64_t dst_mtime = it->second.mtime;

            if (entry.dest_size != entry.s3_size) {
                // Size differs - copy
                entry.action = SyncAction::Copy;
            } else if (destination_is_current(src_mtime, dst_mtime)) {
                entry.action = SyncAction::Skip;
            } else {
                entry.action = SyncAction::Copy;
            }
        }

        entries.push_back(std::move(entry));
    }

    // Files only in dest (for --delete)
    if (config.delete_orphans) {
        for (const auto& e : dest_entries) {
            if (source_map.find(e.relative_path) == source_map.end()) {
                SyncFileEntry entry;
                entry.relative_path = e.relative_path;
                entry.dest_size = e.size;
                entry.s3_size = -1;
                entry.action = SyncAction::Delete;
                entries.push_back(std::move(entry));
            }
        }
    }

    return entries;
}

// Classify files for local-to-local sync
static std::vector<SyncFileEntry> classify_files_local_to_local(
    const SyncConfig& config,
    SyncProgress& progress,
    bool& enumeration_ok
) {
    std::vector<SyncFileEntry> entries;

    // Normalize paths with trailing slash
    std::string source_path = config.local_path;
    if (!source_path.empty() && source_path.back() != '/') {
        source_path += '/';
    }
    std::string dest_path = config.dest_local_path;
    if (!dest_path.empty() && dest_path.back() != '/') {
        dest_path += '/';
    }

    // Enumerate source and destination in parallel
    // Each future owns its own flag; both are read only after .get().
    bool source_complete = true;
    bool dest_complete = true;
    bool source_alias_omitted = false;

    auto source_future = std::async(std::launch::async, [&]() {
        return parallel_enumerate_local_directory(
            source_path, true, 64,
            progress.files_scanned_local, progress.cancelled,
            &source_complete, &source_alias_omitted);
    });

    auto dest_future = std::async(std::launch::async, [&]() {
        std::vector<DirectoryEntry> result;
        if (fs::exists(dest_path)) {
            result = parallel_enumerate_local_directory(
                dest_path, true, 64,
                progress.files_scanned_dest, progress.cancelled,
                &dest_complete
            );
        }
        return result;
    });

    // Wait for both enumerations
    std::vector<DirectoryEntry> source_entries = source_future.get();
    std::vector<DirectoryEntry> dest_entries = dest_future.get();
    // Only the source side gates the sync - see classify_files().
    if (source_alias_omitted) {
        spdlog::error("A symlinked subtree of the source was omitted because its target was "
                      "already enumerated under another name; the source list is incomplete");
        source_complete = false;
    }
    enumeration_ok = source_complete;
    if (!dest_complete) {
        spdlog::warn("Destination listing was incomplete; existing entries may be re-copied "
                     "and some orphans may not be removed");
    }

    progress.scanning_done = true;

    if (progress.cancelled) {
        return entries;
    }

    // Build lookup maps
    std::unordered_map<std::string, SyncEntryInfo> source_map;
    for (const auto& e : source_entries) {
        source_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    std::unordered_map<std::string, SyncEntryInfo> dest_map;
    for (const auto& e : dest_entries) {
        dest_map[e.relative_path] = SyncEntryInfo{e.size, e.mtime};
    }

    // Classify each source file
    for (const auto& e : source_entries) {
        SyncFileEntry entry;
        entry.relative_path = e.relative_path;
        entry.local_size = e.size;  // Source size
        const int64_t src_mtime = e.mtime;

        auto it = dest_map.find(e.relative_path);
        if (it == dest_map.end()) {
            // File not in dest - copy
            entry.dest_size = -1;
            entry.action = SyncAction::Upload;  // Reuse Upload action for local copy
        } else {
            entry.dest_size = it->second.size;
            const int64_t dst_mtime = it->second.mtime;

            if (entry.dest_size != entry.local_size) {
                // Size differs - copy
                entry.action = SyncAction::Upload;
            } else if (destination_is_current(src_mtime, dst_mtime)) {
                entry.action = SyncAction::Skip;
            } else {
                entry.action = SyncAction::Upload;
            }
        }

        entries.push_back(std::move(entry));
    }

    // Files only in dest (for --delete)
    if (config.delete_orphans) {
        for (const auto& e : dest_entries) {
            if (source_map.find(e.relative_path) == source_map.end()) {
                SyncFileEntry entry;
                entry.relative_path = e.relative_path;
                entry.dest_size = e.size;
                entry.local_size = -1;
                entry.action = SyncAction::Delete;
                entries.push_back(std::move(entry));
            }
        }
    }

    return entries;
}

// Forward declarations for path validation functions
static bool validate_relative_path(const std::string& relative_path);
static bool validate_target_within_base(const fs::path& base_path, const fs::path& target_path);

// Resolve a target's directory chain and require it to stay under the base.
//
// The lexical check above compares strings and resolves nothing, so a
// destination containing a directory symlink - dst/photos -> ../elsewhere -
// produces targets that start with the base but resolve outside it. Following
// them writes to, or deletes, files the user never named.
//
// Only the directory chain needs resolving. The final component is handled by
// the caller: a write opens it with O_NOFOLLOW, and a delete removes a symlink
// as the link it is rather than following it.
//
// This is a check-time guarantee, not an atomic one. Resolution and the
// subsequent open/remove are separate syscalls, so an attacker who can write
// inside the destination can still win the race by swapping a directory for a
// symlink between them (issue #55). Closing that needs an openat-based walk
// holding a directory fd.
//
// The deepest existing ancestor is resolved rather than the immediate parent,
// because a write may target a directory that does not exist yet. Components
// that do not exist cannot be symlinks, so resolving what does exist is
// sufficient.
static bool validate_target_resolves_within_base(const fs::path& base_path,
                                                 const fs::path& target_path) {
    std::error_code ec;
    fs::path canonical_base = fs::canonical(base_path, ec);
    if (ec) {
        // The destination does not exist yet - the first sync into a new
        // directory. Nothing inside it exists, so there is no symlink to
        // traverse and nothing to escape through; it will be created by us.
        // Refusing here would block every first-run sync.
        // The error_code overload: the throwing one would escape a worker
        // thread that has no handler and terminate the process when canonical()
        // fails for a reason other than absence (EACCES, ELOOP, ESTALE).
        std::error_code exist_ec;
        if (!fs::exists(base_path, exist_ec)) {
            return true;
        }
        spdlog::error("Refusing to touch {}: cannot resolve base {}: {}",
                      target_path.string(), base_path.string(), ec.message());
        return false;
    }

    // Walk up to the deepest ancestor that exists.
    fs::path probe = target_path.parent_path();
    fs::path canonical_parent;
    while (true) {
        canonical_parent = fs::canonical(probe, ec);
        if (!ec) break;
        fs::path up = probe.parent_path();
        if (up.empty() || up == probe) {
            spdlog::error("Refusing to touch {}: no part of its directory chain "
                          "could be resolved", target_path.string());
            return false;
        }
        probe = up;
        ec.clear();
    }

    const std::string base_s = canonical_base.string();
    const std::string parent_s = canonical_parent.string();
    const bool base_is_root = (base_s == "/");
    const bool inside = parent_s == base_s ||
                        (base_is_root && parent_s.size() > 1 && parent_s[0] == '/') ||
                        (parent_s.size() > base_s.size() &&
                         parent_s.compare(0, base_s.size(), base_s) == 0 &&
                         parent_s[base_s.size()] == '/');
    if (!inside) {
        spdlog::error("Refusing to touch {}: it resolves under {}, outside {}. A "
                      "directory symlink points out of the tree; remove the symlink "
                      "if reaching outside is intended.",
                      target_path.string(), canonical_parent.string(), base_s);
        return false;
    }
    return true;
}

// Upload from pre-read data with retry logic
static bool upload_from_data(
    const std::string& bucket,
    const std::string& key,
    const std::vector<uint8_t>& data,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_transferred,
    const std::atomic<bool>& cancelled
) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return false;

        if (attempt > 0) {
            spdlog::debug("Retry {} for upload: {}", attempt, key);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        if (s3_client->PutObject(bucket, key, data)) {
            bytes_transferred += data.size();
            return true;
        }
    }

    spdlog::error("Upload failed after {} attempts: {}", MAX_RETRIES + 1, key);
    return false;
}

#ifndef _WIN32
static bool parse_sync_temp_pid(const std::string& filename, pid_t& pid) {
    if (!is_sync_temp_name(filename)) return false;

    const std::string suffix = kTempSuffix;
    std::string body = filename.substr(0, filename.size() - suffix.size());
    if (body.size() < 2 || body.front() != '.') return false;

    const size_t counter_dash = body.rfind('-');
    if (counter_dash == std::string::npos || counter_dash + 1 == body.size()) return false;

    const size_t pid_dash = body.rfind('-', counter_dash - 1);
    if (pid_dash == std::string::npos || pid_dash <= 1 || pid_dash + 1 == counter_dash) {
        return false;
    }

    const std::string pid_part = body.substr(pid_dash + 1, counter_dash - pid_dash - 1);
    const std::string counter_part = body.substr(counter_dash + 1);
    if (!std::all_of(pid_part.begin(), pid_part.end(),
                     [](unsigned char c) { return std::isdigit(c); }) ||
        !std::all_of(counter_part.begin(), counter_part.end(),
                     [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }

    try {
        unsigned long long parsed = std::stoull(pid_part);
        if (parsed == 0 || parsed > static_cast<unsigned long long>(std::numeric_limits<pid_t>::max())) {
            return false;
        }
        pid = static_cast<pid_t>(parsed);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

static bool process_exists(pid_t pid, std::string& error_message) {
    errno = 0;
    if (::kill(pid, 0) == 0) return true;
    if (errno == ESRCH) return false;
    error_message = std::string("could not check owner process ") + std::to_string(pid) +
                    ": " + strerror(errno);
    return true;
}

static bool sweep_orphaned_sync_temps(
    const fs::path& root,
    TempSweepMode mode,
    std::string& error_message
) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return true;
    if (ec) {
        error_message = "Cannot inspect local sync root " + root.string() + ": " + ec.message();
        return false;
    }
    if (!fs::is_directory(root, ec)) return true;
    if (ec) {
        error_message = "Cannot inspect local sync root " + root.string() + ": " + ec.message();
        return false;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error_message = "Cannot scan local sync root " + root.string() + ": " + ec.message();
        return false;
    }

    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            error_message = "Cannot finish scanning local sync root " + root.string() + ": " + ec.message();
            return false;
        }

        const fs::path path = it->path();
        const std::string filename = path.filename().string();
        if (!is_sync_temp_name(filename)) continue;

        pid_t owner_pid = 0;
        if (!parse_sync_temp_pid(filename, owner_pid)) {
            error_message = "Refusing to sync with an unrecognized temporary sync file present: " +
                            path.string();
            return false;
        }

        auto st = it->symlink_status(ec);
        if (ec) {
            error_message = "Cannot inspect temporary sync file " + path.string() + ": " + ec.message();
            return false;
        }
        if (!fs::is_regular_file(st)) {
            error_message = "Refusing to remove non-regular temporary sync entry: " + path.string();
            return false;
        }

        std::string owner_check_error;
        if (process_exists(owner_pid, owner_check_error)) {
            error_message = owner_check_error.empty()
                                ? "Refusing to sync while another process owns temporary sync file: " +
                                      path.string()
                                : "Refusing to sync because " + owner_check_error + " for " +
                                      path.string();
            return false;
        }

        if (mode == TempSweepMode::RefuseOrphans) {
            error_message = "Refusing to sync with orphaned temporary sync file present: " +
                            path.string();
            return false;
        }

        if (::unlink(path.c_str()) != 0) {
            error_message = "Failed to remove orphaned temporary sync file " + path.string() +
                            ": " + strerror(errno);
            return false;
        }
        spdlog::info("Removed orphaned temporary sync file: {}", path.string());
    }
    return true;
}

static bool sweep_local_sync_temps(
    const SyncConfig& config,
    TempSweepMode mode,
    std::string& error_message
) {
    std::vector<fs::path> roots;
    switch (config.direction) {
        case SyncDirection::Upload:
        case SyncDirection::Download:
            roots.emplace_back(config.local_path);
            break;
        case SyncDirection::LocalToLocal:
            roots.emplace_back(config.local_path);
            roots.emplace_back(config.dest_local_path);
            break;
        case SyncDirection::S3ToS3:
            break;
    }

    std::set<std::string> seen;
    for (const auto& root : roots) {
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(root, ec);
        const std::string key = ec ? fs::absolute(root, ec).string() : canonical.string();
        if (!seen.insert(key).second) continue;
        if (!sweep_orphaned_sync_temps(root, mode, error_message)) return false;
    }
    return true;
}

// Replaces a file's contents atomically.
//
// The write paths used to open the destination with O_TRUNC and remove that
// same path if the write failed, which meant a disk-full, EIO, or Ctrl-C
// partway through an update destroyed the existing good copy and left nothing
// in its place - data loss on a sync that was only meant to refresh a file,
// with no --delete in sight (issue #58).
//
// Writing to a temporary file beside the destination and renaming it into
// place means the destination only ever holds the old contents or the complete
// new ones. Anything that goes wrong leaves the original exactly as it was.
//
// This covers the three paths that write a file in full. The differential
// download path deliberately patches an existing file in place - that is the
// whole point of reusing its unchanged chunks - and does not get this
// guarantee; see diff_download_file.
//
// The replacement is a new inode, so a few things that survived an in-place
// write do not survive this: hard links to the destination keep pointing at
// the old contents, and POSIX ACLs and extended attributes are not carried
// over. Permissions and ownership are.
class AtomicFileWriter {
public:
    explicit AtomicFileWriter(const fs::path& target) : target_(target) {
        std::error_code ec;
        auto st = fs::symlink_status(target_, ec);
        if (ec && ec != std::errc::no_such_file_or_directory &&
            ec != std::errc::not_a_directory) {
            // Nothing there is the ordinary case for a new file. Anything else
            // means the destination could not be inspected, and writing blind
            // is how a symlink or a device node gets clobbered.
            spdlog::error("Cannot inspect destination {}: {}", target_.string(), ec.message());
            return;
        }
        if (!ec && fs::exists(st)) {
            // The direct open used O_NOFOLLOW, so a symlinked destination was
            // refused outright. rename() would replace the link rather than
            // write through it, but keep refusing: these paths promise not to
            // touch a destination they cannot vouch for.
            if (fs::is_symlink(st)) {
                spdlog::error("Refusing to write through a symlinked destination: {}",
                              target_.string());
                return;
            }
            if (!fs::is_regular_file(st)) {
                spdlog::error("Destination is not a regular file: {}", target_.string());
                return;
            }
            // Carry the existing file's permissions across. Without this an
            // atomic replace would silently reset every updated file to 0644.
            //
            // setuid and setgid are deliberately dropped: the kernel strips
            // them when a non-privileged process writes a file, so the old
            // in-place path cleared them too. Restoring them here would put
            // setuid bits back on contents that just came from a sync source.
            struct ::stat sb {};
            if (::stat(target_.c_str(), &sb) == 0) {
                // 0000 is a real mode, so it needs its own flag - using 0 as
                // "there was no file" would republish a no-access file at 0644.
                // The sticky bit is kept; setuid and setgid are not.
                existing_mode_ = (sb.st_mode & 07777) & ~(mode_t)(S_ISUID | S_ISGID);
                has_existing_mode_ = true;
                existing_uid_ = sb.st_uid;
                existing_gid_ = sb.st_gid;
            }
        }

        // Keep the temporary file no wider than the mode it will end up with.
        // Creating it 0644 and narrowing at commit would leave the complete
        // contents of a 0600 file world-readable for the whole transfer.
        // A destination that does not exist yet keeps the old 0644 default so
        // the umask still decides, as it did before.
        const mode_t create_mode = has_existing_mode_ ? 0600 : 0644;

        // Try a few names. A stale temporary file from a killed run is normally
        // reaped before enumeration; the extra attempts keep a colliding name
        // planted after that sweep from blocking this file forever.
        for (int attempt = 0; attempt < 8 && fd_ < 0; ++attempt) {
            temp_path_ = target_.parent_path() / make_temp_name();
            fd_ = open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, create_mode);
            if (fd_ < 0 && errno != EEXIST) break;
        }
        if (fd_ < 0) {
            if (errno == EACCES || errno == EROFS) {
                spdlog::error("Cannot create a temporary file in {}: {}. An update is "
                              "written to a temporary file and moved into place, so the "
                              "destination directory has to be writable even when the "
                              "file itself is.",
                              target_.parent_path().string(), strerror(errno));
            } else {
                spdlog::error("Failed to create temporary file {}: {}",
                              temp_path_.string(), strerror(errno));
            }
            temp_path_.clear();
        }
    }

    ~AtomicFileWriter() {
        // Not committed: drop the temporary file and leave the destination be.
        if (fd_ >= 0) close(fd_);
        if (!committed_ && !temp_path_.empty()) {
            std::error_code ec;
            fs::remove(temp_path_, ec);
        }
    }

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // Makes the written bytes the destination's contents. Until this returns
    // true the destination still holds whatever it held before.
    bool commit() {
        if (fd_ < 0) return false;

        if (has_existing_mode_) {
            if (::fchmod(fd_, existing_mode_) != 0) {
                spdlog::warn("Could not preserve permissions on {}: {}",
                             target_.string(), strerror(errno));
            }
            // Best effort: the replacement is a new inode, so without this it
            // would take this process's group rather than the file's. Fails
            // with EPERM for an unprivileged process changing owner, which is
            // expected and not worth reporting.
            if (::fchown(fd_, existing_uid_, existing_gid_) != 0 && errno != EPERM) {
                spdlog::debug("Could not preserve ownership on {}: {}",
                              target_.string(), strerror(errno));
            }
        }
        // The rename must not be able to publish a file whose bytes are still
        // only in the page cache. fdatasync is enough: it flushes the data and
        // the metadata needed to read it back, but not the timestamps.
        //
        // macOS has no fdatasync at all, so fsync stands in. It flushes the page
        // cache to the device, which is what this needs. fcntl(F_FULLFSYNC) would
        // also flush the drive's own write cache - stronger than fdatasync gives on
        // Linux, and slow enough that matching it here would be a different
        // decision rather than a portability fix.
#if defined(__APPLE__)
        if (::fsync(fd_) != 0) {
            spdlog::error("fsync failed for {}: {}", temp_path_.string(), strerror(errno));
            return false;
        }
#else
        if (::fdatasync(fd_) != 0) {
            spdlog::error("fdatasync failed for {}: {}", temp_path_.string(), strerror(errno));
            return false;
        }
#endif
        if (close(fd_) != 0) {
            // Errors deferred from an earlier write surface here.
            spdlog::error("close failed for {}: {}", temp_path_.string(), strerror(errno));
            fd_ = -1;
            return false;
        }
        fd_ = -1;

        if (::rename(temp_path_.c_str(), target_.c_str()) != 0) {
            spdlog::error("Failed to move {} into place at {}: {}",
                          temp_path_.string(), target_.string(), strerror(errno));
            return false;
        }
        committed_ = true;

        // Persist the rename itself, so a crash cannot leave the directory
        // entry pointing at a file that is no longer there.
        int dir_fd = open(target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0) {
            spdlog::debug("Could not open {} to flush the rename: {}",
                          target_.parent_path().string(), strerror(errno));
        } else {
            if (::fsync(dir_fd) != 0) {
                spdlog::debug("Could not flush the rename into {}: {}",
                              target_.parent_path().string(), strerror(errno));
            }
            close(dir_fd);
        }
        return true;
    }

private:
    // ".<name>-<pid>-<counter>.mito-tmp". The marker goes last so a stray file
    // from a killed run can be recognised and reaped before enumeration, and
    // the name is truncated to stay inside NAME_MAX - files with very long names
    // used to sync fine and must keep doing so.
    std::string make_temp_name() const {
        static std::atomic<uint64_t> counter{0};
        std::string suffix = "-" + std::to_string(::getpid()) + "-" +
                             std::to_string(counter.fetch_add(1)) + kTempSuffix;
        std::string stem = target_.filename().string();
        const size_t budget = 255 - suffix.size() - 1;   // 1 for the leading dot
        if (stem.size() > budget) stem.resize(budget);
        return "." + stem + suffix;
    }

    fs::path target_;
    fs::path temp_path_;
    int fd_ = -1;
    bool committed_ = false;
    bool has_existing_mode_ = false;
    mode_t existing_mode_ = 0;
    uid_t existing_uid_ = 0;
    gid_t existing_gid_ = 0;
};
#endif

// Write pre-fetched download data directly to disk using POSIX I/O (faster than streams)
static bool download_from_data(
    const std::string& local_base_path,
    const std::string& relative_path,
    const std::vector<uint8_t>& data,
    std::atomic<size_t>& bytes_transferred
) {
    // Always validate paths, even when disk writes are disabled
    if (!validate_relative_path(relative_path)) {
        return false;
    }

    fs::path base_path(local_base_path);
    fs::path full_path = base_path / relative_path;

    // Security: verify target resolves within base directory (symlink protection)
    if (!validate_target_within_base(base_path, full_path)) {
        return false;
    }
    // O_NOFOLLOW below only guards the final component; a symlinked parent
    // directory would still be traversed and written through (issue #54).
    if (!validate_target_resolves_within_base(base_path, full_path)) {
        return false;
    }

    // Note: directories are pre-created in run_sync for downloads, so we skip mkdir here

#ifdef _WIN32
    // Fall back to streams on Windows
    std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    if (out.fail()) return false;
    bytes_transferred += data.size();
    return true;
#else

    // Use POSIX I/O for speed (O_NOFOLLOW prevents symlink attacks)
    spdlog::debug("Writing from cache: {} ({} bytes)", full_path.string(), data.size());
    AtomicFileWriter writer(full_path);
    if (!writer.valid()) return false;

    if (!data.empty()) {
        size_t total_written = 0;
        while (total_written < data.size()) {
            ssize_t written = write(writer.fd(), data.data() + total_written,
                                    data.size() - total_written);
            if (written < 0) {
                if (errno == EINTR) continue;
                spdlog::error("write failed: {}", strerror(errno));
                return false;   // the destination is left as it was
            }
            if (written == 0) {
                spdlog::error("write returned 0 (disk full?)");
                return false;
            }
            total_written += static_cast<size_t>(written);
        }
    }

    if (!writer.commit()) return false;

    bytes_transferred += data.size();
    return true;
#endif
}

// Upload a single small file with retry logic
static bool upload_small_file(
    const std::string& local_path,
    const std::string& relative_path,
    const std::string& bucket,
    const std::string& s3_prefix,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_transferred,
    const std::atomic<bool>& cancelled
) {
    fs::path full_path = fs::path(local_path) / relative_path;

    // Construct S3 key
    std::string key = s3_prefix;
    if (!key.empty() && key.back() != '/') {
        key += '/';
    }
    key += relative_path;

    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return false;

        if (attempt > 0) {
            spdlog::debug("Retry {} for upload: {}", attempt, relative_path);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        bool success = s3_client->PutObjectFromFile(bucket, key, full_path.string());
        if (success) {
            std::error_code ec;
            bytes_transferred += fs::file_size(full_path, ec);
            return true;
        }
    }

    spdlog::error("Upload failed after {} attempts: {}", MAX_RETRIES + 1, relative_path);
    return false;
}

// Delete a single S3 object with retry logic
static bool delete_s3_object(
    const std::string& bucket,
    const std::string& s3_prefix,
    const std::string& relative_path,
    std::shared_ptr<IS3Client> s3_client,
    const std::atomic<bool>& cancelled
) {
    std::string key = s3_prefix;
    if (!key.empty() && key.back() != '/') {
        key += '/';
    }
    key += relative_path;

    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return false;

        if (attempt > 0) {
            spdlog::debug("Retry {} for delete: {}", attempt, relative_path);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        if (s3_client->DeleteObject(bucket, key)) {
            return true;
        }
    }

    spdlog::error("Delete failed after {} attempts: {}", MAX_RETRIES + 1, relative_path);
    return false;
}

// Check if target path resolves to within the base directory
// Handles symlink attacks by resolving the actual destination
static bool validate_target_within_base(const fs::path& base_path, const fs::path& target_path) {
    // Lexical prefix check only: it compares strings and resolves nothing.
    //
    // This is NOT sufficient on its own, and the reasoning that used to be
    // recorded here was wrong. It claimed O_NOFOLLOW made symlinks safe, but
    // O_NOFOLLOW applies only to the final path component, so a symlinked
    // parent DIRECTORY inside the destination is still traversed - and
    // fs::remove does not use O_NOFOLLOW at all. It also claimed destination
    // directories are created by us rather than attacker-controlled, which is
    // false: the destination of a download or a local-to-local sync is an
    // existing user directory that may already contain symlinks.
    //
    // A path passing this check can therefore still resolve outside the base.
    // delete_local_file() resolves the parent before removing anything (see
    // issue #45); the write paths currently do not.
    //
    // Kept as a cheap first filter - it rejects ".." escapes and obvious
    // mismatches without the ~10 syscalls fs::canonical() costs per file.

    std::string base_str = base_path.string();
    std::string target_str = target_path.string();

    // Ensure base ends with separator for proper prefix matching
    if (!base_str.empty() && base_str.back() != '/') {
        base_str += '/';
    }

    // Target must start with base path
    if (target_str.size() < base_str.size() ||
        target_str.compare(0, base_str.size(), base_str) != 0) {
        spdlog::error("Security: target path {} outside base directory {}",
                      target_path.string(), base_path.string());
        return false;
    }

    return true;
}

// Validate relative path to prevent path traversal attacks
// Returns false if the path attempts to escape the base directory
static bool validate_relative_path(const std::string& relative_path) {
    if (relative_path.empty()) {
        return true;  // Empty path is valid
    }

    // Reject absolute paths
    if (relative_path[0] == '/' || relative_path[0] == '\\') {
        spdlog::error("Invalid relative path (absolute path): {}", relative_path);
        return false;
    }

    // Normalize path separators and split by path component
    std::string path = relative_path;
    std::replace(path.begin(), path.end(), '\\', '/');

    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();

        std::string component = path.substr(start, end - start);

        // Reject empty components (consecutive slashes like foo//bar)
        if (component.empty()) {
            spdlog::error("Invalid relative path (empty component): {}", relative_path);
            return false;
        }

        // Reject ".." (parent directory escape)
        if (component == "..") {
            spdlog::error("Invalid relative path (parent directory reference): {}", relative_path);
            return false;
        }

        // Reject "." (current directory - could cause confusion or edge cases)
        if (component == ".") {
            spdlog::error("Invalid relative path (current directory reference): {}", relative_path);
            return false;
        }

        start = end + 1;
    }
    return true;
}

// Download a single small file with retry logic
static bool download_small_file(
    const std::string& local_base_path,
    const std::string& relative_path,
    const std::string& bucket,
    const std::string& s3_prefix,
    int64_t s3_size,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_transferred,
    const std::atomic<bool>& cancelled
) {
    // Validate s3_size is non-negative
    if (s3_size < 0) {
        spdlog::error("Invalid s3_size {} for download: {}", s3_size, relative_path);
        return false;
    }

    // Validate relative path to prevent path traversal
    if (!validate_relative_path(relative_path)) {
        return false;
    }

    // Construct S3 key
    std::string key = s3_prefix;
    if (!key.empty() && key.back() != '/') {
        key += '/';
    }
    key += relative_path;

    // Construct local path and create parent directories
    fs::path full_path = fs::path(local_base_path) / relative_path;

    // Security: verify target resolves within base directory (symlink protection)
    if (!validate_target_within_base(local_base_path, full_path)) {
        return false;
    }
    if (!validate_target_resolves_within_base(local_base_path, full_path)) {
        return false;
    }

    // Note: directories are pre-created in run_sync for downloads, so we skip mkdir here

    // Handle empty files specially (avoid GetObjectRange with invalid range 0,-1)
    if (s3_size == 0) {
        // Check if target is an existing directory (can't overwrite directory with file)
        if (fs::is_directory(full_path)) {
            spdlog::error("Cannot create file: path is a directory: {}", full_path.string());
            return false;
        }
        std::ofstream out(full_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::error("Failed to create empty file: {}", full_path.string());
            return false;
        }
        spdlog::debug("Created empty file: {}", relative_path);
        return true;
    }

    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return false;

        if (attempt > 0) {
            spdlog::debug("Retry {} for download: {}", attempt, relative_path);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        // Download entire file via GetObjectRange
        std::vector<uint8_t> data = s3_client->GetObjectRange(bucket, key, 0, s3_size - 1);
        if (data.size() != static_cast<size_t>(s3_size)) {
            // Retry on partial or failed download
            spdlog::debug("Partial download ({} of {} bytes), retrying", data.size(), s3_size);
            continue;
        }

        // Write to file using POSIX I/O (O_NOFOLLOW prevents symlink attacks)
        spdlog::debug("Writing file: {} ({} bytes)", full_path.string(), data.size());
        AtomicFileWriter writer(full_path);
        if (!writer.valid()) return false;

        bool write_success = true;
        if (!data.empty()) {
            size_t total_written = 0;
            while (total_written < data.size()) {
                ssize_t written = write(writer.fd(), data.data() + total_written,
                                        data.size() - total_written);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    spdlog::error("write failed: {}", strerror(errno));
                    write_success = false;
                    break;
                }
                if (written == 0) {
                    spdlog::error("write returned 0 (disk full?)");
                    write_success = false;
                    break;
                }
                total_written += static_cast<size_t>(written);
            }
        }

        // Nothing to clean up on failure: the temporary file goes away with
        // the writer and the destination was never touched.
        if (!write_success) return false;
        if (!writer.commit()) return false;

        bytes_transferred += data.size();
        return true;
    }

    spdlog::error("Download failed after {} attempts: {}", MAX_RETRIES + 1, relative_path);
    return false;
}

// Delete a local file with error handling
static bool delete_local_file(
    const std::string& local_base_path,
    const std::string& relative_path,
    const std::atomic<bool>& cancelled
) {
    if (cancelled) return false;

    // Validate relative path to prevent path traversal
    if (!validate_relative_path(relative_path)) {
        return false;
    }

    fs::path full_path = fs::path(local_base_path) / relative_path;

    // Security: verify target resolves within base directory (symlink protection)
    // Prevents deleting files outside base via symlink attacks
    if (!validate_target_within_base(local_base_path, full_path)) {
        return false;
    }

    // A lexical check is not enough here: fs::remove re-resolves the whole path
    // and follows any symlinked parent directory (issue #45).
    if (!validate_target_resolves_within_base(local_base_path, full_path)) {
        return false;
    }

    std::error_code ec;
    // remove() on a symlink removes the link, not its target, so a symlinked
    // entry inside the destination is deleted as the entry it is.
    bool removed = fs::remove(full_path, ec);
    if (ec) {
        spdlog::error("Failed to delete local file {}: {}", full_path.string(), ec.message());
        return false;
    }
    return removed;
}

// Copy a local file from source to destination
static bool copy_local_file(
    const std::string& source_base_path,
    const std::string& dest_base_path,
    const std::string& relative_path,
    std::atomic<size_t>& bytes_transferred,
    const std::atomic<bool>& cancelled
) {
    if (cancelled) return false;

    // Validate relative path to prevent path traversal
    if (!validate_relative_path(relative_path)) {
        return false;
    }

    fs::path source_path = fs::path(source_base_path) / relative_path;
    fs::path dest_path = fs::path(dest_base_path) / relative_path;

    // Security: verify paths resolve within their base directories
    if (!validate_target_within_base(source_base_path, source_path)) {
        return false;
    }
    if (!validate_target_within_base(dest_base_path, dest_path)) {
        return false;
    }
    // Lexical first as a cheap filter, then resolve: the destination may hold a
    // directory symlink pointing out of the tree (issue #54).
    if (!validate_target_resolves_within_base(dest_base_path, dest_path)) {
        return false;
    }

    // Create parent directories if needed
    fs::path parent = dest_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            spdlog::error("Failed to create directory {}: {}", parent.string(), ec.message());
            return false;
        }
    }

#ifdef _WIN32
    // Use std::filesystem::copy on Windows
    std::error_code ec;
    fs::copy_file(source_path, dest_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::error("Failed to copy {} to {}: {}", source_path.string(), dest_path.string(), ec.message());
        return false;
    }
    // Get file size for bytes_transferred
    auto size = fs::file_size(source_path, ec);
    if (!ec) {
        bytes_transferred += size;
    }
    return true;
#else
    // Use POSIX I/O for speed
    constexpr size_t COPY_BUFFER_SIZE = 1024 * 1024;  // 1 MiB buffer

    // Open source file for reading (O_NOFOLLOW prevents symlink attacks)
    int src_fd = open(source_path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) {
        spdlog::error("Failed to open source file {}: {}", source_path.string(), strerror(errno));
        return false;
    }

    // Open dest file for writing
    AtomicFileWriter writer(dest_path);
    if (!writer.valid()) {
        close(src_fd);
        return false;
    }
    const int dst_fd = writer.fd();

    // Allocate buffer
    std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
    size_t total_bytes = 0;
    bool success = true;

    while (!cancelled) {
        ssize_t bytes_read = read(src_fd, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            spdlog::error("Read error on {}: {}", source_path.string(), strerror(errno));
            success = false;
            break;
        }
        if (bytes_read == 0) break;  // EOF

        // Write all bytes read
        size_t to_write = static_cast<size_t>(bytes_read);
        size_t written = 0;
        while (written < to_write && !cancelled) {
            ssize_t w = write(dst_fd, buffer.data() + written, to_write - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                spdlog::error("Write error on {}: {}", dest_path.string(), strerror(errno));
                success = false;
                break;
            }
            if (w == 0) {
                spdlog::error("Write returned 0 (disk full?)");
                success = false;
                break;
            }
            written += static_cast<size_t>(w);
        }

        if (!success) break;
        total_bytes += to_write;
    }

    close(src_fd);

    // A failure or a Ctrl-C here simply never commits: the temporary file is
    // discarded and the destination keeps the contents it already had.
    if (!success || cancelled) return false;
    if (!writer.commit()) return false;

    bytes_transferred += total_bytes;
    return true;
#endif
}

// RAII guard for file descriptors (POSIX only)
#ifndef _WIN32
struct FdGuard {
    int fd = -1;
    ~FdGuard() { if (fd >= 0) close(fd); }
    FdGuard() = default;
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

// Clear a file's mtime so the size/mtime planner cannot mistake a half-patched
// file for a current one. Only ever call this on a file we have actually
// written to: an epoch mtime reads as "unknown" to every direction, so putting
// one on an untouched file makes a later upload push it over a newer remote
// object.
//
// The descriptor is the one already opened with O_NOFOLLOW and checked against
// the base directory, so futimens cannot be redirected by a symlink swapped in
// behind us. Only the exception path, which no longer holds it, goes by name,
// and that one refuses to follow links.
static bool mark_incomplete_diff_download(int fd,
                                          const fs::path& path,
                                          const std::string& relative_path) {
    struct ::timespec times[2];
    times[0].tv_sec = 0; times[0].tv_nsec = UTIME_OMIT;  // atime is not ours to reset
    times[1].tv_sec = 0; times[1].tv_nsec = 0;           // mtime: the epoch, i.e. "unknown"

    const int rc = (fd >= 0)
        ? ::futimens(fd, times)
        : ::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);

    if (rc != 0) {
        spdlog::warn("Diff download failed for {} and its mtime could not be cleared: {}. "
                     "A retry may skip this partially patched file.",
                     relative_path, strerror(errno));
        return false;
    }
    return true;
}
#endif

// Perform differential download of a large file using pwrite
// Downloads only changed chunks, copies unchanged chunks from existing local file
// Returns true on success, updates bytes_transferred and bytes_saved
static bool diff_download_file(
    const std::string& local_base_path,
    const std::string& relative_path,
    const std::string& bucket,
    const std::string& s3_prefix,
    int64_t s3_size,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_transferred,
    std::atomic<size_t>& bytes_saved,
    bool debug,
    const std::atomic<bool>& cancelled,
    size_t max_parallel_chunks = 32
) {
#ifdef _WIN32
    // On Windows, fall back to full download (pwrite not available)
    return download_small_file(local_base_path, relative_path, bucket, s3_prefix,
                               s3_size, s3_client, bytes_transferred, cancelled);
#else
    // Validate s3_size is non-negative
    if (s3_size < 0) {
        spdlog::error("Invalid s3_size {} for diff_download: {}", s3_size, relative_path);
        return false;
    }

    // Handle zero-size files - just create an empty file
    if (s3_size == 0) {
        if (!validate_relative_path(relative_path)) {
            return false;
        }
        fs::path full_path = fs::path(local_base_path) / relative_path;
        if (!validate_target_within_base(local_base_path, full_path)) {
            return false;
        }
        if (!validate_target_resolves_within_base(local_base_path, full_path)) {
            return false;
        }
        // Note: directories are pre-created in run_sync for downloads
        // Create empty file (or truncate existing) using FdGuard for RAII
        // O_NOFOLLOW prevents symlink attacks
        FdGuard guard;
        guard.fd = open(full_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (guard.fd < 0) {
            spdlog::error("Failed to create empty file: {}", full_path.string());
            return false;
        }
        return true;  // FdGuard destructor closes fd
    }

    // Validate relative path to prevent path traversal
    if (!validate_relative_path(relative_path)) {
        return false;
    }

    // Construct S3 key
    std::string key = s3_prefix;
    if (!key.empty() && key.back() != '/') {
        key += '/';
    }
    key += relative_path;

    // Construct local path
    fs::path full_path = fs::path(local_base_path) / relative_path;

    // Security: verify target resolves within base directory (symlink protection)
    if (!validate_target_within_base(local_base_path, full_path)) {
        return false;
    }
    if (!validate_target_resolves_within_base(local_base_path, full_path)) {
        return false;
    }

    // Set as soon as one byte of new content lands in the file, including a
    // partial chunk write. Below this point nothing has touched the file, and
    // the failure paths use it to tell "we patched it and stopped half way"
    // from "we never got as far as writing".
    std::atomic<bool> wrote_any{false};

    try {
        // Get S3 CRC32s
        std::vector<uint32_t> s3_crcs = s3_client->GetChunkCRC32s(
            bucket, key, s3_size, {}, nullptr, debug
        );
        if (s3_crcs.empty()) {
            spdlog::error("Failed to get S3 CRC32s for: {}", key);
            // Fall back to full download
            return download_small_file(local_base_path, relative_path, bucket, s3_prefix,
                                       s3_size, s3_client, bytes_transferred, cancelled);
        }

        size_t num_chunks = s3_crcs.size();

        // Compute local CRC32s if file exists
        std::vector<uint32_t> local_crcs;
        int64_t local_size = 0;
        if (fs::exists(full_path)) {
            std::error_code ec;
            local_size = fs::file_size(full_path, ec);
            if (!ec && local_size > 0) {
                local_crcs = compute_crc32_chunks_boost_asio(
                    full_path.string(), {}, nullptr, DEFAULT_CHUNK_SIZE
                );
            }
        }

        // Determine which chunks are different
        std::vector<bool> chunk_different(num_chunks, true);
        size_t unchanged_chunks = 0;

        if (!local_crcs.empty()) {
            size_t common_chunks = std::min(s3_crcs.size(), local_crcs.size());
            for (size_t i = 0; i < common_chunks; ++i) {
                if (s3_crcs[i] == local_crcs[i]) {
                    chunk_different[i] = false;
                    unchanged_chunks++;
                }
            }
        }

        if (debug) {
            spdlog::debug("diff_download: {} chunks, {} unchanged", num_chunks, unchanged_chunks);
        }

        // If all chunks unchanged and sizes match, nothing to do
        if (unchanged_chunks == num_chunks && local_size == s3_size) {
            if (debug) {
                spdlog::debug("diff_download: file unchanged, skipping");
            }
            return true;
        }

        // If file doesn't exist or is completely different, just do full download
        if (local_crcs.empty() || unchanged_chunks == 0) {
            if (debug) {
                spdlog::debug("diff_download: no reusable chunks, falling back to full download");
            }
            return download_small_file(local_base_path, relative_path, bucket, s3_prefix,
                                       s3_size, s3_client, bytes_transferred, cancelled);
        }

        // Open file for read/write (create if doesn't exist)
        // O_NOFOLLOW prevents symlink attacks
        FdGuard fd_guard;
        fd_guard.fd = open(full_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
        if (fd_guard.fd < 0) {
            spdlog::error("Failed to open file for writing: {}", full_path.string());
            return false;
        }
        int fd = fd_guard.fd;

        // Collect indices of chunks that need downloading
        std::vector<size_t> chunks_to_download;
        for (size_t i = 0; i < num_chunks; ++i) {
            if (chunk_different[i]) {
                chunks_to_download.push_back(i);
            } else {
                // Chunk unchanged - count as saved
                int64_t chunk_start = static_cast<int64_t>(i) * DEFAULT_CHUNK_SIZE;
                int64_t chunk_end = std::min(chunk_start + DEFAULT_CHUNK_SIZE, s3_size);
                bytes_saved += (chunk_end - chunk_start);
            }
        }

        // Download chunks in parallel (capped by max_parallel_chunks parameter)
        std::atomic<bool> download_failed{false};
        std::atomic<size_t> next_chunk_idx{0};

        auto download_worker = [&]() {
            try {
                while (!cancelled && !download_failed) {
                    size_t idx = next_chunk_idx.fetch_add(1);
                    if (idx >= chunks_to_download.size()) break;

                    size_t i = chunks_to_download[idx];
                    int64_t chunk_start = static_cast<int64_t>(i) * DEFAULT_CHUNK_SIZE;
                    int64_t chunk_end = std::min(chunk_start + DEFAULT_CHUNK_SIZE, s3_size);
                    int64_t chunk_len = chunk_end - chunk_start;

                    int backoff_ms = INITIAL_BACKOFF_MS;
                    bool chunk_success = false;

                    for (int attempt = 0; attempt <= MAX_RETRIES && !chunk_success; ++attempt) {
                        if (cancelled || download_failed) break;

                        if (attempt > 0) {
                            spdlog::debug("Retry {} for chunk {} download", attempt, i);
                            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
                            backoff_ms *= 2;
                        }

                        std::vector<uint8_t> data = s3_client->GetObjectRange(
                            bucket, key, chunk_start, chunk_end - 1
                        );

                        if (data.size() == static_cast<size_t>(chunk_len)) {
                            // Write chunk using pwrite with retry loop for partial writes
                            size_t total_written = 0;
                            bool write_failed = false;
                            while (total_written < data.size()) {
                                ssize_t written = pwrite(fd, data.data() + total_written,
                                                        data.size() - total_written,
                                                        chunk_start + static_cast<int64_t>(total_written));
                                if (written < 0) {
                                    if (errno == EINTR) continue;
                                    spdlog::error("pwrite failed for chunk {}: {}", i, strerror(errno));
                                    write_failed = true;
                                    break;
                                }
                                if (written == 0) {
                                    spdlog::error("pwrite returned 0 for chunk {} (disk full?)", i);
                                    write_failed = true;
                                    break;
                                }
                                wrote_any = true;
                                total_written += static_cast<size_t>(written);
                            }
                            if (!write_failed && total_written == data.size()) {
                                chunk_success = true;
                                bytes_transferred += chunk_len;
                            }
                        }
                    }

                    if (!chunk_success && !cancelled) {
                        spdlog::error("Failed to download chunk {} for: {}", i, key);
                        download_failed = true;
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("Exception in download_worker: {}", e.what());
                download_failed = true;
            }
        };

        // Launch parallel download threads
        size_t num_workers = std::min(max_parallel_chunks, chunks_to_download.size());
        std::vector<std::thread> workers;
        workers.reserve(num_workers);
        for (size_t w = 0; w < num_workers; ++w) {
            workers.emplace_back(download_worker);
        }
        for (auto& t : workers) {
            t.join();
        }

        if (cancelled || download_failed) {
            const char* how = cancelled ? "cancelled" : "failed";
            if (!wrote_any) {
                // Nothing was written, so the file is exactly what it was
                // before. Say so rather than clearing an mtime we have no
                // reason to distrust.
                spdlog::warn("Diff download {} for {} before any data was written; "
                             "the local file is unchanged", how, relative_path);
                return false;  // FdGuard closes fd
            }
            // The in-place patch left a blend of old and new chunks. Keep the
            // file, but clear its mtime so the size/mtime planner cannot
            // classify it as current on the next run.
            const bool marked = mark_incomplete_diff_download(fd, full_path, relative_path);
            spdlog::warn("Diff download {} for {}: file may be in inconsistent state{}",
                         how,
                         relative_path,
                         marked ? "; mtime was cleared so a retry will repair it" : "");
            return false;  // FdGuard closes fd
        }

        // Truncate file if S3 version is smaller
        if (s3_size < local_size) {
            if (ftruncate(fd, s3_size) != 0) {
                spdlog::error("Failed to truncate file: {}", full_path.string());
                // A failed truncate changes nothing, so the file is only
                // inconsistent if the chunk writes above already made it so.
                if (wrote_any) {
                    const bool marked = mark_incomplete_diff_download(fd, full_path, relative_path);
                    spdlog::warn("File {} may be in inconsistent state{}",
                                 relative_path,
                                 marked ? "; mtime was cleared so a retry will repair it" : "");
                }
                return false;  // FdGuard closes fd
            }
        }

        // Sync to disk (FdGuard will close fd on scope exit)
        if (fsync(fd) != 0) {
            spdlog::warn("fsync failed for {}: {}", full_path.string(), strerror(errno));
        }

        if (debug) {
            spdlog::debug("diff_download: completed {} with {} chunks ({} transferred, {} saved)",
                         key, num_chunks, bytes_transferred.load(), bytes_saved.load());
        }

        return true;

    } catch (const std::exception& e) {
        spdlog::error("Exception in diff_download_file: {}", e.what());
        // Everything that can throw here - the CRC fetch, the local CRC pass,
        // starting the workers - throws before the first write, and the workers
        // turn their own exceptions into download_failed rather than unwinding
        // through this. So this normally leaves the file untouched, and only
        // marks it when the flag proves otherwise. The descriptor is gone by
        // now, so this call site goes by name.
        if (wrote_any) {
            const bool marked = mark_incomplete_diff_download(-1, full_path, relative_path);
            spdlog::warn("File {} may be in inconsistent state{}",
                         relative_path,
                         marked ? "; mtime was cleared so a retry will repair it" : "");
        }
        return false;
    }
#endif
}

// Copy an S3 object server-side with retry logic
static bool copy_s3_object(
    const std::string& source_bucket,
    const std::string& source_key,
    const std::string& dest_bucket,
    const std::string& dest_key,
    int64_t file_size,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_copied,
    const std::atomic<bool>& cancelled
) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return false;

        if (attempt > 0) {
            spdlog::debug("Retry {} for copy: {}", attempt, source_key);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        if (s3_client->CopyObject(source_bucket, source_key,
                                   dest_bucket, dest_key, file_size, &cancelled)) {
            bytes_copied += static_cast<size_t>(file_size);

            // Record server-side bytes in CloudMetrics for cost estimation.
            // The monitoring layer can't determine size for full-object copies (no range header).
            // For multipart copies (>5GB), the monitoring layer tracks bytes via UploadPartCopy
            // requests which DO have range headers, so we only supplement for simple copies.
            constexpr int64_t FIVE_GB = 5LL * 1024 * 1024 * 1024;
            if (file_size <= FIVE_GB) {
                S3OperationType op_type = (source_bucket == dest_bucket)
                    ? S3OperationType::CopyObject
                    : S3OperationType::CopyObjectRemote;
                CloudMetrics::instance().addServerSideBytes(op_type, static_cast<uint64_t>(file_size));
            }

            return true;
        }
    }
    spdlog::error("Copy failed after {} attempts: {}", MAX_RETRIES + 1, source_key);
    return false;
}

// Retry helper for UploadPart
static S3PartResult upload_part_with_retry(
    std::shared_ptr<IS3Client> s3_client,
    const std::string& bucket,
    const std::string& key,
    const std::string& upload_id,
    int part_number,
    const std::vector<uint8_t>& data,
    uint32_t crc32,
    const std::atomic<bool>& cancelled
) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return {};

        if (attempt > 0) {
            spdlog::debug("Retry {} for UploadPart {}", attempt, part_number);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        S3PartResult result = s3_client->UploadPart(bucket, key, upload_id, part_number, data, crc32);
        if (result.ok()) {
            return result;
        }
    }
    return {};
}

// Retry helper for UploadPartCopy
static S3PartResult upload_part_copy_with_retry(
    std::shared_ptr<IS3Client> s3_client,
    const std::string& bucket,
    const std::string& key,
    const std::string& upload_id,
    int part_number,
    const std::string& source_bucket,
    const std::string& source_key,
    int64_t start_byte,
    int64_t end_byte,
    const std::atomic<bool>& cancelled
) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
        if (cancelled) return {};

        if (attempt > 0) {
            spdlog::debug("Retry {} for UploadPartCopy {}", attempt, part_number);
            std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
            backoff_ms *= 2;
        }

        S3PartResult result = s3_client->UploadPartCopy(
            bucket, key, upload_id, part_number,
            source_bucket, source_key, start_byte, end_byte
        );
        if (result.ok()) {
            return result;
        }
    }
    return {};
}

// Perform differential upload of a large file
// Returns true on success, updates bytes_transferred and bytes_saved
static bool diff_upload_file(
    const std::string& local_base_path,
    const std::string& relative_path,
    const std::string& bucket,
    const std::string& s3_prefix,
    std::shared_ptr<IS3Client> s3_client,
    std::atomic<size_t>& bytes_transferred,
    std::atomic<size_t>& bytes_saved,
    bool debug,
    const std::atomic<bool>& cancelled
) {
    fs::path full_local_path = fs::path(local_base_path) / relative_path;

    // Construct S3 key
    std::string key = s3_prefix;
    if (!key.empty() && key.back() != '/') {
        key += '/';
    }
    key += relative_path;

    // Track upload_id for cleanup in exception handler
    std::string upload_id;

    try {
        // Get local file size
        std::error_code ec;
        int64_t local_size = fs::file_size(full_local_path, ec);
        if (ec || local_size <= 0) {
            spdlog::error("Failed to get size of local file: {}", full_local_path.string());
            return false;
        }

        // Compute local CRC32s
        std::vector<uint32_t> local_crcs = compute_crc32_chunks_boost_asio(
            full_local_path.string(), {}, nullptr, DEFAULT_CHUNK_SIZE
        );
        if (local_crcs.empty()) {
            spdlog::error("Failed to compute CRC32s for: {}", full_local_path.string());
            return false;
        }

        // Get S3 file size
        int64_t s3_size = s3_client->GetObjectSize(bucket, key);

        // Get S3 CRC32s (if file exists in S3)
        std::vector<uint32_t> s3_crcs;
        if (s3_size > 0) {
            s3_crcs = s3_client->GetChunkCRC32s(bucket, key, s3_size, {}, nullptr, debug);
        }

        // Determine which chunks are different
        size_t num_chunks = local_crcs.size();
        std::vector<bool> chunk_different(num_chunks, true);
        size_t unchanged_chunks = 0;

        if (s3_size > 0 && !s3_crcs.empty()) {
            size_t common_chunks = std::min(local_crcs.size(), s3_crcs.size());
            for (size_t i = 0; i < common_chunks; ++i) {
                if (local_crcs[i] == s3_crcs[i]) {
                    chunk_different[i] = false;
                    unchanged_chunks++;
                }
            }
        }

        if (debug) {
            spdlog::debug("diff_upload: {} chunks, {} unchanged", num_chunks, unchanged_chunks);
        }

        // If all chunks are unchanged and sizes match, nothing to do
        if (unchanged_chunks == num_chunks && local_size == s3_size) {
            if (debug) {
                spdlog::debug("diff_upload: file unchanged, skipping");
            }
            return true;
        }

        // With nothing to reuse, a single PutObject is one request instead of
        // one per chunk plus a create and a complete, so it is the cheaper way
        // to send the same bytes - but PutObject stops at 5 GiB. Above that
        // there is no fallback to take: the file has to go up as parts. The
        // loop below already does exactly that when every chunk is marked
        // different, which is the case here, so let it (issues #91, #96).
        const bool nothing_to_reuse = (s3_size <= 0 || s3_crcs.empty() || unchanged_chunks == 0);
        if (nothing_to_reuse && static_cast<uint64_t>(local_size) <= kMaxSinglePutBytes) {
            if (debug) {
                spdlog::debug("diff_upload: no reusable chunks, falling back to full upload");
            }
            std::atomic<size_t> dummy{0};
            bool result = upload_small_file(local_base_path, relative_path, bucket, s3_prefix, s3_client, dummy, cancelled);
            if (result) {
                bytes_transferred += local_size;
            }
            return result;
        }

        // One chunk becomes one part, and S3 takes at most 10,000 of them.
        // Saying so now costs one message; finding out at CompleteMultipartUpload
        // costs the whole transfer first. See #32 for raising the ceiling by
        // sizing parts to the file rather than refusing.
        if (num_chunks > kMaxMultipartParts) {
            spdlog::error(
                "{} is {} bytes, which needs {} parts of {} MiB - over the {}-part limit "
                "for a multipart upload. Uploading a file this large needs a larger part "
                "size, which this path does not choose.",
                full_local_path.string(), local_size, num_chunks,
                DEFAULT_CHUNK_SIZE / (1024 * 1024), kMaxMultipartParts);
            return false;
        }

        if (debug && nothing_to_reuse) {
            spdlog::debug("diff_upload: no reusable chunks but {} bytes is over the "
                          "single-PUT limit, uploading all {} chunks as parts",
                          local_size, num_chunks);
        }

        // Create multipart upload
        upload_id = s3_client->CreateMultipartUpload(bucket, key);
        if (upload_id.empty()) {
            spdlog::error("Failed to create multipart upload for: {}", key);
            return false;
        }

        std::vector<std::pair<int, S3PartResult>> completed_parts;
        bool upload_failed = false;

        // Open local file for reading changed chunks
        std::ifstream file(full_local_path, std::ios::binary);
        if (!file) {
            spdlog::error("Failed to open local file: {}", full_local_path.string());
            s3_client->AbortMultipartUpload(bucket, key, upload_id);
            return false;
        }

        for (size_t i = 0; i < num_chunks && !upload_failed; ++i) {
            int part_number = static_cast<int>(i + 1);  // S3 parts are 1-indexed
            int64_t chunk_start = static_cast<int64_t>(i) * DEFAULT_CHUNK_SIZE;
            int64_t chunk_end = std::min(chunk_start + DEFAULT_CHUNK_SIZE, local_size);
            int64_t chunk_len = chunk_end - chunk_start;

            S3PartResult part_result;

            if (!chunk_different[i] && i < s3_crcs.size()) {
                // Chunk unchanged - copy from existing object (with retry)
                part_result = upload_part_copy_with_retry(
                    s3_client, bucket, key, upload_id, part_number,
                    bucket, key,  // source is the same object
                    chunk_start, chunk_end - 1,  // S3 uses inclusive range
                    cancelled
                );
                if (part_result.ok()) {
                    bytes_saved += chunk_len;
                }
            } else {
                // Chunk changed - upload new data (with retry)
                std::vector<uint8_t> data(chunk_len);
                file.seekg(chunk_start);
                file.read(reinterpret_cast<char*>(data.data()), chunk_len);

                if (file.gcount() != chunk_len) {
                    spdlog::error("Failed to read chunk {} from file", i);
                    upload_failed = true;
                    break;
                }

                part_result = upload_part_with_retry(
                    s3_client, bucket, key, upload_id, part_number,
                    data, local_crcs[i],
                    cancelled
                );
                if (part_result.ok()) {
                    bytes_transferred += chunk_len;
                }
            }

            if (!part_result.ok()) {
                spdlog::error("Failed to upload/copy part {} for: {}", part_number, key);
                upload_failed = true;
            } else {
                completed_parts.emplace_back(part_number, part_result);
            }
        }

        if (upload_failed) {
            s3_client->AbortMultipartUpload(bucket, key, upload_id);
            return false;
        }

        // Complete the multipart upload
        if (!s3_client->CompleteMultipartUpload(bucket, key, upload_id, completed_parts)) {
            spdlog::error("Failed to complete multipart upload for: {}", key);
            s3_client->AbortMultipartUpload(bucket, key, upload_id);
            return false;
        }

        if (debug) {
            spdlog::debug("diff_upload: completed {} with {} chunks ({} transferred, {} saved)",
                         key, num_chunks, bytes_transferred.load(), bytes_saved.load());
        }

        return true;

    } catch (const std::exception& e) {
        spdlog::error("Exception in diff_upload_file: {}", e.what());
        // Clean up multipart upload if one was created
        if (!upload_id.empty()) {
            s3_client->AbortMultipartUpload(bucket, key, upload_id);
        }
        return false;
    }
}

SyncResult run_sync(
    const SyncConfig& config,
    SyncProgress& progress,
    std::shared_ptr<IS3Client> s3_client
) {
    SyncResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Validate max_threads
    // S3-to-S3 uses adaptive concurrency (starts at 64, ramps up on success, backs off on failure)
    // so we allow the user-specified max and let the system find the optimal value.
    // Note: high concurrency requires ulimit -n 4096+ to avoid file descriptor exhaustion
    int effective_max_threads = std::max(1, config.max_threads);
    if (config.max_threads < 1) {
        spdlog::warn("max_threads ({}) is less than 1, clamping to 1", config.max_threads);
    }

    // Validate source and destination for LocalToLocal
    if (config.direction == SyncDirection::LocalToLocal) {
        // Normalize paths to detect overlapping directories
        std::error_code ec;
        fs::path source_canonical = fs::weakly_canonical(config.local_path, ec);
        if (ec) source_canonical = config.local_path;
        fs::path dest_canonical = fs::weakly_canonical(config.dest_local_path, ec);
        if (ec) dest_canonical = config.dest_local_path;

        // Ensure paths end with separator for proper prefix comparison
        std::string source_str = source_canonical.string();
        std::string dest_str = dest_canonical.string();
        if (!source_str.empty() && source_str.back() != '/') source_str += '/';
        if (!dest_str.empty() && dest_str.back() != '/') dest_str += '/';

        if (source_str == dest_str) {
            result.error_message = "Source and destination paths are the same";
            return result;
        }
        // Check if destination is inside source (would cause infinite recursion)
        if (dest_str.find(source_str) == 0) {
            result.error_message = "Destination is inside source directory";
            return result;
        }
        // Check if source is inside destination (would cause data loss)
        if (source_str.find(dest_str) == 0) {
            result.error_message = "Source is inside destination directory";
            return result;
        }
    }

#ifndef _WIN32
    // Keep dry-run side-effect free, but still refuse temp files before they can
    // show up in a misleading plan as normal user data.
    {
        std::string temp_sweep_error;
        const TempSweepMode temp_sweep_mode = config.dry_run
            ? TempSweepMode::RefuseOrphans
            : TempSweepMode::RemoveOrphans;
        if (!sweep_local_sync_temps(config, temp_sweep_mode, temp_sweep_error)) {
            result.error_message = temp_sweep_error;
            spdlog::error("{}", result.error_message);
            return result;
        }
    }
#endif

    try {
        // For S3-to-S3, we may need two clients (source and dest regions)
        std::shared_ptr<IS3Client> source_client = s3_client;
        std::shared_ptr<IS3Client> dest_client = s3_client;

        // Create S3 client(s) if not provided (large connection pool for high concurrency)
        // Skip for LocalToLocal - no S3 access needed
        if (!s3_client && config.direction != SyncDirection::LocalToLocal) {
            // Connection pool size: 2x max_threads for headroom (retries, etc.)
            int connection_pool_size = std::max(512, effective_max_threads * 2);

            if (config.direction == SyncDirection::S3ToS3) {
                // S3-to-S3: may need separate clients for different regions.
                //
                // For cross-region copies:
                //   - source_client: connects to source region, used only for listing source bucket
                //   - dest_client: connects to dest region, used for listing dest bucket AND copy operations
                //
                // AWS S3 CopyObject must be called against the destination bucket's region.
                // The source is specified via the x-amz-copy-source header, and AWS handles
                // the cross-region data transfer server-side.
                source_client = CreateS3Client(config.source.region, config.source.endpoint, connection_pool_size, config.source.profile);
                if (!source_client) {
                    result.error_message = "Failed to create source S3 client";
                    return result;
                }
                if (!should_reuse_s3_client(config.source, config.destination)) {
                    dest_client = CreateS3Client(config.destination.region, config.destination.endpoint, connection_pool_size, config.destination.profile);
                    if (!dest_client) {
                        result.error_message = "Failed to create destination S3 client";
                        return result;
                    }
                } else {
                    dest_client = source_client;
                }
                // Use dest_client for copy operations (CopyObject targets destination region)
                s3_client = dest_client;
            } else {
                // Use source side for downloads, destination side for uploads
                const FileSource& s3_side = (config.direction == SyncDirection::Download)
                                            ? config.source : config.destination;
                s3_client = CreateS3Client(s3_side.region, s3_side.endpoint, connection_pool_size, s3_side.profile);
                if (!s3_client) {
                    result.error_message = "Failed to create S3 client";
                    return result;
                }
                source_client = s3_client;
                dest_client = s3_client;
            }
        }

        // Phase 1: Enumerate and classify
        std::vector<SyncFileEntry> files;
        // Set by the classifier: false if either side could not be listed in
        // full, in which case the inventory is a subset of reality.
        bool enumeration_ok = true;
        if (config.direction == SyncDirection::S3ToS3) {
            spdlog::info("Scanning s3://{}/{} and s3://{}/{}",
                         config.source.bucket,
                         config.source.path,
                         config.destination.bucket,
                         config.destination.path);
            files = classify_files_s3_to_s3(config, progress, source_client, dest_client, enumeration_ok);
        } else if (config.direction == SyncDirection::Download) {
            spdlog::info("Scanning s3://{}/{} and {}",
                         config.source.bucket,
                         config.source.path,
                         config.local_path);
            files = classify_files_download(config, progress, s3_client, enumeration_ok);
        } else if (config.direction == SyncDirection::LocalToLocal) {
            spdlog::info("Scanning {} and {}",
                         config.local_path,
                         config.dest_local_path);
            files = classify_files_local_to_local(config, progress, enumeration_ok);
        } else {
            spdlog::info("Scanning {} and s3://{}/{}",
                         config.local_path,
                         config.destination.bucket,
                         config.destination.path);
            files = classify_files(config, progress, s3_client, enumeration_ok);
        }

        if (progress.cancelled) {
            result.error_message = "Cancelled during enumeration";
            return result;
        }

        // A partial listing is indistinguishable from a smaller tree. Acting on
        // one would upload an arbitrary subset and, with --delete, remove every
        // destination entry that the failed listing did not report - destroying
        // data that is present at the source. Refuse before planning anything.
        if (!enumeration_ok) {
            // Say only what is true for this run: without --delete nothing would
            // have been deleted, so claiming otherwise would be misleading.
            result.error_message =
                std::string("Source enumeration failed: the source could not be listed "
                            "completely, so the file list is a subset of what is really "
                            "there. ") +
                (config.delete_orphans
                     ? "Refusing to continue - entries missing from a partial source list "
                       "look like destination orphans and would be deleted. "
                     : "Refusing to continue - the sync would silently copy only part of "
                       "the source and report success. ") +
                "The specific paths that could not be read are logged above as errors "
                "or warnings.";
            spdlog::error("{}", result.error_message);
            return result;
        }

#ifndef _WIN32
        // A concurrent sync can create a temporary after the initial sweep but
        // before or during local enumeration. Refuse here so that race cannot
        // turn the temp into planned user data.
        {
            std::string temp_sweep_error;
            if (!sweep_local_sync_temps(config, TempSweepMode::RefuseOrphans, temp_sweep_error)) {
                result.error_message = temp_sweep_error;
                spdlog::error("{}", result.error_message);
                return result;
            }
        }
#endif

        // Count by action
        for (const auto& f : files) {
            switch (f.action) {
                case SyncAction::Skip:
                    result.files_skipped++;
                    break;
                case SyncAction::Upload:
                    result.files_uploaded++;
                    break;
                case SyncAction::UploadDiff:
                    result.files_diff_uploaded++;
                    break;
                case SyncAction::Download:
                    result.files_downloaded++;
                    break;
                case SyncAction::DownloadDiff:
                    result.files_diff_downloaded++;
                    break;
                case SyncAction::Copy:
                    result.files_copied++;
                    break;
                case SyncAction::Delete:
                    result.files_deleted++;
                    break;
            }
        }

        // Clear the progress line before logging (progress display uses \r without newline)
        if (!config.quiet) {
            std::cout << "\r\033[K" << std::flush;  // \r = start of line, \033[K = clear to end
        }
        if (config.direction == SyncDirection::S3ToS3) {
            spdlog::info("Scanned {} source S3 objects, {} dest S3 objects",
                         progress.files_scanned_s3.load(),
                         progress.files_scanned_dest.load());
            spdlog::info("Plan: {} copy, {} skip, {} delete",
                         result.files_copied, result.files_skipped, result.files_deleted);
        } else if (config.direction == SyncDirection::Download) {
            spdlog::info("Scanned {} S3 objects, {} local files",
                         progress.files_scanned_s3.load(), progress.files_scanned_local.load());
            spdlog::info("Plan: {} download, {} diff-download, {} skip, {} delete",
                         result.files_downloaded, result.files_diff_downloaded,
                         result.files_skipped, result.files_deleted);
        } else if (config.direction == SyncDirection::LocalToLocal) {
            spdlog::info("Scanned {} source files, {} dest files",
                         progress.files_scanned_local.load(),
                         progress.files_scanned_dest.load());
            spdlog::info("Plan: {} copy, {} skip, {} delete",
                         result.files_uploaded, result.files_skipped, result.files_deleted);
        } else {
            spdlog::info("Scanned {} local files, {} S3 objects",
                         progress.files_scanned_local.load(), progress.files_scanned_s3.load());
            spdlog::info("Plan: {} upload, {} diff-upload, {} skip, {} delete",
                         result.files_uploaded, result.files_diff_uploaded,
                         result.files_skipped, result.files_deleted);
        }

        result.files = std::move(files);

        // Dry-run: show detailed summary
        if (config.dry_run && !config.quiet) {
            if (config.direction == SyncDirection::S3ToS3) {
                uint64_t copy_bytes = 0;
                for (const auto& f : result.files) {
                    if (f.action == SyncAction::Copy && f.s3_size > 0)
                        copy_bytes += static_cast<uint64_t>(f.s3_size);
                }

                spdlog::info("Would copy:   {} files ({:.1f} MB)",
                            result.files_copied, copy_bytes / (1024.0 * 1024.0));
                spdlog::info("Would skip:   {} files", result.files_skipped);
                if (config.delete_orphans) {
                    spdlog::info("Would delete: {} files", result.files_deleted);
                }
            } else if (config.direction == SyncDirection::Download) {
                uint64_t download_bytes = 0, diff_bytes = 0;
                for (const auto& f : result.files) {
                    if (f.action == SyncAction::Download && f.s3_size > 0)
                        download_bytes += static_cast<uint64_t>(f.s3_size);
                    else if (f.action == SyncAction::DownloadDiff && f.s3_size > 0)
                        diff_bytes += static_cast<uint64_t>(f.s3_size);
                }

                spdlog::info("Would download:      {} files ({:.1f} MB)",
                            result.files_downloaded, download_bytes / (1024.0 * 1024.0));
                spdlog::info("Would diff-download: {} files ({:.1f} MB)",
                            result.files_diff_downloaded, diff_bytes / (1024.0 * 1024.0));
                spdlog::info("Would skip:          {} files", result.files_skipped);
                if (config.delete_orphans) {
                    spdlog::info("Would delete:        {} files", result.files_deleted);
                }
            } else if (config.direction == SyncDirection::LocalToLocal) {
                uint64_t copy_bytes = 0;
                for (const auto& f : result.files) {
                    if (f.action == SyncAction::Upload && f.local_size > 0)
                        copy_bytes += static_cast<uint64_t>(f.local_size);
                }

                spdlog::info("Would copy:   {} files ({:.1f} MB)",
                            result.files_uploaded, copy_bytes / (1024.0 * 1024.0));
                spdlog::info("Would skip:   {} files", result.files_skipped);
                if (config.delete_orphans) {
                    spdlog::info("Would delete: {} files", result.files_deleted);
                }
            } else {
                uint64_t upload_bytes = 0, diff_bytes = 0;
                for (const auto& f : result.files) {
                    if (f.action == SyncAction::Upload && f.local_size > 0)
                        upload_bytes += static_cast<uint64_t>(f.local_size);
                    else if (f.action == SyncAction::UploadDiff && f.local_size > 0)
                        diff_bytes += static_cast<uint64_t>(f.local_size);
                }

                spdlog::info("Would upload:      {} files ({:.1f} MB)",
                            result.files_uploaded, upload_bytes / (1024.0 * 1024.0));
                spdlog::info("Would diff-upload: {} files ({:.1f} MB)",
                            result.files_diff_uploaded, diff_bytes / (1024.0 * 1024.0));
                spdlog::info("Would skip:        {} files", result.files_skipped);
                if (config.delete_orphans) {
                    spdlog::info("Would delete:      {} files", result.files_deleted);
                }
            }
        }

        // Phase 2: Execute with parallelization
        if (!config.dry_run) {
            // Reset counts for actual execution
            result.files_uploaded = 0;
            result.files_diff_uploaded = 0;
            result.files_downloaded = 0;
            result.files_diff_downloaded = 0;
            result.files_copied = 0;
            result.files_deleted = 0;
            result.files_failed = 0;

            std::atomic<size_t> bytes_saved{0};
            // Note: We use progress.bytes_transferred directly for real-time progress display
            std::atomic<size_t> upload_count{0};
            std::atomic<size_t> diff_upload_count{0};
            std::atomic<size_t> download_count{0};
            std::atomic<size_t> diff_download_count{0};
            std::atomic<size_t> copy_count{0};
            std::atomic<size_t> delete_count{0};
            std::atomic<size_t> fail_count{0};

            // Filter actionable files
            std::vector<const SyncFileEntry*> actionable;
            for (const auto& f : result.files) {
                if (f.action != SyncAction::Skip) {
                    actionable.push_back(&f);
                }
            }

            // Update files_total to only count actionable files (not skipped)
            progress.files_total = actionable.size();

            // Sort by size: smallest first for maximum throughput on small files
            // Deletes go last (no size, quick operations)
            std::sort(actionable.begin(), actionable.end(),
                [](const SyncFileEntry* a, const SyncFileEntry* b) {
                    // Deletes go last
                    if (a->action == SyncAction::Delete && b->action != SyncAction::Delete) return false;
                    if (a->action != SyncAction::Delete && b->action == SyncAction::Delete) return true;
                    // For uploads, use local_size; for downloads, use s3_size
                    int64_t size_a = (a->action == SyncAction::Download || a->action == SyncAction::DownloadDiff)
                                     ? a->s3_size : a->local_size;
                    int64_t size_b = (b->action == SyncAction::Download || b->action == SyncAction::DownloadDiff)
                                     ? b->s3_size : b->local_size;
                    return size_a < size_b;
                });

            if (!actionable.empty()) {
                // Use thread pool with aggressive concurrency for small file performance
                size_t max_concurrency = std::min(static_cast<size_t>(effective_max_threads), actionable.size());
                // For S3-to-S3: start lower and ramp up aggressively to find optimal concurrency
                // For upload/download: start at full concurrency (bandwidth-limited, not connection-limited)
                size_t initial_concurrency = max_concurrency;
                if (config.direction == SyncDirection::S3ToS3) {
                    // S3 copies are fast (~30ms), so connection churn is the bottleneck
                    // Start at 64 and ramp up to find the sweet spot for this system
                    initial_concurrency = std::min(max_concurrency, static_cast<size_t>(64));
                }

                std::mutex concurrency_mutex;
                std::condition_variable concurrency_cv;
                std::atomic<size_t>& in_flight = progress.files_in_flight;  // Use progress counter
                std::atomic<size_t> current_max_concurrency{initial_concurrency};
                std::atomic<size_t> completed{0};
                std::atomic<size_t> consecutive_successes{0};  // For gradual concurrency recovery

                auto pool = std::make_unique<boost::asio::thread_pool>(max_concurrency);

                // Pre-read/pre-fetch cache for pipelining I/O
                std::unordered_map<std::string, std::vector<uint8_t>> io_cache;
                size_t io_cache_bytes = 0;
                std::mutex io_cache_mutex;
                std::condition_variable io_cache_cv;
                std::atomic<bool> io_cache_done{false};

                // Async disk write queue - decouples network from disk I/O
                struct WriteJob {
                    std::string local_path;
                    std::string relative_path;
                    std::vector<uint8_t> data;
                };
                std::queue<WriteJob> write_queue;
                std::mutex write_queue_mutex;
                std::condition_variable write_queue_cv;
                std::atomic<bool> write_queue_done{false};
                std::atomic<size_t> write_queue_success{0};
                std::atomic<size_t> write_queue_fail{0};
                constexpr size_t WRITE_QUEUE_MAX_SIZE = 1024;  // Backpressure limit

                // Buffer pool for memory reuse - avoids alloc/free per file
                std::queue<std::vector<uint8_t>> buffer_pool;
                std::mutex buffer_pool_mutex;
                constexpr size_t BUFFER_POOL_MAX = 512;

                auto get_buffer = [&]() -> std::vector<uint8_t> {
                    std::lock_guard<std::mutex> lock(buffer_pool_mutex);
                    if (!buffer_pool.empty()) {
                        auto buf = std::move(buffer_pool.front());
                        buffer_pool.pop();
                        buf.clear();  // Reset size, keep capacity
                        return buf;
                    }
                    return {};
                };

                auto return_buffer = [&](std::vector<uint8_t>&& buf) {
                    if (buf.capacity() > 0) {
                        std::lock_guard<std::mutex> lock(buffer_pool_mutex);
                        if (buffer_pool.size() < BUFFER_POOL_MAX) {
                            buf.clear();
                            buffer_pool.push(std::move(buf));
                        }
                    }
                };

                // For downloads: pre-create all directories upfront to avoid per-file syscalls
                if (config.direction == SyncDirection::Download) {
                    // Collect the distinct directories first, then validate each
                    // one once. Validating per file resolved the same directory
                    // repeatedly - measured at up to +88% wall time on a deep
                    // tree - and logged an identical refusal per file rather
                    // than per directory.
                    std::set<std::string> dirs_to_create;
                    for (const auto* f : actionable) {
                        if (f->action == SyncAction::Download || f->action == SyncAction::DownloadDiff) {
                            // Validate path before adding to directory list
                            if (!validate_relative_path(f->relative_path)) {
                                continue;  // Skip malicious paths
                            }
                            fs::path full_path = fs::path(config.local_path) / f->relative_path;
                            if (!validate_target_within_base(config.local_path, full_path)) {
                                continue;  // Skip paths that escape base directory
                            }
                            fs::path dir = full_path.parent_path();
                            if (!dir.empty() && dir != fs::path(config.local_path)) {
                                dirs_to_create.insert(dir.string());
                            }
                        }
                    }
                    // fs::create_directories follows symlinks, so a symlinked
                    // parent would have directories created outside the tree
                    // (issue #54). The per-file write path re-checks anyway, so
                    // a directory refused here simply never gets created.
                    for (auto it = dirs_to_create.begin(); it != dirs_to_create.end(); ) {
                        if (validate_target_resolves_within_base(config.local_path,
                                                                 fs::path(*it) / "x")) {
                            ++it;
                        } else {
                            it = dirs_to_create.erase(it);
                        }
                    }
                    for (const auto& dir : dirs_to_create) {
                        std::error_code ec;
                        fs::create_directories(dir, ec);
                        if (ec) {
                            spdlog::debug("Failed to pre-create directory {}: {}", dir, ec.message());
                        }
                    }
                }

                // Helper to construct S3 key for source (download) or destination (upload)
                auto make_s3_key_download = [&](const std::string& relative_path) {
                    std::string key = config.source.path;
                    if (!key.empty() && key.back() != '/') key += '/';
                    key += relative_path;
                    return key;
                };

                // Spawn background disk writer threads for downloads
                std::vector<std::thread> disk_writer_threads;
                if (config.direction == SyncDirection::Download) {
                    for (size_t t = 0; t < DISK_WRITER_THREADS; ++t) {
                        disk_writer_threads.emplace_back([&]() {
                            while (true) {
                                WriteJob job;
                                {
                                    std::unique_lock<std::mutex> lock(write_queue_mutex);
                                    write_queue_cv.wait(lock, [&] {
                                        return !write_queue.empty() || write_queue_done;
                                    });
                                    if (write_queue.empty() && write_queue_done) break;
                                    if (write_queue.empty()) continue;
                                    job = std::move(write_queue.front());
                                    write_queue.pop();
                                }
                                write_queue_cv.notify_one();  // Notify producers waiting on backpressure

                                // Write file to disk (updates progress.bytes_transferred for live throughput)
                                bool success = download_from_data(job.local_path, job.relative_path,
                                                                  job.data, progress.bytes_transferred);
                                if (success) {
                                    ++write_queue_success;
                                } else {
                                    ++write_queue_fail;
                                }

                                // Return buffer to pool for reuse
                                return_buffer(std::move(job.data));
                            }
                        });
                    }
                }

                // For downloads: use multiple pre-fetch threads for parallel S3 fetches
                // Scale with max_threads but cap reasonably (too many threads = diminishing returns)
                // For uploads: single thread is fine (disk I/O is typically fast)
                size_t num_prefetch = 1;
                if (config.direction == SyncDirection::Download) {
                    // For downloads, prefetch threads do the S3 work - scale aggressively
                    // Use full max_concurrency since each prefetch is an independent S3 request
                    num_prefetch = max_concurrency;
                }

                std::atomic<size_t> prefetch_index{0};
                std::vector<std::thread> io_pipeline_threads;

                for (size_t t = 0; t < num_prefetch; ++t) {
                    io_pipeline_threads.emplace_back([&, t]() {
                        try {
                            while (!progress.cancelled && !io_cache_done) {
                                // Atomically get next file index
                                size_t i = prefetch_index.fetch_add(1);
                                if (i >= actionable.size()) break;

                                const auto* f = actionable[i];

                                // Skip files that don't benefit from pre-fetch
                                bool should_prefetch =
                                    (f->action == SyncAction::Upload && f->local_size > 0 && f->local_size <= PRE_READ_MAX_FILE_SIZE) ||
                                    (f->action == SyncAction::Download && f->s3_size > 0 && f->s3_size <= PRE_READ_MAX_FILE_SIZE);

                                if (!should_prefetch) continue;

                                // Wait if cache is too full
                                {
                                    std::unique_lock<std::mutex> lock(io_cache_mutex);
                                    io_cache_cv.wait(lock, [&] {
                                        return (io_cache.size() < PRE_READ_QUEUE_SIZE &&
                                                io_cache_bytes < PRE_READ_MAX_TOTAL_BYTES) ||
                                               progress.cancelled || io_cache_done;
                                    });
                                    if (io_cache_done || progress.cancelled) break;
                                }

                                std::vector<uint8_t> data;

                                if (f->action == SyncAction::Upload) {
                                    // Pre-read local file
                                    fs::path full_path = fs::path(config.local_path) / f->relative_path;
                                    std::ifstream file(full_path, std::ios::binary);
                                    if (file) {
                                        data.resize(f->local_size);
                                        file.read(reinterpret_cast<char*>(data.data()), f->local_size);
                                        if (file.gcount() != f->local_size) {
                                            data.clear();
                                        }
                                    }
                                } else if (f->action == SyncAction::Download) {
                                    // Pre-fetch from S3 (skip zero-byte files to avoid invalid range request)
                                    if (f->s3_size > 0) {
                                        std::string key = make_s3_key_download(f->relative_path);
                                        // Try to reuse a buffer from the pool
                                        data = get_buffer();
                                        if (!s3_client->GetObjectRangeInto(config.source.bucket, key, 0, f->s3_size - 1, data)) {
                                            return_buffer(std::move(data));
                                            data = {};  // Reset after move, will retry in main path
                                        } else if (data.size() != static_cast<size_t>(f->s3_size)) {
                                            return_buffer(std::move(data));
                                            data = {};  // Reset after move, will retry
                                        }
                                    }
                                    // Zero-byte files handled in main download path
                                }

                                if (!data.empty()) {
                                    std::lock_guard<std::mutex> lock(io_cache_mutex);
                                    if (io_cache_bytes + data.size() <= PRE_READ_MAX_TOTAL_BYTES) {
                                        io_cache_bytes += data.size();
                                        io_cache[f->relative_path] = std::move(data);
                                    }
                                }
                            }
                        } catch (const std::exception& e) {
                            spdlog::debug("I/O pipeline thread {} exception: {}", t, e.what());
                        }
                    });
                }

                // Helper to construct S3 key
                auto make_s3_key = [&](const std::string& relative_path) {
                    std::string key = config.destination.path;
                    if (!key.empty() && key.back() != '/') key += '/';
                    key += relative_path;
                    return key;
                };

                // Returns true on success, false on failure
                auto process_file = [&](const SyncFileEntry* f, std::vector<uint8_t>* pre_read_data) -> bool {
                    if (progress.cancelled) return false;

                    progress.set_current_file(f->relative_path);

                    bool success = false;
                    switch (f->action) {
                        case SyncAction::Upload:
                            if (config.direction == SyncDirection::LocalToLocal) {
                                // Local-to-local copy
                                success = copy_local_file(config.local_path, config.dest_local_path,
                                                         f->relative_path, progress.bytes_transferred,
                                                         progress.cancelled);
                            } else if (pre_read_data && !pre_read_data->empty()) {
                                // Use pre-read data for faster upload
                                success = upload_from_data(
                                    config.destination.bucket,
                                    make_s3_key(f->relative_path),
                                    *pre_read_data,
                                    s3_client, progress.bytes_transferred,
                                    progress.cancelled);
                            } else {
                                // Fall back to reading from disk
                                success = upload_small_file(config.local_path, f->relative_path,
                                                           config.destination.bucket,
                                                           config.destination.path,
                                                           s3_client, progress.bytes_transferred,
                                                           progress.cancelled);
                            }
                            if (success) upload_count++;
                            else if (!progress.cancelled) fail_count++;
                            break;

                        case SyncAction::UploadDiff:
                            success = diff_upload_file(config.local_path, f->relative_path,
                                                      config.destination.bucket,
                                                      config.destination.path,
                                                      s3_client, progress.bytes_transferred,
                                                      bytes_saved, config.debug,
                                                      progress.cancelled);
                            if (success) diff_upload_count++;
                            else if (!progress.cancelled) fail_count++;
                            break;

                        case SyncAction::Download: {
                            std::vector<uint8_t> data_to_write;

                            if (pre_read_data && !pre_read_data->empty()) {
                                // Move pre-fetched data (avoids copy)
                                data_to_write = std::move(*pre_read_data);
                            } else {
                                // Fetch from S3 with retry
                                std::string key = config.source.path;
                                if (!key.empty() && key.back() != '/') key += '/';
                                key += f->relative_path;

                                if (f->s3_size == 0) {
                                    // Empty file - write directly (no data to queue)
                                    success = download_from_data(config.local_path, f->relative_path,
                                                                data_to_write, progress.bytes_transferred);
                                    if (success) download_count++;
                                    else if (!progress.cancelled) fail_count++;
                                    break;
                                }

                                // Retry loop for S3 fetch with buffer reuse
                                int backoff_ms = INITIAL_BACKOFF_MS;
                                bool fetch_success = false;
                                data_to_write = get_buffer();
                                for (int attempt = 0; attempt <= MAX_RETRIES && !progress.cancelled; ++attempt) {
                                    if (attempt > 0) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(add_jitter(backoff_ms)));
                                        backoff_ms *= 2;
                                    }
                                    if (s3_client->GetObjectRangeInto(
                                            config.source.bucket, key, 0, f->s3_size - 1, data_to_write) &&
                                        data_to_write.size() == static_cast<size_t>(f->s3_size)) {
                                        fetch_success = true;
                                        break;
                                    }
                                }

                                if (!fetch_success) {
                                    // The retry loop above also exits on
                                    // cancellation, which is not a failure of
                                    // this file - every other action counts it
                                    // the same way.
                                    if (!progress.cancelled) {
                                        spdlog::warn("Download failed after retries: {}", f->relative_path);
                                        fail_count++;
                                    }
                                    return_buffer(std::move(data_to_write));
                                    break;
                                }
                            }

                            // Enqueue for async disk write with backpressure
                            {
                                std::unique_lock<std::mutex> lock(write_queue_mutex);
                                write_queue_cv.wait(lock, [&] {
                                    return write_queue.size() < WRITE_QUEUE_MAX_SIZE || progress.cancelled;
                                });
                                if (!progress.cancelled) {
                                    write_queue.push(WriteJob{
                                        config.local_path,
                                        f->relative_path,
                                        std::move(data_to_write)
                                    });
                                }
                            }
                            write_queue_cv.notify_one();

                            // Mark as processed (actual count tracked by disk writers)
                            success = true;
                            break;
                        }

                        case SyncAction::DownloadDiff: {
                            // Large file diff downloads write directly (not queued)
                            // Cap parallel chunks to avoid thread explosion with multiple concurrent diff downloads
                            size_t parallel_chunks = std::min(static_cast<size_t>(32),
                                                              static_cast<size_t>(effective_max_threads));
                            success = diff_download_file(config.local_path, f->relative_path,
                                                        config.source.bucket,
                                                        config.source.path,
                                                        f->s3_size,
                                                        s3_client, progress.bytes_transferred,
                                                        bytes_saved, config.debug,
                                                        progress.cancelled,
                                                        parallel_chunks);
                            if (success) diff_download_count++;
                            else if (!progress.cancelled) fail_count++;
                            break;
                        }

                        case SyncAction::Copy: {
                            // S3-to-S3 server-side copy (no client bandwidth)
                            std::string source_key = config.source.path;
                            if (!source_key.empty() && source_key.back() != '/') source_key += '/';
                            source_key += f->relative_path;

                            std::string dest_key = config.destination.path;
                            if (!dest_key.empty() && dest_key.back() != '/') dest_key += '/';
                            dest_key += f->relative_path;

                            success = copy_s3_object(config.source.bucket, source_key,
                                                     config.destination.bucket, dest_key,
                                                     f->s3_size, s3_client,
                                                     progress.bytes_copied_server_side,
                                                     progress.cancelled);
                            if (success) copy_count++;
                            else if (!progress.cancelled) fail_count++;
                            break;
                        }

                        case SyncAction::Delete:
                            if (config.direction == SyncDirection::S3ToS3) {
                                // Delete from dest bucket
                                success = delete_s3_object(config.destination.bucket,
                                                          config.destination.path,
                                                          f->relative_path,
                                                          s3_client,
                                                          progress.cancelled);
                            } else if (config.direction == SyncDirection::Download) {
                                // Delete local file (from local dest)
                                success = delete_local_file(config.local_path,
                                                           f->relative_path,
                                                           progress.cancelled);
                            } else if (config.direction == SyncDirection::LocalToLocal) {
                                // Delete local file (from local dest)
                                success = delete_local_file(config.dest_local_path,
                                                           f->relative_path,
                                                           progress.cancelled);
                            } else {
                                // Upload: Delete S3 object
                                success = delete_s3_object(config.destination.bucket,
                                                          config.destination.path,
                                                          f->relative_path,
                                                          s3_client,
                                                          progress.cancelled);
                            }
                            if (success) delete_count++;
                            else if (!progress.cancelled) fail_count++;
                            break;

                        default:
                            break;
                    }

                    progress.files_processed++;
                    return success;
                };

                auto process_with_tracking = [&](const SyncFileEntry* f, std::vector<uint8_t> pre_read_data) {
                    struct Guard {
                        std::atomic<size_t>& in_flight;
                        std::mutex& mutex;
                        std::condition_variable& cv;
                        ~Guard() {
                            std::lock_guard<std::mutex> lk(mutex);
                            --in_flight;
                            cv.notify_one();
                        }
                    } guard{in_flight, concurrency_mutex, concurrency_cv};

                    bool success = process_file(f, pre_read_data.empty() ? nullptr : &pre_read_data);
                    completed++;

                    // Adaptive concurrency: reduce on failure, gradually increase on success
                    if (!success && !progress.cancelled) {
                        // Reset consecutive success counter on failure
                        consecutive_successes.store(0);
                        // Use compare_exchange to ensure only one thread reduces per failure
                        size_t current = current_max_concurrency.load();
                        size_t new_max = std::max(current / 2, static_cast<size_t>(16));
                        while (new_max < current) {
                            if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                                spdlog::debug("Rate limited - reducing concurrency to {}", new_max);
                                break;
                            }
                            new_max = std::max(current / 2, static_cast<size_t>(16));
                        }
                    } else if (success) {
                        // Gradually recover/increase concurrency
                        // S3-to-S3: ramp up aggressively (double every 100 ops) to find optimal
                        // Upload/Download: recover slowly after rate limiting (50% every 10 consecutive)
                        size_t successes = consecutive_successes.fetch_add(1) + 1;
                        bool is_s3_to_s3 = (config.direction == SyncDirection::S3ToS3);
                        size_t ramp_interval = is_s3_to_s3 ? 100 : 10;
                        if (successes % ramp_interval == 0) {
                            size_t current = current_max_concurrency.load();
                            if (current < max_concurrency) {
                                // S3-to-S3: double concurrency for faster ramp-up
                                // Others: increase by 50% for smoother recovery
                                size_t increment = is_s3_to_s3 ? current : std::max(current / 2, static_cast<size_t>(1));
                                size_t new_max = std::min(current + increment, max_concurrency);
                                if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                                    spdlog::debug("Increasing concurrency to {}", new_max);
                                }
                            }
                        }
                    }
                };

                // Submit tasks with sliding window, using pre-read data when available
                for (size_t i = 0; i < actionable.size(); ++i) {
                    if (progress.cancelled) break;

                    const auto* f = actionable[i];
                    std::vector<uint8_t> file_data;

                    // Check if this file has pre-fetched data available
                    bool use_cache = (f->action == SyncAction::Upload && f->local_size > 0 && f->local_size <= PRE_READ_MAX_FILE_SIZE) ||
                                     (f->action == SyncAction::Download && f->s3_size > 0 && f->s3_size <= PRE_READ_MAX_FILE_SIZE);
                    if (use_cache) {
                        std::unique_lock<std::mutex> lock(io_cache_mutex);
                        auto it = io_cache.find(f->relative_path);
                        if (it != io_cache.end()) {
                            file_data = std::move(it->second);
                            io_cache_bytes -= file_data.size();
                            io_cache.erase(it);
                            lock.unlock();
                            io_cache_cv.notify_one();  // Signal room in cache
                        }
                    }

                    // Wait for concurrency slot
                    {
                        std::unique_lock<std::mutex> lock(concurrency_mutex);
                        concurrency_cv.wait(lock, [&] {
                            return in_flight < current_max_concurrency || progress.cancelled;
                        });
                    }

                    if (progress.cancelled) break;

                    ++in_flight;
                    boost::asio::post(*pool, [&, f, data = std::move(file_data)]() mutable {
                        process_with_tracking(f, std::move(data));
                    });
                }

                // Wait for all tasks
                {
                    std::unique_lock<std::mutex> lock(concurrency_mutex);
                    concurrency_cv.wait(lock, [&] { return in_flight == 0; });
                }

                // Clean up I/O pipeline threads
                {
                    std::lock_guard<std::mutex> lock(io_cache_mutex);
                    io_cache_done = true;
                }
                io_cache_cv.notify_all();
                for (auto& t : io_pipeline_threads) {
                    t.join();
                }

                // Synchronous cleanup - all tasks completed so this returns quickly
                pool->join();

                // Wait for disk writer threads to finish (downloads only)
                if (config.direction == SyncDirection::Download) {
                    {
                        std::lock_guard<std::mutex> lock(write_queue_mutex);
                        write_queue_done = true;
                    }
                    write_queue_cv.notify_all();
                    for (auto& t : disk_writer_threads) {
                        t.join();
                    }

                    // Update counts from async writes (bytes already updated live)
                    download_count += write_queue_success.load();
                    fail_count += write_queue_fail.load();
                }
            }

            result.files_uploaded = upload_count.load();
            result.files_diff_uploaded = diff_upload_count.load();
            result.files_downloaded = download_count.load();
            result.files_diff_downloaded = diff_download_count.load();
            result.files_copied = copy_count.load();
            result.files_deleted = delete_count.load();
            result.files_failed = fail_count.load();
            result.bytes_transferred = progress.bytes_transferred.load();
            result.bytes_copied_server_side = progress.bytes_copied_server_side.load();
            result.bytes_saved = bytes_saved.load();
        }

        if (progress.cancelled) {
            result.success = false;
            if (result.error_message.empty()) {
                result.error_message = "Cancelled";
            }
        } else {
            result.success = (result.files_failed == 0);
        }

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();

    return result;
}
