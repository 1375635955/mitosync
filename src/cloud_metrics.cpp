#include "cloud_metrics.h"
#include "app_settings.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <io.h>      // _commit, _fileno
#include <process.h> // _getpid
#else
#include <unistd.h>  // fsync, fileno, getpid
#include <fcntl.h>   // open, O_RDONLY
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

// Operation type names for display
const char* S3OperationTypeName(S3OperationType op) {
    switch (op) {
        case S3OperationType::HeadObject: return "HeadObject";
        case S3OperationType::GetObject: return "GetObject";
        case S3OperationType::PutObject: return "PutObject";
        case S3OperationType::DeleteObject: return "DeleteObject";
        case S3OperationType::DeleteObjects: return "DeleteObjects";
        case S3OperationType::ListObjectsV2: return "ListObjectsV2";
        case S3OperationType::ListBuckets: return "ListBuckets";
        case S3OperationType::GetBucketLocation: return "GetBucketLocation";
        case S3OperationType::CreateMultipartUpload: return "CreateMultipartUpload";
        case S3OperationType::UploadPart: return "UploadPart";
        case S3OperationType::UploadPartCopy: return "UploadPartCopy";
        case S3OperationType::UploadPartCopyRemote: return "UploadPartCopy (remote)";
        case S3OperationType::CopyObject: return "CopyObject";
        case S3OperationType::CopyObjectRemote: return "CopyObject (remote)";
        case S3OperationType::AbortMultipartUpload: return "AbortMultipartUpload";
        case S3OperationType::CompleteMultipartUpload: return "CompleteMultipartUpload";
        case S3OperationType::ListParts: return "ListParts";
        case S3OperationType::ListMultipartUploads: return "ListMultipartUploads";
        case S3OperationType::Unknown: return "Unknown";
        default: return "Unknown";
    }
}

// Parse operation name from AWS SDK request name
S3OperationType ParseS3OperationType(const std::string& requestName) {
    // AWS SDK uses operation names like "HeadObject", "GetObject", etc.
    // Some SDK versions may include "Request" suffix
    if (requestName == "HeadObject" || requestName == "HeadObjectRequest")
        return S3OperationType::HeadObject;
    if (requestName == "GetObject" || requestName == "GetObjectRequest")
        return S3OperationType::GetObject;
    if (requestName == "PutObject" || requestName == "PutObjectRequest")
        return S3OperationType::PutObject;
    if (requestName == "DeleteObject" || requestName == "DeleteObjectRequest")
        return S3OperationType::DeleteObject;
    if (requestName == "DeleteObjects" || requestName == "DeleteObjectsRequest")
        return S3OperationType::DeleteObjects;
    if (requestName == "ListObjectsV2" || requestName == "ListObjectsV2Request" ||
        requestName == "ListObjects" || requestName == "ListObjectsRequest")
        return S3OperationType::ListObjectsV2;
    if (requestName == "ListBuckets" || requestName == "ListBucketsRequest")
        return S3OperationType::ListBuckets;
    if (requestName == "GetBucketLocation" || requestName == "GetBucketLocationRequest")
        return S3OperationType::GetBucketLocation;
    if (requestName == "CreateMultipartUpload" || requestName == "CreateMultipartUploadRequest")
        return S3OperationType::CreateMultipartUpload;
    if (requestName == "UploadPart" || requestName == "UploadPartRequest")
        return S3OperationType::UploadPart;
    if (requestName == "UploadPartCopy" || requestName == "UploadPartCopyRequest")
        return S3OperationType::UploadPartCopy;
    if (requestName == "UploadPartCopy (remote)" || requestName == "UploadPartCopyRemote")
        return S3OperationType::UploadPartCopyRemote;
    if (requestName == "CopyObject" || requestName == "CopyObjectRequest")
        return S3OperationType::CopyObject;
    if (requestName == "CopyObject (remote)" || requestName == "CopyObjectRemote")
        return S3OperationType::CopyObjectRemote;
    if (requestName == "AbortMultipartUpload" || requestName == "AbortMultipartUploadRequest")
        return S3OperationType::AbortMultipartUpload;
    if (requestName == "CompleteMultipartUpload" || requestName == "CompleteMultipartUploadRequest")
        return S3OperationType::CompleteMultipartUpload;
    if (requestName == "ListParts" || requestName == "ListPartsRequest")
        return S3OperationType::ListParts;
    if (requestName == "ListMultipartUploads" || requestName == "ListMultipartUploadsRequest")
        return S3OperationType::ListMultipartUploads;
    return S3OperationType::Unknown;
}

