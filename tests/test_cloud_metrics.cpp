#include <gtest/gtest.h>
#include "cloud_metrics.h"
#include "cloud_metrics_test_dir.h"
#include "app_settings.h"
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <iterator>
#include <cstdio>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#define getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// ============================================================================
// S3OperationType Tests
// ============================================================================

TEST(S3OperationTypeTest, OperationTypeNames) {
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::HeadObject), "HeadObject");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::GetObject), "GetObject");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::PutObject), "PutObject");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::DeleteObject), "DeleteObject");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::DeleteObjects), "DeleteObjects");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::ListObjectsV2), "ListObjectsV2");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::ListBuckets), "ListBuckets");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::GetBucketLocation), "GetBucketLocation");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::CreateMultipartUpload), "CreateMultipartUpload");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::UploadPart), "UploadPart");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::UploadPartCopy), "UploadPartCopy");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::UploadPartCopyRemote), "UploadPartCopy (remote)");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::CopyObject), "CopyObject");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::CopyObjectRemote), "CopyObject (remote)");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::AbortMultipartUpload), "AbortMultipartUpload");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::CompleteMultipartUpload), "CompleteMultipartUpload");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::ListMultipartUploads), "ListMultipartUploads");
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::Unknown), "Unknown");
}

TEST(S3OperationTypeTest, ParseOperationType) {
    EXPECT_EQ(ParseS3OperationType("HeadObject"), S3OperationType::HeadObject);
    EXPECT_EQ(ParseS3OperationType("GetObject"), S3OperationType::GetObject);
    EXPECT_EQ(ParseS3OperationType("PutObject"), S3OperationType::PutObject);
    EXPECT_EQ(ParseS3OperationType("DeleteObject"), S3OperationType::DeleteObject);
    EXPECT_EQ(ParseS3OperationType("DeleteObjects"), S3OperationType::DeleteObjects);
    EXPECT_EQ(ParseS3OperationType("ListObjectsV2"), S3OperationType::ListObjectsV2);
    EXPECT_EQ(ParseS3OperationType("ListBuckets"), S3OperationType::ListBuckets);
    EXPECT_EQ(ParseS3OperationType("GetBucketLocation"), S3OperationType::GetBucketLocation);
    EXPECT_EQ(ParseS3OperationType("CreateMultipartUpload"), S3OperationType::CreateMultipartUpload);
    EXPECT_EQ(ParseS3OperationType("UploadPart"), S3OperationType::UploadPart);
    EXPECT_EQ(ParseS3OperationType("UploadPartCopy"), S3OperationType::UploadPartCopy);
    EXPECT_EQ(ParseS3OperationType("CopyObject"), S3OperationType::CopyObject);
    EXPECT_EQ(ParseS3OperationType("AbortMultipartUpload"), S3OperationType::AbortMultipartUpload);
    EXPECT_EQ(ParseS3OperationType("CompleteMultipartUpload"), S3OperationType::CompleteMultipartUpload);
    EXPECT_EQ(ParseS3OperationType("ListMultipartUploads"), S3OperationType::ListMultipartUploads);
    EXPECT_EQ(ParseS3OperationType("UnknownOperation"), S3OperationType::Unknown);
    EXPECT_EQ(ParseS3OperationType(""), S3OperationType::Unknown);
}

TEST(S3OperationTypeTest, ParseOperationTypeWithRequestSuffix) {
    // AWS SDK may include "Request" suffix in some versions
    EXPECT_EQ(ParseS3OperationType("HeadObjectRequest"), S3OperationType::HeadObject);
    EXPECT_EQ(ParseS3OperationType("GetObjectRequest"), S3OperationType::GetObject);
    EXPECT_EQ(ParseS3OperationType("PutObjectRequest"), S3OperationType::PutObject);
    EXPECT_EQ(ParseS3OperationType("DeleteObjectRequest"), S3OperationType::DeleteObject);
    EXPECT_EQ(ParseS3OperationType("DeleteObjectsRequest"), S3OperationType::DeleteObjects);
    EXPECT_EQ(ParseS3OperationType("ListObjectsV2Request"), S3OperationType::ListObjectsV2);
    EXPECT_EQ(ParseS3OperationType("ListBucketsRequest"), S3OperationType::ListBuckets);
    EXPECT_EQ(ParseS3OperationType("GetBucketLocationRequest"), S3OperationType::GetBucketLocation);
    EXPECT_EQ(ParseS3OperationType("CreateMultipartUploadRequest"), S3OperationType::CreateMultipartUpload);
    EXPECT_EQ(ParseS3OperationType("UploadPartRequest"), S3OperationType::UploadPart);
    EXPECT_EQ(ParseS3OperationType("UploadPartCopyRequest"), S3OperationType::UploadPartCopy);
    EXPECT_EQ(ParseS3OperationType("CopyObjectRequest"), S3OperationType::CopyObject);
    EXPECT_EQ(ParseS3OperationType("AbortMultipartUploadRequest"), S3OperationType::AbortMultipartUpload);
    EXPECT_EQ(ParseS3OperationType("CompleteMultipartUploadRequest"), S3OperationType::CompleteMultipartUpload);
}

TEST(S3OperationTypeTest, ParseOperationTypeListObjectsLegacy) {
    // Old ListObjects API (deprecated) should map to ListObjectsV2
    EXPECT_EQ(ParseS3OperationType("ListObjects"), S3OperationType::ListObjectsV2);
    EXPECT_EQ(ParseS3OperationType("ListObjectsRequest"), S3OperationType::ListObjectsV2);
}

// ============================================================================
// OperationMetrics Tests
// ============================================================================

