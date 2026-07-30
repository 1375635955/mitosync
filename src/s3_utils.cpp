#include "../include/s3_utils.h"
#include "../include/crc32_chunks.h"
#include "../include/app_settings.h"
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <spdlog/spdlog.h>
#include <iomanip>
#include <future>
#include <vector>
#include <numeric>
#include <unordered_set>
#include <sstream>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <random>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
// Maximum number of errors before aborting parallel operations
constexpr int MAX_ERRORS_BEFORE_ABORT = 3;

// Global shutdown flag for graceful termination
static std::atomic<bool> g_shutdown_requested{false};

void RequestShutdown() {
    g_shutdown_requested.store(true);
}

void ResetShutdown() {
    g_shutdown_requested.store(false);
}

bool IsShutdownRequested() {
    return g_shutdown_requested.load();
}

// UploadRegistry implementation
UploadRegistry& UploadRegistry::instance() {
    static UploadRegistry registry;
    return registry;
}

void UploadRegistry::register_upload(const ActiveUpload& upload) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_uploads[std::string(upload.upload_id.c_str())] = upload;
    spdlog::debug("Registered upload: {} (total: {})", upload.upload_id, m_uploads.size());
}

void UploadRegistry::unregister_upload(const Aws::String& upload_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_uploads.erase(std::string(upload_id.c_str()));
    spdlog::debug("Unregistered upload: {} (remaining: {})", upload_id, m_uploads.size());
}

void UploadRegistry::abort_all() {
    // Copy uploads under lock, then release before making network calls
    // This prevents blocking other threads during potentially slow abort operations
    std::vector<ActiveUpload> uploads_to_abort;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_uploads.empty()) return;

        uploads_to_abort.reserve(m_uploads.size());
        for (const auto& [id, upload] : m_uploads) {
            uploads_to_abort.push_back(upload);
        }
        m_uploads.clear();  // Clear immediately so new uploads can be tracked
    }

    spdlog::info("Aborting {} orphaned multipart upload(s)", uploads_to_abort.size());
    for (const auto& upload : uploads_to_abort) {
        if (!upload.client) {
            spdlog::warn("Skipping upload {} with null client", upload.upload_id);
            continue;
        }
        try {
            Aws::S3::Model::AbortMultipartUploadRequest abort_req;
            abort_req.SetBucket(upload.bucket);
            abort_req.SetKey(upload.key);
            abort_req.SetUploadId(upload.upload_id);
            auto outcome = upload.client->AbortMultipartUpload(abort_req);
            if (outcome.IsSuccess()) {
                spdlog::info("Aborted upload: {}", upload.upload_id);
            } else {
                spdlog::error("Failed to abort upload {}: {}", upload.upload_id, outcome.GetError().GetMessage());
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception aborting upload {}: {}", upload.upload_id, e.what());
        }
    }
}

size_t UploadRegistry::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_uploads.size();
}

// Thread-safe jitter calculation for exponential backoff (0-25% of base delay)
// Uses thread-local random engine to avoid data races
int GetJitter(int base_delay_ms) {
    thread_local std::mt19937 tls_gen{std::random_device{}()};
    int max_jitter = base_delay_ms / 4 + 1;  // +1 to avoid modulo by zero
    return tls_gen() % max_jitter;
}

// Extract the numeric curl code from a message like "curlCode: 28, Timeout was reached".
// Returns -1 when the message carries no curl code.
//
// The digits must be matched in full: a substring test for "curlCode: 6" would
// also fire on "curlCode: 60".."curlCode: 69", which are permanent failures
// (60 bad SSL peer certificate, 67 login denied, 77 bad CA cert).
static int ExtractCurlCode(const std::string& msg) {
    static const std::string marker = "curlCode: ";
    size_t pos = msg.find(marker);
    if (pos == std::string::npos) return -1;

    size_t start = pos + marker.size();
    size_t end = start;
    while (end < msg.size() && std::isdigit(static_cast<unsigned char>(msg[end]))) ++end;
    if (end == start) return -1;

    try {
        return std::stoi(msg.substr(start, end - start));
    } catch (const std::exception&) {
        return -1;  // out of int range; not a curl code we act on
    }
}

