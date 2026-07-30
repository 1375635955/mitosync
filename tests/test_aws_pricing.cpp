#include <gtest/gtest.h>
#include "aws_pricing.h"
#include "cloud_metrics.h"
#include "cloud_metrics_test_dir.h"
#include "pricing_cache_test_dir.h"
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>
#include <atomic>

// ============================================================================
// formatCost Tests
// ============================================================================

TEST(FormatCostTest, Zero) {
    EXPECT_EQ(formatCost(0.0), "$0.00");
}

TEST(FormatCostTest, VerySmallValues) {
    EXPECT_EQ(formatCost(0.000001), "$0.000001");
    EXPECT_EQ(formatCost(0.00001), "$0.000010");
    EXPECT_EQ(formatCost(0.0001), "$0.000100");
    EXPECT_EQ(formatCost(0.001), "$0.001000");
    EXPECT_EQ(formatCost(0.009), "$0.009000");
}

TEST(FormatCostTest, SmallValues) {
    EXPECT_EQ(formatCost(0.01), "$0.0100");
    EXPECT_EQ(formatCost(0.1), "$0.1000");
    EXPECT_EQ(formatCost(0.99), "$0.9900");
}

TEST(FormatCostTest, LargeValues) {
    EXPECT_EQ(formatCost(1.0), "$1.00");
    EXPECT_EQ(formatCost(10.5), "$10.50");
    EXPECT_EQ(formatCost(100.0), "$100.00");
    EXPECT_EQ(formatCost(1234.56), "$1234.56");
}

// Note: formatBytes was removed. The CLI formats byte counts with the private
// format_bytes helper in src/main.cpp.

// ============================================================================
// S3Pricing Tests
// ============================================================================

TEST(S3PricingTest, DefaultValues) {
    S3Pricing pricing;
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.0004);
    EXPECT_DOUBLE_EQ(pricing.head_per_1000, 0.0004);
    EXPECT_DOUBLE_EQ(pricing.put_per_1000, 0.005);
    EXPECT_DOUBLE_EQ(pricing.delete_per_1000, 0.0);
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.09);
    EXPECT_DOUBLE_EQ(pricing.data_in_per_gb, 0.0);
    EXPECT_FALSE(pricing.valid);
}

TEST(S3PricingTest, IsStale) {
    S3Pricing pricing;
    pricing.valid = true;
    pricing.fetched_at = std::chrono::system_clock::now();

    // Fresh pricing should not be stale
    EXPECT_FALSE(pricing.isStale(24));

    // Invalid pricing is always stale
    pricing.valid = false;
    EXPECT_TRUE(pricing.isStale(24));
}

TEST(S3PricingTest, IsStaleAfterTTL) {
    S3Pricing pricing;
    pricing.valid = true;

    // Set fetched_at to 25 hours ago
    pricing.fetched_at = std::chrono::system_clock::now() - std::chrono::hours(25);

    EXPECT_TRUE(pricing.isStale(24));
    EXPECT_FALSE(pricing.isStale(26));
}

TEST(S3PricingTest, GetDefaultUSRegion) {
    auto pricing = S3Pricing::getDefault("us-east-1");

    EXPECT_EQ(pricing.region, "us-east-1");
    EXPECT_TRUE(pricing.valid);
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.0004);
    EXPECT_DOUBLE_EQ(pricing.put_per_1000, 0.005);
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.09);
}

TEST(S3PricingTest, GetDefaultEURegion) {
    auto pricing = S3Pricing::getDefault("eu-west-1");

    EXPECT_EQ(pricing.region, "eu-west-1");
    EXPECT_TRUE(pricing.valid);
    // EU regions have same pricing as US (default)
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.09);
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.0004);
    EXPECT_DOUBLE_EQ(pricing.put_per_1000, 0.005);
}

TEST(S3PricingTest, GetDefaultAPRegion) {
    auto pricing = S3Pricing::getDefault("ap-northeast-1");

    EXPECT_EQ(pricing.region, "ap-northeast-1");
    EXPECT_TRUE(pricing.valid);
    // AP regions are more expensive
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.12);
}

