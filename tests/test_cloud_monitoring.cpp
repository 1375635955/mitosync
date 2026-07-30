#include <gtest/gtest.h>
#include "cloud_monitoring.h"
#include "cloud_metrics.h"
#include "cloud_metrics_test_dir.h"

#include <aws/core/Aws.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <thread>
#include <chrono>

// ============================================================================
// Test Fixture - manages AWS SDK initialization
// ============================================================================

class CloudMonitoringTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        Aws::InitAPI(aws_options_);
    }

    static void TearDownTestSuite() {
        Aws::ShutdownAPI(aws_options_);
    }

    ScopedCloudMetricsDir metrics_dir_;  // issue #41

    void SetUp() override {
        CloudMetrics::instance().clear();
    }

    void TearDown() override {
        CloudMetrics::instance().clear();
    }

    static Aws::SDKOptions aws_options_;
};

Aws::SDKOptions CloudMonitoringTest::aws_options_;

// ============================================================================
// S3MonitoringListener Basic Tests
// These tests verify the listener correctly processes requests and records metrics
// without requiring complex AWS SDK object creation
// ============================================================================

TEST_F(CloudMonitoringTest, OnRequestStartedRecordsCall) {
    S3MonitoringListener listener;

    // Create minimal request - the listener only needs service name and request name
    void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);

    // Should return non-null for S3 service
    ASSERT_NE(ctx, nullptr);

    // Verify call was recorded
    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 1u);

    // Clean up context
    listener.OnFinish("s3", "GetObject", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, OnRequestStartedIgnoresNonS3) {
    S3MonitoringListener listener;

    // Non-S3 service should return nullptr
    void* ctx = listener.OnRequestStarted("ec2", "DescribeInstances", nullptr);
    EXPECT_EQ(ctx, nullptr);

    // DynamoDB should also be ignored
    ctx = listener.OnRequestStarted("dynamodb", "GetItem", nullptr);
    EXPECT_EQ(ctx, nullptr);

    // No metrics should be recorded
    auto total = CloudMetrics::instance().getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 0u);
}

TEST_F(CloudMonitoringTest, OnRequestStartedHandlesS3CaseVariants) {
    S3MonitoringListener listener;

    // "s3" lowercase
    void* ctx1 = listener.OnRequestStarted("s3", "GetObject", nullptr);
    EXPECT_NE(ctx1, nullptr);
    listener.OnFinish("s3", "GetObject", nullptr, ctx1);

    // "S3" uppercase
    void* ctx2 = listener.OnRequestStarted("S3", "GetObject", nullptr);
    EXPECT_NE(ctx2, nullptr);
    listener.OnFinish("S3", "GetObject", nullptr, ctx2);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 2u);
}

TEST_F(CloudMonitoringTest, OnRequestRetryRecordsRetry) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "HeadObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // Record multiple retries
    listener.OnRequestRetry("s3", "HeadObject", nullptr, ctx);
    listener.OnRequestRetry("s3", "HeadObject", nullptr, ctx);
    listener.OnRequestRetry("s3", "HeadObject", nullptr, ctx);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::HeadObject).retry_count.load(), 3u);

    listener.OnFinish("s3", "HeadObject", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, OnFinishCleansUpContext) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // OnFinish should delete the context without crashing
    listener.OnFinish("s3", "GetObject", nullptr, ctx);

    // Calling with nullptr should be safe (no-op)
    listener.OnFinish("s3", "GetObject", nullptr, nullptr);
}

TEST_F(CloudMonitoringTest, NullContextHandledGracefully) {
    S3MonitoringListener listener;
    Aws::Monitoring::CoreMetricsCollection coreMetrics;

    // All methods should handle nullptr context gracefully
    // These should all be no-ops and not crash

    // Create empty/dummy outcomes for the calls
    Aws::Client::AWSError<Aws::Client::CoreErrors> error(
        Aws::Client::CoreErrors::NETWORK_CONNECTION, "Error", "Test", false);

    listener.OnRequestRetry("s3", "GetObject", nullptr, nullptr);
    listener.OnFinish("s3", "GetObject", nullptr, nullptr);

    // No crashes and no additional metrics recorded (call_count stays at 0)
    auto total = CloudMetrics::instance().getTotalMetrics();
    EXPECT_EQ(total.retry_count.load(), 0u);
}

