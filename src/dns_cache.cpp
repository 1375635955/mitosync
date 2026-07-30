#include "dns_cache.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

DnsCache& DnsCache::Instance() {
    static DnsCache instance;
    return instance;
}

std::vector<std::string> DnsCache::Resolve(const std::string& hostname) {
    // Fast path: shared lock for cache hit (common case during S3 operations)
    {
        std::shared_lock<std::shared_mutex> read_lock(m_mutex);
        auto it = m_cache.find(hostname);
        if (it != m_cache.end()) {
            return it->second;
        }
    }

    // Slow path: cache miss - resolve DNS (outside lock to avoid blocking readers)
    auto ips = ResolveHostname(hostname);

    // Exclusive lock to insert into cache
    // Re-check in case another thread resolved while we were waiting
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    auto it = m_cache.find(hostname);
    if (it != m_cache.end()) {
        // Another thread beat us to it - use their result
        return it->second;
    }

    if (!ips.empty()) {
        m_cache[hostname] = ips;
    }

    return ips;
}

std::string DnsCache::GetResolveString(const std::string& hostname, int port) {
    auto ips = Resolve(hostname);
    if (ips.empty()) {
        return "";
    }

    // Format: "hostname:port:ip1,ip2,..."
    std::string result = hostname + ":" + std::to_string(port) + ":";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) result += ",";
        result += ips[i];
    }
    return result;
}

bool DnsCache::Warmup(const std::string& hostname) {
    auto ips = Resolve(hostname);
    if (!ips.empty()) {
        spdlog::debug("DNS warmup for '{}': resolved to {} IP(s)", hostname, ips.size());
        for (const auto& ip : ips) {
            spdlog::debug("  -> {}", ip);
        }
        return true;
    }
    spdlog::warn("DNS warmup failed for '{}'", hostname);
    return false;
}

void DnsCache::Clear() {
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_cache.clear();
}

std::vector<std::string> ResolveHostname(const std::string& hostname) {
    std::vector<std::string> results;

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    if (status != 0) {
        spdlog::error("DNS resolution failed for '{}': {}", hostname, gai_strerror(status));
        return results;
    }

    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        void* addr;

        if (p->ai_family == AF_INET) {
            struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            addr = &(ipv4->sin_addr);
        } else {
            struct sockaddr_in6* ipv6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            addr = &(ipv6->sin6_addr);
        }

        if (inet_ntop(p->ai_family, addr, ip, sizeof(ip)) != nullptr) {
            results.push_back(ip);
        }
    }

    freeaddrinfo(res);

    // Remove duplicates
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    return results;
}

std::string GetS3Hostname(const std::string& bucket, const std::string& region) {
    // S3 virtual-hosted style: bucket.s3.region.amazonaws.com
    return bucket + ".s3." + region + ".amazonaws.com";
}