TEST(S3PricingTest, GetDefaultSARegion) {
    auto pricing = S3Pricing::getDefault("sa-east-1");

    EXPECT_EQ(pricing.region, "sa-east-1");
    EXPECT_TRUE(pricing.valid);
    // SA region is most expensive
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.15);
}

// ============================================================================
// AwsPricingCache Tests
// ============================================================================

class AwsPricingCacheTest : public ::testing::Test {
protected:
    ScopedCloudMetricsDir metrics_dir_;  // issue #41
    // getPricing() writes through to disk on a miss, so this fixture needs the
    // cache redirected too or it leaves ~/.mitosync/pricing_cache.json behind.
    ScopedPricingCacheDir pricing_dir_;

    void SetUp() override {
        CloudMetrics::instance().clear();
        AwsPricingCache::instance().clearCache();
    }

    void TearDown() override {
        CloudMetrics::instance().clear();
        AwsPricingCache::instance().clearCache();
    }
};

TEST_F(AwsPricingCacheTest, Singleton) {
    auto& instance1 = AwsPricingCache::instance();
    auto& instance2 = AwsPricingCache::instance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(AwsPricingCacheTest, GetPricingReturnsValidPricing) {
    auto& cache = AwsPricingCache::instance();

    auto pricing = cache.getPricing("us-east-1");

    EXPECT_TRUE(pricing.valid);
    EXPECT_EQ(pricing.region, "us-east-1");
}

TEST_F(AwsPricingCacheTest, GetPricingCachesResults) {
    auto& cache = AwsPricingCache::instance();

    // First call
    auto pricing1 = cache.getPricing("us-west-2");

    // Second call should return cached result
    auto pricing2 = cache.getPricing("us-west-2");

    EXPECT_EQ(pricing1.region, pricing2.region);
    EXPECT_DOUBLE_EQ(pricing1.get_per_1000, pricing2.get_per_1000);
}

TEST_F(AwsPricingCacheTest, EstimateCostZeroCalls) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    double cost = cache.estimateCost(metrics, "us-east-1");
    EXPECT_DOUBLE_EQ(cost, 0.0);
}

TEST_F(AwsPricingCacheTest, EstimateCostEmptyRegion) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record some activity
    for (int i = 0; i < 100; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 1024);
    }

    // Empty region should still work (uses default pricing)
    double cost = cache.estimateCost(metrics, "");
    EXPECT_GT(cost, 0.0);  // Should have some cost from data transfer
}

TEST_F(AwsPricingCacheTest, EstimateCostGetRequests) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 GET requests
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // 1000 GET requests @ $0.0004 per 1000 = $0.0004
    EXPECT_NEAR(cost, 0.0004, 0.00001);
}

TEST_F(AwsPricingCacheTest, EstimateCostPutRequests) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 PUT requests
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::PutObject);
        metrics.recordSuccess(S3OperationType::PutObject, 10, 1024, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // 1000 PUT requests @ $0.005 per 1000 = $0.005
    EXPECT_NEAR(cost, 0.005, 0.00001);
}

TEST_F(AwsPricingCacheTest, EstimateCostDataTransfer) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Download 1 GB (1e9 bytes) - AWS uses decimal GB for pricing
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 1'000'000'000ULL);

    double cost = cache.estimateCost(metrics, "us-east-1");

    // 1 GET request (negligible) + 1 GB out @ $0.09 = ~$0.09
    EXPECT_NEAR(cost, 0.09, 0.001);
}

TEST_F(AwsPricingCacheTest, EstimateCostMixedOperations) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // 1000 GET + 1000 HEAD + 1000 PUT + 1000 DELETE
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 1024);  // 1 KB each

        metrics.recordStart(S3OperationType::HeadObject);
        metrics.recordSuccess(S3OperationType::HeadObject, 5, 0, 0);

        metrics.recordStart(S3OperationType::PutObject);
        metrics.recordSuccess(S3OperationType::PutObject, 15, 1024, 0);

        metrics.recordStart(S3OperationType::DeleteObject);
        metrics.recordSuccess(S3OperationType::DeleteObject, 5, 0, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // GET: 1000 @ $0.0004/1000 = $0.0004
    // HEAD: 1000 @ $0.0004/1000 = $0.0004
    // PUT: 1000 @ $0.005/1000 = $0.005
    // DELETE: free
    // Data out: 1000 KB = ~0.00095 GB @ $0.09/GB = ~$0.000086
    // Total: ~$0.0059
    EXPECT_GT(cost, 0.005);
    EXPECT_LT(cost, 0.007);
}