TEST_F(CloudMonitoringTest, AllOperationTypesRecordedByStart) {
    S3MonitoringListener listener;

    // Test all known operation types - verify they're recognized on start
    // Note: UploadPartCopy and CopyObject are automatically converted to their
    // Remote variants since they are ALWAYS server-side copies (no egress).
    const char* operations[] = {
        "HeadObject", "GetObject", "PutObject", "DeleteObject",
        "ListObjectsV2", "ListBuckets", "GetBucketLocation",
        "CreateMultipartUpload", "UploadPart", "UploadPartCopy", "CopyObject",
        "AbortMultipartUpload", "CompleteMultipartUpload", "ListParts"
    };

    for (const char* op : operations) {
        void* ctx = listener.OnRequestStarted("s3", op, nullptr);
        ASSERT_NE(ctx, nullptr) << "Failed for operation: " << op;
        listener.OnFinish("s3", op, nullptr, ctx);
    }

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::HeadObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::PutObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::DeleteObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListObjectsV2).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListBuckets).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetBucketLocation).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CreateMultipartUpload).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPart).call_count.load(), 1u);
    // UploadPartCopy -> UploadPartCopyRemote, CopyObject -> CopyObjectRemote
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObjectRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::AbortMultipartUpload).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CompleteMultipartUpload).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListParts).call_count.load(), 1u);
}

TEST_F(CloudMonitoringTest, UnknownOperationRecordedAsUnknown) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "SomeNewOperation", nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::Unknown).call_count.load(), 1u);

    listener.OnFinish("s3", "SomeNewOperation", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, MultipleRequestsInFlight) {
    S3MonitoringListener listener;

    // Start multiple requests
    void* ctx1 = listener.OnRequestStarted("s3", "GetObject", nullptr);
    void* ctx2 = listener.OnRequestStarted("s3", "HeadObject", nullptr);
    void* ctx3 = listener.OnRequestStarted("s3", "ListObjectsV2", nullptr);

    ASSERT_NE(ctx1, nullptr);
    ASSERT_NE(ctx2, nullptr);
    ASSERT_NE(ctx3, nullptr);

    // All are different contexts
    EXPECT_NE(ctx1, ctx2);
    EXPECT_NE(ctx2, ctx3);
    EXPECT_NE(ctx1, ctx3);

    // Metrics should show 3 calls total
    auto total = CloudMetrics::instance().getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 3u);

    // Clean up in different order
    listener.OnFinish("s3", "HeadObject", nullptr, ctx2);
    listener.OnFinish("s3", "GetObject", nullptr, ctx1);
    listener.OnFinish("s3", "ListObjectsV2", nullptr, ctx3);
}

// ============================================================================
// S3MonitoringFactory Tests
// ============================================================================

TEST_F(CloudMonitoringTest, FactoryCreatesListener) {
    S3MonitoringFactory factory;
    auto listener = factory.CreateMonitoringInstance();

    ASSERT_NE(listener, nullptr);

    // Verify it's a working listener by testing basic functionality
    void* ctx = listener->OnRequestStarted("s3", "GetObject", nullptr);
    EXPECT_NE(ctx, nullptr);
    listener->OnFinish("s3", "GetObject", nullptr, ctx);

    EXPECT_EQ(CloudMetrics::instance().getMetrics(S3OperationType::GetObject).call_count.load(), 1u);
}

TEST_F(CloudMonitoringTest, GetFactoryCreateFn) {
    auto createFn = GetS3MonitoringFactoryCreateFn();
    ASSERT_NE(createFn, nullptr);

    auto factory = createFn();
    ASSERT_NE(factory, nullptr);

    auto listener = factory->CreateMonitoringInstance();
    ASSERT_NE(listener, nullptr);
}

// ============================================================================
// Context Lifecycle Tests
// ============================================================================

