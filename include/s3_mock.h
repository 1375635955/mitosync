#pragma once

#include "s3_interface.h"
#include "s3_utils.h"
#include "crc32_hw.h"
#include "crc32_chunks.h"

#include <aws/core/utils/HashingUtils.h>

#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <set>
#include <cassert>
#include <fstream>
#include <thread>
#include <chrono>
#include <functional>
#include <spdlog/spdlog.h>

// Type-safe enum for error injection methods
enum class S3MockMethod {
    GetObjectSize,
    CheckObjectPresence,
    GetObjectRange,
    GetChunkCRC32s,
    PutObject,
    DeleteObject,
    CopyObject,
    ListObjects
};

inline std::string S3MockMethodToString(S3MockMethod method) {
    switch (method) {
        case S3MockMethod::GetObjectSize: return "GetObjectSize";
        case S3MockMethod::CheckObjectPresence: return "CheckObjectPresence";
        case S3MockMethod::GetObjectRange: return "GetObjectRange";
        case S3MockMethod::GetChunkCRC32s: return "GetChunkCRC32s";
        case S3MockMethod::PutObject: return "PutObject";
        case S3MockMethod::DeleteObject: return "DeleteObject";
        case S3MockMethod::CopyObject: return "CopyObject";
        case S3MockMethod::ListObjects: return "ListObjects";
    }
    return "Unknown";
}

// Mock S3 client for unit testing
// Stores objects in memory and computes CRC32s locally
class MockS3Client : public IS3Client {
public:
    MockS3Client() = default;