TEST_F(AwsPricingCacheTest, EstimateCostFailedRequestsNotCounted) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 failed GET requests
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        metrics.recordFailure(S3OperationType::GetObject, 10);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Failed requests should not be counted for cost (they weren't successful)
    EXPECT_DOUBLE_EQ(cost, 0.0);
}

TEST_F(AwsPricingCacheTest, EstimateCostListOperations) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 LIST requests
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::ListObjectsV2);
        metrics.recordSuccess(S3OperationType::ListObjectsV2, 20, 0, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // LIST is classified as PUT pricing: 1000 @ $0.005/1000 = $0.005
    EXPECT_NEAR(cost, 0.005, 0.00001);
}

TEST_F(AwsPricingCacheTest, EstimateCostServerSideCopy) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 server-side copy requests (charged at PUT pricing)
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::UploadPartCopyRemote);
        metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 10, 0, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Server-side copies charged at PUT pricing: 1000 @ $0.005/1000 = $0.005
    EXPECT_NEAR(cost, 0.005, 0.00001);
}

TEST_F(AwsPricingCacheTest, EstimateOperationCostServerSideCopy) {
    auto& cache = AwsPricingCache::instance();

    // 1000 server-side copy requests should cost $0.005 (PUT pricing)
    double cost = cache.estimateOperationCost(
        S3OperationType::UploadPartCopyRemote, 1000, "us-east-1");
    EXPECT_NEAR(cost, 0.005, 0.00001);

    // UploadPartCopy also charged at PUT pricing
    double local_cost = cache.estimateOperationCost(
        S3OperationType::UploadPartCopy, 1000, "us-east-1");
    EXPECT_NEAR(local_cost, 0.005, 0.00001);
}

TEST_F(AwsPricingCacheTest, RefreshPricing) {
    auto& cache = AwsPricingCache::instance();

    // Get initial pricing
    auto pricing1 = cache.getPricing("us-east-1");

    // Refresh
    cache.refreshPricing("us-east-1");

    // Get refreshed pricing
    auto pricing2 = cache.getPricing("us-east-1");

    // Both should be valid
    EXPECT_TRUE(pricing1.valid);
    EXPECT_TRUE(pricing2.valid);
}

TEST_F(AwsPricingCacheTest, ClearCache) {
    auto& cache = AwsPricingCache::instance();

    cache.getPricing("us-east-1");
    cache.getPricing("eu-west-1");

    cache.clearCache();

    // After clearing, getting pricing should work (fetches defaults again)
    auto pricing = cache.getPricing("us-east-1");
    EXPECT_TRUE(pricing.valid);
}

// ============================================================================
// Disk Persistence Tests
// ============================================================================

#include <fstream>
#include <cstdlib>
#include <filesystem>

class AwsPricingCacheDiskTest : public ::testing::Test {
protected:
    ScopedCloudMetricsDir metrics_dir_;  // issue #41
    std::string test_cache_dir_;

    void SetUp() override {
        CloudMetrics::instance().clear();
        AwsPricingCache::instance().clearCache();

        // Create a temporary directory for test cache
        test_cache_dir_ = std::filesystem::temp_directory_path().string() + "/mito_pricing_disk_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(test_cache_dir_);
        AwsPricingCache::instance().setCacheDir(test_cache_dir_);
    }

    void TearDown() override {
        CloudMetrics::instance().clear();
        AwsPricingCache::instance().clearCache();
        AwsPricingCache::instance().setCacheDir("");  // Reset to default

        // Clean up temp directory
        std::error_code ec;
        std::filesystem::remove_all(test_cache_dir_, ec);
    }
};

