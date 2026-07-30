#include "directory_comparison.h"

#include <sys/stat.h>
#include "s3_interface.h"
#include "s3_utils.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <future>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

namespace fs = std::filesystem;

// Adaptive block size based on file size for useful visualization
// Targets roughly 64-256 blocks for good UI granularity
int64_t compute_adaptive_block_size(int64_t file_size) {
    if (file_size <= 0) return 1024;  // 1 KiB minimum

    // Scale block size to get reasonable number of blocks for visualization
    // Smaller blocks = more detail in the UI grid

    if (file_size <= 64 * 1024) {            // <= 64 KiB: 1 KiB blocks (up to 64 blocks)
        return 1024;
    } else if (file_size <= 256 * 1024) {    // <= 256 KiB: 2 KiB blocks (up to 128 blocks)
        return 2 * 1024;
    } else if (file_size <= 512 * 1024) {    // <= 512 KiB: 4 KiB blocks (up to 128 blocks)
        return 4 * 1024;
    } else if (file_size <= 1024 * 1024) {   // <= 1 MiB: 8 KiB blocks (up to 128 blocks)
        return 8 * 1024;
    } else if (file_size <= 4 * 1024 * 1024) {   // <= 4 MiB: 16 KiB blocks (up to 256 blocks)
        return 16 * 1024;
    } else if (file_size <= 16 * 1024 * 1024) {  // <= 16 MiB: 64 KiB blocks (up to 256 blocks)
        return 64 * 1024;
    } else {                                  // > 16 MiB: 128 KiB blocks
        return 128 * 1024;
    }
}

// Read file content for block comparison (local files only)
static std::vector<uint8_t> read_local_file(const std::string& path, int64_t max_size) {
    std::vector<uint8_t> data;
    std::ifstream file(path, std::ios::binary);
    if (!file) return data;

    file.seekg(0, std::ios::end);
    int64_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size > max_size) size = max_size;
    if (size <= 0) return data;

    data.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// Perform block-level comparison for small files
// Only works for local files currently
static void analyze_small_file_blocks(
    const FileSource& source_a,
    const FileSource& source_b,
    FileCompareResult& result,
    int64_t chunk_size
) {
    // Only analyze local files for now (S3 would require downloading)
    if (source_a.type != SourceType::Local || source_b.type != SourceType::Local) {
        return;
    }

    // Analyze files up to 5 chunks
    int64_t max_size = std::max(result.size_a, result.size_b);
    if (max_size <= 0 || max_size > 5 * chunk_size) {
        return;
    }

    // Read both files
    std::vector<uint8_t> data_a = read_local_file(source_a.path, max_size);
    std::vector<uint8_t> data_b = read_local_file(source_b.path, max_size);

    if (data_a.empty() && result.size_a > 0) return;  // Read error
    if (data_b.empty() && result.size_b > 0) return;  // Read error

    // Compute adaptive block size
    int64_t block_size = compute_adaptive_block_size(max_size);
    result.block_size = block_size;
    result.has_block_analysis = true;

    int64_t size_a = static_cast<int64_t>(data_a.size());
    int64_t size_b = static_cast<int64_t>(data_b.size());
    int64_t common_size = std::min(size_a, size_b);

    // Count blocks
    int64_t blocks_a = (size_a + block_size - 1) / block_size;
    int64_t blocks_b = (size_b + block_size - 1) / block_size;
    int64_t common_blocks = (common_size + block_size - 1) / block_size;
    result.total_blocks = static_cast<size_t>(std::max(blocks_a, blocks_b));

    // Compare common blocks
    for (int64_t i = 0; i < common_blocks; ++i) {
        int64_t start = i * block_size;
        int64_t end = std::min(start + block_size, common_size);

        bool match = true;
        for (int64_t j = start; j < end && match; ++j) {
            if (data_a[j] != data_b[j]) {
                match = false;
            }
        }
        if (!match) {
            result.mismatched_blocks.push_back(static_cast<size_t>(i));
        }
    }

    // Extra blocks in A (if A is larger)
    for (int64_t i = common_blocks; i < blocks_a; ++i) {
        result.extra_blocks_in_a.push_back(static_cast<size_t>(i));
    }

    // Extra blocks in B (if B is larger)
    for (int64_t i = common_blocks; i < blocks_b; ++i) {
        result.extra_blocks_in_b.push_back(static_cast<size_t>(i));
    }
}

bool is_directory_source(const FileSource& source) {
    if (source.type == SourceType::Local) {
        std::error_code ec;
        return fs::is_directory(source.path, ec);
    } else {
        // For S3, trailing slash is definitely a directory/prefix
        if (!source.path.empty() && source.path.back() == '/') {
            return true;
        }
        // No trailing slash - assume it's a file
        // The actual check happens when we try to list/head the object
        return false;
    }
}

// Check if S3 path is a prefix (directory) by checking if the key exists as an object
bool is_s3_prefix(const FileSource& source) {
    if (source.type != SourceType::S3) return false;
    if (source.path.empty()) return true;  // Root is a prefix
    if (source.path.back() == '/') return true;  // Trailing slash = prefix

    // Check if the key exists as an object
    if (source.region.empty() || source.bucket.empty()) {
        return false;
    }
    try {
        auto client = CreateS3Client(source.region, source.endpoint, 10, source.profile);
        if (client) {
            int64_t size = client->GetObjectSize(source.bucket, source.path);
            return size < 0;  // Doesn't exist as object = treat as prefix
        }
    } catch (...) {
    }
    return false;
}

namespace {
// Size and modification time in one stat(). fs::last_write_time's epoch is
// implementation-defined in C++17, so it cannot be compared against an S3
// LastModified; st_mtime is Unix epoch seconds on every supported platform.
inline bool stat_size_and_mtime(const std::string& path, int64_t& size, int64_t& mtime) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    size = static_cast<int64_t>(st.st_size);
    mtime = static_cast<int64_t>(st.st_mtime);
    return true;
}
}  // namespace