    // Error injection: set a method to fail permanently for a specific bucket/key
    void SetFailure(const std::string& bucket, const std::string& key, S3MockMethod method) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failures[bucket + "/" + key + "/" + S3MockMethodToString(method)] = true;
    }

    // Error injection: set a method to fail N times, then succeed (for testing retry logic)
    // If retryable=true, the mock's internal retry logic will retry (simulating S3ClientImpl behavior)
    // If retryable=false, the failure is permanent (like ACCESS_DENIED)
    void SetTransientFailure(const std::string& bucket, const std::string& key, S3MockMethod method,
                             int fail_count, bool retryable = true) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/" + S3MockMethodToString(method);
        m_transient_failures[failure_key] = {fail_count, retryable};
    }

    void ClearFailure(const std::string& bucket, const std::string& key, S3MockMethod method) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/" + S3MockMethodToString(method);
        m_failures.erase(failure_key);
        m_transient_failures.erase(failure_key);
    }

    // Per-object LastModified, mirroring what S3 reports on a listing. Tests can
    // set it explicitly to model an object written before or after a local file.
    void SetObjectMtime(const std::string& bucket, const std::string& key, int64_t mtime) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mtimes[bucket + "/" + key] = mtime;
    }

    // Make DeleteObject remove the object and still report failure, which is
    // what S3 does under rate limiting and is the case the post-delete
    // verification exists to recover.
    void SetDeleteAppliesButReportsFailure(const std::string& bucket, const std::string& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_delete_applies_but_fails.insert(bucket + "/" + key);
    }


    void ClearAllFailures() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failures.clear();
        m_transient_failures.clear();
        m_call_counts.clear();
        m_delete_applies_but_fails.clear();
    }

    // Get how many times a method was called for a specific bucket/key
    int GetCallCount(const std::string& bucket, const std::string& key, S3MockMethod method) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string call_key = bucket + "/" + key + "/" + S3MockMethodToString(method);
        return m_call_counts[call_key];
    }

    // Simulate batch-level rate limiting: DeleteObjects returns all keys as failed
    // for the first N calls (simulating S3 503 SlowDown)
    void SetBatchRateLimit(int fail_count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_batch_rate_limit_remaining = fail_count;
    }

    // Get total DeleteObjects batch call count
    int GetDeleteObjectsBatchCallCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_delete_objects_batch_count;
    }

    // Get peak concurrent DeleteObjects calls (for testing concurrency limits)
    int GetPeakDeleteObjectsConcurrency() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_peak_delete_objects_concurrency;
    }

    // Get peak concurrent individual DeleteObject calls
    int GetPeakDeleteObjectConcurrency() {
        return m_peak_delete_object_concurrency.load();
    }

    // Simulate rate limiting for individual DeleteObject: first N calls fail
    // Fires at the start of every DeleteObject, before anything is removed. Lets a test act
    // at an exact point in a run - flipping a cancellation flag on the Nth delete, say -
    // instead of racing a separate thread against the delete loop and hoping it wins.
    void SetOnDeleteObject(std::function<void(const std::string&, const std::string&)> cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_on_delete_object = std::move(cb);
    }

    void SetDeleteObjectRateLimit(int fail_count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_delete_object_rate_limit_remaining = fail_count;
    }

    // Reset concurrency tracking
    void ResetConcurrencyTracking() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_delete_objects_concurrency = 0;
        m_peak_delete_objects_concurrency = 0;
        m_delete_objects_batch_count = 0;
        m_current_delete_object_concurrency.store(0);
        m_peak_delete_object_concurrency.store(0);
    }

    int64_t GetObjectSize(const std::string& bucket, const std::string& key) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/GetObjectSize";

        // Track call count
        m_call_counts[failure_key]++;

        // Check permanent failure
        if (m_failures.count(failure_key)) {
            return -1;
        }

        // Check transient failure - fail this call but decrement counter
        auto transient_it = m_transient_failures.find(failure_key);
        if (transient_it != m_transient_failures.end()) {
            auto& [remaining, retryable] = transient_it->second;
            if (remaining > 0) {
                --remaining;
                return -1;  // Caller should retry
            }
            // remaining == 0: transient failure period over, proceed normally
        }

        // Normal operation
        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) {
            return -1;
        }
        auto obj_it = bucket_it->second.find(key);
        if (obj_it == bucket_it->second.end()) {
            return -1;
        }
        return static_cast<int64_t>(obj_it->second.size());
    }

    S3ObjectPresence CheckObjectPresence(const std::string& bucket,
                                         const std::string& key) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Error injection, in priority order:
        //
        //  1. This method's own key, permanent then transient. Use it to model
        //     a service that cannot answer whether the key is there.
        //  2. Failing that, GetObjectSize's PERMANENT key, so a test that
        //     disables the size lookup outright gets an inconclusive presence
        //     check too - that is one service being unreachable, not two
        //     independent calls disagreeing.
        //
        // GetObjectSize's TRANSIENT key is not consulted, which has always
        // been true here and is worth keeping: a throttle that hits one
        // request need not hit the next, and it is the only way a test can
        // express "the object is there but its size lookup failed".
        std::string own_key = bucket + "/" + key + "/" +
                              S3MockMethodToString(S3MockMethod::CheckObjectPresence);
        ++m_call_counts[own_key];

        if (m_failures.count(own_key)) {
            return S3ObjectPresence::Unknown;
        }
        auto transient_it = m_transient_failures.find(own_key);
        if (transient_it != m_transient_failures.end() && transient_it->second.first > 0) {
            --transient_it->second.first;
            return S3ObjectPresence::Unknown;
        }
        std::string size_key = bucket + "/" + key + "/" +
                               S3MockMethodToString(S3MockMethod::GetObjectSize);
        if (m_failures.count(size_key)) {
            return S3ObjectPresence::Unknown;
        }
        auto bucket_it = m_buckets.find(bucket);
        // A missing bucket says nothing about the key - matching the real
        // client, which confirms the bucket before trusting a bare 404.
        if (bucket_it == m_buckets.end()) return S3ObjectPresence::Unknown;
        return bucket_it->second.count(key) ? S3ObjectPresence::Exists
                                            : S3ObjectPresence::NotFound;
    }

    std::vector<uint8_t> GetObjectRange(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/GetObjectRange";

        // Track call count
        m_call_counts[failure_key]++;

        // Check permanent failure
        if (m_failures.count(failure_key)) {
            return {};  // Simulate failure
        }

        // Check transient failure - fail this call but decrement counter
        auto transient_it = m_transient_failures.find(failure_key);
        if (transient_it != m_transient_failures.end()) {
            auto& [remaining, retryable] = transient_it->second;
            if (remaining > 0) {
                --remaining;
                return {};  // Caller should retry
            }
            // remaining == 0: transient failure period over, proceed normally
        }

        // Normal operation
        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) {
            return {};
        }
        auto obj_it = bucket_it->second.find(key);
        if (obj_it == bucket_it->second.end()) {
            return {};
        }

        const auto& data = obj_it->second;
        // Reject rather than clamp. This used to answer a past-the-end request
        // with whatever it could reach, which made the mock the only
        // implementation for which a short read was a success: the real adapter
        // now fails one, and every caller in the tree computes an exact
        // in-bounds range from a size it already knows (issue #23).
        if (start < 0 || end < start || end >= static_cast<int64_t>(data.size())) {
            return {};
        }

        return std::vector<uint8_t>(
            data.begin() + start,
            data.begin() + end + 1
        );
    }

    bool GetObjectRangeInto(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end,
        std::vector<uint8_t>& buffer
    ) override {
        // Reuse GetObjectRange, which enforces the range rules, and answer
        // exactly as it does.
        //
        // This used to return true whenever GetObjectRange came back empty for
        // a reversed range - the guard read `data.empty() && end >= start`, and
        // a reversed range fails that second clause - so `(100, 50)` was a
        // success here and a failure in the real adapter. It also left the
        // caller's previous bytes in the buffer on the failure path, while
        // IS3Client documents an empty buffer. Both mattered: this entry point
        // exists for buffer reuse, so stale bytes are the bytes of whatever was
        // read last (issue #23).
        auto data = GetObjectRange(bucket, key, start, end);
        if (data.empty()) {
            buffer.clear();
            return false;
        }
        buffer.resize(data.size());
        std::memcpy(buffer.data(), data.data(), data.size());
        return true;
    }

    S3ListResult ListObjects(
        const std::string& bucket,
        const std::string& prefix,
        const std::string& delimiter,
        const std::string& continuation_token,
        int max_keys
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        S3ListResult result;

        // Error injection. Keyed on the prefix, so a test can fail one subtree
        // and leave the rest listable - the partial-enumeration case.
        std::string failure_key = bucket + "/" + prefix + "/" +
                                  S3MockMethodToString(S3MockMethod::ListObjects);
        if (m_failures.count(failure_key)) {
            result.success = false;
            result.error_message = "Injected ListObjects failure";
            return result;
        }

        // Validate max_keys - S3 requires 1-1000, we allow any positive value
        if (max_keys <= 0) {
            result.error_message = "MaxKeys must be positive";
            return result;
        }

        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) {
            result.error_message = "Bucket not found";
            return result;
        }

        // Collect and sort keys for consistent pagination
        std::vector<std::string> sorted_keys;
        for (const auto& [key, data] : bucket_it->second) {
            sorted_keys.push_back(key);
        }
        std::sort(sorted_keys.begin(), sorted_keys.end());

        std::set<std::string> prefixes_seen;
        int count = 0;

        // Parse opaque continuation token (format: "mock_token:<key>")
        std::string start_after;
        const std::string token_prefix = "mock_token:";
        if (!continuation_token.empty()) {
            if (continuation_token.find(token_prefix) == 0) {
                start_after = continuation_token.substr(token_prefix.length());
            } else {
                // Invalid token format
                result.error_message = "Invalid continuation token";
                return result;
            }
        }
        bool past_token = start_after.empty();

        for (const auto& key : sorted_keys) {
            // Skip until we're past the continuation token
            if (!past_token) {
                if (key > start_after) {
                    past_token = true;
                } else {
                    continue;
                }
            }

            // Skip if doesn't match prefix
            if (!prefix.empty() && key.find(prefix) != 0) {
                continue;
            }

            // Handle delimiter (folder simulation)
            if (!delimiter.empty()) {
                size_t prefix_len = prefix.length();
                size_t delim_pos = key.find(delimiter, prefix_len);
                if (delim_pos != std::string::npos) {
                    std::string common_prefix = key.substr(0, delim_pos + delimiter.length());
                    if (prefixes_seen.find(common_prefix) == prefixes_seen.end()) {
                        result.common_prefixes.push_back(common_prefix);
                        prefixes_seen.insert(common_prefix);
                    }
                    continue;
                }
            }

            if (count >= max_keys) {
                result.is_truncated = true;
                result.next_continuation_token = token_prefix + result.objects.back().key;
                break;
            }

            S3ObjectInfo info;
            info.key = key;
            info.size = static_cast<int64_t>(bucket_it->second.at(key).size());
            info.last_modified = get_mtime_unlocked(bucket, key);
            result.objects.push_back(std::move(info));
            ++count;
        }

        result.success = true;
        return result;
    }

    bool PutObject(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/PutObject";

        // Track call count
        m_call_counts[failure_key]++;

        // Check permanent failure
        if (m_failures.count(failure_key)) {
            return false;
        }

        // Check transient failure - fail this call but decrement counter
        auto transient_it = m_transient_failures.find(failure_key);
        if (transient_it != m_transient_failures.end()) {
            auto& [remaining, retryable] = transient_it->second;
            if (remaining > 0) {
                --remaining;
                return false;  // Caller should retry
            }
            // remaining == 0: transient failure period over, proceed normally
        }

        // Normal operation
        if (m_buckets.find(bucket) == m_buckets.end()) {
            return false;  // Bucket doesn't exist
        }
        m_buckets[bucket][key] = data;
        m_mtimes[bucket + "/" + key] = now_seconds();
        return true;
    }

    bool PutObjectWithCRC32(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data,
        uint32_t crc32
    ) override {
        // Mock ignores CRC32 - just delegate to PutObject
        (void)crc32;
        return PutObject(bucket, key, data);
    }

    bool PutObjectFromFile(
        const std::string& bucket,
        const std::string& key,
        const std::string& file_path
    ) override {
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const std::streamoff size = file.tellg();
        if (size < 0) return false;

        // The real client refuses an oversized file before it reads a byte, so
        // the mock has to refuse it too: a test written against a mock that
        // accepts what S3 would reject proves nothing about the path it covers.
        // Holding the body in a map instead of streaming it is a difference
        // that does not matter here; what is a legal request is.
        if (static_cast<uint64_t>(size) > kMaxSinglePutBytes) {
            spdlog::error("{} is {} bytes, over the 5 GiB limit for a single PutObject. "
                          "Uploading a file this large needs multipart, which this path "
                          "does not do.", file_path, size);
            return false;
        }

        file.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);
        // PutObject handles failure injection for PutObject key
        return PutObject(bucket, key, data);
    }

    bool DeleteObject(const std::string& bucket, const std::string& key) override {
        // Copied out under the lock, then invoked without it: a callback that reaches back
        // into the mock would otherwise deadlock on m_mutex.
        {
            std::function<void(const std::string&, const std::string&)> on_delete;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                on_delete = m_on_delete_object;
            }
            if (on_delete) on_delete(bucket, key);
        }

        // Track concurrency atomically (outside lock for accurate measurement)
        int current = ++m_current_delete_object_concurrency;
        int peak = m_peak_delete_object_concurrency.load();
        while (current > peak && !m_peak_delete_object_concurrency.compare_exchange_weak(peak, current)) {
            // Keep trying to update peak if we're higher
        }

        struct ConcurrencyGuard {
            std::atomic<int>& counter;
            ~ConcurrencyGuard() { --counter; }
        } guard{m_current_delete_object_concurrency};

        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = bucket + "/" + key + "/DeleteObject";

        // Track call count
        m_call_counts[failure_key]++;

        if (m_delete_applies_but_fails.count(bucket + "/" + key)) {
            auto bit = m_buckets.find(bucket);
            if (bit != m_buckets.end()) bit->second.erase(key);
            m_mtimes.erase(bucket + "/" + key);
            return false;   // applied, but reported as a failure
        }

        // Check rate limiting
        if (m_delete_object_rate_limit_remaining > 0) {
            --m_delete_object_rate_limit_remaining;
            return false;
        }

        // Check permanent failure
        if (m_failures.count(failure_key)) {
            return false;
        }

        // Check transient failure - fail this call but decrement counter
        auto transient_it = m_transient_failures.find(failure_key);
        if (transient_it != m_transient_failures.end()) {
            auto& [remaining, retryable] = transient_it->second;
            if (remaining > 0) {
                --remaining;
                return false;  // Caller should retry
            }
            // remaining == 0: transient failure period over, proceed normally
        }

        // Normal operation
        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) {
            return false;
        }
        bucket_it->second.erase(key);
        return true;
    }

    bool CopyObject(
        const std::string& source_bucket,
        const std::string& source_key,
        const std::string& dest_bucket,
        const std::string& dest_key,
        int64_t /* source_size */ = -1,
        const std::atomic<bool>* cancelled = nullptr
    ) override {
        // Check cancellation before acquiring lock
        if (cancelled && cancelled->load()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        std::string failure_key = dest_bucket + "/" + dest_key + "/CopyObject";

        // Track call count
        m_call_counts[failure_key]++;

        // Check permanent failure
        if (m_failures.count(failure_key)) {
            return false;
        }

        // Check transient failure
        auto transient_it = m_transient_failures.find(failure_key);
        if (transient_it != m_transient_failures.end()) {
            auto& [remaining, retryable] = transient_it->second;
            if (remaining > 0) {
                --remaining;
                return false;
            }
        }

        // Check source bucket exists
        auto source_bucket_it = m_buckets.find(source_bucket);
        if (source_bucket_it == m_buckets.end()) {
            return false;
        }

        // Check source object exists
        auto source_obj_it = source_bucket_it->second.find(source_key);
        if (source_obj_it == source_bucket_it->second.end()) {
            return false;
        }

        // Check dest bucket exists
        auto dest_bucket_it = m_buckets.find(dest_bucket);
        if (dest_bucket_it == m_buckets.end()) {
            return false;
        }

        // Copy the object
        dest_bucket_it->second[dest_key] = source_obj_it->second;
        m_mtimes[dest_bucket + "/" + dest_key] = now_seconds();
        return true;
    }

    std::vector<std::string> DeleteObjects(
        const std::string& bucket,
        const std::vector<std::string>& keys
    ) override {
        // Track concurrency (enter)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_current_delete_objects_concurrency;
            if (m_current_delete_objects_concurrency > m_peak_delete_objects_concurrency) {
                m_peak_delete_objects_concurrency = m_current_delete_objects_concurrency;
            }
            ++m_delete_objects_batch_count;
        }

        // RAII guard to decrement concurrency on exit
        struct ConcurrencyGuard {
            MockS3Client& client;
            ~ConcurrencyGuard() {
                std::lock_guard<std::mutex> lock(client.m_mutex);
                --client.m_current_delete_objects_concurrency;
            }
        } guard{*this};

        // Simulate some work (helps test concurrency)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> failed;

        // Check batch-level rate limiting (simulates 503 SlowDown)
        if (m_batch_rate_limit_remaining > 0) {
            --m_batch_rate_limit_remaining;
            return keys;  // All keys fail
        }

        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) {
            // Bucket doesn't exist - all keys fail
            return keys;
        }

        for (const auto& key : keys) {
            if (m_delete_applies_but_fails.count(bucket + "/" + key)) {
                auto bit = m_buckets.find(bucket);
                if (bit != m_buckets.end()) bit->second.erase(key);
                m_mtimes.erase(bucket + "/" + key);
                failed.push_back(key);   // applied, but reported as a failure
                continue;
            }
            std::string failure_key = bucket + "/" + key + "/DeleteObject";

            // Check permanent failure
            if (m_failures.count(failure_key)) {
                failed.push_back(key);
                continue;
            }

            // Check transient failure - fail this call but decrement counter
            auto transient_it = m_transient_failures.find(failure_key);
            if (transient_it != m_transient_failures.end()) {
                auto& [remaining, retryable] = transient_it->second;
                if (remaining > 0) {
                    --remaining;
                    failed.push_back(key);
                    continue;
                }
                // remaining == 0: transient failure period over, proceed normally
            }

            bucket_it->second.erase(key);
        }
        return failed;
    }

    bool CreateBucket(const std::string& bucket) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buckets[bucket];  // Creates empty bucket if not exists
        return true;
    }

    // Multipart upload state for testing
    struct MultipartUploadState {
        std::string bucket;
        std::string key;
        std::chrono::system_clock::time_point initiated;
        std::unordered_map<int, std::vector<uint8_t>> parts;  // part_number -> data
    };

    std::vector<uint32_t> GetChunkCRC32s(
        const std::string& bucket,
        const std::string& key,
        int64_t file_size,
        const std::vector<int64_t>& chunk_ids,
        std::function<void(double)> progress_cb,
        bool /*debug*/,
        int /*num_threads*/,
        bool /*ramp_up*/,
        int64_t chunk_size
    ) override {
        // Copy data under lock, then compute CRCs without holding the lock
        std::vector<uint8_t> data_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_failures.count(bucket + "/" + key + "/GetChunkCRC32s")) {
                return {};  // Simulate failure
            }

            auto bucket_it = m_buckets.find(bucket);
            if (bucket_it == m_buckets.end()) {
                return {};
            }
            auto obj_it = bucket_it->second.find(key);
            if (obj_it == bucket_it->second.end()) {
                return {};
            }

            data_copy = obj_it->second;
        }

        // Use the actual copied data size for all calculations (thread-safe)
        int64_t actual_size = static_cast<int64_t>(data_copy.size());

        // Validate file_size matches actual data size - mismatch indicates a caller bug
        // Real S3 would use the provided file_size for chunk calculations, so mismatches
        // would cause incorrect results. Fail early to catch these bugs in tests.
        if (file_size != actual_size) {
            spdlog::error("MockS3Client::GetChunkCRC32s: file_size ({}) != actual size ({})",
                          file_size, actual_size);
            return {};
        }

        // Use default if not specified
        if (chunk_size <= 0) {
            chunk_size = DEFAULT_CHUNK_SIZE;
        }

        int64_t num_chunks = (actual_size + chunk_size - 1) / chunk_size;

        // Determine which chunks to process
        std::vector<int64_t> ids_to_process;
        if (chunk_ids.empty()) {
            ids_to_process.resize(num_chunks);
            std::iota(ids_to_process.begin(), ids_to_process.end(), 0);
        } else {
            // Validate chunk_ids - reject negative values
            for (int64_t id : chunk_ids) {
                if (id < 0) {
                    spdlog::error("MockS3Client::GetChunkCRC32s: invalid negative chunk_id {}", id);
                    return {};
                }
            }
            ids_to_process = chunk_ids;
        }

        // Validate all chunk IDs are within bounds before processing
        // This matches real S3 behavior where out-of-bounds requests fail
        // Compare against the chunk count, not chunk_id * chunk_size: that
        // product overflows int64_t for a large id, wraps negative, passes a
        // ">= actual_size" test and then indexes the buffer at a negative
        // offset. Comparing the ids directly cannot overflow.
        for (int64_t chunk_id : ids_to_process) {
            if (chunk_id >= num_chunks) {
                spdlog::error("MockS3Client::GetChunkCRC32s: chunk_id {} is out of bounds (file has {} chunks)",
                              chunk_id, num_chunks);
                return {};
            }
        }

        std::vector<uint32_t> results;
        results.reserve(ids_to_process.size());

        size_t processed = 0;
        for (int64_t chunk_id : ids_to_process) {
            int64_t start = chunk_id * chunk_size;
            int64_t end = std::min(start + chunk_size, actual_size);
            int64_t len = end - start;

            uint32_t crc = crc32_hw(data_copy.data() + start, len);
            results.push_back(crc);

            ++processed;
            if (progress_cb) {
                progress_cb(100.0 * processed / ids_to_process.size());
            }
        }

        return results;
    }

    std::string CreateMultipartUpload(
        const std::string& bucket,
        const std::string& key
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_buckets.find(bucket) == m_buckets.end()) {
            return "";  // Bucket doesn't exist
        }
        // Generate a mock upload ID
        std::string upload_id = "mock-upload-" + std::to_string(m_next_upload_id++);
        MultipartUploadState state;
        state.bucket = bucket;
        state.key = key;
        state.initiated = std::chrono::system_clock::now();
        m_multipart_uploads[upload_id] = state;
        return upload_id;
    }

    // Test helper: set initiated time for an upload
    void SetUploadInitiatedTime(const std::string& upload_id,
                                 std::chrono::system_clock::time_point time) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_multipart_uploads.find(upload_id);
        if (it != m_multipart_uploads.end()) {
            it->second.initiated = time;
        }
    }

    S3PartResult UploadPart(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::vector<uint8_t>& data,
        uint32_t /*crc32*/
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_multipart_uploads.find(upload_id);
        if (it == m_multipart_uploads.end()) {
            return {};  // Invalid upload ID
        }
        if (it->second.bucket != bucket || it->second.key != key) {
            return {};  // Bucket/key mismatch
        }
        it->second.parts[part_number] = data;
        // Report the checksum the way a real endpoint does - computed from what
        // was stored, not echoed from the caller - so a caller that drops it on
        // the way to CompleteMultipartUpload is caught here rather than in
        // production.
        return S3PartResult{"\"mock-etag-part-" + std::to_string(part_number) + "\"",
                            base64_crc32(data)};
    }

    S3PartResult UploadPartCopy(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::string& source_bucket,
        const std::string& source_key,
        int64_t start_byte,
        int64_t end_byte
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_multipart_uploads.find(upload_id);
        if (it == m_multipart_uploads.end()) {
            return {};  // Invalid upload ID
        }
        if (it->second.bucket != bucket || it->second.key != key) {
            return {};  // Bucket/key mismatch
        }
        // Get source object data
        auto src_bucket_it = m_buckets.find(source_bucket);
        if (src_bucket_it == m_buckets.end()) {
            return {};
        }
        auto src_obj_it = src_bucket_it->second.find(source_key);
        if (src_obj_it == src_bucket_it->second.end()) {
            return {};
        }
        const auto& src_data = src_obj_it->second;
        if (start_byte < 0 || end_byte >= static_cast<int64_t>(src_data.size()) || start_byte > end_byte) {
            return {};  // Invalid range
        }
        // Copy the range
        std::vector<uint8_t> part_data(src_data.begin() + start_byte, src_data.begin() + end_byte + 1);
        it->second.parts[part_number] = part_data;
        return S3PartResult{"\"mock-etag-copy-part-" + std::to_string(part_number) + "\"",
                            base64_crc32(part_data)};
    }

    bool CompleteMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        const std::vector<std::pair<int, S3PartResult>>& parts
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_multipart_uploads.find(upload_id);
        if (it == m_multipart_uploads.end()) {
            return false;  // Invalid upload ID
        }
        if (it->second.bucket != bucket || it->second.key != key) {
            return false;  // Bucket/key mismatch
        }
        // Assemble the complete object from parts (in order)
        std::vector<uint8_t> complete_data;
        for (const auto& [part_num, result] : parts) {
            auto part_it = it->second.parts.find(part_num);
            if (part_it == it->second.parts.end()) {
                return false;  // Missing part
            }
            // Every upload this mock serves was created with a checksum
            // algorithm, matching S3ClientImpl. A real endpoint rejects such an
            // upload if the completion request does not name each part's
            // checksum, so refuse it here too - otherwise the mock is the one
            // place this bug cannot be seen (issue #98).
            const std::string expected = base64_crc32(part_it->second);
            if (result.checksum_crc32.empty()) {
                spdlog::error("CompleteMultipartUpload: part {} has no checksum, but the "
                              "upload declared a checksum algorithm", part_num);
                return false;
            }
            if (result.checksum_crc32 != expected) {
                spdlog::error("CompleteMultipartUpload: part {} checksum {} does not match "
                              "the stored part ({})", part_num, result.checksum_crc32, expected);
                return false;
            }
            complete_data.insert(complete_data.end(),
                                part_it->second.begin(),
                                part_it->second.end());
        }
        // Store the assembled object
        m_buckets[bucket][key] = complete_data;
        m_mtimes[bucket + "/" + key] = now_seconds();
        // Clean up upload state
        m_multipart_uploads.erase(it);
        return true;
    }

    bool AbortMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        (void)bucket;
        (void)key;
        auto it = m_multipart_uploads.find(upload_id);
        if (it != m_multipart_uploads.end()) {
            m_multipart_uploads.erase(it);
        }
        return true;  // S3 doesn't fail even if upload doesn't exist
    }

    S3ListMultipartUploadsResult ListMultipartUploads(
        const std::string& bucket,
        const std::string& prefix = "",
        const std::string& key_marker = "",
        const std::string& upload_id_marker = "",
        int max_uploads = 1000
    ) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        S3ListMultipartUploadsResult result;

        if (m_buckets.find(bucket) == m_buckets.end()) {
            result.error_message = "Bucket not found: " + bucket;
            return result;
        }

        // Collect uploads matching prefix
        std::vector<S3MultipartUploadInfo> matching;
        for (const auto& [upload_id, state] : m_multipart_uploads) {
            if (state.bucket != bucket) continue;
            if (!prefix.empty() && state.key.rfind(prefix, 0) != 0) continue;

            // Apply key_marker/upload_id_marker pagination
            if (!key_marker.empty()) {
                if (state.key < key_marker) continue;
                if (state.key == key_marker && !upload_id_marker.empty()) {
                    if (upload_id <= upload_id_marker) continue;
                }
            }

            S3MultipartUploadInfo info;
            info.key = state.key;
            info.upload_id = upload_id;
            info.initiated = state.initiated;
            matching.push_back(info);
        }

        // Sort by key, then upload_id
        std::sort(matching.begin(), matching.end(), [](const auto& a, const auto& b) {
            if (a.key != b.key) return a.key < b.key;
            return a.upload_id < b.upload_id;
        });

        // Apply max_uploads limit
        if (static_cast<int>(matching.size()) > max_uploads) {
            result.is_truncated = true;
            matching.resize(max_uploads);
            result.next_key_marker = matching.back().key;
            result.next_upload_id_marker = matching.back().upload_id;
        }

        result.uploads = std::move(matching);
        result.success = true;
        return result;
    }

    // A part checksum in the form S3 reports it: the 4-byte big-endian CRC32,
    // base64-encoded. Matches Crc32ToBase64 in the real client. Public so a
    // test fake that overrides the multipart calls can report checksums the
    // same way this mock does.
    static std::string base64_crc32(const std::vector<uint8_t>& data) {
        return base64_crc32_impl(data);
    }

    // Test helper: clear all data
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buckets.clear();
    }

    // Test helper: check if object exists
    bool ObjectExists(const std::string& bucket, const std::string& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto bucket_it = m_buckets.find(bucket);
        if (bucket_it == m_buckets.end()) return false;
        return bucket_it->second.find(key) != bucket_it->second.end();
    }