TEST_F(AwsPricingCacheDiskTest, PricingPersistedToDisk) {
    auto& cache = AwsPricingCache::instance();

    // Get pricing (should create cache file)
    cache.getPricing("us-east-1");

    // Check that the cache file was created
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    EXPECT_TRUE(std::filesystem::exists(cache_file));

    // Verify file contains expected content
    std::ifstream file(cache_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_TRUE(content.find("us-east-1") != std::string::npos);
    EXPECT_TRUE(content.find("get_per_1000") != std::string::npos);
}

TEST_F(AwsPricingCacheDiskTest, PricingLoadedFromDisk) {
    auto& cache = AwsPricingCache::instance();

    // Write a custom pricing to disk
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    {
        std::ofstream file(cache_file);
        file << R"({
            "test-region-1": {
                "get_per_1000": 0.001,
                "head_per_1000": 0.001,
                "put_per_1000": 0.01,
                "delete_per_1000": 0.0,
                "data_out_per_gb": 0.15,
                "data_in_per_gb": 0.0,
                "fetched_at": "2099-01-01T00:00:00Z"
            }
        })";
    }

    // Get pricing - should load from disk
    auto pricing = cache.getPricing("test-region-1");

    EXPECT_TRUE(pricing.valid);
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.001);
    EXPECT_DOUBLE_EQ(pricing.data_out_per_gb, 0.15);
}

TEST_F(AwsPricingCacheDiskTest, StalePricingRefreshed) {
    auto& cache = AwsPricingCache::instance();

    // Write old pricing to disk (dated far in the past)
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    {
        std::ofstream file(cache_file);
        file << R"({
            "old-region": {
                "get_per_1000": 0.999,
                "head_per_1000": 0.999,
                "put_per_1000": 0.999,
                "delete_per_1000": 0.999,
                "data_out_per_gb": 0.999,
                "data_in_per_gb": 0.999,
                "fetched_at": "2000-01-01T00:00:00Z"
            }
        })";
    }

    // Get pricing - should detect stale cache and return fresh defaults
    auto pricing = cache.getPricing("old-region");

    EXPECT_TRUE(pricing.valid);
    // Should have fresh default values, not the stale 0.999 values
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.0004);
}

TEST_F(AwsPricingCacheDiskTest, MultipleRegionsPersisted) {
    auto& cache = AwsPricingCache::instance();

    // Get pricing for multiple regions
    cache.getPricing("us-east-1");
    cache.getPricing("eu-west-1");
    cache.getPricing("ap-northeast-1");

    // Read back the cache file
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    std::ifstream file(cache_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // All regions should be present
    EXPECT_TRUE(content.find("us-east-1") != std::string::npos);
    EXPECT_TRUE(content.find("eu-west-1") != std::string::npos);
    EXPECT_TRUE(content.find("ap-northeast-1") != std::string::npos);
}

TEST_F(AwsPricingCacheDiskTest, CorruptedCacheHandled) {
    auto& cache = AwsPricingCache::instance();

    // Write corrupted JSON to disk
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    {
        std::ofstream file(cache_file);
        file << "{ this is not valid json !!!";
    }

    // Should not crash, should return valid default pricing
    auto pricing = cache.getPricing("us-east-1");
    EXPECT_TRUE(pricing.valid);
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.0004);
}

TEST_F(AwsPricingCacheDiskTest, MissingFieldsUseDefaults) {
    auto& cache = AwsPricingCache::instance();

    // Write partial pricing to disk (missing some fields)
    std::string cache_file = test_cache_dir_ + "/pricing_cache.json";
    {
        std::ofstream file(cache_file);
        file << R"({
            "partial-region": {
                "get_per_1000": 0.002,
                "fetched_at": "2099-01-01T00:00:00Z"
            }
        })";
    }

    auto pricing = cache.getPricing("partial-region");

    EXPECT_TRUE(pricing.valid);
    EXPECT_DOUBLE_EQ(pricing.get_per_1000, 0.002);  // From file
    EXPECT_DOUBLE_EQ(pricing.put_per_1000, 0.005);  // Default (not in file)
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_F(AwsPricingCacheTest, ConcurrentGetPricing) {
    auto& cache = AwsPricingCache::instance();

    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};
    std::atomic<int> success_count{0};

    // Multiple threads getting pricing for different regions
    const char* regions[] = {"us-east-1", "eu-west-1", "ap-northeast-1", "sa-east-1"};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            while (!start_flag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < ops_per_thread; ++i) {
                const char* region = regions[(t + i) % 4];
                auto pricing = cache.getPricing(region);
                if (pricing.valid) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    start_flag.store(true);

    for (auto& t : threads) {
        t.join();
    }

    // All operations should succeed
    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);
}