TEST(OperationMetricsTest, DefaultState) {
    OperationMetrics metrics;
    EXPECT_EQ(metrics.call_count.load(), 0u);
    EXPECT_EQ(metrics.success_count.load(), 0u);
    EXPECT_EQ(metrics.failure_count.load(), 0u);
    EXPECT_EQ(metrics.retry_count.load(), 0u);
    EXPECT_EQ(metrics.bytes_uploaded.load(), 0u);
    EXPECT_EQ(metrics.bytes_downloaded.load(), 0u);
    EXPECT_EQ(metrics.total_latency_ms.load(), 0);
    EXPECT_EQ(metrics.min_latency_ms.load(), INT64_MAX);
    EXPECT_EQ(metrics.max_latency_ms.load(), 0);
}

TEST(OperationMetricsTest, AvgLatency) {
    OperationMetrics metrics;

    // No samples - should return 0
    EXPECT_DOUBLE_EQ(metrics.avgLatencyMs(), 0.0);

    // Add some samples
    metrics.success_count.store(3);
    metrics.failure_count.store(2);
    metrics.total_latency_ms.store(500);

    // 500ms / 5 calls = 100ms average
    EXPECT_DOUBLE_EQ(metrics.avgLatencyMs(), 100.0);
}

TEST(OperationMetricsTest, Reset) {
    OperationMetrics metrics;

    // Set some values
    metrics.call_count.store(10);
    metrics.success_count.store(8);
    metrics.failure_count.store(2);
    metrics.retry_count.store(3);
    metrics.bytes_uploaded.store(1024);
    metrics.bytes_downloaded.store(2048);
    metrics.total_latency_ms.store(500);
    metrics.min_latency_ms.store(10);
    metrics.max_latency_ms.store(200);

    metrics.reset();

    EXPECT_EQ(metrics.call_count.load(), 0u);
    EXPECT_EQ(metrics.success_count.load(), 0u);
    EXPECT_EQ(metrics.failure_count.load(), 0u);
    EXPECT_EQ(metrics.retry_count.load(), 0u);
    EXPECT_EQ(metrics.bytes_uploaded.load(), 0u);
    EXPECT_EQ(metrics.bytes_downloaded.load(), 0u);
    EXPECT_EQ(metrics.total_latency_ms.load(), 0);
    EXPECT_EQ(metrics.min_latency_ms.load(), INT64_MAX);
    EXPECT_EQ(metrics.max_latency_ms.load(), 0);
}

TEST(OperationMetricsTest, CopyConstructor) {
    OperationMetrics source;
    source.call_count.store(10);
    source.success_count.store(8);
    source.bytes_downloaded.store(2048);
    source.min_latency_ms.store(50);
    source.max_latency_ms.store(150);

    OperationMetrics copy(source);

    EXPECT_EQ(copy.call_count.load(), 10u);
    EXPECT_EQ(copy.success_count.load(), 8u);
    EXPECT_EQ(copy.bytes_downloaded.load(), 2048u);
    EXPECT_EQ(copy.min_latency_ms.load(), 50);
    EXPECT_EQ(copy.max_latency_ms.load(), 150);
}

TEST(OperationMetricsTest, CopyAssignment) {
    OperationMetrics source;
    source.call_count.store(10);
    source.success_count.store(8);

    OperationMetrics dest;
    dest.call_count.store(5);  // Different value

    dest = source;

    EXPECT_EQ(dest.call_count.load(), 10u);
    EXPECT_EQ(dest.success_count.load(), 8u);
}

// ============================================================================
// CloudMetrics Tests
// ============================================================================

class CloudMetricsTest : public ::testing::Test {
protected:
    // Redirects persistence at a scratch directory for the whole test, so
    // nothing here can reach the user's real metrics file (issue #41).
    ScopedCloudMetricsDir metrics_dir_;

    void SetUp() override {
        CloudMetrics::instance().clear();
    }

    void TearDown() override {
        CloudMetrics::instance().clear();
    }
};

TEST_F(CloudMetricsTest, Singleton) {
    auto& instance1 = CloudMetrics::instance();
    auto& instance2 = CloudMetrics::instance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(CloudMetricsTest, RecordStart) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordStart(S3OperationType::HeadObject);

    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 2u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::HeadObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::PutObject).call_count.load(), 0u);
}

TEST_F(CloudMetricsTest, RecordSuccess) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024);

    const auto& m = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_EQ(m.bytes_downloaded.load(), 1024u);
    EXPECT_EQ(m.bytes_uploaded.load(), 0u);
    EXPECT_EQ(m.total_latency_ms.load(), 100);
    EXPECT_EQ(m.min_latency_ms.load(), 100);
    EXPECT_EQ(m.max_latency_ms.load(), 100);
}

TEST_F(CloudMetricsTest, RecordFailure) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordFailure(S3OperationType::PutObject, 50);

    const auto& m = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(m.failure_count.load(), 1u);
    EXPECT_EQ(m.total_latency_ms.load(), 50);
}

TEST_F(CloudMetricsTest, RecordRetry) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordRetry(S3OperationType::GetObject);
    metrics.recordRetry(S3OperationType::GetObject);

    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).retry_count.load(), 2u);
}

TEST_F(CloudMetricsTest, MinMaxLatency) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 0);
    metrics.recordSuccess(S3OperationType::GetObject, 50, 0, 0);
    metrics.recordSuccess(S3OperationType::GetObject, 200, 0, 0);
    metrics.recordSuccess(S3OperationType::GetObject, 75, 0, 0);

    const auto& m = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(m.min_latency_ms.load(), 50);
    EXPECT_EQ(m.max_latency_ms.load(), 200);
}

