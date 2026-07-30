#pragma once

// AWS S3 Pricing API integration with disk caching
// Fetches current S3 prices from AWS Pricing API and caches them locally

#include <string>
#include <map>
#include <mutex>
#include <chrono>
#include <cstdint>

#include "cloud_metrics.h"

// AWS S3 pricing for a specific region
struct S3Pricing {
    // Request pricing (per 1,000 requests)
    double get_per_1000{0.0004};      // GET, SELECT requests (Tier 1)
    double head_per_1000{0.0004};     // HEAD requests (same as GET)
    double put_per_1000{0.005};       // PUT, COPY, POST, LIST requests (Tier 2)
    double delete_per_1000{0.0};      // DELETE requests (free)

    // Data transfer pricing (per GB)
    double data_out_per_gb{0.09};     // Data transfer out to internet
    double data_in_per_gb{0.0};       // Data transfer in (free)

    // Metadata
    std::string region;
    std::chrono::system_clock::time_point fetched_at;
    bool valid{false};

    // Check if pricing is stale (older than TTL)
    bool isStale(int ttl_hours = 24) const;

    // Get default pricing for a region (fallback values)
    static S3Pricing getDefault(const std::string& region);
};

// Pricing cache with disk persistence
class AwsPricingCache {
public:
    // Get singleton instance
    static AwsPricingCache& instance();

    // Get pricing for a region (uses cache, falls back to API, then defaults)
    S3Pricing getPricing(const std::string& region);

    // Calculate estimated cost from cloud metrics
    // Returns cost in USD
    double estimateCost(const CloudMetrics& metrics, const std::string& region);

    // Calculate cost for a specific operation type and count
    // Returns cost in USD
    double estimateOperationCost(S3OperationType op, uint64_t success_count, const std::string& region);

    // Force refresh of pricing for a region
    void refreshPricing(const std::string& region);

    // Clear all cached pricing
    void clearCache();

    // Set cache directory (default: $HOME/.mitosync/)
    void setCacheDir(const std::string& dir);

    // The directory currently in force, empty when it resolves to the default.
    // Lets a test scope restore what it found instead of resetting to the real
    // location - see tests/pricing_cache_test_dir.h.
    std::string getCacheDir() const;

    // Cache TTL in hours (default: 24)
    static constexpr int DEFAULT_CACHE_TTL_HOURS = 24;

private:
    AwsPricingCache() = default;
    ~AwsPricingCache() = default;

    // Prevent copying
    AwsPricingCache(const AwsPricingCache&) = delete;
    AwsPricingCache& operator=(const AwsPricingCache&) = delete;

    // Get path to cache file
    std::string getCachePath() const;

    // Load pricing from disk cache
    bool loadFromDisk(const std::string& region, S3Pricing& pricing);

    // Save pricing to disk cache
    void saveToDisk(const std::string& region, const S3Pricing& pricing);

    // Fetch pricing from AWS Pricing API
    // Returns default pricing if API call fails
    S3Pricing fetchFromApi(const std::string& region);

    // In-memory cache
    std::map<std::string, S3Pricing> cache_;
    mutable std::mutex mutex_;

    // Cache directory
    std::string cache_dir_;
};

// Utility function to format cost as string (e.g., "$0.0023" or "$1.23")
std::string formatCost(double cost_usd);