std::vector<DirectoryEntry> enumerate_local_directory(
    const std::string& root_path,
    bool recursive,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    bool* out_complete
) {
    if (out_complete) *out_complete = true;
    std::vector<DirectoryEntry> entries;

    try {
        fs::path root(root_path);
        std::error_code ec;

        if (!fs::is_directory(root, ec)) {
            spdlog::error("Not a directory: '{}'", root_path);
            if (out_complete) *out_complete = false;
            return entries;
        }

        // Recursive walks go through the BFS implementation even at one worker.
        // std::filesystem::recursive_directory_iterator cannot report a
        // directory it failed to descend into: skip_permission_denied hides
        // EACCES outright, and without it increment(ec) still reports success
        // while silently omitting the subtree. That is precisely the "partial
        // listing looks complete" failure this out-parameter exists to prevent,
        // so the per-directory error checking in the BFS version is used here
        // too. (No recursion: the BFS version only delegates back for
        // non-recursive walks.)
        if (recursive) {
            return parallel_enumerate_local_directory(
                root_path, true, 1, files_found, cancelled, out_complete);
        } else {
            // Same reasoning as above: no skip_permission_denied, and the
            // constructor's error is checked before iterating.
            auto options = fs::directory_options::follow_directory_symlink;
            fs::directory_iterator dir_it(root, options, ec);
            if (ec) {
                spdlog::warn("Cannot read directory '{}': {}", root.string(), ec.message());
                if (out_complete) *out_complete = false;
                ec.clear();
                return entries;
            }
            for (auto& entry : dir_it) {
                if (cancelled) break;

                if (ec) {
                    spdlog::warn("Error during directory iteration: {}", ec.message());
                    if (out_complete) *out_complete = false;
                    ec.clear();
                    continue;
                }

                // Follow symlinks - check if target is a regular file
                bool is_symlink = entry.is_symlink(ec);
                ec.clear();

                // Use fs::is_regular_file(path) which follows symlinks
                bool is_file = fs::is_regular_file(entry.path(), ec);
                if (ec) {
                    // Same rule as the recursive walk: only "the target cannot
                    // exist" is safe to skip quietly. Any other errno means a
                    // real entry was dropped and the listing is short.
                    const bool absent =
                        ec == std::errc::no_such_file_or_directory ||
                        ec == std::errc::too_many_symbolic_link_levels ||
                        ec == std::errc::not_a_directory ||
                        ec == std::errc::filename_too_long;
                    if (absent) {
                        if (is_symlink) {
                            spdlog::debug("Skipping broken symlink: '{}'", entry.path().string());
                        }
                    } else {
                        spdlog::warn("Cannot stat '{}': {}",
                                     entry.path().string(), ec.message());
                        if (out_complete) *out_complete = false;
                    }
                    ec.clear();
                    continue;
                }

                if (is_file) {
                    DirectoryEntry de;
                    de.relative_path = entry.path().filename().string();
                    if (!stat_size_and_mtime(entry.path().string(), de.size, de.mtime)) {
                        de.size = 0;
                        de.mtime = 0;
                    }
                    ec.clear();
                    if (ec) {
                        de.size = 0;
                        ec.clear();
                    }
                    if (is_symlink) {
                        spdlog::debug("Including symlink: '{}' -> target size {}", de.relative_path, de.size);
                    }
                    entries.push_back(std::move(de));
                    files_found = entries.size();
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        spdlog::error("Filesystem error enumerating '{}': {}", root_path, e.what());
        if (out_complete) *out_complete = false;
    }

    return entries;
}

std::vector<DirectoryEntry> enumerate_s3_prefix(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    bool* out_complete
) {
    if (out_complete) *out_complete = true;
    std::vector<DirectoryEntry> entries;

    if (!client) {
        spdlog::error("S3 client is null");
        if (out_complete) *out_complete = false;
        return entries;
    }

    // Normalize prefix to end with / if not empty
    std::string normalized_prefix = prefix;
    if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
        normalized_prefix += '/';
    }

    std::string continuation_token;

    do {
        if (cancelled) break;

        // Empty delimiter for recursive (flat listing), "/" for single level
        std::string delimiter = recursive ? "" : "/";

        auto result = client->ListObjects(
            bucket,
            normalized_prefix,
            delimiter,
            continuation_token,
            1000
        );

        if (!result.success) {
            spdlog::error("Failed to list S3 objects in s3://{}/{}: {}",
                         bucket, normalized_prefix, result.error_message);
            if (out_complete) *out_complete = false;
            break;
        }

        for (const auto& obj : result.objects) {
            if (cancelled) break;

            // Skip the prefix itself if it appears as an object
            if (obj.key == normalized_prefix) continue;
            // Skip "directory markers" (objects ending with /)
            if (!obj.key.empty() && obj.key.back() == '/') continue;
            // Skip if key doesn't start with our prefix (shouldn't happen)
            if (obj.key.length() <= normalized_prefix.length()) continue;

            DirectoryEntry de;
            // Make path relative to prefix
            de.relative_path = obj.key.substr(normalized_prefix.length());
            de.size = obj.size;
            de.mtime = obj.last_modified;
            entries.push_back(std::move(de));
            files_found = entries.size();
        }

        if (result.is_truncated && result.next_continuation_token.empty()) {
            // More results exist but the endpoint gave us no way to ask for
            // them, so this listing is truncated.
            spdlog::error("Truncated listing for s3://{}/{} with no continuation token",
                          bucket, normalized_prefix);
            if (out_complete) *out_complete = false;
            break;
        }
        continuation_token = result.next_continuation_token;
    } while (!continuation_token.empty() && !cancelled);

    return entries;
}

// Parallel local directory enumeration using level-by-level BFS
std::vector<DirectoryEntry> parallel_enumerate_local_directory(
    const std::string& root_path,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    bool* out_complete,
    bool* out_alias_omitted
) {
    if (out_complete) *out_complete = true;
    if (out_alias_omitted) *out_alias_omitted = false;
    // Written by worker threads on their error paths.
    std::atomic<bool> enumeration_complete{true};
    std::atomic<bool> alias_omitted{false};
    std::vector<DirectoryEntry> all_entries;

    // Clamp workers to 1-128
    max_workers = std::max(1, std::min(128, max_workers));

    try {
        fs::path root(root_path);
        std::error_code ec;

        if (!fs::is_directory(root, ec)) {
            spdlog::error("Not a directory: '{}'", root_path);
            if (out_complete) *out_complete = false;
            return all_entries;
        }

        // Deliberately NOT skip_permission_denied: that option reports an
        // unreadable directory as an empty one with no error, which is how a
        // partial listing used to masquerade as a complete one. Each worker
        // checks the constructor's error_code instead.
        auto options = fs::directory_options::follow_directory_symlink;

        // For non-recursive mode, just use the simple single-threaded version
        if (!recursive) {
            return enumerate_local_directory(root_path, false, files_found, cancelled,
                                             out_complete);
        }

        // Track symlink targets by canonical path to detect cycles
        // Only symlink targets are tracked - regular directories are always enumerated
        std::set<std::string> visited_symlink_targets;
        std::mutex visited_mutex;

        // Add root as a visited target to prevent symlinks pointing back to root
        auto root_canonical = fs::canonical(root, ec);
        if (!ec) {
            visited_symlink_targets.insert(root_canonical.string());
        }

        // Level-by-level BFS with parallel processing
        std::vector<fs::path> current_level = {root};

        // Reuse thread pool across all BFS levels (optimization: avoid pool creation overhead)
        // Use unique_ptr so we can move ownership to background thread for cleanup
        auto pool = std::make_unique<boost::asio::thread_pool>(max_workers);

        while (!current_level.empty() && !cancelled) {
            std::vector<fs::path> next_level;
            std::vector<DirectoryEntry> level_entries;
            std::mutex mutex;
            std::atomic<size_t> pending_tasks{0};
            std::condition_variable cv;
            std::mutex cv_mutex;

            // Lambda to process a single directory
            auto process_directory = [&](const fs::path& dir) {
                if (cancelled) {
                    // Acquire lock BEFORE decrement to prevent race where main thread
                    // sees pending_tasks==0 before we call notify_one()
                    std::lock_guard<std::mutex> lk(cv_mutex);
                    if (--pending_tasks == 0) {
                        cv.notify_one();
                    }
                    return;
                }

                std::error_code local_ec;
                std::vector<DirectoryEntry> local_files;
                std::vector<fs::path> local_subdirs;

                try {
                    // Construct explicitly so the error is visible. Note that
                    // fs::directory_options::skip_permission_denied would turn
                    // EACCES into an empty listing with a CLEARED error code -
                    // indistinguishable from an empty directory, which is exactly
                    // how an unreadable subtree used to be silently dropped.
                    fs::directory_iterator dir_it(dir, options, local_ec);
                    if (local_ec) {
                        // A directory removed mid-scan holds nothing we are missing;
                        // one we are not allowed to read does. Only the latter makes
                        // the inventory partial.
                        if (local_ec == std::errc::no_such_file_or_directory ||
                            local_ec == std::errc::too_many_symbolic_link_levels ||
                            local_ec == std::errc::not_a_directory ||
                            local_ec == std::errc::filename_too_long) {
                            spdlog::debug("Directory not resolvable during scan: '{}': {}",
                                          dir.string(), local_ec.message());
                        } else {
                            spdlog::warn("Cannot read directory '{}': {}",
                                         dir.string(), local_ec.message());
                            enumeration_complete = false;
                        }
                        local_ec.clear();
                        std::lock_guard<std::mutex> lk(cv_mutex);
                        if (--pending_tasks == 0) cv.notify_one();
                        return;
                    }

                    for (const auto& entry : dir_it) {
                        if (cancelled) break;

                        // Use cached symlink check from directory_entry
                        bool is_symlink = entry.is_symlink(local_ec);
                        local_ec.clear();

                        // Use cached file status from directory_entry (avoids extra stat call)
                        // Note: For symlinks, we need to check the target, so use status() not symlink_status()
                        auto status = entry.status(local_ec);
                        if (local_ec) {
                            // Only "it is not there" is safe to skip quietly, and it
                            // is safe whether or not the entry is a symlink: a
                            // dangling link is not a file, and an entry deleted
                            // mid-scan is not missing data. Any other errno -
                            // EACCES, EIO, ESTALE - means a real entry exists that
                            // we could not read, so the inventory is short.
                            // These all mean the path cannot resolve to an existing
                            // file, so nothing was dropped from the inventory:
                            //   ENOENT        target is not there
                            //   ELOOP         symlink loop - unresolvable
                            //   ENOTDIR       a path component is not a directory
                            //   ENAMETOOLONG  target name cannot exist
                            // Anything else (EACCES, EPERM, EIO, ESTALE, EOVERFLOW)
                            // means a real entry exists that we could not read.
                            const bool absent =
                                local_ec == std::errc::no_such_file_or_directory ||
                                local_ec == std::errc::too_many_symbolic_link_levels ||
                                local_ec == std::errc::not_a_directory ||
                                local_ec == std::errc::filename_too_long;
                            if (absent) {
                                spdlog::debug("Skipping entry that is not there: '{}'",
                                              entry.path().string());
                            } else {
                                spdlog::warn("Cannot stat '{}': {}",
                                             entry.path().string(), local_ec.message());
                                enumeration_complete = false;
                            }
                            local_ec.clear();
                            continue;
                        }

                        if (fs::is_regular_file(status)) {
                            DirectoryEntry de;
                            de.relative_path = entry.path().lexically_relative(root).string();
                            if (de.relative_path.empty()) continue;

                                    if (!stat_size_and_mtime(entry.path().string(), de.size, de.mtime)) {
                                de.size = 0;
                                de.mtime = 0;
                            }
                            local_ec.clear();
                            if (is_symlink) {
                                spdlog::debug("Including symlink: '{}' -> target size {}", de.relative_path, de.size);
                            }
                            local_files.push_back(std::move(de));
                        } else if (fs::is_directory(status)) {
                            // Only check for cycles when following a symlink
                            // Regular directories are always enumerated
                            if (is_symlink) {
                                auto canonical = fs::canonical(entry.path(), local_ec);
                                if (local_ec) {
                                    // Cannot resolve the link, so this subtree is
                                    // skipped entirely - the listing is partial.
                                    spdlog::warn("Cannot resolve symlink '{}': {}",
                                                 entry.path().string(), local_ec.message());
                                    enumeration_complete = false;
                                    local_ec.clear();
                                    continue;
                                }

                                // Each symlink target is followed once. That is what
                                // makes mutual cycles (A -> B -> A) terminate, but it
                                // also means a second, legitimate link to the same
                                // directory is skipped - and its files are then absent
                                // from the inventory. Absent files look like orphans,
                                // so this MUST be reported as an incomplete listing
                                // rather than skipped silently.
                                std::string canonical_str = canonical.string();
                                bool is_cycle = false;
                                {
                                    std::lock_guard<std::mutex> vlock(visited_mutex);
                                    if (visited_symlink_targets.count(canonical_str)) {
                                        is_cycle = true;
                                        spdlog::debug("Not following '{}' -> '{}': that target has "
                                                      "already been enumerated; this subtree is "
                                                      "omitted from the listing",
                                                      entry.path().string(), canonical_str);
                                        alias_omitted = true;
                                    } else {
                                        visited_symlink_targets.insert(canonical_str);
                                    }
                                }

                                if (!is_cycle) {
                                    local_subdirs.push_back(entry.path());
                                }
                            } else {
                                // Regular directory - always enumerate
                                local_subdirs.push_back(entry.path());
                            }
                        }
                    }
                } catch (const fs::filesystem_error& e) {
                    spdlog::warn("Error scanning directory '{}': {}", dir.string(), e.what());
                    enumeration_complete = false;
                }

                // Merge results under lock (no reserve inside lock - vectors grow efficiently)
                if (!local_files.empty() || !local_subdirs.empty()) {
                    std::lock_guard<std::mutex> lock(mutex);
                    level_entries.insert(level_entries.end(),
                        std::make_move_iterator(local_files.begin()),
                        std::make_move_iterator(local_files.end()));
                    next_level.insert(next_level.end(),
                        std::make_move_iterator(local_subdirs.begin()),
                        std::make_move_iterator(local_subdirs.end()));
                    // Update progress more frequently (per-directory batch)
                    files_found = all_entries.size() + level_entries.size();
                }

                // Acquire lock BEFORE decrement to prevent race where main thread
                // sees pending_tasks==0 before we call notify_one()
                {
                    std::lock_guard<std::mutex> lk(cv_mutex);
                    if (--pending_tasks == 0) {
                        cv.notify_one();
                    }
                }
            };

            // Optimization: skip thread pool overhead for single directory
            if (current_level.size() == 1) {
                pending_tasks = 1;
                process_directory(current_level[0]);
            } else {
                // Post all directories to the thread pool
                // Increment pending_tasks before posting to avoid race condition
                for (const auto& dir : current_level) {
                    if (cancelled) break;
                    ++pending_tasks;
                    boost::asio::post(*pool, [&, dir]() { process_directory(dir); });
                }

                // Wait for all posted tasks to complete (must wait even if cancelled
                // to avoid data race on level_entries/next_level)
                if (pending_tasks > 0) {
                    std::unique_lock<std::mutex> lock(cv_mutex);
                    cv.wait(lock, [&]() { return pending_tasks == 0; });
                }
            }

            // Add this level's entries to results using move iterators
            all_entries.reserve(all_entries.size() + level_entries.size());
            all_entries.insert(all_entries.end(),
                std::make_move_iterator(level_entries.begin()),
                std::make_move_iterator(level_entries.end()));
            files_found = all_entries.size();

            // Move to next level
            current_level = std::move(next_level);
        }

        // ============================================================================
        // Background Thread Pool Cleanup Pattern
        // ============================================================================
        //
        // We move the thread pool to a detached background thread for cleanup to avoid
        // blocking the caller while worker threads finish their epilogue (stack unwinding,
        // lambda destructor execution, etc.).
        //
        // THREAD SAFETY REASONING:
        //
        // 1. Task Completion Synchronization:
        //    Before reaching this point, we wait on pending_tasks == 0, which ensures all
        //    tasks have signaled completion via the condition variable. At this point:
        //    - All task work is done
        //    - All captured-by-reference variables have been "released" (no more access)
        //    - Worker threads may still be unwinding their stacks
        //
        // 2. Lock-Before-Decrement Pattern:
        //    Workers acquire cv_mutex BEFORE decrementing pending_tasks and calling
        //    notify_one(). This prevents a race where the main thread could see
        //    pending_tasks==0 (via spurious wakeup) and proceed before notify_one()
        //    completes. The main thread must acquire cv_mutex to check the wait
        //    predicate, so it cannot proceed until the worker releases the lock
        //    (which happens after notify_one()).
        //
        // 3. Why No Captured References Are Accessed After Lock Release:
        //    After releasing cv_mutex, workers only execute lambda/stack cleanup,
        //    which doesn't access captured-by-reference variables.
        //
        // 4. Why join() in Background is Safe:
        //    boost::asio::thread_pool::join() waits for all threads to fully terminate,
        //    not just for work items to complete. Moving the pool to a background thread
        //    lets the caller return immediately while join() waits for thread cleanup.
        //    The unique_ptr ensures the pool is destroyed after join() completes.
        //
        // 5. Why stop() is Not Needed:
        //    stop() prevents new work from being posted. Since we've verified all tasks
        //    are complete (pending_tasks == 0), no new work can be posted, making stop()
        //    unnecessary. If this invariant were violated, join() would block indefinitely
        //    in the background thread (a silent hang, not a crash).
        //
        // 6. Program Termination:
        //    If the program exits while cleanup threads are running, they will be
        //    terminated by the OS. This is safe because:
        //    - No shared state with the main program is being modified
        //    - The pool is self-contained in the unique_ptr
        //    - At worst, some thread_pool internal cleanup is skipped
        //
        // ============================================================================
        std::thread([p = std::move(pool)]() {
            try { p->join(); } catch (const std::exception& e) {
                spdlog::error("Thread pool cleanup failed: {}", e.what());
            }
        }).detach();

    } catch (const fs::filesystem_error& e) {
        spdlog::error("Filesystem error in parallel enumerate '{}': {}", root_path, e.what());
        enumeration_complete = false;
    }

    // Publish what the workers recorded. Safe here: every worker has finished
    // and released cv_mutex before pending_tasks reached zero.
    if (out_complete && !enumeration_complete) *out_complete = false;
    if (out_alias_omitted && alias_omitted) *out_alias_omitted = true;

    return all_entries;
}

// Parallel S3 prefix enumeration using level-by-level BFS
std::vector<DirectoryEntry> parallel_enumerate_s3_prefix(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    bool* out_complete,
    bool include_directory_markers
) {
    if (out_complete) *out_complete = true;
    // Written by worker threads on their error paths.
    std::atomic<bool> enumeration_complete{true};
    std::vector<DirectoryEntry> all_entries;

    if (!client) {
        spdlog::error("S3 client is null");
        if (out_complete) *out_complete = false;
        return all_entries;
    }

    // Clamp workers to 1-80 for S3 listing
    // S3 can throttle ListObjects but exponential retry handles rate limiting
    int original_workers = max_workers;
    max_workers = std::max(1, std::min(80, max_workers));
    if (original_workers > 80) {
        spdlog::info("S3 parallel discovery workers clamped from {} to 80", original_workers);
    }

    // Non-recursive mode: no parallelization benefit
    if (!recursive) {
        return enumerate_s3_prefix(bucket, prefix, false, files_found, cancelled, client,
                                   out_complete);
    }

    // Normalize prefix to end with / if not empty
    std::string normalized_prefix = prefix;
    if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
        normalized_prefix += '/';
    }

    // Level-by-level BFS with parallel processing
    std::vector<std::string> current_level = {normalized_prefix};

    // Reuse thread pool across all BFS levels (optimization: avoid pool creation overhead)
    // Use unique_ptr so we can move ownership to background thread for cleanup
    auto pool = std::make_unique<boost::asio::thread_pool>(max_workers);

    while (!current_level.empty() && !cancelled) {
        std::vector<std::string> next_level;
        std::vector<DirectoryEntry> level_entries;
        std::mutex mutex;
        std::atomic<size_t> pending_tasks{0};
        std::condition_variable cv;
        std::mutex cv_mutex;

        // Lambda to process a single S3 prefix
        auto process_prefix = [&](const std::string& current_prefix) {
            if (cancelled) {
                // Acquire lock BEFORE decrement to prevent race where main thread
                // sees pending_tasks==0 before we call notify_one()
                std::lock_guard<std::mutex> lk(cv_mutex);
                if (--pending_tasks == 0) {
                    cv.notify_one();
                }
                return;
            }

            std::vector<DirectoryEntry> local_entries;
            std::vector<std::string> local_subdirs;
            std::string continuation_token;

            // List this prefix with delimiter to get objects and common_prefixes
            do {
                if (cancelled) break;

                auto result = client->ListObjects(
                    bucket,
                    current_prefix,
                    "/",  // Use delimiter to get subdirectories
                    continuation_token,
                    1000
                );

                if (!result.success) {
                    spdlog::error("Failed to list S3 prefix '{}': {}",
                                  current_prefix, result.error_message);
                    enumeration_complete = false;
                    break;
                }

                // Collect files from this prefix
                for (const auto& obj : result.objects) {
                    if (cancelled) break;
                    if (include_directory_markers) {
                        // Keep the folder markers, the prefix's own included.
                        // Listing with a delimiter means each marker is returned
                        // only when its own prefix is listed, so this cannot
                        // yield the same key twice. A key of exactly the
                        // prefix's length is that marker, and gives an empty
                        // relative_path, which the caller appends to the prefix
                        // to get the key back.
                        if (obj.key.length() < normalized_prefix.length()) continue;
                    } else {
                        if (obj.key == current_prefix) continue;
                        if (!obj.key.empty() && obj.key.back() == '/') continue;
                        if (obj.key.length() <= normalized_prefix.length()) continue;
                    }

                    DirectoryEntry de;
                    de.relative_path = obj.key.substr(normalized_prefix.length());
                    de.size = obj.size;
                    de.mtime = obj.last_modified;
                    local_entries.push_back(std::move(de));
                }

                // Queue subdirectories for next level
                for (const auto& subprefix : result.common_prefixes) {
                    local_subdirs.push_back(subprefix);
                }

                if (result.is_truncated && result.next_continuation_token.empty()) {
                    spdlog::error("Truncated listing for prefix '{}' with no continuation token",
                                  current_prefix);
                    enumeration_complete = false;
                    break;
                }
                continuation_token = result.next_continuation_token;
            } while (!continuation_token.empty() && !cancelled);

            // Merge results under lock (no reserve inside lock - vectors grow efficiently)
            if (!local_entries.empty() || !local_subdirs.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                level_entries.insert(level_entries.end(),
                    std::make_move_iterator(local_entries.begin()),
                    std::make_move_iterator(local_entries.end()));
                next_level.insert(next_level.end(),
                    std::make_move_iterator(local_subdirs.begin()),
                    std::make_move_iterator(local_subdirs.end()));
                // Update progress more frequently (per-prefix batch)
                files_found = all_entries.size() + level_entries.size();
            }

            // Acquire lock BEFORE decrement to prevent race where main thread
            // sees pending_tasks==0 before we call notify_one()
            {
                std::lock_guard<std::mutex> lk(cv_mutex);
                if (--pending_tasks == 0) {
                    cv.notify_one();
                }
            }
        };

        // Optimization: skip thread pool overhead for single prefix
        if (current_level.size() == 1) {
            pending_tasks = 1;
            process_prefix(current_level[0]);
        } else {
            // Post all prefixes to the thread pool
            // Increment pending_tasks before posting to avoid race condition
            for (const auto& current_prefix : current_level) {
                if (cancelled) break;
                ++pending_tasks;
                boost::asio::post(*pool, [&, current_prefix]() { process_prefix(current_prefix); });
            }

            // Wait for all posted tasks to complete (must wait even if cancelled
            // to avoid data race on level_entries/next_level)
            if (pending_tasks > 0) {
                std::unique_lock<std::mutex> lock(cv_mutex);
                cv.wait(lock, [&]() { return pending_tasks == 0; });
            }
        }

        // Add this level's entries to results using move iterators
        all_entries.reserve(all_entries.size() + level_entries.size());
        all_entries.insert(all_entries.end(),
            std::make_move_iterator(level_entries.begin()),
            std::make_move_iterator(level_entries.end()));
        files_found = all_entries.size();

        spdlog::debug("S3 BFS level complete: {} files found, {} subdirs to process",
                     all_entries.size(), next_level.size());

        // Move to next level
        current_level = std::move(next_level);
    }

    // Background thread pool cleanup - see detailed comment in parallel_enumerate_local_directory()
    // for thread safety reasoning. Key invariant: workers acquire cv_mutex before decrementing
    // pending_tasks and calling notify_one(), so when we see pending_tasks==0, all workers
    // have released the lock and finished accessing shared state.
    std::thread([p = std::move(pool)]() {
        try { p->join(); } catch (const std::exception& e) {
            spdlog::error("Thread pool cleanup failed: {}", e.what());
        }
    }).detach();

    // Publish what the workers recorded (same invariant as above).
    if (out_complete && !enumeration_complete) *out_complete = false;

    return all_entries;
}

void parallel_enumerate_s3_prefix_streaming(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    EnumerateCallback on_entries
) {
    if (!client) {
        spdlog::error("S3 client is null");
        return;
    }

    if (!on_entries) {
        spdlog::error("Callback is null for streaming enumeration");
        return;
    }

    // Clamp workers to 1-80 for S3 listing
    max_workers = std::max(1, std::min(80, max_workers));

    // Non-recursive mode: enumerate and call callback once
    if (!recursive) {
        auto entries = enumerate_s3_prefix(bucket, prefix, false, files_found, cancelled, client);
        if (!entries.empty()) {
            on_entries(std::move(entries));
        }
        return;
    }

    // Normalize prefix to end with / if not empty
    std::string normalized_prefix = prefix;
    if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
        normalized_prefix += '/';
    }

    // Level-by-level BFS with parallel processing
    std::vector<std::string> current_level = {normalized_prefix};
    auto pool = std::make_unique<boost::asio::thread_pool>(max_workers);
    size_t total_files = 0;

    while (!current_level.empty() && !cancelled) {
        std::vector<std::string> next_level;
        std::vector<DirectoryEntry> level_entries;
        std::mutex mutex;
        std::atomic<size_t> pending_tasks{0};
        std::condition_variable cv;
        std::mutex cv_mutex;

        auto process_prefix = [&](const std::string& current_prefix) {
            if (cancelled) {
                std::lock_guard<std::mutex> lk(cv_mutex);
                if (--pending_tasks == 0) {
                    cv.notify_one();
                }
                return;
            }

            std::vector<DirectoryEntry> local_entries;
            std::vector<std::string> local_subdirs;
            std::string continuation_token;

            do {
                if (cancelled) break;

                auto result = client->ListObjects(
                    bucket,
                    current_prefix,
                    "/",
                    continuation_token,
                    1000
                );

                if (!result.success) {
                    spdlog::warn("Failed to list S3 prefix '{}': {}",
                                current_prefix, result.error_message);
                    break;
                }

                for (const auto& obj : result.objects) {
                    if (cancelled) break;
                    if (obj.key == current_prefix) continue;
                    if (!obj.key.empty() && obj.key.back() == '/') continue;
                    if (obj.key.length() <= normalized_prefix.length()) continue;

                    DirectoryEntry de;
                    de.relative_path = obj.key.substr(normalized_prefix.length());
                    de.size = obj.size;
                    de.mtime = obj.last_modified;
                    local_entries.push_back(std::move(de));
                }

                for (const auto& subprefix : result.common_prefixes) {
                    local_subdirs.push_back(subprefix);
                }

                continuation_token = result.next_continuation_token;
            } while (!continuation_token.empty() && !cancelled);

            if (!local_entries.empty() || !local_subdirs.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                level_entries.insert(level_entries.end(),
                    std::make_move_iterator(local_entries.begin()),
                    std::make_move_iterator(local_entries.end()));
                next_level.insert(next_level.end(),
                    std::make_move_iterator(local_subdirs.begin()),
                    std::make_move_iterator(local_subdirs.end()));
            }

            {
                std::lock_guard<std::mutex> lk(cv_mutex);
                if (--pending_tasks == 0) {
                    cv.notify_one();
                }
            }
        };

        if (current_level.size() == 1) {
            pending_tasks = 1;
            process_prefix(current_level[0]);
        } else {
            for (const auto& current_prefix : current_level) {
                if (cancelled) break;
                ++pending_tasks;
                boost::asio::post(*pool, [&, current_prefix]() { process_prefix(current_prefix); });
            }

            if (pending_tasks > 0) {
                std::unique_lock<std::mutex> lock(cv_mutex);
                cv.wait(lock, [&]() { return pending_tasks == 0; });
            }
        }

        // Call callback with this level's entries (streaming)
        if (!level_entries.empty() && !cancelled) {
            total_files += level_entries.size();
            files_found = total_files;
            spdlog::debug("S3 BFS level: {} files, {} subdirs queued",
                         level_entries.size(), next_level.size());

            // Callback returns false to stop enumeration
            try {
                if (!on_entries(std::move(level_entries))) {
                    cancelled = true;
                    break;
                }
            } catch (const std::exception& e) {
                spdlog::error("Streaming callback threw exception: {}", e.what());
                cancelled = true;
                break;
            }
        }

        current_level = std::move(next_level);
    }

    // Background cleanup
    std::thread([p = std::move(pool)]() {
        try { p->join(); } catch (const std::exception& e) {
            spdlog::error("Thread pool cleanup failed: {}", e.what());
        }
    }).detach();
}

