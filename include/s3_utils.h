#pragma once
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "s3_interface.h"  // For S3ListResult

// Tracks an active multipart upload for cleanup purposes
struct ActiveUpload {
    std::shared_ptr<Aws::S3::S3Client> client;
    Aws::String bucket;
    Aws::String key;
    Aws::String upload_id;
};

// Thread-safe registry for tracking active multipart uploads
// Use this to clean up orphaned uploads on error or early exit
class UploadRegistry {
public:
    static UploadRegistry& instance();

    // Register a new upload - call after CreateMultipartUpload succeeds
    void register_upload(const ActiveUpload& upload);

    // Unregister an upload - call after AbortMultipartUpload completes
    void unregister_upload(const Aws::String& upload_id);

    // Abort all tracked uploads - call on cleanup/exit
    // Thread-safe: releases lock before making network calls
    void abort_all();

    // Get count of active uploads
    size_t count() const;

private:
    UploadRegistry() = default;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ActiveUpload> m_uploads;
};

// RAII guard for multipart upload cleanup with exception safety.
//
// Usage pattern:
//   1. Register upload with UploadRegistry
//   2. Create UploadGuard
//   3. Do work
//   4. Call AbortMultipartUpload (or CompleteMultipartUpload)
//   5. Call guard.release() to unregister from registry
//
// On exception: guard destructor does nothing, leaving the upload
// registered so abort_all() can clean it up on program exit.
class UploadGuard {
public:
    explicit UploadGuard(const Aws::String& upload_id)
        : m_upload_id(upload_id), m_released(false) {}

    ~UploadGuard() {
        // If not released, intentionally leave upload in registry
        // so abort_all() can clean it up on program exit
    }

    // Call after successful abort/complete to unregister from registry
    void release() {
        if (!m_released) {
            UploadRegistry::instance().unregister_upload(m_upload_id);
            m_released = true;
        }
    }

    // Non-copyable, non-movable
    UploadGuard(const UploadGuard&) = delete;
    UploadGuard& operator=(const UploadGuard&) = delete;
    UploadGuard(UploadGuard&&) = delete;
    UploadGuard& operator=(UploadGuard&&) = delete;

private:
    Aws::String m_upload_id;
    bool m_released;
};

// Global shutdown flag for graceful termination
// Set by signal handlers, checked by worker threads
void RequestShutdown();
void ResetShutdown();  // Reset flag for new operations (e.g., after GUI cancel)
bool IsShutdownRequested();

// Get the size of an S3 object. Returns -1 on error.
int64_t GetS3ObjectSize(const Aws::S3::S3Client& s3_client, const Aws::String& bucket, const Aws::String& key);

// Get the size of a local file. Returns -1 on error.
int64_t GetLocalFileSize(const std::string& path);

// List objects in an S3 bucket with optional prefix
S3ListResult ListS3Objects(
    const Aws::S3::S3Client& s3_client,
    const Aws::String& bucket,
    const Aws::String& prefix = "",
    const Aws::String& delimiter = "/",
    const Aws::String& continuation_token = "",
    int max_keys = 1000
);

// Check if an S3 error is retryable (network/DNS issues, throttling)
bool IsRetryableS3Error(const Aws::S3::S3Error& error);

// True when an endpoint refused UploadPartCopy because it wants a checksum the
// client has no way to supply - the same capability gap as returning no
// checksum at all, announced as a rejection rather than as a silence.
//
// Matched on an absent checksum specifically. A 400 that merely mentions one
// can also be a value that did not match, and answering that with "this
// endpoint cannot do checksums" would be wrong and quiet about a real
// integrity failure (issue #99).
bool IsMissingChecksumSupportError(const Aws::S3::S3Error& error);

// Thread-safe jitter calculation for exponential backoff (0-25% of base delay)
int GetJitter(int base_delay_ms);

class S3MultipartCopy {
public:
    // Takes shared_ptr to ensure S3Client outlives this object
    // chunk_size: size of each chunk in bytes (default 8 MiB)
    // max_retries: number of retry attempts for transient failures
    S3MultipartCopy(std::shared_ptr<Aws::S3::S3Client> client, const Aws::String& bucket,
                    const Aws::String& src_key, const Aws::String& dst_key, int64_t filesize,
                    bool debug = false, int64_t chunk_size = 8 * 1024 * 1024, int max_retries = 5);
    ~S3MultipartCopy();
    Aws::String CreateMultipartUpload();
    // Implementation using std::async (unbounded parallelism)
    std::vector<uint32_t> ParallelUploadPartCopyRequests(const Aws::String& upload_id, const std::vector<int64_t>& chunk_ids = {}, std::function<void(double)> progress_cb = nullptr);
    // Implementation using boost::asio::thread_pool (bounded parallelism)
    // ramp_up: if true, gradually increase concurrency to warm up DNS cache and connection pool
    std::vector<uint32_t> ParallelUploadPartCopyRequestsThreadPool(const Aws::String& upload_id, size_t num_threads, bool ramp_up, const std::vector<int64_t>& chunk_ids = {}, std::function<void(double)> progress_cb = nullptr);
    // Returns true if abort succeeded, false on failure
    bool AbortMultipartUpload(const Aws::String& upload_id);
    // num_threads: 0 = unbounded (std::async), >0 = use thread pool with N threads
    // ramp_up: gradually increase concurrency to avoid overwhelming DNS
    //
    // Follows the IS3Client::GetChunkCRC32s contract for chunk_ids: one entry
    // per requested id in request order, duplicates included, and an id outside
    // [0, num_chunks) fails the whole request rather than being clamped or
    // skipped. Requesting a chunk twice costs one request, not two.
    std::vector<uint32_t> GetHashes(const std::vector<int64_t>& chunk_ids = {}, std::function<void(double)> progress_cb = nullptr, int num_threads = 64, bool ramp_up = false);
private:
    // Process a single chunk with retry logic. Returns CRC32 on success, nullopt on failure.
    // Increments error_count on permanent failure.
    std::optional<uint32_t> ProcessSingleChunk(int64_t chunk_id, const Aws::String& upload_id,
                                               std::atomic<size_t>& error_count, std::atomic<bool>& should_abort);

    // Explain, once per process, that this endpoint cannot produce server-side
    // chunk checksums. `how` names the way it said so.
    void ReportChecksumsUnsupported(const char* how);

    // Set when this copy hit that capability gap. Every part of every file hits
    // it, so the per-run tally is noise once the cause has been reported.
    std::atomic<bool> m_checksums_unsupported{false};

    std::shared_ptr<Aws::S3::S3Client> m_s3client;
    Aws::String m_bucket;
    Aws::String m_src_key;
    Aws::String m_dst_key;
    int64_t m_filesize;
    int64_t m_chunk_size;
    int m_max_retries;
    bool m_debug;
};