// Static member for test directory override
std::string CloudMetrics::test_data_directory_;

// Singleton instance
CloudMetrics& CloudMetrics::instance() {
    static CloudMetrics instance;
    return instance;
}

void CloudMetrics::setTestDataDirectory(const std::string& path) {
    test_data_directory_ = path;
}

std::string CloudMetrics::testDataDirectory() {
    return test_data_directory_;
}

void CloudMetrics::recordStart(S3OperationType op) {
    if (op >= S3OperationType::COUNT) return;
    metrics_[static_cast<size_t>(op)].call_count.fetch_add(1, std::memory_order_relaxed);
}

void CloudMetrics::recordSuccess(S3OperationType op, int64_t latency_ms,
                                  uint64_t bytes_up, uint64_t bytes_down,
                                  uint64_t bytes_server_side) {
    if (op >= S3OperationType::COUNT) return;

    auto& m = metrics_[static_cast<size_t>(op)];
    m.success_count.fetch_add(1, std::memory_order_relaxed);
    m.total_latency_ms.fetch_add(latency_ms, std::memory_order_relaxed);
    m.bytes_uploaded.fetch_add(bytes_up, std::memory_order_relaxed);
    m.bytes_downloaded.fetch_add(bytes_down, std::memory_order_relaxed);
    m.bytes_server_side.fetch_add(bytes_server_side, std::memory_order_relaxed);

    updateMinLatency(op, latency_ms);
    updateMaxLatency(op, latency_ms);
}

void CloudMetrics::recordFailure(S3OperationType op, int64_t latency_ms) {
    if (op >= S3OperationType::COUNT) return;

    auto& m = metrics_[static_cast<size_t>(op)];
    m.failure_count.fetch_add(1, std::memory_order_relaxed);
    m.total_latency_ms.fetch_add(latency_ms, std::memory_order_relaxed);

    updateMinLatency(op, latency_ms);
    updateMaxLatency(op, latency_ms);
}

void CloudMetrics::recordRetry(S3OperationType op) {
    if (op >= S3OperationType::COUNT) return;
    metrics_[static_cast<size_t>(op)].retry_count.fetch_add(1, std::memory_order_relaxed);
}

void CloudMetrics::addServerSideBytes(S3OperationType op, uint64_t bytes) {
    if (op >= S3OperationType::COUNT || bytes == 0) return;
    metrics_[static_cast<size_t>(op)].bytes_server_side.fetch_add(bytes, std::memory_order_relaxed);
}

const OperationMetrics& CloudMetrics::getMetrics(S3OperationType op) const {
    static OperationMetrics empty;
    if (op >= S3OperationType::COUNT) return empty;
    return metrics_[static_cast<size_t>(op)];
}

std::map<S3OperationType, OperationMetrics> CloudMetrics::getAllMetrics() const {
    std::map<S3OperationType, OperationMetrics> result;
    for (size_t i = 0; i < static_cast<size_t>(S3OperationType::COUNT); ++i) {
        auto op = static_cast<S3OperationType>(i);
        if (metrics_[i].call_count.load() > 0) {
            result[op] = metrics_[i];
        }
    }
    return result;
}