TEST_F(CloudMetricsTest, GetAllMetrics) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordStart(S3OperationType::ListObjectsV2);

    auto all = metrics.getAllMetrics();

    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all[S3OperationType::GetObject].call_count.load(), 1u);
    EXPECT_EQ(all[S3OperationType::HeadObject].call_count.load(), 1u);
    EXPECT_EQ(all[S3OperationType::ListObjectsV2].call_count.load(), 1u);

    // Operations with no calls should not be in the map
    EXPECT_EQ(all.find(S3OperationType::PutObject), all.end());
}

TEST_F(CloudMetricsTest, GetTotalMetrics) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024);

    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 50, 0, 0);

    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordFailure(S3OperationType::PutObject, 200);

    auto total = metrics.getTotalMetrics();

    EXPECT_EQ(total.call_count.load(), 3u);
    EXPECT_EQ(total.success_count.load(), 2u);
    EXPECT_EQ(total.failure_count.load(), 1u);
    EXPECT_EQ(total.bytes_downloaded.load(), 1024u);
    EXPECT_EQ(total.total_latency_ms.load(), 350);  // 100 + 50 + 200
    EXPECT_EQ(total.min_latency_ms.load(), 50);
    EXPECT_EQ(total.max_latency_ms.load(), 200);
}

TEST_F(CloudMetricsTest, Clear) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024);
    metrics.setRegion("us-east-1");

    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getRegion(), "us-east-1");

    metrics.clear();

    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 0u);
    EXPECT_TRUE(metrics.getRegion().empty());
}

TEST_F(CloudMetricsTest, RegionSingleRegion) {
    auto& metrics = CloudMetrics::instance();

    metrics.setRegion("us-east-1");
    EXPECT_EQ(metrics.getRegion(), "us-east-1");

    auto regions = metrics.getRegions();
    EXPECT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0], "us-east-1");
}

TEST_F(CloudMetricsTest, RegionMultipleRegions) {
    auto& metrics = CloudMetrics::instance();

    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    metrics.addRegion("ap-northeast-1");

    EXPECT_EQ(metrics.getRegion(), "us-east-1");  // Primary region

    auto regions = metrics.getRegions();
    EXPECT_EQ(regions.size(), 3u);
    EXPECT_EQ(regions[0], "us-east-1");
    EXPECT_EQ(regions[1], "eu-west-1");
    EXPECT_EQ(regions[2], "ap-northeast-1");
}

TEST_F(CloudMetricsTest, RegionNoDuplicates) {
    auto& metrics = CloudMetrics::instance();

    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    metrics.addRegion("us-east-1");  // Duplicate
    metrics.addRegion("eu-west-1");  // Duplicate

    auto regions = metrics.getRegions();
    EXPECT_EQ(regions.size(), 2u);
}

TEST_F(CloudMetricsTest, SetRegionClearsPrevious) {
    auto& metrics = CloudMetrics::instance();

    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    EXPECT_EQ(metrics.getRegions().size(), 2u);

    metrics.setRegion("ap-northeast-1");

    auto regions = metrics.getRegions();
    EXPECT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0], "ap-northeast-1");
}

TEST_F(CloudMetricsTest, EmptyRegion) {
    auto& metrics = CloudMetrics::instance();

    metrics.setRegion("");
    EXPECT_TRUE(metrics.getRegion().empty());
    EXPECT_TRUE(metrics.getRegions().empty());

    metrics.addRegion("");
    EXPECT_TRUE(metrics.getRegions().empty());
}

TEST_F(CloudMetricsTest, InvalidOperationType) {
    auto& metrics = CloudMetrics::instance();

    // Recording with COUNT (invalid) should be a no-op
    metrics.recordStart(S3OperationType::COUNT);
    metrics.recordSuccess(S3OperationType::COUNT, 100, 0, 0);
    metrics.recordFailure(S3OperationType::COUNT, 100);
    metrics.recordRetry(S3OperationType::COUNT);

    // Verify nothing was recorded
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 0u);
}

TEST_F(CloudMetricsTest, SampleRateLimiting) {
    auto& metrics = CloudMetrics::instance();

    // Multiple rapid samples should be rate-limited
    for (int i = 0; i < 10; ++i) {
        metrics.sample();
    }

    // Should have at most 1 sample (rate-limited to 500ms intervals)
    EXPECT_LE(metrics.historySize(), 1u);
}

TEST_F(CloudMetricsTest, SampleAfterInterval) {
    auto& metrics = CloudMetrics::instance();

    metrics.sample();
    size_t initial_size = metrics.historySize();

    // Wait for sample interval to pass
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(CloudMetrics::SAMPLE_INTERVAL_MS) + 50));

    metrics.sample();

    // Should have added a new sample
    EXPECT_GT(metrics.historySize(), initial_size);
}

TEST_F(CloudMetricsTest, CurrentSampleEmpty) {
    auto& metrics = CloudMetrics::instance();

    auto sample = metrics.currentSample();
    EXPECT_EQ(sample.total_calls, 0u);
    EXPECT_EQ(sample.total_bytes, 0u);
    EXPECT_EQ(sample.delta_calls, 0u);
    EXPECT_EQ(sample.delta_bytes, 0u);
}

TEST_F(CloudMetricsTest, ConcurrentAccess) {
    auto& metrics = CloudMetrics::instance();
    constexpr int num_threads = 4;
    constexpr int ops_per_thread = 1000;

    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            while (!start_flag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < ops_per_thread; ++i) {
                S3OperationType op = static_cast<S3OperationType>(t % 4);
                metrics.recordStart(op);
                if (i % 2 == 0) {
                    metrics.recordSuccess(op, 10, 100, 200);
                } else {
                    metrics.recordFailure(op, 5);
                }
            }
        });
    }

    start_flag.store(true);

    for (auto& t : threads) {
        t.join();
    }

    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), num_threads * ops_per_thread);
    EXPECT_EQ(total.success_count.load() + total.failure_count.load(),
              num_threads * ops_per_thread);
}