TEST_F(AwsPricingCacheTest, ConcurrentEstimateCost) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Set up some metrics
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        metrics.recordSuccess(S3OperationType::GetObject, 10, 0, 1024);
    }

    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};
    std::atomic<int> valid_costs{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!start_flag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < ops_per_thread; ++i) {
                double cost = cache.estimateCost(metrics, "us-east-1");
                if (cost > 0) {
                    valid_costs.fetch_add(1);
                }
            }
        });
    }

    start_flag.store(true);

    for (auto& t : threads) {
        t.join();
    }

    // All cost calculations should return valid values
    EXPECT_EQ(valid_costs.load(), num_threads * ops_per_thread);
}

TEST_F(AwsPricingCacheTest, ConcurrentRefreshAndGet) {
    auto& cache = AwsPricingCache::instance();

    constexpr int num_threads = 4;
    constexpr int ops_per_thread = 50;

    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};
    std::atomic<int> success_count{0};

    // Half threads refresh, half get
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            while (!start_flag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < ops_per_thread; ++i) {
                if (t % 2 == 0) {
                    cache.refreshPricing("us-east-1");
                } else {
                    auto pricing = cache.getPricing("us-east-1");
                    if (pricing.valid) {
                        success_count.fetch_add(1);
                    }
                }
            }
        });
    }

    start_flag.store(true);

    for (auto& t : threads) {
        t.join();
    }

    // Get operations should all succeed
    EXPECT_EQ(success_count.load(), (num_threads / 2) * ops_per_thread);
}

// ============================================================================
// ListParts Pricing Tests
// ============================================================================

TEST_F(AwsPricingCacheTest, EstimateCostListParts) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Record 1000 ListParts requests
    for (int i = 0; i < 1000; ++i) {
        metrics.recordStart(S3OperationType::ListParts);
        metrics.recordSuccess(S3OperationType::ListParts, 15, 0, 0);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // ListParts is classified as PUT pricing: 1000 @ $0.005/1000 = $0.005
    EXPECT_NEAR(cost, 0.005, 0.00001);
}

TEST_F(AwsPricingCacheTest, EstimateOperationCostListParts) {
    auto& cache = AwsPricingCache::instance();

    // 1000 ListParts requests should cost $0.005 (PUT pricing)
    double cost = cache.estimateOperationCost(
        S3OperationType::ListParts, 1000, "us-east-1");
    EXPECT_NEAR(cost, 0.005, 0.00001);
}

// ============================================================================
// Integration Tests: Monitoring -> Metrics -> Pricing
// ============================================================================

TEST_F(AwsPricingCacheTest, IntegrationMonitoringToPricing) {
    // This test simulates a complete workflow where operations are recorded
    // through the metrics layer (as the monitoring layer would do) and then
    // cost is calculated using the pricing cache.

    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Simulate a typical directory comparison workflow:
    // - 100 HeadObject calls to check metadata
    // - 50 GetObject calls downloading 100 MB total
    // - 10 ListObjectsV2 calls

    for (int i = 0; i < 100; ++i) {
        metrics.recordStart(S3OperationType::HeadObject);
        metrics.recordSuccess(S3OperationType::HeadObject, 20, 0, 0);
    }

    for (int i = 0; i < 50; ++i) {
        metrics.recordStart(S3OperationType::GetObject);
        // Each GetObject downloads ~2 MB (100 MB / 50)
        metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 2 * 1024 * 1024);
    }

    for (int i = 0; i < 10; ++i) {
        metrics.recordStart(S3OperationType::ListObjectsV2);
        metrics.recordSuccess(S3OperationType::ListObjectsV2, 50, 0, 0);
    }

    // Calculate cost
    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost breakdown:
    // HEAD: 100 @ $0.0004/1000 = $0.00004
    // GET: 50 @ $0.0004/1000 = $0.00002
    // LIST: 10 @ $0.005/1000 = $0.00005
    // Data out: 100 MB = 0.1 GB @ $0.09/GB = $0.009
    // Total: ~$0.00911

    EXPECT_GT(cost, 0.009);
    EXPECT_LT(cost, 0.010);

    // Verify metrics are as expected
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 160u);
    EXPECT_EQ(total.success_count.load(), 160u);
    EXPECT_EQ(total.bytes_downloaded.load(), 100u * 1024 * 1024);
}