// Check if an S3 error is retryable (network/DNS issues, throttling, timeouts, server errors)
bool IsRetryableS3Error(const Aws::S3::S3Error& error) {
    // Retry on network/DNS errors, throttling, and server errors
    auto error_type = error.GetErrorType();
    if (error_type == Aws::S3::S3Errors::NETWORK_CONNECTION ||
        error_type == Aws::S3::S3Errors::REQUEST_TIMEOUT ||
        error_type == Aws::S3::S3Errors::THROTTLING ||
        error_type == Aws::S3::S3Errors::SLOW_DOWN ||
        error_type == Aws::S3::S3Errors::INTERNAL_FAILURE ||
        error_type == Aws::S3::S3Errors::SERVICE_UNAVAILABLE) {
        return true;
    }
    // Also check for curl error codes and server error messages
    std::string msg = error.GetMessage();

    // Transient curl failures: 6 DNS resolution, 7 connection failed,
    // 28 operation timeout. Matched exactly, not by prefix.
    int curl_code = ExtractCurlCode(msg);
    if (curl_code == 6 || curl_code == 7 || curl_code == 28) {
        return true;
    }

    if (msg.find("Could not resolve") != std::string::npos ||
        msg.find("getaddrinfo") != std::string::npos ||
        msg.find("Could not connect") != std::string::npos ||
        msg.find("internal error") != std::string::npos ||  // AWS internal server error
        msg.find("Internal error") != std::string::npos ||
        msg.find("Please try again") != std::string::npos) {  // Generic retry hint from AWS
        return true;
    }
    return false;
}

// Matched on an absent checksum specifically, not on the word alone. A 400 that
// merely mentions a checksum can also be one that did not match - the source
// object's stored value, say - and answering that with "this endpoint cannot do
// checksums" would be both wrong and quiet about a real integrity failure.
// Wanting one it never got is a different sentence from disliking one it did
// (issue #99).
bool IsMissingChecksumSupportError(const Aws::S3::S3Error& error) {
    if (error.GetResponseCode() != Aws::Http::HttpResponseCode::BAD_REQUEST) return false;
    std::string message(error.GetMessage().c_str());
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (message.find("checksum") == std::string::npos) return false;
    for (const char* absent : {"missing", "required", "unsupported", "not supported"}) {
        if (message.find(absent) != std::string::npos) return true;
    }
    return false;
}

int64_t GetS3ObjectSize(const Aws::S3::S3Client& s3_client, const Aws::String& bucket, const Aws::String& key) {
    try {
        Aws::S3::Model::HeadObjectRequest head_request;
        head_request.SetBucket(bucket);
        head_request.SetKey(key);
        auto head_outcome = s3_client.HeadObject(head_request);
        if (!head_outcome.IsSuccess()) {
            spdlog::debug("Failed to get object size: {}", head_outcome.GetError().GetMessage());
            return -1;
        }
        return head_outcome.GetResult().GetContentLength();
    } catch (const std::exception& e) {
        spdlog::debug("Exception in GetS3ObjectSize: {}", e.what());
        return -1;
    }
}

int64_t GetLocalFileSize(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        spdlog::error("Failed to get file size for '{}': {}", path, strerror(errno));
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
}