OperationMetrics CloudMetrics::getTotalMetrics() const {
    OperationMetrics total;
    total.min_latency_ms.store(INT64_MAX);
    total.max_latency_ms.store(0);

    for (size_t i = 0; i < static_cast<size_t>(S3OperationType::COUNT); ++i) {
        const auto& m = metrics_[i];
        total.call_count.fetch_add(m.call_count.load(), std::memory_order_relaxed);
        total.success_count.fetch_add(m.success_count.load(), std::memory_order_relaxed);
        total.failure_count.fetch_add(m.failure_count.load(), std::memory_order_relaxed);
        total.retry_count.fetch_add(m.retry_count.load(), std::memory_order_relaxed);
        total.bytes_uploaded.fetch_add(m.bytes_uploaded.load(), std::memory_order_relaxed);
        total.bytes_downloaded.fetch_add(m.bytes_downloaded.load(), std::memory_order_relaxed);
        total.bytes_server_side.fetch_add(m.bytes_server_side.load(), std::memory_order_relaxed);
        total.total_latency_ms.fetch_add(m.total_latency_ms.load(), std::memory_order_relaxed);

        int64_t min_lat = m.min_latency_ms.load();
        int64_t max_lat = m.max_latency_ms.load();

        // Update global min (only if valid)
        if (min_lat != INT64_MAX) {
            int64_t current_min = total.min_latency_ms.load();
            while (min_lat < current_min &&
                   !total.min_latency_ms.compare_exchange_weak(current_min, min_lat)) {
                // Retry if compare_exchange fails
            }
        }

        // Update global max
        if (max_lat > 0) {
            int64_t current_max = total.max_latency_ms.load();
            while (max_lat > current_max &&
                   !total.max_latency_ms.compare_exchange_weak(current_max, max_lat)) {
                // Retry if compare_exchange fails
            }
        }
    }

    return total;
}

void CloudMetrics::clear() {
    // Note: This provides "best effort" clearing. Operations that are in-flight
    // during clear() may or may not be reflected in the final state. This is
    // acceptable for a user-initiated reset action - any visible in-flight
    // operations would show up in the UI anyway.
    //
    // This resets memory and nothing else. It used to call save() at the end,
    // which made a reset reach through to ~/.local/share/mitosync (or the
    // platform equivalent) - and since CloudMetrics is a singleton whose data
    // directory defaults to the real one, that meant any test calling clear()
    // overwrote the metrics of whoever ran the suite with an empty file
    // (issue #41). Persisting is now the caller's decision, which is a single
    // save() in `mito stats --reset`, the one place that means it.

    // Reset all per-operation metrics (atomic operations)
    for (size_t i = 0; i < static_cast<size_t>(S3OperationType::COUNT); ++i) {
        metrics_[i].reset();
    }

    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        history_.clear();
        prev_total_calls_ = 0;
        prev_total_bytes_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(region_mutex_);
        regions_.clear();
    }

    // Reset auto-save tracking. The next sample() decides on its own whether
    // enough has accumulated to be worth writing.
    last_saved_calls_.store(0, std::memory_order_relaxed);
}

void CloudMetrics::sample() {
    auto now = std::chrono::steady_clock::now();
    bool should_save = false;

    {
        // Lock protects last_sample_time_, prev_total_*, and history_
        std::lock_guard<std::mutex> lock(history_mutex_);

        // Check if enough time has passed since last sample
        double elapsed_ms = std::chrono::duration<double, std::milli>(
            now - last_sample_time_).count();

        if (elapsed_ms < SAMPLE_INTERVAL_MS && !history_.empty()) {
            return;
        }
        last_sample_time_ = now;

        // Calculate totals (uses atomics, safe to call while holding mutex)
        OperationMetrics total = getTotalMetrics();
        uint64_t total_calls = total.call_count.load();
        uint64_t total_bytes = total.bytes_uploaded.load() + total.bytes_downloaded.load();

        Sample s;
        s.total_calls = total_calls;
        s.total_bytes = total_bytes;
        s.delta_calls = total_calls - prev_total_calls_;
        s.delta_bytes = total_bytes - prev_total_bytes_;

        prev_total_calls_ = total_calls;
        prev_total_bytes_ = total_bytes;

        history_.push_back(s);
        if (history_.size() > HISTORY_SIZE) {
            history_.pop_front();
        }

        // Check if auto-save is needed (atomic CAS to avoid duplicate saves)
        uint64_t last_saved = last_saved_calls_.load(std::memory_order_relaxed);
        while (total_calls >= last_saved + AUTO_SAVE_THRESHOLD) {
            if (last_saved_calls_.compare_exchange_weak(last_saved, total_calls,
                                                         std::memory_order_relaxed)) {
                should_save = true;
                break;
            }
            // If CAS failed, last_saved is updated with current value; re-check condition
        }
    }  // Release mutex before I/O

    if (should_save) {
        save();
    }
}