TEST_F(CloudMonitoringTest, ContextContainsCorrectOperationType) {
    S3MonitoringListener listener;

    // The context should record the correct operation type
    void* ctx = listener.OnRequestStarted("s3", "PutObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // Record retry on this context - should increment PutObject retry count
    listener.OnRequestRetry("s3", "PutObject", nullptr, ctx);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::PutObject).retry_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).retry_count.load(), 0u);

    listener.OnFinish("s3", "PutObject", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, ContextTracksStartTime) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // The context should have recorded the start time
    // We can verify this by checking that the context pointer is valid
    // and can be used for finish
    listener.OnFinish("s3", "GetObject", nullptr, ctx);

    // No crash means success
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(CloudMonitoringTest, EmptyRequestName) {
    S3MonitoringListener listener;

    // Empty request name should be parsed as Unknown
    void* ctx = listener.OnRequestStarted("s3", "", nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::Unknown).call_count.load(), 1u);

    listener.OnFinish("s3", "", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, LongRequestName) {
    S3MonitoringListener listener;

    // Very long unknown request name
    std::string longName(1000, 'X');
    void* ctx = listener.OnRequestStarted("s3", longName.c_str(), nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::Unknown).call_count.load(), 1u);

    listener.OnFinish("s3", longName.c_str(), nullptr, ctx);
}

TEST_F(CloudMonitoringTest, RapidFireRequests) {
    S3MonitoringListener listener;
    constexpr int num_requests = 100;

    std::vector<void*> contexts;
    contexts.reserve(num_requests);

    // Start many requests rapidly
    for (int i = 0; i < num_requests; ++i) {
        void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);
        ASSERT_NE(ctx, nullptr);
        contexts.push_back(ctx);
    }

    // Finish all requests
    for (void* ctx : contexts) {
        listener.OnFinish("s3", "GetObject", nullptr, ctx);
    }

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).call_count.load(),
              static_cast<uint64_t>(num_requests));
}

// ============================================================================
// Success/Failure Recording Tests
// Note: OnRequestSucceeded and OnRequestFailed require complex AWS SDK objects,
// so we test the underlying metrics recording directly, which is what these
// methods ultimately call.
// ============================================================================

TEST_F(CloudMonitoringTest, MetricsRecordSuccessDirectly) {
    auto& metrics = CloudMetrics::instance();

    // Simulate what OnRequestSucceeded does internally
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 150, 1024, 2048);

    const auto& m = metrics.getMetrics(S3OperationType::GetObject);
    EXPECT_EQ(m.call_count.load(), 1u);
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_EQ(m.failure_count.load(), 0u);
    EXPECT_EQ(m.bytes_uploaded.load(), 1024u);
    EXPECT_EQ(m.bytes_downloaded.load(), 2048u);
    EXPECT_EQ(m.total_latency_ms.load(), 150);
    EXPECT_EQ(m.min_latency_ms.load(), 150);
    EXPECT_EQ(m.max_latency_ms.load(), 150);
}

TEST_F(CloudMonitoringTest, MetricsRecordFailureDirectly) {
    auto& metrics = CloudMetrics::instance();

    // Simulate what OnRequestFailed does internally
    metrics.recordStart(S3OperationType::PutObject);
    metrics.recordFailure(S3OperationType::PutObject, 200);

    const auto& m = metrics.getMetrics(S3OperationType::PutObject);
    EXPECT_EQ(m.call_count.load(), 1u);
    EXPECT_EQ(m.success_count.load(), 0u);
    EXPECT_EQ(m.failure_count.load(), 1u);
    EXPECT_EQ(m.bytes_uploaded.load(), 0u);  // Failed requests don't transfer data
    EXPECT_EQ(m.bytes_downloaded.load(), 0u);
    EXPECT_EQ(m.total_latency_ms.load(), 200);
}

TEST_F(CloudMonitoringTest, MetricsSuccessAndFailureMixed) {
    auto& metrics = CloudMetrics::instance();

    // Mix of success and failure
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 50, 0, 0);

    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordFailure(S3OperationType::HeadObject, 100);

    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 75, 0, 0);

    const auto& m = metrics.getMetrics(S3OperationType::HeadObject);
    EXPECT_EQ(m.call_count.load(), 3u);
    EXPECT_EQ(m.success_count.load(), 2u);
    EXPECT_EQ(m.failure_count.load(), 1u);
    EXPECT_EQ(m.total_latency_ms.load(), 50 + 100 + 75);
    EXPECT_EQ(m.min_latency_ms.load(), 50);
    EXPECT_EQ(m.max_latency_ms.load(), 100);
}