S3ListResult ListS3Objects(
    const Aws::S3::S3Client& s3_client,
    const Aws::String& bucket,
    const Aws::String& prefix,
    const Aws::String& delimiter,
    const Aws::String& continuation_token,
    int max_keys
) {
    S3ListResult result;
    try {
        Aws::S3::Model::ListObjectsV2Request request;
        request.SetBucket(bucket);
        request.SetMaxKeys(max_keys);

        if (!prefix.empty()) {
            request.SetPrefix(prefix);
        }
        if (!delimiter.empty()) {
            request.SetDelimiter(delimiter);
        }
        if (!continuation_token.empty()) {
            request.SetContinuationToken(continuation_token);
        }

        auto outcome = s3_client.ListObjectsV2(request);
        if (!outcome.IsSuccess()) {
            result.error_message = outcome.GetError().GetMessage();
            spdlog::error("Failed to list objects: {}", result.error_message);
            return result;
        }

        const auto& list_result = outcome.GetResult();

        // Extract object keys and sizes
        for (const auto& object : list_result.GetContents()) {
            S3ObjectInfo info;
            info.key = object.GetKey();
            info.size = object.GetSize();
            info.last_modified = object.GetLastModified().Seconds();
            result.objects.push_back(std::move(info));
        }

        // Extract common prefixes (folders)
        for (const auto& cp : list_result.GetCommonPrefixes()) {
            result.common_prefixes.push_back(cp.GetPrefix());
        }

        result.is_truncated = list_result.GetIsTruncated();
        if (result.is_truncated) {
            result.next_continuation_token = list_result.GetNextContinuationToken();
        }
        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
        spdlog::error("Exception in ListS3Objects: {}", e.what());
    }
    return result;
}

S3MultipartCopy::S3MultipartCopy(std::shared_ptr<Aws::S3::S3Client> client, const Aws::String& bucket,
                                 const Aws::String& src_key, const Aws::String& dst_key, int64_t filesize,
                                 bool debug, int64_t chunk_size, int max_retries)
    : m_s3client(std::move(client)), m_bucket(bucket), m_src_key(src_key), m_dst_key(dst_key),
      m_filesize(filesize), m_chunk_size(chunk_size > 0 ? chunk_size : DEFAULT_CHUNK_SIZE),
      m_max_retries(max_retries), m_debug(debug) {}

S3MultipartCopy::~S3MultipartCopy() {}

void S3MultipartCopy::ReportChecksumsUnsupported(const char* how) {
    m_checksums_unsupported.store(true, std::memory_order_relaxed);

    // Once per process, not once per part or once per file. Every part of every
    // file meets the same wall, and the cause and the consequence do not change
    // between them - repeating it buries whatever else the run has to say.
    static std::once_flag warned;
    std::call_once(warned, [this, how] {
        // Render the threshold in whichever unit reads sensibly; integer MiB
        // would show "0 MiB" for sub-MiB chunk sizes.
        std::string threshold =
            m_chunk_size >= 1024 * 1024
                ? std::to_string(m_chunk_size / (1024 * 1024)) + " MiB"
                : std::to_string(m_chunk_size) + " bytes";
        spdlog::error(
            "s3://{}/{}: {}, so server-side checksums are unsupported here. "
            "mito derives remote chunk checksums from UploadPartCopy, so files "
            "larger than the {} chunk size cannot be compared against this "
            "endpoint, and a sync of one re-uploads it in full instead of "
            "reusing the chunks that already match. Smaller files are "
            "unaffected - they are checksummed locally after download.",
            std::string(m_bucket.c_str()), std::string(m_src_key.c_str()), how, threshold);
    });
}

Aws::String S3MultipartCopy::CreateMultipartUpload() {
    try {
        Aws::S3::Model::CreateMultipartUploadRequest create_req;
        create_req.SetBucket(m_bucket);
        create_req.SetKey(m_dst_key);
        create_req.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::CRC32);
        auto create_out = m_s3client->CreateMultipartUpload(create_req);
        if (!create_out.IsSuccess()) {
            spdlog::error("Error initiating multipart upload: {}", create_out.GetError().GetMessage());
            return "";
        }
        return create_out.GetResult().GetUploadId();
    } catch (const std::exception& e) {
        spdlog::error("Exception in CreateMultipartUpload: {}", e.what());
        return "";
    }
}