// Helper to build FileSource for a specific file within a directory
static FileSource make_file_source(const FileSource& dir_source, const std::string& relative_path) {
    FileSource file_source = dir_source;

    if (dir_source.type == SourceType::Local) {
        fs::path dir_path(dir_source.path);
        fs::path file_path = dir_path / relative_path;
        file_source.path = file_path.string();
    } else {
        // S3: append relative path to prefix
        std::string prefix = dir_source.path;
        if (!prefix.empty() && prefix.back() != '/') {
            prefix += '/';
        }
        file_source.path = prefix + relative_path;
    }

    return file_source;
}

DirectoryComparisonResult run_directory_comparison(
    const DirectoryComparisonConfig& config,
    DirectoryComparisonProgress& progress,
    std::shared_ptr<IS3Client> s3_client_a,
    std::shared_ptr<IS3Client> s3_client_b
) {
    DirectoryComparisonResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Create S3 clients if needed
        if (config.source_a.type == SourceType::S3 && !s3_client_a) {
            s3_client_a = CreateS3Client(config.source_a.region, config.source_a.endpoint, config.max_connections, config.source_a.profile);
            if (!s3_client_a) {
                result.error_message = "Failed to create S3 client for source A";
                return result;
            }
        }

        if (config.source_b.type == SourceType::S3 && !s3_client_b) {
            if (should_reuse_s3_client(config.source_a, config.source_b)) {
                s3_client_b = s3_client_a;  // Reuse client for same region and profile
            } else {
                s3_client_b = CreateS3Client(config.source_b.region, config.source_b.endpoint, config.max_connections, config.source_b.profile);
                if (!s3_client_b) {
                    result.error_message = "Failed to create S3 client for source B";
                    return result;
                }
            }
        }

        // Phase 1: Enumerate both directories in parallel
        if (config.parallel_discovery) {
            spdlog::info("Scanning directories (parallel discovery with {} workers)...",
                        config.parallel_discovery_workers);
        } else {
            spdlog::info("Scanning directories...");
        }

        std::vector<DirectoryEntry> entries_a, entries_b;
        // Each future writes its own flag; both are read only after .get().
        // A partial listing on either side invents "only in A"/"only in B"
        // entries, which is what a user acts on.
        bool complete_a = true;
        bool complete_b = true;

        auto future_a = std::async(std::launch::async, [&]() {
            if (config.source_a.type == SourceType::Local) {
                if (config.parallel_discovery && config.recursive) {
                    return parallel_enumerate_local_directory(
                        config.source_a.path, config.recursive,
                        config.parallel_discovery_workers,
                        progress.files_scanned_a, progress.cancelled, &complete_a);
                }
                return enumerate_local_directory(
                    config.source_a.path, config.recursive,
                    progress.files_scanned_a, progress.cancelled, &complete_a);
            } else {
                if (config.parallel_discovery && config.recursive) {
                    return parallel_enumerate_s3_prefix(
                        config.source_a.bucket, config.source_a.path,
                        config.recursive, config.parallel_discovery_workers,
                        progress.files_scanned_a, progress.cancelled, s3_client_a,
                        &complete_a);
                }
                return enumerate_s3_prefix(
                    config.source_a.bucket, config.source_a.path,
                    config.recursive, progress.files_scanned_a,
                    progress.cancelled, s3_client_a, &complete_a);
            }
        });

        auto future_b = std::async(std::launch::async, [&]() {
            if (config.source_b.type == SourceType::Local) {
                if (config.parallel_discovery && config.recursive) {
                    return parallel_enumerate_local_directory(
                        config.source_b.path, config.recursive,
                        config.parallel_discovery_workers,
                        progress.files_scanned_b, progress.cancelled, &complete_b);
                }
                return enumerate_local_directory(
                    config.source_b.path, config.recursive,
                    progress.files_scanned_b, progress.cancelled, &complete_b);
            } else {
                if (config.parallel_discovery && config.recursive) {
                    return parallel_enumerate_s3_prefix(
                        config.source_b.bucket, config.source_b.path,
                        config.recursive, config.parallel_discovery_workers,
                        progress.files_scanned_b, progress.cancelled, s3_client_b,
                        &complete_b);
                }
                return enumerate_s3_prefix(
                    config.source_b.bucket, config.source_b.path,
                    config.recursive, progress.files_scanned_b,
                    progress.cancelled, s3_client_b, &complete_b);
            }
        });

        entries_a = future_a.get();
        entries_b = future_b.get();

        // A side that could not be listed in full is not an empty side. Reporting
        // its missing files as "only in the other source" turns a typo, a
        // permission problem or a transient listing error into a confident,
        // wrong answer - and this is a tool people run to decide whether it is
        // safe to delete the other copy.
        if (!complete_a || !complete_b) {
            auto describe = [](const FileSource& src) {
                return src.type == SourceType::Local
                           ? src.path
                           : "s3://" + src.bucket + "/" + src.path;
            };
            std::string which;
            if (!complete_a && !complete_b) {
                which = "Both sources (" + describe(config.source_a) + ", " +
                        describe(config.source_b) + ")";
            } else if (!complete_a) {
                which = "Source A (" + describe(config.source_a) + ")";
            } else {
                which = "Source B (" + describe(config.source_b) + ")";
            }
            result.error_message =
                which + " could not be listed completely, so entries missing from it "
                "would be reported as differences. The unreadable paths are logged "
                "above.";
            result.success = false;
            return result;
        }

        auto discovery_end_time = std::chrono::high_resolution_clock::now();
        result.discovery_elapsed = std::chrono::duration<double>(discovery_end_time - start_time).count();

        progress.scanning_done = true;

        if (progress.cancelled) {
            result.error_message = "Cancelled during enumeration";
            return result;
        }

        spdlog::info("Found {} files in source A, {} files in source B",
                    entries_a.size(), entries_b.size());

        // Phase 2: Build lookup maps and compute file sets
        std::unordered_map<std::string, int64_t> map_a, map_b;
        for (const auto& e : entries_a) {
            map_a[e.relative_path] = e.size;
            result.total_bytes_a += e.size;
        }
        for (const auto& e : entries_b) {
            map_b[e.relative_path] = e.size;
            result.total_bytes_b += e.size;
        }

        // Find files in both, only in A, only in B
        std::vector<std::string> common_files, only_a, only_b;
        for (const auto& [path, size] : map_a) {
            if (map_b.count(path)) {
                common_files.push_back(path);
            } else {
                only_a.push_back(path);
            }
        }
        for (const auto& [path, size] : map_b) {
            if (!map_a.count(path)) {
                only_b.push_back(path);
            }
        }

        // Sort for consistent ordering
        std::sort(common_files.begin(), common_files.end());
        std::sort(only_a.begin(), only_a.end());
        std::sort(only_b.begin(), only_b.end());

        progress.total_files = common_files.size() + only_a.size() + only_b.size();

        // Use configured chunk size for calculations
        const int64_t chunk_size = config.chunk_size;

        // Record files only in A (treated as 100% mismatch - all content is "extra in A")
        for (const auto& path : only_a) {
            FileCompareResult fcr;
            fcr.relative_path = path;
            fcr.status = FileCompareStatus::Mismatch;
            fcr.size_a = map_a[path];
            fcr.size_b = -1;  // Not present in B

            // Mark all chunks as extra in A (file doesn't exist in B)
            int64_t file_size = fcr.size_a;
            fcr.total_chunks = (file_size > 0) ? static_cast<size_t>((file_size + chunk_size - 1) / chunk_size) : 0;
            for (size_t i = 0; i < fcr.total_chunks; ++i) {
                fcr.extra_chunks_in_a.push_back(i);
            }

            // Block-level analysis: mark all blocks as extra in A
            if (file_size > 0 && file_size <= 5 * chunk_size) {
                fcr.has_block_analysis = true;
                fcr.block_size = compute_adaptive_block_size(file_size);
                fcr.total_blocks = static_cast<size_t>((file_size + fcr.block_size - 1) / fcr.block_size);
                for (size_t i = 0; i < fcr.total_blocks; ++i) {
                    fcr.extra_blocks_in_a.push_back(i);
                }
            }

            result.files.push_back(std::move(fcr));
            result.only_in_a++;
            result.mismatched_files++;
            progress.files_compared++;
        }

        // Record files only in B (treated as 100% mismatch - all content is "extra in B")
        for (const auto& path : only_b) {
            FileCompareResult fcr;
            fcr.relative_path = path;
            fcr.status = FileCompareStatus::Mismatch;
            fcr.size_a = -1;  // Not present in A
            fcr.size_b = map_b[path];

            // Mark all chunks as extra in B (file doesn't exist in A)
            int64_t file_size = fcr.size_b;
            fcr.total_chunks = (file_size > 0) ? static_cast<size_t>((file_size + chunk_size - 1) / chunk_size) : 0;
            for (size_t i = 0; i < fcr.total_chunks; ++i) {
                fcr.extra_chunks_in_b.push_back(i);
            }

            // Block-level analysis: mark all blocks as extra in B
            if (file_size > 0 && file_size <= 5 * chunk_size) {
                fcr.has_block_analysis = true;
                fcr.block_size = compute_adaptive_block_size(file_size);
                fcr.total_blocks = static_cast<size_t>((file_size + fcr.block_size - 1) / fcr.block_size);
                for (size_t i = 0; i < fcr.total_blocks; ++i) {
                    fcr.extra_blocks_in_b.push_back(i);
                }
            }

            result.files.push_back(std::move(fcr));
            result.only_in_b++;
            result.mismatched_files++;
            progress.files_compared++;
        }

        // Phase 3: Compare common files in parallel
        // Analyze file sizes from discovery to set optimal parallelism
        int64_t total_bytes_to_compare = 0;
        size_t multi_chunk_files = 0;  // Files that benefit from chunk-level parallelism
        size_t single_chunk_files = 0; // Small files (1 chunk or less)

        for (const auto& path : common_files) {
            int64_t size = std::max(map_a[path], map_b[path]);
            total_bytes_to_compare += size;
            if (size > chunk_size) {
                multi_chunk_files++;
            } else {
                single_chunk_files++;
            }
        }

        // Maximum thread budget
        int max_threads = config.num_threads > 0 ? config.num_threads : 1024;

        // Set parallelism based on actual file characteristics
        int threads_per_file;
        size_t initial_concurrency;

        if (common_files.empty()) {
            threads_per_file = 1;
            initial_concurrency = 0;
        } else if (multi_chunk_files == 0) {
            // All small files: maximize file-level parallelism, start high
            threads_per_file = 1;
            initial_concurrency = std::min(common_files.size(), static_cast<size_t>(max_threads));
        } else if (single_chunk_files == 0) {
            // All large files: use chunk-level parallelism
            threads_per_file = std::max(1, max_threads / std::max(1, static_cast<int>(common_files.size())));
            initial_concurrency = std::min(common_files.size(), static_cast<size_t>(max_threads / threads_per_file));
        } else {
            // Mixed workload: balance based on ratio
            // More multi-chunk files = more threads per file, fewer concurrent files
            double large_file_ratio = static_cast<double>(multi_chunk_files) / common_files.size();
            if (large_file_ratio > 0.5) {
                // Majority large files: favor chunk parallelism
                threads_per_file = std::max(1, static_cast<int>(4 * large_file_ratio + 1));
                initial_concurrency = std::min(common_files.size(), static_cast<size_t>(max_threads / threads_per_file));
            } else {
                // Majority small files: favor file parallelism
                threads_per_file = 1;
                initial_concurrency = std::min(common_files.size(), static_cast<size_t>(max_threads));
            }
        }

        size_t max_concurrency = static_cast<size_t>(max_threads / threads_per_file);
        initial_concurrency = std::min(initial_concurrency, max_concurrency);

        spdlog::info("Comparing {} files ({} large, {} small, {:.1f} MB total)",
                      common_files.size(), multi_chunk_files, single_chunk_files,
                      total_bytes_to_compare / (1024.0 * 1024.0));
        spdlog::info("Concurrency: initial {}, max {} (threads/file: {})",
                      initial_concurrency, max_concurrency, threads_per_file);

        // Thread-safe result collection
        std::mutex result_mutex;
        std::vector<FileCompareResult> parallel_results;
        std::atomic<size_t> matching_count{0};
        std::atomic<size_t> mismatch_count{0};
        std::atomic<size_t> error_count{0};

        // Adaptive concurrency state
        std::mutex concurrency_mutex;
        std::condition_variable concurrency_cv;
        std::atomic<size_t> in_flight{0};
        std::atomic<size_t> current_max_concurrency{initial_concurrency};
        std::atomic<size_t> completed_files{0};

        // Throughput tracking for adaptive scaling
        // These variables are accessed under concurrency_mutex, but using atomics
        // makes the thread-safety explicit and allows lock-free reads for early-exit checks
        auto start_time = std::chrono::steady_clock::now();
        std::atomic<size_t> last_measurement_completed{0};
        std::chrono::steady_clock::time_point last_measurement_time = std::chrono::steady_clock::now();
        double last_throughput = 0.0;
        // Ensure at least 2 measurement opportunities for batches, but minimum 20 files between measurements
        std::atomic<size_t> measurement_interval{std::max(static_cast<size_t>(20), initial_concurrency)};
        if (common_files.size() > 0) {
            size_t adjusted = std::min(measurement_interval.load(), common_files.size() / 2);
            if (adjusted == 0) adjusted = 1;
            measurement_interval.store(adjusted);
        }
        std::atomic<bool> reached_limit{false};
        int consecutive_flat_measurements = 0;  // Count of consecutive measurements without improvement

        // Worker function that processes a single file
        auto process_file = [&](size_t idx) {
            if (progress.cancelled) return;

            const auto& rel_path = common_files[idx];

            // Build FileSource for each file
            FileSource file_a = make_file_source(config.source_a, rel_path);
            FileSource file_b = make_file_source(config.source_b, rel_path);

            if (config.debug) {
                spdlog::debug("Comparing file {}/{}: {} (A: {}, B: {})",
                             idx + 1, common_files.size(), rel_path,
                             file_a.type == SourceType::S3 ? "s3://" + file_a.bucket + "/" + file_a.path : file_a.path,
                             file_b.type == SourceType::S3 ? "s3://" + file_b.bucket + "/" + file_b.path : file_b.path);
            }

            // Reuse existing run_comparison for file-level comparison
            ComparisonConfig file_config;
            file_config.source_a = file_a;
            file_config.source_b = file_b;
            file_config.debug = config.debug;
            file_config.num_threads = threads_per_file;
            file_config.ramp_up = config.ramp_up;
            file_config.chunk_size = config.chunk_size;
            file_config.block_size = config.block_size;

            ComparisonProgress file_progress;
            file_progress.cancelled = progress.cancelled.load();
            auto file_result = run_comparison(file_config, file_progress, s3_client_a, s3_client_b);

            // Skip result processing if cancelled during comparison
            if (progress.cancelled) return;

            FileCompareResult fcr;
            fcr.relative_path = rel_path;
            fcr.size_a = file_result.size_a;
            fcr.size_b = file_result.size_b;
            fcr.total_chunks = std::max(file_result.source_a_crcs.size(),
                                        file_result.source_b_crcs.size());

            if (!file_result.success) {
                fcr.status = FileCompareStatus::Error;
                fcr.error_message = file_result.error_message;
                error_count++;
            } else if (file_result.all_match) {
                fcr.status = FileCompareStatus::Match;
                matching_count++;
            } else {
                fcr.status = FileCompareStatus::Mismatch;
                fcr.mismatched_chunks = file_result.mismatched_chunks;
                fcr.extra_chunks_in_a = file_result.extra_chunks_in_a;
                fcr.extra_chunks_in_b = file_result.extra_chunks_in_b;
                mismatch_count++;

                // For files with few chunks, do block-level analysis for finer granularity
                if (fcr.total_chunks <= 5) {
                    analyze_small_file_blocks(file_a, file_b, fcr, config.chunk_size);
                }
            }

            // Thread-safe result insertion
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                parallel_results.push_back(std::move(fcr));
            }

            progress.files_compared++;
        };

        // Wrapper that tracks in-flight count and handles adaptive scaling
        auto process_with_tracking = [&](size_t idx) {
            // RAII guard to ensure in_flight is always decremented, even on exception
            struct InFlightGuard {
                std::atomic<size_t>& in_flight_ref;
                std::mutex& mutex_ref;
                std::condition_variable& cv_ref;
                ~InFlightGuard() {
                    // notify_one() MUST be inside the lock to prevent race where main
                    // thread sees in_flight==0 and proceeds before we call notify
                    std::lock_guard<std::mutex> lock(mutex_ref);
                    --in_flight_ref;
                    cv_ref.notify_one();
                }
            } guard{in_flight, concurrency_mutex, concurrency_cv};

            process_file(idx);

            std::unique_lock<std::mutex> lock(concurrency_mutex);
            size_t done = ++completed_files;

            // Check if we should adjust concurrency (every measurement_interval completions)
            // Early exit check using atomics avoids unnecessary lock contention
            if (!reached_limit.load() && done % measurement_interval.load() == 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - last_measurement_time).count();

                if (elapsed > 0.1) {  // At least 100ms between measurements
                    size_t files_in_interval = done - last_measurement_completed.load();
                    double current_throughput = files_in_interval / elapsed;

                    size_t cur_max = current_max_concurrency.load();

                    // If throughput improved by >10%, increase concurrency
                    if (last_throughput > 0 && current_throughput > last_throughput * 1.10 && cur_max < max_concurrency) {
                        size_t new_max = std::min(cur_max * 2, max_concurrency);
                        current_max_concurrency.store(new_max);
                        spdlog::info("Adaptive: throughput {:.1f} -> {:.1f} files/s (+{:.0f}%), increasing concurrency {} -> {}",
                                    last_throughput, current_throughput,
                                    (current_throughput / last_throughput - 1) * 100, cur_max, new_max);
                        measurement_interval.store(std::max(static_cast<size_t>(20), new_max));
                        consecutive_flat_measurements = 0;  // Reset on improvement
                    } else if (last_throughput > 0 && current_throughput <= last_throughput * 1.10) {
                        // Throughput didn't improve - count consecutive flat measurements
                        consecutive_flat_measurements++;
                        if (consecutive_flat_measurements >= 2) {
                            // Require 2+ consecutive flat measurements to lock in
                            // This prevents locking due to temporary slowdowns
                            reached_limit.store(true);
                            spdlog::info("Adaptive: throughput stabilized at {:.1f} files/s, concurrency locked at {}",
                                        current_throughput, cur_max);
                        }
                    }

                    last_throughput = current_throughput;
                    last_measurement_time = now;
                    last_measurement_completed.store(done);
                }
            }
            // Note: in_flight decrement and notify_one are handled by InFlightGuard destructor
        };

        // Use thread pool with sliding window for adaptive concurrency
        // Size pool based on actual max concurrency needed, not the full thread budget
        // Use unique_ptr so we can move ownership to background thread for cleanup
        size_t pool_size = std::min(max_concurrency, common_files.size());
        if (pool_size == 0) pool_size = 1;  // Minimum 1 thread for empty case
        auto pool = std::make_unique<boost::asio::thread_pool>(pool_size);

        // Submit tasks using sliding window - keep current_max_concurrency in flight
        for (size_t idx = 0; idx < common_files.size(); ++idx) {
            if (progress.cancelled) break;

            // Wait for a slot to become available
            std::unique_lock<std::mutex> lock(concurrency_mutex);
            concurrency_cv.wait(lock, [&] {
                return in_flight < current_max_concurrency.load() || progress.cancelled;
            });

            if (progress.cancelled) break;

            ++in_flight;
            boost::asio::post(*pool, [&, idx]() {
                process_with_tracking(idx);
            });
        }

        // Wait for all in-flight tasks to complete (including InFlightGuard destructors)
        // Must wait on in_flight == 0, not completed_files, because tasks may still be
        // executing after incrementing completed_files (e.g., InFlightGuard destructor)
        {
            std::unique_lock<std::mutex> lock(concurrency_mutex);
            concurrency_cv.wait(lock, [&] {
                return in_flight == 0;
            });
        }

        // Background thread pool cleanup - see detailed comment in parallel_enumerate_local_directory()
        // for thread safety reasoning.
        //
        // Key invariant: InFlightGuard::~InFlightGuard() calls notify_one() while holding
        // concurrency_mutex, then releases the lock. The main thread cannot see in_flight==0
        // until it acquires the lock (which blocks until InFlightGuard releases it). Therefore,
        // when we reach this point, all workers have finished accessing shared state.
        // Remaining worker activity is just lambda/stack cleanup which doesn't touch shared state.
        std::thread([p = std::move(pool)]() {
            try { p->join(); } catch (const std::exception& e) {
                spdlog::error("Thread pool cleanup failed: {}", e.what());
            }
        }).detach();

        auto end_time = std::chrono::steady_clock::now();
        double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
        double final_throughput = total_elapsed > 0 ? completed_files.load() / total_elapsed : 0.0;
        spdlog::info("Adaptive concurrency complete: {} files in {:.1f}s ({:.1f} files/s), final concurrency: {}",
                    completed_files.load(), total_elapsed, final_throughput, current_max_concurrency.load());

        // Merge parallel results into main result
        result.matching_files += matching_count.load();
        result.mismatched_files += mismatch_count.load();
        result.errors += error_count.load();
        for (auto& fcr : parallel_results) {
            result.files.push_back(std::move(fcr));
        }

        // Sort results: mismatches first, then errors, then matches
        std::sort(result.files.begin(), result.files.end(),
            [](const FileCompareResult& a, const FileCompareResult& b) {
                auto priority = [](FileCompareStatus s) {
                    switch (s) {
                        case FileCompareStatus::Mismatch: return 0;
                        case FileCompareStatus::Error: return 1;
                        case FileCompareStatus::Match: return 2;
                        default: return 3;
                    }
                };
                if (priority(a.status) != priority(b.status)) {
                    return priority(a.status) < priority(b.status);
                }
                return a.relative_path < b.relative_path;
            });

        result.total_files = result.files.size();
        if (progress.cancelled) {
            result.success = false;
            result.error_message = "Cancelled during comparison";
        } else {
            result.success = true;
        }

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    result.comparison_elapsed = result.total_elapsed - result.discovery_elapsed;

    return result;
}
