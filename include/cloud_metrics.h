#pragma once

// Cloud metrics tracking for AWS S3 API calls
// Uses AWS SDK's MonitoringInterface to collect statistics

#include <atomic>
#include <array>
#include <deque>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <climits>

// S3 operation types we track
enum class S3OperationType {
    HeadObject = 0,
    GetObject,
    PutObject,
    DeleteObject,
    DeleteObjects,           // Batch delete (POST, charged as PUT-class)
    ListObjectsV2,
    ListBuckets,
    GetBucketLocation,
    CreateMultipartUpload,
    UploadPart,              // Regular part upload (data uploaded from client)
    UploadPartCopy,          // Same-bucket server-side copy (free, no data transfer)
    UploadPartCopyRemote,    // Cross-bucket server-side copy (may have transfer costs)
    CopyObject,              // Same-bucket server-side copy (free, no data transfer)
    CopyObjectRemote,        // Cross-bucket server-side copy (may have transfer costs)
    AbortMultipartUpload,
    CompleteMultipartUpload,
    ListParts,               // List parts of a multipart upload
    ListMultipartUploads,    // List active multipart uploads
    Unknown,
    COUNT  // Must be last - used for array sizing
};

// Note on UploadPartCopy/CopyObject vs their Remote variants:
// The monitoring layer (S3MonitoringListener) distinguishes between same-bucket
// and cross-bucket copies by comparing the source bucket (from x-amz-copy-source
// header) with the destination bucket (from Host header or request path).
//
// Same-bucket copies (UploadPartCopy, CopyObject):
//   - Data stays within the same bucket
//   - No data transfer charges
//   - Charged only at PUT request pricing
//
// Cross-bucket copies (UploadPartCopyRemote, CopyObjectRemote):
//   - Data transferred between buckets (possibly cross-region)
//   - May incur cross-region data transfer charges
//   - Charged at PUT request pricing plus potential transfer costs

// Convert operation type to string
const char* S3OperationTypeName(S3OperationType op);

// Parse operation name from AWS SDK request name
S3OperationType ParseS3OperationType(const std::string& requestName);

// Per-operation metrics (thread-safe via atomics)
// Thread safety notes:
// - All individual field operations are atomic and thread-safe
// - Copy constructor/assignment read each field with acquire semantics
//   to ensure visibility of writes from other threads
// - The resulting snapshot may not be perfectly consistent across all fields
//   if writes occur during the copy, but each field value is valid and
//   reflects a real point in time. This is acceptable for UI display.
struct OperationMetrics {
    std::atomic<uint64_t> call_count{0};
    std::atomic<uint64_t> success_count{0};
    std::atomic<uint64_t> failure_count{0};
    std::atomic<uint64_t> retry_count{0};
    std::atomic<uint64_t> bytes_uploaded{0};
    std::atomic<uint64_t> bytes_downloaded{0};
    std::atomic<uint64_t> bytes_server_side{0};  // Server-side transfers (no egress)

    // Latency tracking (in milliseconds)
    std::atomic<int64_t> total_latency_ms{0};
    std::atomic<int64_t> min_latency_ms{INT64_MAX};
    std::atomic<int64_t> max_latency_ms{0};

    // Default constructor
    OperationMetrics() = default;

    // Copy constructor (for snapshots - see note above about consistency)
    OperationMetrics(const OperationMetrics& other)
        : call_count(other.call_count.load(std::memory_order_acquire))
        , success_count(other.success_count.load(std::memory_order_acquire))
        , failure_count(other.failure_count.load(std::memory_order_acquire))
        , retry_count(other.retry_count.load(std::memory_order_acquire))
        , bytes_uploaded(other.bytes_uploaded.load(std::memory_order_acquire))
        , bytes_downloaded(other.bytes_downloaded.load(std::memory_order_acquire))
        , bytes_server_side(other.bytes_server_side.load(std::memory_order_acquire))
        , total_latency_ms(other.total_latency_ms.load(std::memory_order_acquire))
        , min_latency_ms(other.min_latency_ms.load(std::memory_order_acquire))
        , max_latency_ms(other.max_latency_ms.load(std::memory_order_acquire))
    {}