namespace {

int64_t chunk_count_for_size(int64_t filesize, int64_t chunk_size) {
    if (filesize < 0 || chunk_size <= 0) {
        return -1;
    }
    return filesize == 0 ? 0
                         : filesize / chunk_size + (filesize % chunk_size != 0 ? 1 : 0);
}

// True when every requested id names a chunk this object actually has.
//
// Both fan-outs below index crc32_results by the caller's id, to write the
// result and again to select it afterwards. An id past the end is therefore an
// out-of-bounds vector access - undefined behaviour rather than a failed
// request (issue #52) - and an id that merely looks plausible would send a
// bogus byte range to S3 on the way there. S3ClientImpl validates before it
// gets here; this keeps a direct user of S3MultipartCopy from stepping in the
// same hole.
bool chunk_request_is_valid(const std::vector<int64_t>& chunk_ids, int64_t filesize,
                            int64_t num_chunks) {
    // A negative size is not a size. Checked directly rather than through the
    // chunk count: for a small negative filesize the count rounds to 0 and
    // looks harmless, while a larger one gives a negative count that throws
    // length_error out of std::vector when the results are sized. Neither
    // deserves a request to S3.
    if (filesize < 0 || num_chunks < 0) {
        spdlog::error("S3MultipartCopy: object size {} gives {} chunks - refusing the request",
                      filesize, num_chunks);
        return false;
    }
    for (int64_t id : chunk_ids) {
        if (id < 0 || id >= num_chunks) {
            spdlog::error("S3MultipartCopy: chunk_id {} is out of range ({} chunks)",
                          id, num_chunks);
            return false;
        }
    }
    return true;
}

// True for the way an endpoint says it cannot checksum a copied part.
//
// MinIO answers UploadPartCopy into a checksum-declaring upload with 400
// InvalidArgument naming a missing checksum. No request field could satisfy
// that: UploadPartCopyRequest has no checksum member at all, and the client
// cannot know the checksum of a range whose bytes it never reads - which is
// the entire reason this class asks the server for it. So it is the same
// capability gap as an absent checksum in the response, announced as a
// rejection rather than as a silence, and it deserves the same handling.
//

}  // namespace