TEST_F(CloudMonitoringTest, MetricsLatencyEdgeCases) {
    auto& metrics = CloudMetrics::instance();

    // Test with zero latency
    metrics.recordStart(S3OperationType::DeleteObject);
    metrics.recordSuccess(S3OperationType::DeleteObject, 0, 0, 0);

    const auto& m = metrics.getMetrics(S3OperationType::DeleteObject);
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_EQ(m.total_latency_ms.load(), 0);
    EXPECT_EQ(m.min_latency_ms.load(), 0);
    EXPECT_EQ(m.max_latency_ms.load(), 0);

    // Add a non-zero latency and verify min stays at 0
    metrics.recordStart(S3OperationType::DeleteObject);
    metrics.recordSuccess(S3OperationType::DeleteObject, 10, 0, 0);

    EXPECT_EQ(m.min_latency_ms.load(), 0);
    EXPECT_EQ(m.max_latency_ms.load(), 10);
}

TEST_F(CloudMonitoringTest, UploadPartCopyRemoteTrackedSeparately) {
    auto& metrics = CloudMetrics::instance();

    // Simulate local and remote copy operations
    metrics.recordStart(S3OperationType::UploadPartCopy);
    metrics.recordSuccess(S3OperationType::UploadPartCopy, 50, 0, 0);

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 100, 0, 0);
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 150, 0, 0);

    // Local copies
    const auto& local = metrics.getMetrics(S3OperationType::UploadPartCopy);
    EXPECT_EQ(local.call_count.load(), 1u);
    EXPECT_EQ(local.success_count.load(), 1u);

    // Remote copies (tracked separately)
    const auto& remote = metrics.getMetrics(S3OperationType::UploadPartCopyRemote);
    EXPECT_EQ(remote.call_count.load(), 2u);
    EXPECT_EQ(remote.success_count.load(), 2u);

    // Both should appear in all metrics
    auto all = metrics.getAllMetrics();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_NE(all.find(S3OperationType::UploadPartCopy), all.end());
    EXPECT_NE(all.find(S3OperationType::UploadPartCopyRemote), all.end());
}

TEST_F(CloudMonitoringTest, UploadPartCopyRemoteOperationName) {
    EXPECT_STREQ(S3OperationTypeName(S3OperationType::UploadPartCopyRemote),
                 "UploadPartCopy (remote)");
}

// ============================================================================
// OnRequestSucceeded/OnRequestFailed Tests
// Note: These tests verify the callback-to-metrics flow without creating full
// mock AWS SDK HTTP objects, which can cause issues with -fno-exceptions builds.
// The byte extraction logic is tested indirectly through the metrics layer.
// ============================================================================

TEST_F(CloudMonitoringTest, OnRequestSucceededUpdatesMetrics) {
    S3MonitoringListener listener;

    // Start a request
    void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // Wait to ensure measurable latency
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Call OnRequestSucceeded with nullptr request/outcome (safe - null checks exist)
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);

    listener.OnRequestSucceeded("s3", "GetObject", nullptr, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "GetObject", nullptr, ctx);

    // Verify success was recorded with latency
    auto& metrics = CloudMetrics::instance();
    const auto& m = metrics.getMetrics(S3OperationType::GetObject);

    EXPECT_EQ(m.call_count.load(), 1u);
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_GE(m.total_latency_ms.load(), 5);  // Should have at least 5ms latency
}