TEST_F(CloudMetricsTest, HistoryConstants) {
    EXPECT_EQ(CloudMetrics::HISTORY_SIZE, 120u);
    EXPECT_EQ(CloudMetrics::SAMPLE_INTERVAL_MS, 500.0);
}

TEST_F(CloudMetricsTest, CurrentSampleWithData) {
    auto& metrics = CloudMetrics::instance();

    // Record some activity
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 512, 1024);
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 50, 0, 0);

    // Take first sample
    metrics.sample();

    auto sample1 = metrics.currentSample();
    EXPECT_EQ(sample1.total_calls, 2u);
    EXPECT_EQ(sample1.total_bytes, 512u + 1024u);  // bytes_up + bytes_down
    EXPECT_EQ(sample1.delta_calls, 2u);  // First sample, all calls are delta
    EXPECT_EQ(sample1.delta_bytes, 512u + 1024u);

    // Wait for next sample interval
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(CloudMetrics::SAMPLE_INTERVAL_MS) + 50));

    // Record more activity
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 75, 2048, 0);

    // Take second sample
    metrics.sample();

    auto sample2 = metrics.currentSample();
    EXPECT_EQ(sample2.total_calls, 3u);
    EXPECT_EQ(sample2.total_bytes, 512u + 1024u + 2048u);
    EXPECT_EQ(sample2.delta_calls, 1u);  // Only one new call since last sample
    EXPECT_EQ(sample2.delta_bytes, 2048u);  // Only new bytes
}

TEST_F(CloudMetricsTest, HistoryOverflowEviction) {
    auto& metrics = CloudMetrics::instance();

    // Record some activity so samples have data
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 100);

    // Force exactly HISTORY_SIZE samples
    for (size_t i = 0; i < CloudMetrics::HISTORY_SIZE; ++i) {
        metrics.forceSample();
    }

    EXPECT_EQ(metrics.historySize(), CloudMetrics::HISTORY_SIZE);

    // Get the first sample's total_calls before overflow
    auto history_before = metrics.getHistory();
    uint64_t first_sample_calls = history_before.front().total_calls;

    // Add more activity and force another sample (should evict oldest)
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 20, 50, 0);
    metrics.forceSample();

    // Size should still be HISTORY_SIZE (oldest was evicted)
    EXPECT_EQ(metrics.historySize(), CloudMetrics::HISTORY_SIZE);

    // The first sample should now be different (old one was evicted)
    auto history_after = metrics.getHistory();
    // New front should be what was the second sample before
    EXPECT_EQ(history_after.front().total_calls, first_sample_calls);

    // Latest sample should reflect the new activity
    EXPECT_EQ(history_after.back().total_calls, 2u);  // 1 GetObject + 1 PutObject
}

TEST_F(CloudMetricsTest, ForceSampleBypassesRateLimiting) {
    auto& metrics = CloudMetrics::instance();

    // Multiple forceSample calls should all succeed without waiting
    for (int i = 0; i < 10; ++i) {
        metrics.forceSample();
    }

    // All 10 samples should be in history
    EXPECT_EQ(metrics.historySize(), 10u);
}

TEST_F(CloudMetricsTest, GetHistoryReturnsThreadSafeCopy) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 100);
    metrics.forceSample();

    // Get a copy of history
    auto history_copy = metrics.getHistory();
    EXPECT_EQ(history_copy.size(), 1u);

    // Modify original by adding more samples
    metrics.forceSample();
    metrics.forceSample();

    // Our copy should still have 1 sample (it's a copy, not a reference)
    EXPECT_EQ(history_copy.size(), 1u);

    // New call should reflect updated state
    EXPECT_EQ(metrics.historySize(), 3u);
}

// ============================================================================
// bytes_server_side Tests
// ============================================================================

TEST_F(CloudMetricsTest, BytesServerSideAccumulation) {
    auto& metrics = CloudMetrics::instance();

    // Record server-side copies with bytes_server_side parameter
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 1024);

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 60, 0, 0, 2048);

    metrics.recordStart(S3OperationType::CopyObjectRemote);
    metrics.recordSuccess(S3OperationType::CopyObjectRemote, 40, 0, 0, 4096);

    // Check individual operation metrics
    const auto& upload_copy = metrics.getMetrics(S3OperationType::UploadPartCopyRemote);
    EXPECT_EQ(upload_copy.bytes_server_side.load(), 1024u + 2048u);

    const auto& copy_obj = metrics.getMetrics(S3OperationType::CopyObjectRemote);
    EXPECT_EQ(copy_obj.bytes_server_side.load(), 4096u);

    // Check total metrics includes all server-side bytes
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.bytes_server_side.load(), 1024u + 2048u + 4096u);
}

TEST_F(CloudMetricsTest, BytesServerSideNotAffectedByRegularOps) {
    auto& metrics = CloudMetrics::instance();

    // Regular operations should not affect bytes_server_side
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024);  // bytes_down = 1024

    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 100, 2048, 0);  // bytes_up = 2048

    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.bytes_downloaded.load(), 1024u);
    EXPECT_EQ(total.bytes_uploaded.load(), 2048u);
    EXPECT_EQ(total.bytes_server_side.load(), 0u);  // No server-side transfers
}