    // Copy assignment (see note above about consistency)
    OperationMetrics& operator=(const OperationMetrics& other) {
        if (this != &other) {
            call_count.store(other.call_count.load(std::memory_order_acquire), std::memory_order_release);
            success_count.store(other.success_count.load(std::memory_order_acquire), std::memory_order_release);
            failure_count.store(other.failure_count.load(std::memory_order_acquire), std::memory_order_release);
            retry_count.store(other.retry_count.load(std::memory_order_acquire), std::memory_order_release);
            bytes_uploaded.store(other.bytes_uploaded.load(std::memory_order_acquire), std::memory_order_release);
            bytes_downloaded.store(other.bytes_downloaded.load(std::memory_order_acquire), std::memory_order_release);
            bytes_server_side.store(other.bytes_server_side.load(std::memory_order_acquire), std::memory_order_release);
            total_latency_ms.store(other.total_latency_ms.load(std::memory_order_acquire), std::memory_order_release);
            min_latency_ms.store(other.min_latency_ms.load(std::memory_order_acquire), std::memory_order_release);
            max_latency_ms.store(other.max_latency_ms.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    // Calculate average latency
    double avgLatencyMs() const {
        uint64_t count = success_count.load() + failure_count.load();
        if (count == 0) return 0.0;
        return static_cast<double>(total_latency_ms.load()) / static_cast<double>(count);
    }

    // Reset all counters
    void reset() {
        call_count.store(0);
        success_count.store(0);
        failure_count.store(0);
        retry_count.store(0);
        bytes_uploaded.store(0);
        bytes_downloaded.store(0);
        bytes_server_side.store(0);
        total_latency_ms.store(0);
        min_latency_ms.store(INT64_MAX);
        max_latency_ms.store(0);
    }
};

// Thread-safe metrics collector (singleton)
class CloudMetrics {
public:
    static constexpr size_t HISTORY_SIZE = 120;  // 1 minute at 500ms intervals
    static constexpr double SAMPLE_INTERVAL_MS = 500.0;

    // History sample for graphing
    struct Sample {
        uint64_t total_calls{0};
        uint64_t total_bytes{0};
        uint64_t delta_calls{0};     // Calls since last sample
        uint64_t delta_bytes{0};     // Bytes since last sample
    };

    // Get singleton instance
    static CloudMetrics& instance();

    // Record a request start (increments call_count)
    void recordStart(S3OperationType op);

    // Record a successful request
    // bytes_server_side: data transferred within S3 (server-side copies, no egress)
    void recordSuccess(S3OperationType op, int64_t latency_ms,
                       uint64_t bytes_up, uint64_t bytes_down,
                       uint64_t bytes_server_side = 0);

    // Record a failed request
    void recordFailure(S3OperationType op, int64_t latency_ms);

    // Record a retry
    void recordRetry(S3OperationType op);

    // Add server-side bytes to an operation type (for when monitoring layer can't determine size)
    // Use this after CopyObject succeeds to record the actual bytes copied
    void addServerSideBytes(S3OperationType op, uint64_t bytes);

    // Get metrics for a specific operation type
    const OperationMetrics& getMetrics(S3OperationType op) const;

    // Get snapshot of all metrics
    std::map<S3OperationType, OperationMetrics> getAllMetrics() const;

    // Get total metrics across all operation types
    OperationMetrics getTotalMetrics() const;

    // Clear all metrics in memory. Does NOT write to disk: call save() after it
    // if the reset is meant to outlive the process. Persisting from here made
    // every test that reset the singleton overwrite the user's real metrics
    // file (issue #41).
    void clear();

    // Persistence - save/load cumulative metrics to disk
    // File: <app_data_dir>/cloud_metrics.json, or the directory given to
    // setTestDataDirectory() below.
    bool save() const;
    bool load();

    // Sample current state for history (call periodically from GUI)
    void sample();

    // Get history for graphing (returns a copy for thread safety)
    std::deque<Sample> getHistory() const;
    size_t historySize() const;

    // Force add a sample bypassing rate limiting (for testing only)
    void forceSample();

    // Get the current sample (most recent)
    Sample currentSample() const;

    // Set/add region (for cost estimation)
    // Multiple regions can be tracked if comparing across regions
    void setRegion(const std::string& region);
    void addRegion(const std::string& region);
    std::string getRegion() const;  // Returns primary region
    std::vector<std::string> getRegions() const;  // Returns all regions

    // Check if we're operating across different regions
    // Returns true if multiple distinct regions are being tracked
    bool isCrossRegion() const;

    // Get total bytes from cross-bucket server-side copies
    // These incur egress charges when crossing regions
    uint64_t getCrossBucketServerSideBytes() const;

    // Testing support: override the data directory for tests
    // Pass empty string to reset to default
    static void setTestDataDirectory(const std::string& path);

    // The override currently in force, empty when persistence resolves to the
    // real app data directory. Exists so a test scope can restore what it
    // found rather than resetting to production, which would aim the singleton
    // at the user's file while an outer scope still expects a scratch one.
    static std::string testDataDirectory();

private:
    static std::string test_data_directory_;  // For testing only
    CloudMetrics() = default;
    ~CloudMetrics() = default;

    // Prevent copying
    CloudMetrics(const CloudMetrics&) = delete;
    CloudMetrics& operator=(const CloudMetrics&) = delete;

    // Metrics array indexed by S3OperationType
    std::array<OperationMetrics, static_cast<size_t>(S3OperationType::COUNT)> metrics_;

    // History for graphing
    std::deque<Sample> history_;
    mutable std::mutex history_mutex_;
    std::chrono::steady_clock::time_point last_sample_time_;

    // Previous totals for delta calculation
    uint64_t prev_total_calls_{0};
    uint64_t prev_total_bytes_{0};

    // Regions used for cost estimation (first is primary)
    std::vector<std::string> regions_;
    mutable std::mutex region_mutex_;

    // Auto-save state (atomic for thread-safety across sample/clear/load)
    mutable std::atomic<uint64_t> last_saved_calls_{0};
    static constexpr uint64_t AUTO_SAVE_THRESHOLD = 100;  // Save after N new calls

    // Update min/max latency atomically
    void updateMinLatency(S3OperationType op, int64_t latency_ms);
    void updateMaxLatency(S3OperationType op, int64_t latency_ms);

    // Get the metrics file path (uses test directory if set)
    static std::string getMetricsFilePath();
};