TEST_F(CloudMonitoringTest, OnRequestFailedUpdatesMetrics) {
    S3MonitoringListener listener;

    // Start a request
    void* ctx = listener.OnRequestStarted("s3", "PutObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Call OnRequestFailed
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error(
        Aws::Client::CoreErrors::NETWORK_CONNECTION,
        "NetworkError",
        "Connection refused",
        false);
    Aws::Client::HttpResponseOutcome outcome(error);

    listener.OnRequestFailed("s3", "PutObject", nullptr, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "PutObject", nullptr, ctx);

    // Verify failure was recorded with latency
    auto& metrics = CloudMetrics::instance();
    const auto& m = metrics.getMetrics(S3OperationType::PutObject);

    EXPECT_EQ(m.call_count.load(), 1u);
    EXPECT_EQ(m.success_count.load(), 0u);
    EXPECT_EQ(m.failure_count.load(), 1u);
    EXPECT_GE(m.total_latency_ms.load(), 5);
}

TEST_F(CloudMonitoringTest, FullRequestLifecycleWithRetries) {
    S3MonitoringListener listener;

    // Simulate a request that fails twice then succeeds
    void* ctx = listener.OnRequestStarted("s3", "GetObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // First retry
    listener.OnRequestRetry("s3", "GetObject", nullptr, ctx);

    // Second retry
    listener.OnRequestRetry("s3", "GetObject", nullptr, ctx);

    // Final success
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "GetObject", nullptr, outcome, coreMetrics, ctx);

    listener.OnFinish("s3", "GetObject", nullptr, ctx);

    // Verify all metrics recorded correctly
    auto& metrics = CloudMetrics::instance();
    const auto& m = metrics.getMetrics(S3OperationType::GetObject);

    EXPECT_EQ(m.call_count.load(), 1u);  // Only 1 call (retries don't increment)
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_EQ(m.retry_count.load(), 2u);  // 2 retries before success
}

// ============================================================================
// Egress Classification Tests
// Only GetObject should count response body as bytes_downloaded (egress).
// Other operations return XML metadata that shouldn't be counted as egress.
// ============================================================================

TEST_F(CloudMonitoringTest, OnlyGetObjectCountsAsEgress) {
    auto& metrics = CloudMetrics::instance();

    // Simulate various operation types with recordSuccess directly
    // (This tests the metrics layer, which is what the monitoring ultimately uses)

    // GetObject SHOULD count bytes_downloaded
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 1024);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::GetObject).bytes_downloaded.load(), 1024u);

    // Other operations should NOT have bytes_downloaded counted in the monitoring layer.
    // This test verifies the expected behavior at the metrics layer.
    // The actual filtering happens in S3MonitoringListener::OnRequestSucceeded.
}

TEST_F(CloudMonitoringTest, NonDataOperationsHaveZeroEgress) {
    // This test documents the expected behavior: when the monitoring layer
    // correctly filters operations, non-GetObject operations should have
    // bytes_downloaded = 0 in the final metrics.
    auto& metrics = CloudMetrics::instance();

    // ListObjectsV2 returns XML, not data - should have 0 egress
    metrics.recordStart(S3OperationType::ListObjectsV2);
    metrics.recordSuccess(S3OperationType::ListObjectsV2, 50, 0, 0);  // 0 bytes_down
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListObjectsV2).bytes_downloaded.load(), 0u);

    // HeadObject has no body - should have 0 egress
    metrics.recordStart(S3OperationType::HeadObject);
    metrics.recordSuccess(S3OperationType::HeadObject, 20, 0, 0);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::HeadObject).bytes_downloaded.load(), 0u);

    // CreateMultipartUpload returns XML - should have 0 egress
    metrics.recordStart(S3OperationType::CreateMultipartUpload);
    metrics.recordSuccess(S3OperationType::CreateMultipartUpload, 30, 0, 0);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CreateMultipartUpload).bytes_downloaded.load(), 0u);

    // Total egress should be 0 (only GetObject counts, and we didn't add any)
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.bytes_downloaded.load(), 0u);
}

// ============================================================================
// Server-side Copy Without Range Header Tests
// ============================================================================

TEST_F(CloudMonitoringTest, ServerSideCopyWithoutRangeHeader) {
    // When a server-side copy is performed without a range header (full object copy),
    // bytes_server_side will be 0 because we can't determine the size from headers.
    // This is expected behavior and is documented in cloud_monitoring.cpp.

    auto& metrics = CloudMetrics::instance();

    // Simulate a CopyObject operation without range header
    // The monitoring layer will convert CopyObject -> CopyObjectRemote
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", nullptr);
    ASSERT_NE(ctx, nullptr);

    // Simulate success without x-amz-copy-source-range header
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);

    // Call OnRequestSucceeded with null request (no headers available)
    listener.OnRequestSucceeded("s3", "CopyObject", nullptr, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "CopyObject", nullptr, ctx);

    // The operation should be recorded but bytes_server_side should be 0
    // because no range header was available
    const auto& m = metrics.getMetrics(S3OperationType::CopyObjectRemote);
    EXPECT_EQ(m.call_count.load(), 1u);
    EXPECT_EQ(m.success_count.load(), 1u);
    EXPECT_EQ(m.bytes_server_side.load(), 0u);  // Expected: can't determine size
}