TEST_F(CloudMetricsTest, BytesServerSideResetOnClear) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 5000);

    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).bytes_server_side.load(), 5000u);

    metrics.clear();

    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).bytes_server_side.load(), 0u);
}

// ============================================================================
// Self-Assignment Tests
// ============================================================================

TEST(OperationMetricsTest, SelfAssignment) {
    OperationMetrics metrics;
    metrics.call_count.store(10);
    metrics.success_count.store(8);
    metrics.bytes_downloaded.store(2048);
    metrics.min_latency_ms.store(50);
    metrics.max_latency_ms.store(150);

    // Self-assignment should be a no-op
    metrics = metrics;

    // Values should remain unchanged
    EXPECT_EQ(metrics.call_count.load(), 10u);
    EXPECT_EQ(metrics.success_count.load(), 8u);
    EXPECT_EQ(metrics.bytes_downloaded.load(), 2048u);
    EXPECT_EQ(metrics.min_latency_ms.load(), 50);
    EXPECT_EQ(metrics.max_latency_ms.load(), 150);
}

// ============================================================================
// ListParts Operation Tests
// ============================================================================

TEST(S3OperationTypeTest, ListPartsOperationType) {
    EXPECT_EQ(ParseS3OperationType("ListParts"), S3OperationType::ListParts);
    EXPECT_EQ(ParseS3OperationType("ListPartsRequest"), S3OperationType::ListParts);
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::ListParts), "ListParts");
}

// ============================================================================
// Cross-Region Detection Tests
// ============================================================================

TEST_F(CloudMetricsTest, IsCrossRegionNoRegions) {
    auto& metrics = CloudMetrics::instance();
    EXPECT_FALSE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsTest, IsCrossRegionSingleRegion) {
    auto& metrics = CloudMetrics::instance();
    metrics.setRegion("us-east-1");
    EXPECT_FALSE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsTest, IsCrossRegionSameRegionTwice) {
    auto& metrics = CloudMetrics::instance();
    metrics.setRegion("us-east-1");
    metrics.addRegion("us-east-1");  // Same region added again
    EXPECT_FALSE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsTest, IsCrossRegionDifferentRegions) {
    auto& metrics = CloudMetrics::instance();
    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    EXPECT_TRUE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsTest, IsCrossRegionThreeRegions) {
    auto& metrics = CloudMetrics::instance();
    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    metrics.addRegion("ap-northeast-1");
    EXPECT_TRUE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsTest, GetCrossBucketServerSideBytesEmpty) {
    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 0u);
}

TEST_F(CloudMetricsTest, GetCrossBucketServerSideBytesOnlyCrossBucket) {
    auto& metrics = CloudMetrics::instance();

    // Same-bucket copies should NOT be counted
    metrics.recordStart(S3OperationType::UploadPartCopy);
    metrics.recordSuccess(S3OperationType::UploadPartCopy, 50, 0, 0, 1000);

    metrics.recordStart(S3OperationType::CopyObject);
    metrics.recordSuccess(S3OperationType::CopyObject, 50, 0, 0, 2000);

    // Cross-bucket copies SHOULD be counted
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 4000);

    metrics.recordStart(S3OperationType::CopyObjectRemote);
    metrics.recordSuccess(S3OperationType::CopyObjectRemote, 50, 0, 0, 8000);

    // Only cross-bucket bytes should be returned
    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 4000u + 8000u);

    // Total bytes_server_side should include all
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.bytes_server_side.load(), 1000u + 2000u + 4000u + 8000u);
}

TEST_F(CloudMetricsTest, GetCrossBucketServerSideBytesClearedOnReset) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 5000);

    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 5000u);

    metrics.clear();

    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 0u);
}

TEST_F(CloudMetricsTest, AddServerSideBytesSupplementsExisting) {
    auto& metrics = CloudMetrics::instance();

    // Simulate a CopyObject call where monitoring layer recorded 0 bytes
    // (happens for full-object copies without range header)
    metrics.recordStart(S3OperationType::CopyObjectRemote);
    metrics.recordSuccess(S3OperationType::CopyObjectRemote, 100, 0, 0, 0);

    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 0u);

    // Now supplement with the actual bytes (as sync_task.cpp does)
    metrics.addServerSideBytes(S3OperationType::CopyObjectRemote, 1000000);

    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 1000000u);

    // Adding more bytes should accumulate
    metrics.addServerSideBytes(S3OperationType::CopyObjectRemote, 500000);

    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 1500000u);
}

TEST_F(CloudMetricsTest, AddServerSideBytesZeroDoesNothing) {
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::CopyObject);
    metrics.recordSuccess(S3OperationType::CopyObject, 50, 0, 0, 1000);

    auto before = metrics.getMetrics(S3OperationType::CopyObject).bytes_server_side.load();

    // Adding 0 bytes should not change anything
    metrics.addServerSideBytes(S3OperationType::CopyObject, 0);

    auto after = metrics.getMetrics(S3OperationType::CopyObject).bytes_server_side.load();
    EXPECT_EQ(before, after);
}

// ============================================================================
// Persistence Tests (save/load)
// ============================================================================

// Test fixture that uses a temporary directory to avoid destroying real user data.
// This fixture always redirected - it is the pattern the other three forgot
// (issue #41) - and now shares the guard with them rather than hand-rolling it,
// so the file demonstrates one convention instead of two.
class CloudMetricsPersistenceTest : public ::testing::Test {
protected:
    ScopedCloudMetricsDir metrics_dir_;

    void SetUp() override {
        CloudMetrics::instance().clear();
    }

    void TearDown() override {
        CloudMetrics::instance().clear();
    }