std::vector<uint32_t> S3MultipartCopy::ParallelUploadPartCopyRequests(const Aws::String& upload_id, const std::vector<int64_t>& chunk_ids, std::function<void(double)> progress_cb) {
    int64_t num_chunks = chunk_count_for_size(m_filesize, m_chunk_size);
    if (!chunk_request_is_valid(chunk_ids, m_filesize, num_chunks)) {
        return {};
    }
    std::vector<uint32_t> crc32_results(num_chunks);

    // Build list of chunk IDs to process, one entry per *distinct* chunk.
    //
    // A repeated id used to mean a repeated task, and two tasks for one chunk
    // write the same crc32_results slot from two threads - a data race
    // ThreadSanitizer reports, benign in practice but undefined - and send two
    // UploadPartCopy calls with the same PartNumber against one upload id,
    // paying twice for the same answer. Duplicates belong in the reply, not in
    // the work: the filter at the end restores them.
    std::vector<int64_t> ids_storage;
    if (chunk_ids.empty()) {
        ids_storage.resize(num_chunks);
        std::iota(ids_storage.begin(), ids_storage.end(), 0);
    } else {
        std::unordered_set<int64_t> seen;
        ids_storage.reserve(chunk_ids.size());
        for (int64_t id : chunk_ids) {
            if (seen.insert(id).second) {
                ids_storage.push_back(id);
            }
        }
    }
    const std::vector<int64_t>& ids = ids_storage;

    std::atomic<size_t> completed{0};
    // Chunks that actually produced a checksum. `completed` counts every chunk
    // that ran, and `errors` counts only chunks that failed outright - neither
    // notices a chunk skipped because shutdown was requested, which would
    // otherwise leave a fabricated zero in the results (issue #51).
    std::atomic<size_t> succeeded{0};
    std::atomic<size_t> errors{0};
    std::atomic<bool> should_abort{false};
    size_t total = ids.size();

    if (m_debug) {
        spdlog::debug("Starting parallel upload of {} chunks", total);
    }

    auto process_chunk = [&](int64_t i) {
        auto result = ProcessSingleChunk(i, upload_id, errors, should_abort);
        if (result.has_value()) {
            crc32_results[i] = result.value();
            ++succeeded;
        }
        // Note: error_count is already incremented by ProcessSingleChunk on failure
        if (errors > MAX_ERRORS_BEFORE_ABORT) {
            should_abort = true;
        }
        size_t done = ++completed;
        if (progress_cb) progress_cb(100.0 * done / total);
    };

    // Launch all requests in parallel (DNS is pre-warmed by caller)
    std::vector<std::future<void>> futures;
    futures.reserve(ids.size());

    for (int64_t i : ids) {
        futures.push_back(std::async(std::launch::async, [&, i]() {
            process_chunk(i);
        }));
    }

    // Wait for all futures to complete (MUST wait for all to avoid use-after-free)
    const auto timeout = std::chrono::seconds(30);
    bool abort_triggered = false;
    for (auto& f : futures) {
        // Check for accumulated errors or shutdown - signal abort to remaining requests
        if (!abort_triggered && (errors > MAX_ERRORS_BEFORE_ABORT || IsShutdownRequested())) {
            if (IsShutdownRequested()) {
                spdlog::info("Shutdown requested, signaling abort to remaining requests");
            } else {
                spdlog::error("Too many errors ({}), signaling abort to remaining requests", errors.load());
            }
            should_abort = true;
            abort_triggered = true;
        }
        // Wait with timeout to avoid hanging on DNS failures
        if (f.wait_for(timeout) == std::future_status::timeout) {
            spdlog::error("Request timed out after {}s", timeout.count());
            ++errors;
        } else {
            f.get();
        }
    }

    if (errors > 0) {
        if (m_checksums_unsupported.load(std::memory_order_relaxed)) {
            // The cause was reported once, at error level, with what it means.
            // Repeating a tally per file - for a run that then falls back and
            // finishes correctly - reads as breakage when nothing is broken.
            spdlog::debug("Failed to copy {} of {} chunks: this endpoint cannot "
                          "checksum copied parts", errors.load(), total);
        } else {
            spdlog::error("Failed to copy {} of {} chunks", errors.load(), total);
        }
        return {};
    }
    // A chunk skipped by shutdown fails neither test above, so without this an
    // interrupted run returned a full-size vector with zeros in the slots that
    // were never computed - and zero is a value a real chunk can produce.
    if (succeeded.load() != total) {
        // A user-requested interrupt is not an error; only log it as one when
        // chunks went missing for some other reason.
        if (IsShutdownRequested()) {
            spdlog::info("Interrupted after {} of {} chunks; discarding the partial "
                         "result rather than reporting unread chunks as zero",
                         succeeded.load(), total);
        } else {
            spdlog::error("Only {} of {} chunks were checksummed; discarding the "
                          "partial result rather than reporting unread chunks as zero",
                          succeeded.load(), total);
        }
        return {};
    }

    // If chunk_ids is empty, return all results; else, return only requested chunk results
    if (chunk_ids.empty()) return crc32_results;
    std::vector<uint32_t> filtered;
    for (int64_t i : chunk_ids) filtered.push_back(crc32_results[i]);
    return filtered;
}

