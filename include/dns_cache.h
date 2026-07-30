#pragma once

#include <string>
#include <vector>
#include <shared_mutex>
#include <unordered_map>

// Thread-safe DNS cache that resolves hostnames once and stores the results
// Uses shared_mutex for read-heavy workload: many concurrent readers during
// S3 operations, writes only at startup (warmup) and shutdown (clear)
class DnsCache {
public:
    static DnsCache& Instance();

    // Resolve a hostname to IP addresses (returns empty vector on failure)
    // Results are cached for subsequent calls
    std::vector<std::string> Resolve(const std::string& hostname);

    // Get a curl CURLOPT_RESOLVE-style string: "hostname:port:ip1,ip2,..."
    // Returns empty string if hostname hasn't been resolved
    std::string GetResolveString(const std::string& hostname, int port = 443);

    // Pre-warm the cache for a hostname
    bool Warmup(const std::string& hostname);

    // Clear the cache
    void Clear();

private:
    DnsCache() = default;
    mutable std::shared_mutex m_mutex;  // shared for reads, exclusive for writes
    std::unordered_map<std::string, std::vector<std::string>> m_cache;
};

// Resolve a hostname to IP addresses using getaddrinfo
// Returns empty vector on failure
std::vector<std::string> ResolveHostname(const std::string& hostname);

// Extract hostname from an S3 bucket URL (e.g., "bucket.s3.region.amazonaws.com")
std::string GetS3Hostname(const std::string& bucket, const std::string& region);