    std::string getTestMetricsFilePath() const {
        return metrics_dir_.metrics_file().string();
    }
};

TEST_F(CloudMetricsPersistenceTest, SaveAndLoadRoundTrip) {
    auto& metrics = CloudMetrics::instance();

    // Record some metrics
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024, 0);
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 200, 0, 2048, 0);

    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 50, 512, 0, 0);
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordFailure(S3OperationType::PutObject, 30);

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 80, 0, 0, 4096);

    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");

    // Save metrics
    ASSERT_TRUE(metrics.save());

    // Simulate app restart: clear() saves empty state, so we need to
    // restore the saved file before loading. Instead, we test that
    // load() on top of existing in-memory state works (additive).
    // First verify current state
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 5u);

    // Now load again - should overwrite with same values
    ASSERT_TRUE(metrics.load());

    // Verify data is still correct after load
    auto getMetrics = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(getMetrics.call_count.load(), 2u);
    EXPECT_EQ(getMetrics.success_count.load(), 2u);
    EXPECT_EQ(getMetrics.bytes_downloaded.load(), 1024u + 2048u);

    auto putMetrics = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(putMetrics.call_count.load(), 2u);
    EXPECT_EQ(putMetrics.success_count.load(), 1u);
    EXPECT_EQ(putMetrics.failure_count.load(), 1u);
    EXPECT_EQ(putMetrics.bytes_uploaded.load(), 512u);

    auto copyMetrics = metrics.getMetrics(S3OperationType::UploadPartCopyRemote);
    EXPECT_EQ(copyMetrics.call_count.load(), 1u);
    EXPECT_EQ(copyMetrics.bytes_server_side.load(), 4096u);

    // Verify regions restored
    auto regions = metrics.getRegions();
    EXPECT_EQ(regions.size(), 2u);
    EXPECT_TRUE(metrics.isCrossRegion());
}

TEST_F(CloudMetricsPersistenceTest, LoadNonExistentFile) {
    auto& metrics = CloudMetrics::instance();

    // Ensure file doesn't exist
    std::remove(getTestMetricsFilePath().c_str());

    // Load should return false but not crash
    EXPECT_FALSE(metrics.load());

    // Metrics should still be empty
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 0u);
}

TEST_F(CloudMetricsPersistenceTest, LoadCorruptedJson) {
    auto& metrics = CloudMetrics::instance();

    // Write corrupted JSON to the file
    std::ofstream out(getTestMetricsFilePath());
    out << "{ this is not valid json }}}";
    out.close();

    // Load should return false but not crash
    EXPECT_FALSE(metrics.load());

    // Metrics should still be empty
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 0u);
}

TEST_F(CloudMetricsPersistenceTest, LoadEmptyFile) {
    auto& metrics = CloudMetrics::instance();

    // Write empty file
    std::ofstream out(getTestMetricsFilePath());
    out.close();

    // Load should return false but not crash
    EXPECT_FALSE(metrics.load());
}

TEST_F(CloudMetricsPersistenceTest, LoadUnknownVersion) {
    auto& metrics = CloudMetrics::instance();

    // Write JSON with unknown version
    std::ofstream out(getTestMetricsFilePath());
    out << R"({"version": 999, "metrics": {}})";
    out.close();

    // Load should return false due to version mismatch
    EXPECT_FALSE(metrics.load());
}

TEST_F(CloudMetricsPersistenceTest, LoadUnknownOperationsSkipped) {
    auto& metrics = CloudMetrics::instance();

    // Write JSON with unknown operation type
    std::ofstream out(getTestMetricsFilePath());
    out << R"({
        "version": 1,
        "metrics": {
            "GetObject": {"calls": 10, "success": 10, "failures": 0, "retries": 0, "bytes_up": 0, "bytes_down": 1000, "bytes_server": 0},
            "FutureUnknownOp": {"calls": 5, "success": 5, "failures": 0, "retries": 0, "bytes_up": 0, "bytes_down": 500, "bytes_server": 0}
        },
        "regions": []
    })";
    out.close();

    // Load should succeed, unknown operations skipped
    EXPECT_TRUE(metrics.load());

    // GetObject should be loaded
    auto getMetrics = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(getMetrics.call_count.load(), 10u);

    // Unknown operation should not affect totals unexpectedly
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 10u);  // Only GetObject
}

TEST_F(CloudMetricsPersistenceTest, SaveCreatesDirectory) {
    auto& metrics = CloudMetrics::instance();

    // Record minimal metrics
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 10, 0, 0, 0);

    // Save should succeed (creates directory if needed)
    EXPECT_TRUE(metrics.save());

    // File should exist
    std::ifstream in(getTestMetricsFilePath());
    EXPECT_TRUE(in.good());
}

TEST_F(CloudMetricsPersistenceTest, ClearThenSavePersistsEmptyState) {
    // This used to be ClearSavesEmptyState, asserting that clear() wrote to
    // disk by itself. That is the behaviour issue #41 removed: clear() now
    // resets memory only. The requirement it was really guarding - that
    // `mito stats --reset` survives a restart - lives in the clear()+save()
    // pair main.cpp performs, which is what this asserts instead.
    auto& metrics = CloudMetrics::instance();

    // Record some metrics and save
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024, 0);
    ASSERT_TRUE(metrics.save());

    metrics.clear();
    ASSERT_TRUE(metrics.save());

    // Load should succeed with empty metrics
    ASSERT_TRUE(metrics.load());
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 0u);
}