std::vector<uint32_t> S3MultipartCopy::ParallelUploadPartCopyRequestsThreadPool(
    const Aws::String& upload_id,
    size_t num_threads,
    bool ramp_up,
    const std::vector<int64_t>& chunk_ids,
    std::function<void(double)> progress_cb
) {
    int64_t num_chunks = chunk_count_for_size(m_filesize, m_chunk_size);
    if (!chunk_request_is_valid(chunk_ids, m_filesize, num_chunks)) {
        return {};
    }
    std::vector<uint32_t> crc32_results(num_chunks);

    // Build list of chunk IDs to process, one entry per *distinct* chunk.
    //
    // A repeated id used to mean a repeated task, and two tasks for one chunk
    // write the same crc32_results slot from two threads - a data race
    // ThreadSanitizer reports, benign in practice but undefined - and send two
    // UploadPartCopy calls with the same PartNumber against one upload id,
    // paying twice for the same answer. Duplicates belong in the reply, not in
    // the work: the filter at the end restores them.
    std::vector<int64_t> ids_storage;
    if (chunk_ids.empty()) {
        ids_storage.resize(num_chunks);
        std::iota(ids_storage.begin(), ids_storage.end(), 0);
    } else {
        std::unordered_set<int64_t> seen;
        ids_storage.reserve(chunk_ids.size());
        for (int64_t id : chunk_ids) {
            if (seen.insert(id).second) {
                ids_storage.push_back(id);
            }
        }
    }
    const std::vector<int64_t>& ids = ids_storage;

    std::atomic<size_t> completed{0};
    // Chunks that actually produced a checksum. `completed` counts every chunk
    // that ran, and `errors` counts only chunks that failed outright - neither
    // notices a chunk skipped because shutdown was requested, which would
    // otherwise leave a fabricated zero in the results (issue #51).
    std::atomic<size_t> succeeded{0};
    std::atomic<size_t> errors{0};
    std::atomic<bool> should_abort{false};
    size_t total = ids.size();

    if (m_debug) {
        spdlog::debug("Starting thread pool upload of {} chunks with {} threads{}",
                      total, num_threads, ramp_up ? " (with ramp-up)" : "");
    }

    auto process_chunk = [&](int64_t i) {
        auto result = ProcessSingleChunk(i, upload_id, errors, should_abort);
        if (result.has_value()) {
            crc32_results[i] = result.value();
            ++succeeded;
        }
        // Note: error_count is already incremented by ProcessSingleChunk on failure
        if (errors > MAX_ERRORS_BEFORE_ABORT) {
            should_abort = true;
        }
        size_t done = ++completed;
        if (progress_cb) progress_cb(100.0 * done / total);
    };

    // Both ramp_up and non-ramp_up modes use adaptive throttling:
    // - On errors (after retries fail), reduce concurrency by half (min 8)
    // - On successful completions, gradually ramp back up
    {
        boost::asio::thread_pool pool(num_threads);

        std::mutex mtx;
        std::condition_variable cv;
        size_t in_flight = 0;
        std::atomic<size_t> current_max_concurrency{ramp_up ? std::min(static_cast<size_t>(16), num_threads) : num_threads};
        std::atomic<size_t> ramp_threshold{current_max_concurrency.load()};  // Ramp up after this many completions

        if (m_debug) {
            spdlog::debug("Starting thread pool: initial concurrency={}, adaptive throttling enabled",
                          current_max_concurrency.load());
        }

        // Wrapper that tracks in-flight count and handles adaptive concurrency
        auto process_with_tracking = [&](int64_t chunk_id) {
            size_t errors_before = errors.load();
            process_chunk(chunk_id);
            size_t errors_after = errors.load();
            bool this_failed = errors_after > errors_before;

            std::unique_lock<std::mutex> lock(mtx);
            --in_flight;

            if (this_failed) {
                // Reduce concurrency on failure (halve, min 16 but respect num_threads limit)
                size_t min_concurrency = std::min(static_cast<size_t>(16), num_threads);
                size_t current = current_max_concurrency.load();
                size_t new_max = std::max(current / 2, min_concurrency);
                while (new_max < current) {
                    if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                        spdlog::debug("Diff: Rate limited - reducing concurrency to {}", new_max);
                        break;
                    }
                    new_max = std::max(current / 2, min_concurrency);
                }
            } else {
                // Check if we should ramp up concurrency
                size_t done = completed.load();
                if (done >= ramp_threshold.load()) {
                    size_t current = current_max_concurrency.load();
                    if (current < num_threads) {
                        size_t new_max = std::min(current * 2, num_threads);
                        if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                            ramp_threshold.store(done + new_max);  // Next ramp-up after another batch
                            if (m_debug) {
                                spdlog::debug("Ramping up concurrency: {} -> {} (after {} completions)",
                                              current, new_max, done);
                            }
                        }
                    }
                }
            }

            cv.notify_one();  // Wake up submitter thread
        };

        // Submit tasks using sliding window - always keep max_concurrency in flight
        for (int64_t chunk_id : ids) {
            if (should_abort.load() || IsShutdownRequested()) break;

            // Wait for a slot to become available
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&] {
                return in_flight < current_max_concurrency.load() || should_abort.load() || IsShutdownRequested();
            });

            if (should_abort.load() || IsShutdownRequested()) break;

            ++in_flight;
            boost::asio::post(pool, [&, chunk_id]() {
                process_with_tracking(chunk_id);
            });
        }

        pool.join();
    }

    if (errors > 0) {
        if (m_checksums_unsupported.load(std::memory_order_relaxed)) {
            // The cause was reported once, at error level, with what it means.
            // Repeating a tally per file - for a run that then falls back and
            // finishes correctly - reads as breakage when nothing is broken.
            spdlog::debug("Failed to copy {} of {} chunks: this endpoint cannot "
                          "checksum copied parts", errors.load(), total);
        } else {
            spdlog::error("Failed to copy {} of {} chunks", errors.load(), total);
        }
        return {};
    }
    // A chunk skipped by shutdown fails neither test above, so without this an
    // interrupted run returned a full-size vector with zeros in the slots that
    // were never computed - and zero is a value a real chunk can produce.
    if (succeeded.load() != total) {
        // A user-requested interrupt is not an error; only log it as one when
        // chunks went missing for some other reason.
        if (IsShutdownRequested()) {
            spdlog::info("Interrupted after {} of {} chunks; discarding the partial "
                         "result rather than reporting unread chunks as zero",
                         succeeded.load(), total);
        } else {
            spdlog::error("Only {} of {} chunks were checksummed; discarding the "
                          "partial result rather than reporting unread chunks as zero",
                          succeeded.load(), total);
        }
        return {};
    }

    // If chunk_ids is empty, return all results; else, return only requested chunk results
    if (chunk_ids.empty()) return crc32_results;
    std::vector<uint32_t> filtered;
    for (int64_t i : chunk_ids) filtered.push_back(crc32_results[i]);
    return filtered;
}