std::deque<CloudMetrics::Sample> CloudMetrics::getHistory() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return history_;
}

size_t CloudMetrics::historySize() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return history_.size();
}

void CloudMetrics::forceSample() {
    bool should_save = false;

    {
        std::lock_guard<std::mutex> lock(history_mutex_);

        // Calculate totals
        OperationMetrics total = getTotalMetrics();
        uint64_t total_calls = total.call_count.load();
        uint64_t total_bytes = total.bytes_uploaded.load() + total.bytes_downloaded.load();

        Sample s;
        s.total_calls = total_calls;
        s.total_bytes = total_bytes;
        s.delta_calls = total_calls - prev_total_calls_;
        s.delta_bytes = total_bytes - prev_total_bytes_;

        prev_total_calls_ = total_calls;
        prev_total_bytes_ = total_bytes;

        history_.push_back(s);
        if (history_.size() > HISTORY_SIZE) {
            history_.pop_front();
        }

        // Check if auto-save is needed (atomic CAS to avoid duplicate saves)
        uint64_t last_saved = last_saved_calls_.load(std::memory_order_relaxed);
        while (total_calls >= last_saved + AUTO_SAVE_THRESHOLD) {
            if (last_saved_calls_.compare_exchange_weak(last_saved, total_calls,
                                                         std::memory_order_relaxed)) {
                should_save = true;
                break;
            }
        }
    }  // Release mutex before I/O

    if (should_save) {
        save();
    }
}

CloudMetrics::Sample CloudMetrics::currentSample() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    if (history_.empty()) {
        return Sample{};
    }
    return history_.back();
}

void CloudMetrics::setRegion(const std::string& region) {
    std::lock_guard<std::mutex> lock(region_mutex_);
    regions_.clear();
    if (!region.empty()) {
        regions_.push_back(region);
    }
}

void CloudMetrics::addRegion(const std::string& region) {
    if (region.empty()) return;
    std::lock_guard<std::mutex> lock(region_mutex_);
    // Only add if not already present
    for (const auto& r : regions_) {
        if (r == region) return;
    }
    regions_.push_back(region);
}

std::string CloudMetrics::getRegion() const {
    std::lock_guard<std::mutex> lock(region_mutex_);
    return regions_.empty() ? std::string() : regions_.front();
}

std::vector<std::string> CloudMetrics::getRegions() const {
    std::lock_guard<std::mutex> lock(region_mutex_);
    return regions_;
}

bool CloudMetrics::isCrossRegion() const {
    std::lock_guard<std::mutex> lock(region_mutex_);
    if (regions_.size() < 2) return false;

    // Check if we have at least two different regions
    const std::string& first = regions_.front();
    for (size_t i = 1; i < regions_.size(); ++i) {
        if (regions_[i] != first) {
            return true;
        }
    }
    return false;
}