TEST_F(CloudMetricsPersistenceTest, RemoteOperationsRoundTrip) {
    auto& metrics = CloudMetrics::instance();

    // Record remote copy operations (these use display names with "(remote)")
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 100, 0, 0, 8192);

    metrics.recordStart(S3OperationType::CopyObjectRemote);
    metrics.recordSuccess(S3OperationType::CopyObjectRemote, 50, 0, 0, 4096);

    ASSERT_TRUE(metrics.save());
    // clear() no longer writes to disk (issue #41), so it would be harmless
    // here; load() alone is still what this test is about.
    ASSERT_TRUE(metrics.load());

    // Verify remote operations are correctly restored
    auto uploadCopyRemote = metrics.getMetrics(S3OperationType::UploadPartCopyRemote);
    EXPECT_EQ(uploadCopyRemote.call_count.load(), 1u);
    EXPECT_EQ(uploadCopyRemote.bytes_server_side.load(), 8192u);

    auto copyRemote = metrics.getMetrics(S3OperationType::CopyObjectRemote);
    EXPECT_EQ(copyRemote.call_count.load(), 1u);
    EXPECT_EQ(copyRemote.bytes_server_side.load(), 4096u);

    // Cross-bucket bytes should be correct
    EXPECT_EQ(metrics.getCrossBucketServerSideBytes(), 8192u + 4096u);
}

TEST_F(CloudMetricsPersistenceTest, LoadClearsExistingMetrics) {
    auto& metrics = CloudMetrics::instance();

    // Save a known state with only GetObject
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1000, 0);
    ASSERT_TRUE(metrics.save());

    // Add more metrics in memory (not saved) - PutObject
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 50, 500, 0, 0);

    // Verify PutObject is in memory
    auto putBefore = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(putBefore.call_count.load(), 1u);

    // Load should restore to saved state, clearing in-memory PutObject
    ASSERT_TRUE(metrics.load());

    // PutObject should be cleared (wasn't in saved file)
    auto putAfter = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(putAfter.call_count.load(), 0u);

    // GetObject should still have the saved values
    auto getAfter = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(getAfter.call_count.load(), 1u);
    EXPECT_EQ(getAfter.bytes_downloaded.load(), 1000u);
}

TEST_F(CloudMetricsPersistenceTest, LoadTruncatedJson) {
    auto& metrics = CloudMetrics::instance();

    // Write a truncated JSON file (simulates crash during write)
    std::ofstream out(getTestMetricsFilePath());
    out << R"({"version": 1, "metrics": {"GetObject": {"calls": 10)";  // Truncated
    out.close();

    // Load should fail gracefully
    EXPECT_FALSE(metrics.load());
}

TEST_F(CloudMetricsPersistenceTest, LoadPreservesStateOnParseFailure) {
    auto& metrics = CloudMetrics::instance();

    // Record some metrics
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 5000, 0);
    ASSERT_TRUE(metrics.save());

    // Record additional metrics in memory (not saved)
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordSuccess(S3OperationType::PutObject, 50, 1000, 0, 0);

    // Write a corrupted file
    std::ofstream out(getTestMetricsFilePath());
    out << "{ invalid json";
    out.close();

    // Load should fail
    EXPECT_FALSE(metrics.load());

    // In-memory metrics should be UNCHANGED (not cleared)
    // GetObject should still have its values
    auto getMetrics = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(getMetrics.call_count.load(), 1u);

    // PutObject should still have its values (wasn't cleared by failed load)
    auto putMetrics = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(putMetrics.call_count.load(), 1u);
}

TEST_F(CloudMetricsPersistenceTest, AutoSaveThresholdBehavior) {
    auto& metrics = CloudMetrics::instance();

    // Remove any existing file
    std::remove(getTestMetricsFilePath().c_str());

    // Record 99 calls (below threshold of 100)
    for (int i = 0; i < 99; ++i) {
        metrics.recordStart(S3OperationType::HeadObject);
        metrics.recordSuccess(S3OperationType::HeadObject, 10, 0, 0, 0);
    }

    // Force a sample to trigger threshold check
    metrics.forceSample();

    // File should NOT exist yet (below threshold)
    std::ifstream check1(getTestMetricsFilePath());
    EXPECT_FALSE(check1.good()) << "File should not exist below threshold";

    // Record one more call (now at 100)
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 10, 0, 0, 0);

    // Force another sample to trigger threshold check
    metrics.forceSample();

    // File SHOULD exist now (at threshold)
    std::ifstream check2(getTestMetricsFilePath());
    EXPECT_TRUE(check2.good()) << "File should exist at threshold";
}

// ============================================================================
// Resetting must not reach the user's real metrics file (issue #41)
// ============================================================================