TEST_F(AwsPricingCacheTest, IntegrationServerSideCopyCostEstimation) {
    // Test that server-side copies are correctly priced (PUT pricing, no egress)

    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Simulate a multipart copy operation:
    // - 1 CreateMultipartUpload
    // - 100 UploadPartCopyRemote (8 MB parts = 800 MB server-side transfer)
    // - 1 CompleteMultipartUpload

    metrics.recordStart(S3OperationType::CreateMultipartUpload);
    metrics.recordSuccess(S3OperationType::CreateMultipartUpload, 50, 0, 0);

    for (int i = 0; i < 100; ++i) {
        metrics.recordStart(S3OperationType::UploadPartCopyRemote);
        // 8 MB server-side transfer per part
        metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 30, 0, 0, 8 * 1024 * 1024);
    }

    metrics.recordStart(S3OperationType::CompleteMultipartUpload);
    metrics.recordSuccess(S3OperationType::CompleteMultipartUpload, 100, 0, 0);

    // Verify metrics
    auto total = metrics.getTotalMetrics();
    EXPECT_EQ(total.call_count.load(), 102u);
    EXPECT_EQ(total.bytes_server_side.load(), 100u * 8 * 1024 * 1024);
    EXPECT_EQ(total.bytes_downloaded.load(), 0u);  // No egress

    // Calculate cost
    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost:
    // All 102 requests @ PUT pricing: 102 @ $0.005/1000 = $0.00051
    // No egress cost (server-side copies in same region)
    // Total: ~$0.00051

    EXPECT_NEAR(cost, 0.00051, 0.0001);
}

// ============================================================================
// Cross-Region Cost Estimation Tests
// ============================================================================

TEST_F(AwsPricingCacheTest, EstimateCostCrossRegionServerSideCopy) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Set up cross-region scenario
    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");
    ASSERT_TRUE(metrics.isCrossRegion());

    // Simulate cross-bucket server-side copies (1 GB total)
    for (int i = 0; i < 100; ++i) {
        metrics.recordStart(S3OperationType::UploadPartCopyRemote);
        // 10 MB per part = 1 GB total
        metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 10 * 1024 * 1024);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost:
    // PUT requests: 100 @ $0.005/1000 = $0.0005
    // Cross-region egress: ~1 GB @ $0.09/GB = $0.09
    // Total: ~$0.0905

    // 100 * 10 MB = 1000 MB = 1000 * 1024 * 1024 bytes
    // = 1,048,576,000 bytes / 1e9 = 1.048576 GB
    double expected_egress = 1.048576 * 0.09;  // ~$0.0944
    double expected_requests = 0.0005;
    double expected_total = expected_egress + expected_requests;

    EXPECT_NEAR(cost, expected_total, 0.001);
}

TEST_F(AwsPricingCacheTest, EstimateCostSameRegionNoEgressForCopies) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Set up same-region scenario (single region)
    metrics.setRegion("us-east-1");
    ASSERT_FALSE(metrics.isCrossRegion());

    // Simulate cross-bucket server-side copies (1 GB total)
    for (int i = 0; i < 100; ++i) {
        metrics.recordStart(S3OperationType::UploadPartCopyRemote);
        metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 10 * 1024 * 1024);
    }

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost:
    // PUT requests: 100 @ $0.005/1000 = $0.0005
    // NO egress (same region)
    // Total: $0.0005

    EXPECT_NEAR(cost, 0.0005, 0.0001);
}

