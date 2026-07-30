#include "aws_pricing.h"
#include "cloud_metrics.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

using json = nlohmann::json;

// Check if pricing is stale
bool S3Pricing::isStale(int ttl_hours) const {
    if (!valid) return true;

    auto now = std::chrono::system_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::hours>(now - fetched_at);
    return age.count() >= ttl_hours;
}

// Get default pricing for a region (based on AWS pricing as of late 2024)
S3Pricing S3Pricing::getDefault(const std::string& region) {
    S3Pricing pricing;
    pricing.region = region;
    pricing.fetched_at = std::chrono::system_clock::now();
    pricing.valid = true;

    // Default pricing (US regions - adjust for other regions)
    pricing.get_per_1000 = 0.0004;    // $0.0004 per 1,000 GET requests
    pricing.head_per_1000 = 0.0004;   // Same as GET
    pricing.put_per_1000 = 0.005;     // $0.005 per 1,000 PUT/COPY/POST/LIST requests
    pricing.delete_per_1000 = 0.0;    // Free
    pricing.data_out_per_gb = 0.09;   // $0.09 per GB (first 10 TB)
    pricing.data_in_per_gb = 0.0;     // Free

    // Adjust for specific regions (approximate, based on AWS pricing as of late 2024)
    // EU regions have same pricing as US for egress
    if (region.find("ap-") == 0) {
        // Asia Pacific regions
        pricing.data_out_per_gb = 0.12;
    } else if (region.find("sa-") == 0) {
        // South America
        pricing.data_out_per_gb = 0.15;
    }

    return pricing;
}

// Singleton instance
AwsPricingCache& AwsPricingCache::instance() {
    static AwsPricingCache instance;
    return instance;
}

S3Pricing AwsPricingCache::getPricing(const std::string& region) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check in-memory cache first
    auto it = cache_.find(region);
    if (it != cache_.end() && !it->second.isStale()) {
        return it->second;
    }

    // Try to load from disk
    S3Pricing pricing;
    if (loadFromDisk(region, pricing) && !pricing.isStale()) {
        cache_[region] = pricing;
        return pricing;
    }

    // Try to fetch from API (currently just returns defaults)
    pricing = fetchFromApi(region);
    cache_[region] = pricing;

    // Save to disk for future use
    saveToDisk(region, pricing);

    return pricing;
}

double AwsPricingCache::estimateCost(const CloudMetrics& metrics, const std::string& region) {
    S3Pricing pricing = getPricing(region);
    double total_cost = 0.0;

    // Calculate request costs
    auto all_metrics = metrics.getAllMetrics();

    for (const auto& [op, m] : all_metrics) {
        uint64_t calls = m.success_count.load();
        if (calls == 0) continue;

        double cost_per_1000 = 0.0;

        switch (op) {
            case S3OperationType::GetObject:
            case S3OperationType::HeadObject:
                cost_per_1000 = pricing.get_per_1000;
                break;

            case S3OperationType::PutObject:
            case S3OperationType::UploadPart:
            case S3OperationType::CreateMultipartUpload:
            case S3OperationType::UploadPartCopy:
            case S3OperationType::UploadPartCopyRemote:  // Server-side copies charged at PUT pricing
            case S3OperationType::CopyObject:
            case S3OperationType::CopyObjectRemote:      // Server-side copies charged at PUT pricing
            case S3OperationType::CompleteMultipartUpload:
            case S3OperationType::ListObjectsV2:
            case S3OperationType::ListBuckets:
            case S3OperationType::ListParts:             // List operations charged at PUT pricing
            case S3OperationType::ListMultipartUploads:
            case S3OperationType::GetBucketLocation:
                cost_per_1000 = pricing.put_per_1000;
                break;

            case S3OperationType::DeleteObject:
            case S3OperationType::DeleteObjects:
            case S3OperationType::AbortMultipartUpload:
                cost_per_1000 = 0.0;
                break;

            default:
                cost_per_1000 = pricing.put_per_1000;  // Assume PUT pricing for unknown
                break;
        }

        total_cost += (static_cast<double>(calls) / 1000.0) * cost_per_1000;
    }

    // Calculate data transfer costs
    // Note: AWS prices per GB (10^9 bytes), not GiB (2^30 bytes)
    OperationMetrics total = metrics.getTotalMetrics();
    uint64_t bytes_out = total.bytes_downloaded.load();
    double gb_out = static_cast<double>(bytes_out) / 1e9;
    total_cost += gb_out * pricing.data_out_per_gb;

    // Cross-region server-side copies incur data transfer charges
    // Same-region copies (even cross-bucket) are free for data transfer
    if (metrics.isCrossRegion()) {
        uint64_t cross_region_bytes = metrics.getCrossBucketServerSideBytes();
        double cross_region_gb = static_cast<double>(cross_region_bytes) / 1e9;
        total_cost += cross_region_gb * pricing.data_out_per_gb;
    }

    // Data transfer in is free
    // uint64_t bytes_in = total.bytes_uploaded.load();

    return total_cost;
}