TEST_F(CloudMetricsPersistenceTest, ClearLeavesTheSavedFileAlone) {
    // clear() used to end in save(), which is what made it dangerous. Here the
    // persistence is redirected, so this is about the split rather than about
    // safety: a reset changes memory, and only an explicit save() changes disk.
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 4096, 0);
    ASSERT_TRUE(metrics.save());

    std::ifstream before(getTestMetricsFilePath());
    ASSERT_TRUE(before.good()) << "the file we are about to check must exist";
    const std::string saved((std::istreambuf_iterator<char>(before)),
                            std::istreambuf_iterator<char>());
    before.close();

    metrics.clear();

    EXPECT_EQ(metrics.getTotalMetrics().call_count.load(), 0u)
        << "clear() must still reset memory";

    std::ifstream after(getTestMetricsFilePath());
    ASSERT_TRUE(after.good()) << "clear() must not delete the saved file";
    const std::string still((std::istreambuf_iterator<char>(after)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(saved, still) << "clear() must not rewrite the saved file";

    // And the reset does become permanent when the caller asks for it, which is
    // what `mito stats --reset` does. Reading it back beats comparing the file
    // to its old text: save() stamps a fresh saved_at, so the bytes would
    // differ even if nothing had been reset.
    ASSERT_TRUE(metrics.save());
    ASSERT_TRUE(metrics.load());
    EXPECT_EQ(metrics.getTotalMetrics().call_count.load(), 0u)
        << "an explicit save() after clear() must persist the reset";
}

TEST(CloudMetricsRealDirectorySafety, ResettingWritesNothingToTheAppDataDirectory) {
    // The regression test for #41 proper. Every other test in this file
    // redirects persistence first; this one deliberately does not, because the
    // bug was precisely about what happens when a test forgets. The real
    // directory is faked out through the environment so the assertion can be
    // made without gambling with the user's file.
    namespace fs = std::filesystem;

#ifdef _WIN32
    GTEST_SKIP() << "GetAppDataDirectory() resolves through %APPDATA% on Windows, "
                    "which this test does not redirect";
#else
    // Restoring the environment has to survive an exception or a fatal
    // assertion: leaving the process pointed at a temporary home would corrupt
    // every test after this one, and the directory is deleted below.
    struct EnvRestore {
        std::string home;
        std::string xdg;
        bool had_home;
        bool had_xdg;
        ~EnvRestore() {
            if (had_home) { ::setenv("HOME", home.c_str(), 1); } else { ::unsetenv("HOME"); }
            if (had_xdg) { ::setenv("XDG_DATA_HOME", xdg.c_str(), 1); } else { ::unsetenv("XDG_DATA_HOME"); }
        }
    };

    const char* old_home = std::getenv("HOME");
    const char* old_xdg = std::getenv("XDG_DATA_HOME");
    EnvRestore restore{old_home ? old_home : "", old_xdg ? old_xdg : "",
                       old_home != nullptr, old_xdg != nullptr};

    const fs::path fake_home = fs::temp_directory_path() /
                               ("mito_fake_home_" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ec;
    fs::remove_all(fake_home, ec);
    // Checked, not discarded: a missing directory would make the scan below
    // iterate nothing and the test pass while checking nothing.
    ASSERT_TRUE(fs::create_directories(fake_home, ec) || fs::exists(fake_home))
        << "could not create " << fake_home.string() << ": " << ec.message();

    ::setenv("HOME", fake_home.c_str(), 1);          // macOS resolves through $HOME
    ::setenv("XDG_DATA_HOME", fake_home.c_str(), 1); // Linux prefers $XDG_DATA_HOME
    CloudMetrics::setTestDataDirectory("");          // no override: the production path

    // The redirect is the premise of the whole test. If GetAppDataDirectory()
    // ever stopped consulting the environment, everything below would pass
    // while clear() wrote to the developer's real directory.
    ASSERT_EQ(GetAppDataDirectory().rfind(fake_home.string(), 0), 0u)
        << "the app data directory is " << GetAppDataDirectory()
        << ", so this test is not measuring what it claims to";

    auto& metrics = CloudMetrics::instance();
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 128, 0);
    metrics.clear();

    // Whatever the platform's layout is, no metrics file may have appeared
    // anywhere beneath the home we just invented.
    bool wrote_something = false;
    fs::path written;
    for (const auto& entry : fs::recursive_directory_iterator(fake_home, ec)) {
        if (entry.path().filename() == "cloud_metrics.json") {
            wrote_something = true;
            written = entry.path();
            break;
        }
    }

    // No cleanup clear() here, deliberately. It used to run after the
    // environment was restored and with no override in force - so on the day
    // this test earns its keep, that reset would have written an empty file
    // into the developer's real data directory while reporting the failure.
    // The clear() above already left the singleton empty.
    fs::remove_all(fake_home, ec);

    EXPECT_FALSE(wrote_something)
        << "clear() persisted to the app data directory: " << written.string();
#endif
}

// ============================================================================
// Saving without loading first (issue #35)
// ============================================================================

TEST(CloudMetricsPersistence, LoadingBeforeSavingPreservesAndAccumulates) {
    // Load, then record, then save - what every command that persists metrics
    // has to do, and what run_leftovers_command was not doing (issue #35).
    //
    // This pins the CloudMetrics side of that contract. It cannot catch a
    // command that forgets to call load(), because main.cpp is not linked into
    // any test binary; CliLeftoversPreservesAccumulatedMetrics drives the real
    // executable for that.
    ScopedCloudMetricsDir dir;

    CloudMetrics::instance().clear();
    for (int i = 0; i < 4242; ++i) {
        CloudMetrics::instance().recordStart(S3OperationType::HeadObject);
        CloudMetrics::instance().recordSuccess(S3OperationType::HeadObject, 1, 0, 0, 0);
    }
    ASSERT_TRUE(CloudMetrics::instance().save());

    // A new process that loads first.
    CloudMetrics::instance().clear();
    ASSERT_TRUE(CloudMetrics::instance().load());
    CloudMetrics::instance().recordStart(S3OperationType::ListMultipartUploads);
    CloudMetrics::instance().recordSuccess(S3OperationType::ListMultipartUploads, 1, 0, 0, 0);
    ASSERT_TRUE(CloudMetrics::instance().save());

    CloudMetrics::instance().clear();
    ASSERT_TRUE(CloudMetrics::instance().load());
    EXPECT_EQ(CloudMetrics::instance().getMetrics(S3OperationType::HeadObject).call_count.load(), 4242u)
        << "the earlier history has to survive";
    EXPECT_EQ(CloudMetrics::instance().getMetrics(S3OperationType::ListMultipartUploads)
                  .call_count.load(),
              1u)
        << "and this run's calls have to be added to it";

    CloudMetrics::instance().clear();
}