uint64_t CloudMetrics::getCrossBucketServerSideBytes() const {
    // Sum bytes_server_side from cross-bucket copy operations only
    // (UploadPartCopyRemote and CopyObjectRemote)
    uint64_t total = 0;
    total += metrics_[static_cast<size_t>(S3OperationType::UploadPartCopyRemote)]
                 .bytes_server_side.load(std::memory_order_relaxed);
    total += metrics_[static_cast<size_t>(S3OperationType::CopyObjectRemote)]
                 .bytes_server_side.load(std::memory_order_relaxed);
    return total;
}

void CloudMetrics::updateMinLatency(S3OperationType op, int64_t latency_ms) {
    if (op >= S3OperationType::COUNT) return;

    auto& min_lat = metrics_[static_cast<size_t>(op)].min_latency_ms;
    int64_t current = min_lat.load(std::memory_order_relaxed);

    while (latency_ms < current) {
        if (min_lat.compare_exchange_weak(current, latency_ms,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
            break;
        }
        // current is updated by compare_exchange_weak on failure
    }
}

void CloudMetrics::updateMaxLatency(S3OperationType op, int64_t latency_ms) {
    if (op >= S3OperationType::COUNT) return;

    auto& max_lat = metrics_[static_cast<size_t>(op)].max_latency_ms;
    int64_t current = max_lat.load(std::memory_order_relaxed);

    while (latency_ms > current) {
        if (max_lat.compare_exchange_weak(current, latency_ms,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
            break;
        }
        // current is updated by compare_exchange_weak on failure
    }
}

// Get the metrics file path (uses test directory if set)
std::string CloudMetrics::getMetricsFilePath() {
    std::string dir = test_data_directory_.empty()
                    ? GetAppDataDirectory()
                    : test_data_directory_;
#ifdef _WIN32
    return dir + "\\cloud_metrics.json";
#else
    return dir + "/cloud_metrics.json";
#endif
}

// Removes leftover metrics temporaries older than an hour. Only files whose
// name is exactly the metrics path plus the ".tmp." prefix this code writes,
// so nothing else in the directory can match.
static void sweep_stale_metrics_temps(const std::string& metrics_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path file(metrics_path);
    fs::path dir = file.parent_path();
    if (dir.empty()) return;

    const std::string prefix = file.filename().string() + ".tmp.";
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(1);

    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        auto mtime = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        if (mtime > cutoff) continue;
        std::error_code rm_ec;
        fs::remove(it->path(), rm_ec);
        if (!rm_ec) {
            spdlog::debug("Removed a stale metrics temporary: {}", it->path().string());
        }
    }
}

bool CloudMetrics::save() const {
    std::string tmp_path;  // Declared here for cleanup in catch block

    try {
        json j;
        j["version"] = 1;

        // Save timestamp (thread-safe)
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &time_t);
#else
        gmtime_r(&time_t, &tm_buf);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
        j["saved_at"] = ss.str();

        // Save per-operation metrics
        json metrics_json = json::object();
        for (size_t i = 0; i < static_cast<size_t>(S3OperationType::COUNT); ++i) {
            const auto& m = metrics_[i];
            uint64_t calls = m.call_count.load();
            if (calls == 0) continue;  // Skip unused operations

            auto op = static_cast<S3OperationType>(i);
            json op_json;
            op_json["calls"] = calls;
            op_json["success"] = m.success_count.load();
            op_json["failures"] = m.failure_count.load();
            op_json["retries"] = m.retry_count.load();
            op_json["bytes_up"] = m.bytes_uploaded.load();
            op_json["bytes_down"] = m.bytes_downloaded.load();
            op_json["bytes_server"] = m.bytes_server_side.load();

            metrics_json[S3OperationTypeName(op)] = op_json;
        }
        j["metrics"] = metrics_json;

        // Save regions
        {
            std::lock_guard<std::mutex> lock(region_mutex_);
            j["regions"] = regions_;
        }

        // Atomic write: write to temp file, fsync, then rename
        std::string path = getMetricsFilePath();

        // A hard kill between creating the temporary file and renaming it
        // leaves the temporary behind. The old fixed name reclaimed itself -
        // the next save reopened it - so unique names would otherwise grow
        // without bound in the user's data directory. Reap the old ones.
        // An hour is far longer than any save takes, so this cannot remove a
        // temporary another process is still writing.
        sweep_stale_metrics_temps(path);

        // The temporary file has to be unique. It used to be a fixed
        // "<path>.tmp" shared by every mito on the machine, so two runs saving
        // at once had one truncating the file the other was about to rename
        // into place - wiping the accumulated history outright, or leaving an
        // unparseable half-and-half file (issue #81).
        //
        // mkstemp rather than a pid-derived name: pids are only unique within
        // a pid namespace, and two containers sharing a mounted data directory
        // both start numbering from 1. That reproduced the original race even
        // with a pid in the name.
        mode_t existing_mode = 0;
        struct ::stat existing {};
        if (::stat(path.c_str(), &existing) == 0) {
            existing_mode = existing.st_mode & 07777;
        }

        std::string tmpl = path + ".tmp.XXXXXX";
        std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
        tmpl_buf.push_back('\0');
        int tmp_fd = mkstemp(tmpl_buf.data());
        if (tmp_fd < 0) {
            spdlog::warn("Failed to create a temporary file next to {}: {}",
                         path, std::strerror(errno));
            return false;
        }
        tmp_path.assign(tmpl_buf.data());

        // mkstemp creates 0600. Keep whatever the existing file had, so saving
        // does not quietly change the permissions of a file the user already
        // has.
        if (existing_mode != 0 && ::fchmod(tmp_fd, existing_mode) != 0) {
            spdlog::debug("Could not preserve permissions on {}: {}",
                          path, std::strerror(errno));
        }

        FILE* fp = ::fdopen(tmp_fd, "w");
        if (!fp) {
            spdlog::warn("Failed to open {} for writing: {}", tmp_path, std::strerror(errno));
            ::close(tmp_fd);
            std::remove(tmp_path.c_str());
            return false;
        }

        std::string content = j.dump(2);
        size_t written = std::fwrite(content.data(), 1, content.size(), fp);
        if (written != content.size()) {
            spdlog::warn("Failed to write to {}", tmp_path);
            std::fclose(fp);
            std::remove(tmp_path.c_str());
            return false;
        }

        // These returns have to be checked. The payload is a few hundred bytes,
        // so fwrite never reaches the disk at all - it fills the stdio buffer
        // and reports success. The write really happens at fflush, and on a
        // full disk that is where it fails. Ignoring it renamed an empty
        // temporary file over a perfectly good history, and exited 0.
        if (std::fflush(fp) != 0) {
            spdlog::warn("Failed to flush {}: {}. The existing metrics file is "
                         "left as it was.", tmp_path, std::strerror(errno));
            std::fclose(fp);
            std::remove(tmp_path.c_str());
            return false;
        }
#ifdef _WIN32
        if (_commit(_fileno(fp)) != 0) {
#else
        if (fsync(fileno(fp)) != 0) {
#endif
            spdlog::warn("Failed to sync {}: {}", tmp_path, std::strerror(errno));
            std::fclose(fp);
            std::remove(tmp_path.c_str());
            return false;
        }
        if (std::fclose(fp) != 0) {
            spdlog::warn("Failed to close {}: {}", tmp_path, std::strerror(errno));
            std::remove(tmp_path.c_str());
            return false;
        }

        // Rename temp file to final (atomic on POSIX/NTFS)
        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            spdlog::warn("Failed to rename {} to {}", tmp_path, path);
            std::remove(tmp_path.c_str());
            return false;
        }

#ifndef _WIN32
        // Sync parent directory for full crash-consistency on POSIX
        // Extract directory from path (works with both real and test directories)
        std::string dir = path.substr(0, path.find_last_of('/'));
        int dir_fd = open(dir.c_str(), O_RDONLY);
        if (dir_fd >= 0) {
            fsync(dir_fd);
            close(dir_fd);
        }
#endif

        spdlog::debug("Saved cloud metrics to {}", path);
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("Exception saving cloud metrics: {}", e.what());
        // Clean up temp file if it was created
        if (!tmp_path.empty()) {
            std::remove(tmp_path.c_str());
        }
        return false;
    }
}