double AwsPricingCache::estimateOperationCost(S3OperationType op, uint64_t success_count,
                                               const std::string& region) {
    if (success_count == 0) return 0.0;

    S3Pricing pricing = getPricing(region);
    double cost_per_1000 = 0.0;

    switch (op) {
        case S3OperationType::GetObject:
        case S3OperationType::HeadObject:
            cost_per_1000 = pricing.get_per_1000;
            break;

        case S3OperationType::PutObject:
        case S3OperationType::UploadPart:
        case S3OperationType::CreateMultipartUpload:
        case S3OperationType::UploadPartCopy:
        case S3OperationType::UploadPartCopyRemote:  // Server-side copies charged at PUT pricing
        case S3OperationType::CopyObject:
        case S3OperationType::CopyObjectRemote:      // Server-side copies charged at PUT pricing
        case S3OperationType::CompleteMultipartUpload:
        case S3OperationType::ListObjectsV2:
        case S3OperationType::ListBuckets:
        case S3OperationType::ListParts:             // List operations charged at PUT pricing
        case S3OperationType::ListMultipartUploads:
        case S3OperationType::GetBucketLocation:
            cost_per_1000 = pricing.put_per_1000;
            break;

        case S3OperationType::DeleteObject:
        case S3OperationType::DeleteObjects:
        case S3OperationType::AbortMultipartUpload:
            cost_per_1000 = 0.0;
            break;

        default:
            cost_per_1000 = pricing.put_per_1000;
            break;
    }

    return (static_cast<double>(success_count) / 1000.0) * cost_per_1000;
}

void AwsPricingCache::refreshPricing(const std::string& region) {
    std::lock_guard<std::mutex> lock(mutex_);

    S3Pricing pricing = fetchFromApi(region);
    cache_[region] = pricing;
    saveToDisk(region, pricing);
}

void AwsPricingCache::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

void AwsPricingCache::setCacheDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_dir_ = dir;
}

std::string AwsPricingCache::getCacheDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_dir_;
}

std::string AwsPricingCache::getCachePath() const {
    if (!cache_dir_.empty()) {
        return cache_dir_ + "/pricing_cache.json";
    }

    // Default to ~/.mitosync/pricing_cache.json
    std::string home;
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        home = userprofile;
    }
#else
    const char* home_env = std::getenv("HOME");
    if (home_env) {
        home = home_env;
    }
#endif

    if (home.empty()) {
        return "pricing_cache.json";
    }

    std::string cache_dir = home + "/.mitosync";

    // Create directory if it doesn't exist (ignore EEXIST to avoid TOCTOU race)
    if (mkdir(cache_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        spdlog::debug("Failed to create cache directory {}: {}", cache_dir, std::strerror(errno));
    }

    return cache_dir + "/pricing_cache.json";
}