bool S3MultipartCopy::AbortMultipartUpload(const Aws::String& upload_id) {
    try {
        Aws::S3::Model::AbortMultipartUploadRequest abort_req;
        abort_req.SetBucket(m_bucket);
        abort_req.SetKey(m_dst_key);
        abort_req.SetUploadId(upload_id);
        auto abort_out = m_s3client->AbortMultipartUpload(abort_req);
        if (!abort_out.IsSuccess()) {
            spdlog::error("Error aborting multipart upload: {}", abort_out.GetError().GetMessage());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Exception in AbortMultipartUpload: {}", e.what());
        return false;
    }
}

std::optional<uint32_t> S3MultipartCopy::ProcessSingleChunk(
    int64_t chunk_id,
    const Aws::String& upload_id,
    std::atomic<size_t>& error_count,
    std::atomic<bool>& should_abort
) {
    if (should_abort.load() || IsShutdownRequested()) return std::nullopt;

    const int max_retries = m_max_retries;
    int retry_delay_ms = 100;

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        if (should_abort.load() || IsShutdownRequested()) return std::nullopt;
        try {
            int64_t start = chunk_id * m_chunk_size;
            int64_t end = std::min(start + m_chunk_size - 1, m_filesize - 1);
            Aws::String range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);

            Aws::S3::Model::UploadPartCopyRequest copy_req;
            copy_req.SetBucket(m_bucket);
            copy_req.SetKey(m_dst_key);
            copy_req.SetUploadId(upload_id);
            copy_req.SetPartNumber(static_cast<int>(chunk_id + 1));
            // SDK handles URL encoding internally
            Aws::String copy_source = m_bucket + "/" + m_src_key;
            copy_req.SetCopySource(copy_source);
            copy_req.SetCopySourceRange(range);

            spdlog::trace("UploadPartCopy: part={} range={} copy_source={}",
                          chunk_id + 1, std::string(range.c_str()), std::string(copy_source.c_str()));

            auto copy_out = m_s3client->UploadPartCopy(copy_req);
            if (copy_out.IsSuccess()) {
                Aws::String b64_crc32 = copy_out.GetResult().GetCopyPartResult().GetChecksumCRC32();
                Aws::Utils::ByteBuffer decoded = Aws::Utils::HashingUtils::Base64Decode(b64_crc32);
                if (decoded.GetLength() == 4) {
                    return (static_cast<uint32_t>(decoded[0]) << 24) |
                           (static_cast<uint32_t>(decoded[1]) << 16) |
                           (static_cast<uint32_t>(decoded[2]) << 8) |
                           static_cast<uint32_t>(decoded[3]);
                }

                if (b64_crc32.empty()) {
                    // The copy succeeded but no checksum came back. This is what
                    // an S3-compatible gateway without AWS "additional checksums"
                    // support looks like (Storj, and some MinIO/Ceph builds): the
                    // request is accepted, the x-amz-checksum-crc32 header is
                    // simply absent.
                    ReportChecksumsUnsupported(
                        "the endpoint accepted UploadPartCopy but returned no CRC32 checksum");
                } else {
                    // A checksum was returned but is not 4 bytes once decoded.
                    spdlog::error("Part {}: unexpected checksum format (length={})",
                                  chunk_id, decoded.GetLength());
                }
                ++error_count;
                return std::nullopt;
            } else {
                const auto& error = copy_out.GetError();
                // Checked before the retry test: no number of attempts makes an
                // endpoint grow a feature, and this arrives once per part of
                // every file, so retrying it multiplies a fixed answer.
                if (IsMissingChecksumSupportError(error)) {
                    ReportChecksumsUnsupported(
                        "the endpoint rejected an UploadPartCopy for an upload that declares CRC32");
                    ++error_count;
                    return std::nullopt;
                }
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    // Add jitter (0-25%) to prevent thundering herd
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for UploadPartCopy part {}: {}",
                                  attempt + 1, chunk_id, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                // Include full details for debugging key-not-found errors
                spdlog::error("Error copying part {} from s3://{}/{}: {} [ErrorType={}, HTTP={}]",
                             chunk_id, m_bucket, m_src_key, error.GetMessage(),
                             static_cast<int>(error.GetErrorType()),
                             static_cast<int>(error.GetResponseCode()));
                ++error_count;
                return std::nullopt;
            }
        } catch (const std::exception& e) {
            if (attempt < max_retries) {
                int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                spdlog::debug("Retry {} for UploadPartCopy part {}: {}",
                              attempt + 1, chunk_id, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                retry_delay_ms *= 2;
                continue;
            }
            spdlog::error("Exception copying part {}: {}", chunk_id, e.what());
            ++error_count;
            return std::nullopt;
        }
    }
    // Unreachable: all paths through the loop either return or continue
    return std::nullopt;
}