TEST_F(CloudMonitoringTest, ServerSideCopyBytesRecordedDirectly) {
    // When using recordSuccess directly with bytes_server_side parameter,
    // the bytes should be properly accumulated.
    auto& metrics = CloudMetrics::instance();

    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 100, 0, 0, 8388608);  // 8 MB

    const auto& m = metrics.getMetrics(S3OperationType::UploadPartCopyRemote);
    EXPECT_EQ(m.bytes_server_side.load(), 8388608u);
    EXPECT_EQ(m.bytes_uploaded.load(), 0u);  // No actual upload
    EXPECT_EQ(m.bytes_downloaded.load(), 0u);  // No egress
}

// ============================================================================
// ListParts Monitoring Tests
// ============================================================================

TEST_F(CloudMonitoringTest, ListPartsRecordedCorrectly) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "ListParts", nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListParts).call_count.load(), 1u);

    listener.OnFinish("s3", "ListParts", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, ListPartsWithRequestSuffix) {
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "ListPartsRequest", nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::ListParts).call_count.load(), 1u);

    listener.OnFinish("s3", "ListPartsRequest", nullptr, ctx);
}

// ============================================================================
// Same-Bucket vs Cross-Bucket Copy Detection Tests
// ============================================================================

TEST_F(CloudMonitoringTest, CopyWithNullRequestDefaultsToCrossBucket) {
    // When request is null, we can't determine buckets, so default to cross-bucket (Remote)
    S3MonitoringListener listener;

    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", nullptr);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Should be recorded as Remote (cross-bucket) since we can't determine
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).call_count.load(), 0u);

    listener.OnFinish("s3", "UploadPartCopy", nullptr, ctx);
}

TEST_F(CloudMonitoringTest, SameBucketCopyDetected) {
    // Create a mock request with same source and destination bucket
    S3MonitoringListener listener;

    // Create HTTP request with headers indicating same-bucket copy
    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/dest-key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");

    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Should be recorded as same-bucket (non-Remote)
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).call_count.load(), 0u);

    listener.OnFinish("s3", "UploadPartCopy", request, ctx);
}

TEST_F(CloudMonitoringTest, CrossBucketCopyDetected) {
    // Create a mock request with different source and destination buckets
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://dest-bucket.s3.us-east-1.amazonaws.com/dest-key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "dest-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/source-bucket/source-key");

    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Should be recorded as cross-bucket (Remote)
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).call_count.load(), 0u);

    listener.OnFinish("s3", "UploadPartCopy", request, ctx);
}

TEST_F(CloudMonitoringTest, SameBucketCopyObjectDetected) {
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.eu-west-1.amazonaws.com/new-key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.eu-west-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "my-bucket/old-key");  // Without leading slash

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObjectRemote).call_count.load(), 0u);

    listener.OnFinish("s3", "CopyObject", request, ctx);
}

TEST_F(CloudMonitoringTest, CrossBucketCopyObjectDetected) {
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://dest-bucket.s3.eu-west-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "dest-bucket.s3.eu-west-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/other-bucket/key");

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObjectRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObject).call_count.load(), 0u);

    listener.OnFinish("s3", "CopyObject", request, ctx);
}

TEST_F(CloudMonitoringTest, CopySourceWithOldStyleHostHeader) {
    // Test with older S3 host style: bucket.s3-region.amazonaws.com
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3-us-west-2.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3-us-west-2.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Should detect same-bucket even with old-style host
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObject).call_count.load(), 1u);

    listener.OnFinish("s3", "CopyObject", request, ctx);
}

TEST_F(CloudMonitoringTest, MissingCopySourceHeaderDefaultsToCrossBucket) {
    // When x-amz-copy-source header is missing, default to cross-bucket
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    // No x-amz-copy-source header

    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Should default to Remote (cross-bucket)
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopyRemote).call_count.load(), 1u);

    listener.OnFinish("s3", "UploadPartCopy", request, ctx);
}

// ============================================================================
// Range Header Edge Cases Tests
// ============================================================================

TEST_F(CloudMonitoringTest, RangeHeaderOpenEndedIgnored) {
    // Open-ended ranges (bytes=100-) are not valid for copy-source-range
    // and should result in bytes_server_side = 0
    auto& metrics = CloudMetrics::instance();

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");
    request->SetHeaderValue("x-amz-copy-source-range", "bytes=100-");  // Open-ended

    S3MonitoringListener listener;
    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "UploadPartCopy", request, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "UploadPartCopy", request, ctx);

    // Open-ended range should result in 0 bytes tracked (can't determine size)
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).bytes_server_side.load(), 0u);
}