bool AwsPricingCache::loadFromDisk(const std::string& region, S3Pricing& pricing) {
    try {
        std::string path = getCachePath();
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;
        }

        json j;
        file >> j;

        if (!j.contains(region)) {
            return false;
        }

        const auto& r = j[region];

        pricing.region = region;
        pricing.get_per_1000 = r.value("get_per_1000", 0.0004);
        pricing.head_per_1000 = r.value("head_per_1000", 0.0004);
        pricing.put_per_1000 = r.value("put_per_1000", 0.005);
        pricing.delete_per_1000 = r.value("delete_per_1000", 0.0);
        pricing.data_out_per_gb = r.value("data_out_per_gb", 0.09);
        pricing.data_in_per_gb = r.value("data_in_per_gb", 0.0);

        // Parse timestamp (stored as UTC)
        if (r.contains("fetched_at")) {
            std::string ts = r["fetched_at"].get<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (!ss.fail()) {
                // Use timegm (POSIX) or _mkgmtime (Windows) to interpret as UTC
#ifdef _WIN32
                pricing.fetched_at = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));
#else
                pricing.fetched_at = std::chrono::system_clock::from_time_t(timegm(&tm));
#endif
            } else {
                pricing.fetched_at = std::chrono::system_clock::now();
            }
        } else {
            pricing.fetched_at = std::chrono::system_clock::now();
        }

        pricing.valid = true;
        return true;

    } catch (const std::exception& e) {
        spdlog::debug("Failed to load pricing cache: {}", e.what());
        return false;
    }
}

void AwsPricingCache::saveToDisk(const std::string& region, const S3Pricing& pricing) {
    try {
        std::string path = getCachePath();
        std::string temp_path = path + ".tmp";

        // Load existing cache
        json j;
        std::ifstream in_file(path);
        if (in_file.is_open()) {
            try {
                in_file >> j;
            } catch (...) {
                j = json::object();
            }
            in_file.close();
        }

        // Format timestamp (use thread-safe gmtime_r on POSIX, gmtime_s on Windows)
        auto time_t_val = std::chrono::system_clock::to_time_t(pricing.fetched_at);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &time_t_val);
#else
        gmtime_r(&time_t_val, &tm_buf);
#endif
        std::ostringstream ts;
        ts << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");

        // Add/update region pricing
        j[region] = {
            {"get_per_1000", pricing.get_per_1000},
            {"head_per_1000", pricing.head_per_1000},
            {"put_per_1000", pricing.put_per_1000},
            {"delete_per_1000", pricing.delete_per_1000},
            {"data_out_per_gb", pricing.data_out_per_gb},
            {"data_in_per_gb", pricing.data_in_per_gb},
            {"fetched_at", ts.str()}
        };

        // Write to temp file first, then atomic rename
        std::ofstream out_file(temp_path);
        if (out_file.is_open()) {
            out_file << j.dump(2);
            out_file.close();

            // Atomic rename (overwrites destination on POSIX, need remove on Windows)
#ifdef _WIN32
            std::remove(path.c_str());
#endif
            if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
                spdlog::debug("Failed to rename temp pricing cache: {}", std::strerror(errno));
                std::remove(temp_path.c_str());
            }
        }

    } catch (const std::exception& e) {
        spdlog::debug("Failed to save pricing cache: {}", e.what());
    }
}

S3Pricing AwsPricingCache::fetchFromApi(const std::string& region) {
    // TODO: Implement AWS Pricing API call
    // For now, return default pricing
    //
    // The AWS Pricing API endpoint is:
    // https://pricing.us-east-1.amazonaws.com/offers/v1.0/aws/AmazonS3/current/index.json
    //
    // This is a large JSON file (~50MB) so we'd need to:
    // 1. Use the GetProducts API with filters instead
    // 2. Or parse the regional pricing from the bulk file
    //
    // For simplicity, we use hardcoded defaults that are reasonably accurate

    spdlog::debug("Using default S3 pricing for region '{}' (API fetch not implemented)", region);
    return S3Pricing::getDefault(region);
}

// Format cost as string
std::string formatCost(double cost_usd) {
    std::ostringstream ss;
    if (cost_usd == 0.0) {
        return "$0.00";
    } else if (cost_usd < 0.01) {
        // Show more precision for small values
        ss << "$" << std::fixed << std::setprecision(6) << cost_usd;
    } else if (cost_usd < 1.0) {
        ss << "$" << std::fixed << std::setprecision(4) << cost_usd;
    } else {
        ss << "$" << std::fixed << std::setprecision(2) << cost_usd;
    }
    return ss.str();
}