TEST_F(AwsPricingCacheTest, EstimateCostCrossRegionOnlyCrossBucketCopiesCharged) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Set up cross-region scenario
    metrics.setRegion("us-east-1");
    metrics.addRegion("ap-northeast-1");  // AP region has higher egress ($0.12/GB)
    ASSERT_TRUE(metrics.isCrossRegion());

    // Same-bucket copies (should NOT incur egress even in cross-region scenario)
    metrics.recordStart(S3OperationType::UploadPartCopy);
    metrics.recordSuccess(S3OperationType::UploadPartCopy, 50, 0, 0, 500 * 1024 * 1024);  // 500 MB

    metrics.recordStart(S3OperationType::CopyObject);
    metrics.recordSuccess(S3OperationType::CopyObject, 50, 0, 0, 500 * 1024 * 1024);  // 500 MB

    // Cross-bucket copies (SHOULD incur egress)
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 100 * 1024 * 1024);  // 100 MB

    metrics.recordStart(S3OperationType::CopyObjectRemote);
    metrics.recordSuccess(S3OperationType::CopyObjectRemote, 50, 0, 0, 100 * 1024 * 1024);  // 100 MB

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost:
    // PUT requests: 4 @ $0.005/1000 = ~$0.00002
    // Cross-bucket egress only: 200 MB = 209,715,200 bytes / 1e9 = 0.2097 GB @ $0.09/GB = ~$0.0189
    // Same-bucket bytes (1 GB) should NOT be charged
    // Total: ~$0.019

    double cross_bucket_gb = (200.0 * 1024 * 1024) / 1e9;
    double expected_egress = cross_bucket_gb * 0.09;
    double expected_requests = 4.0 / 1000.0 * 0.005;
    double expected_total = expected_egress + expected_requests;

    EXPECT_NEAR(cost, expected_total, 0.001);
}

TEST_F(AwsPricingCacheTest, EstimateCostCrossRegionWithGetObjectEgress) {
    auto& cache = AwsPricingCache::instance();
    auto& metrics = CloudMetrics::instance();

    // Set up cross-region scenario
    metrics.setRegion("us-east-1");
    metrics.addRegion("eu-west-1");

    // Regular GetObject egress (always charged)
    metrics.recordStart(S3OperationType::GetObject);
    metrics.recordSuccess(S3OperationType::GetObject, 100, 0, 500 * 1024 * 1024);  // 500 MB download

    // Cross-bucket server-side copy (charged in cross-region)
    metrics.recordStart(S3OperationType::UploadPartCopyRemote);
    metrics.recordSuccess(S3OperationType::UploadPartCopyRemote, 50, 0, 0, 500 * 1024 * 1024);  // 500 MB

    double cost = cache.estimateCost(metrics, "us-east-1");

    // Expected cost:
    // GET request: 1 @ $0.0004/1000 = ~$0.0000004
    // PUT request: 1 @ $0.005/1000 = ~$0.000005
    // GetObject egress: 500 MB @ $0.09/GB
    // Cross-bucket egress: 500 MB @ $0.09/GB
    // Total egress: 1 GB @ $0.09/GB = $0.09 (approximately)

    double total_egress_bytes = 2 * 500.0 * 1024 * 1024;
    double total_egress_gb = total_egress_bytes / 1e9;
    double expected_egress = total_egress_gb * 0.09;

    EXPECT_NEAR(cost, expected_egress, 0.01);  // Requests are negligible
}


// ============================================================================
// The pricing cache must never write to the real home directory in tests
// ============================================================================

TEST_F(AwsPricingCacheTest, WritesItsCacheInsideTheTestDirectory) {
    // Sibling of the CloudMetrics safety test. This one cannot assert that
    // nothing is written - a pricing cache is meant to persist - so it asserts
    // that what gets written lands in the scratch directory, which is the thing
    // a future fixture would forget. getCachePath() is private, so this goes
    // through the behaviour rather than the accessor.
    AwsPricingCache::instance().getPricing("us-east-1");

    const std::filesystem::path cache_file = pricing_dir_.path() / "pricing_cache.json";
    EXPECT_TRUE(std::filesystem::exists(cache_file))
        << "nothing was written to " << cache_file.string()
        << ", so the cache is going somewhere else - most likely the real home directory";
}