std::vector<uint32_t> S3MultipartCopy::GetHashes(const std::vector<int64_t>& chunk_ids, std::function<void(double)> progress_cb, int num_threads, bool ramp_up) {
    // Before CreateMultipartUpload, not after: a bad id should not cost a
    // create-and-abort round trip. The fan-outs check again, since they are
    // reachable on their own.
    if (!chunk_request_is_valid(chunk_ids, m_filesize,
                                chunk_count_for_size(m_filesize, m_chunk_size))) {
        return {};
    }

    Aws::String upload_id = CreateMultipartUpload();
    if (upload_id.empty()) return {};

    // Register upload for cleanup on early exit (e.g., signal, exception)
    UploadRegistry::instance().register_upload({m_s3client, m_bucket, m_dst_key, upload_id});
    UploadGuard guard(upload_id);  // On exception, leaves registered for abort_all()

    std::vector<uint32_t> crc32_results;
    if (num_threads > 0) {
        // Use thread pool with bounded parallelism
        crc32_results = ParallelUploadPartCopyRequestsThreadPool(upload_id, static_cast<size_t>(num_threads), ramp_up, chunk_ids, progress_cb);
    } else {
        // Use std::async for unbounded parallelism
        crc32_results = ParallelUploadPartCopyRequests(upload_id, chunk_ids, progress_cb);
    }
    if (AbortMultipartUpload(upload_id)) {
        guard.release();  // Only unregister if abort succeeded; otherwise abort_all() will retry
    }

    return crc32_results;
}