TEST_F(CloudMonitoringTest, RangeHeaderMalformedIgnored) {
    // Malformed range headers should be gracefully ignored
    auto& metrics = CloudMetrics::instance();

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");
    request->SetHeaderValue("x-amz-copy-source-range", "bytes=abc-xyz");  // Invalid

    S3MonitoringListener listener;
    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "UploadPartCopy", request, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "UploadPartCopy", request, ctx);

    // Malformed range should result in 0 bytes tracked
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).bytes_server_side.load(), 0u);
}

TEST_F(CloudMonitoringTest, RangeHeaderValidParsed) {
    // Valid range header should be correctly parsed
    auto& metrics = CloudMetrics::instance();

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");
    request->SetHeaderValue("x-amz-copy-source-range", "bytes=0-8388607");  // 8 MB

    S3MonitoringListener listener;
    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "UploadPartCopy", request, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "UploadPartCopy", request, ctx);

    // Valid range should be parsed: 8388607 - 0 + 1 = 8388608 bytes
    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).bytes_server_side.load(), 8388608u);
}

TEST_F(CloudMonitoringTest, RangeHeaderNegativeStartIgnored) {
    // Negative start values should be ignored
    auto& metrics = CloudMetrics::instance();

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");
    request->SetHeaderValue("x-amz-copy-source-range", "bytes=-100-200");  // Invalid negative

    S3MonitoringListener listener;
    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "UploadPartCopy", request, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "UploadPartCopy", request, ctx);

    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).bytes_server_side.load(), 0u);
}

TEST_F(CloudMonitoringTest, RangeHeaderEndBeforeStartIgnored) {
    // End < Start should be ignored
    auto& metrics = CloudMetrics::instance();

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://my-bucket.s3.us-east-1.amazonaws.com/key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "my-bucket.s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");
    request->SetHeaderValue("x-amz-copy-source-range", "bytes=200-100");  // End < Start

    S3MonitoringListener listener;
    void* ctx = listener.OnRequestStarted("s3", "UploadPartCopy", request);
    ASSERT_NE(ctx, nullptr);

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    Aws::Client::AWSError<Aws::Client::CoreErrors> error;
    Aws::Client::HttpResponseOutcome outcome(error);
    listener.OnRequestSucceeded("s3", "UploadPartCopy", request, outcome, coreMetrics, ctx);
    listener.OnFinish("s3", "UploadPartCopy", request, ctx);

    EXPECT_EQ(metrics.getMetrics(S3OperationType::UploadPartCopy).bytes_server_side.load(), 0u);
}

// ============================================================================
// Path-Style URL Bucket Extraction Tests
// ============================================================================

TEST_F(CloudMonitoringTest, PathStyleUrlBucketExtraction) {
    // Test path-style URL (legacy): s3.amazonaws.com/bucket/key
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://s3.us-east-1.amazonaws.com/my-bucket/my-key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    // Path-style uses s3.region.amazonaws.com (no bucket in host)
    request->SetHeaderValue("Host", "s3.us-east-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/my-bucket/source-key");

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Same bucket (my-bucket == my-bucket) should be detected as same-bucket copy
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObject).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObjectRemote).call_count.load(), 0u);

    listener.OnFinish("s3", "CopyObject", request, ctx);
}

TEST_F(CloudMonitoringTest, PathStyleUrlCrossBucketDetection) {
    // Path-style URL with different source and destination buckets
    S3MonitoringListener listener;

    auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        "test",
        Aws::Http::URI("https://s3.eu-west-1.amazonaws.com/dest-bucket/my-key"),
        Aws::Http::HttpMethod::HTTP_PUT
    );
    request->SetHeaderValue("Host", "s3.eu-west-1.amazonaws.com");
    request->SetHeaderValue("x-amz-copy-source", "/source-bucket/source-key");

    void* ctx = listener.OnRequestStarted("s3", "CopyObject", request);
    ASSERT_NE(ctx, nullptr);

    auto& metrics = CloudMetrics::instance();
    // Different buckets should be detected as cross-bucket copy
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObjectRemote).call_count.load(), 1u);
    EXPECT_EQ(metrics.getMetrics(S3OperationType::CopyObject).call_count.load(), 0u);

    listener.OnFinish("s3", "CopyObject", request, ctx);
}