bool CloudMetrics::load() {
    try {
        std::string path = getMetricsFilePath();
        std::ifstream in(path);
        if (!in) {
            spdlog::debug("No cloud metrics file found at {}", path);
            return false;
        }

        json j;
        in >> j;

        // Check version
        int version = j.value("version", 0);
        if (version != 1) {
            spdlog::warn("Unknown cloud metrics version: {}", version);
            return false;
        }

        // Parse all data into temporary storage BEFORE modifying state.
        // This ensures that if parsing fails, in-memory metrics remain unchanged.
        struct ParsedMetrics {
            uint64_t calls{0}, success{0}, failures{0}, retries{0};
            uint64_t bytes_up{0}, bytes_down{0}, bytes_server{0};
        };
        std::array<ParsedMetrics, static_cast<size_t>(S3OperationType::COUNT)> parsed{};
        std::vector<std::string> parsed_regions;

        // Parse per-operation metrics
        if (j.contains("metrics") && j["metrics"].is_object()) {
            for (auto& [op_name, op_json] : j["metrics"].items()) {
                S3OperationType op = ParseS3OperationType(op_name);
                if (op == S3OperationType::Unknown || op >= S3OperationType::COUNT) {
                    continue;  // Skip unknown operations
                }

                auto& p = parsed[static_cast<size_t>(op)];
                p.calls = op_json.value("calls", 0ULL);
                p.success = op_json.value("success", 0ULL);
                p.failures = op_json.value("failures", 0ULL);
                p.retries = op_json.value("retries", 0ULL);
                p.bytes_up = op_json.value("bytes_up", 0ULL);
                p.bytes_down = op_json.value("bytes_down", 0ULL);
                p.bytes_server = op_json.value("bytes_server", 0ULL);
            }
        }

        // Parse regions
        if (j.contains("regions") && j["regions"].is_array()) {
            for (const auto& r : j["regions"]) {
                if (r.is_string()) {
                    parsed_regions.push_back(r.get<std::string>());
                }
            }
        }

        // Parsing succeeded - now update in-memory state atomically
        // Clear and load metrics (order doesn't matter for atomics)
        for (size_t i = 0; i < static_cast<size_t>(S3OperationType::COUNT); ++i) {
            auto& m = metrics_[i];
            const auto& p = parsed[i];
            m.call_count.store(p.calls);
            m.success_count.store(p.success);
            m.failure_count.store(p.failures);
            m.retry_count.store(p.retries);
            m.bytes_uploaded.store(p.bytes_up);
            m.bytes_downloaded.store(p.bytes_down);
            m.bytes_server_side.store(p.bytes_server);
            // Reset latency stats (not persisted per design decision)
            m.total_latency_ms.store(0);
            m.min_latency_ms.store(INT64_MAX);
            m.max_latency_ms.store(0);
        }

        // Update regions
        {
            std::lock_guard<std::mutex> lock(region_mutex_);
            regions_ = std::move(parsed_regions);
        }

        // Update auto-save tracking and reset history state
        OperationMetrics total = getTotalMetrics();
        uint64_t total_calls = total.call_count.load();
        uint64_t total_bytes = total.bytes_uploaded.load() + total.bytes_downloaded.load();
        last_saved_calls_.store(total_calls, std::memory_order_relaxed);

        // Reset history to avoid incorrect delta spike on first sample after load
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.clear();
            prev_total_calls_ = total_calls;
            prev_total_bytes_ = total_bytes;
        }

        spdlog::info("Loaded cloud metrics from {} ({} total calls)",
                     path, total_calls);
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("Exception loading cloud metrics: {}", e.what());
        return false;
    }
}