private:
    static int64_t now_seconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string base64_crc32_impl(const std::vector<uint8_t>& data) {
        const uint32_t crc = crc32_hw(data.data(), data.size());
        const uint8_t bytes[4] = {
            static_cast<uint8_t>((crc >> 24) & 0xFF), static_cast<uint8_t>((crc >> 16) & 0xFF),
            static_cast<uint8_t>((crc >> 8) & 0xFF),  static_cast<uint8_t>(crc & 0xFF)};
        Aws::Utils::ByteBuffer buffer(bytes, 4);
        const Aws::String encoded = Aws::Utils::HashingUtils::Base64Encode(buffer);
        return std::string(encoded.c_str(), encoded.size());
    }

    // Caller must hold m_mutex.
    int64_t get_mtime_unlocked(const std::string& bucket, const std::string& key) const {
        auto it = m_mtimes.find(bucket + "/" + key);
        return it == m_mtimes.end() ? 0 : it->second;
    }
    mutable std::unordered_map<std::string, int64_t> m_mtimes;
    std::set<std::string> m_delete_applies_but_fails;


    mutable std::mutex m_mutex;
    // bucket -> (key -> data)
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<uint8_t>>> m_buckets;
    // "bucket/key/method" -> true for permanent injected failures
    std::unordered_map<std::string, bool> m_failures;
    // "bucket/key/method" -> {remaining_failures, is_retryable} for transient failures
    std::unordered_map<std::string, std::pair<int, bool>> m_transient_failures;
    // "bucket/key/method" -> call count for tracking retries
    std::unordered_map<std::string, int> m_call_counts;
    // Multipart upload state: upload_id -> state
    std::unordered_map<std::string, MultipartUploadState> m_multipart_uploads;
    int m_next_upload_id = 1;

    // Batch rate limiting: number of DeleteObjects calls that should fail entirely
    int m_batch_rate_limit_remaining = 0;
    // Concurrency tracking for DeleteObjects (batch)
    int m_current_delete_objects_concurrency = 0;
    int m_peak_delete_objects_concurrency = 0;
    int m_delete_objects_batch_count = 0;
    // Rate limiting for individual DeleteObject calls
    int m_delete_object_rate_limit_remaining = 0;
    std::function<void(const std::string&, const std::string&)> m_on_delete_object;
    // Concurrency tracking for individual DeleteObject calls (atomic for thread safety)
    std::atomic<int> m_current_delete_object_concurrency{0};
    std::atomic<int> m_peak_delete_object_concurrency{0};
};
