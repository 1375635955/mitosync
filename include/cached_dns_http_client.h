#pragma once

#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/curl/CurlHttpClient.h>
#include <aws/core/client/ClientConfiguration.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>

// Custom HTTP client that uses pre-resolved DNS to avoid repeated lookups
class CachedDnsHttpClient : public Aws::Http::CurlHttpClient {
public:
    CachedDnsHttpClient(const Aws::Client::ClientConfiguration& config);

    // Add a DNS resolution to cache (format: "hostname:port:ip1,ip2,...")
    // Thread-safe. Old resolve lists are preserved indefinitely to prevent
    // use-after-free from pooled curl handles that may still reference them.
    static void AddDnsEntry(const std::string& resolveString);

    // Prepare for shutdown - stops applying DNS cache to new requests.
    // Does NOT free memory (curl handles may still hold raw pointers).
    // Call this before Aws::ShutdownAPI() to ensure clean shutdown.
    static void PrepareForShutdown();

protected:
    // Override to add CURLOPT_RESOLVE before each request
    void OverrideOptionsOnConnectionHandle(CURL* connectionHandle) const override;

private:
    static std::mutex s_mutex;
    static std::vector<std::string> s_resolveStrings;
    // Old lists are preserved indefinitely (never freed) because pooled curl
    // handles may still hold raw pointers. Memory is reclaimed at process exit.
    static std::shared_ptr<struct curl_slist> s_resolveList;

    // Creates a new curl_slist from current strings (caller must hold s_mutex)
    static std::shared_ptr<struct curl_slist> BuildResolveList();
};

// Factory that creates CachedDnsHttpClient instances
class CachedDnsHttpClientFactory : public Aws::Http::HttpClientFactory {
public:
    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(
        const Aws::Client::ClientConfiguration& clientConfiguration) const override;

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::String& uri,
        Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& streamFactory) const override;

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::Http::URI& uri,
        Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& streamFactory) const override;
};

// Initialize the custom HTTP client factory for the AWS SDK
// Call this before any AWS SDK operations
void InitCachedDnsHttpClient();

// Pre-resolve DNS for an S3 bucket and add to cache
// Returns true on success
// Resolve the S3 hostname for a bucket up front and seed the connection cache.
//
// endpoint: when non-empty the request targets an S3-compatible gateway rather
// than AWS, and the warmup is skipped - see the definition for why.
bool WarmupS3Dns(const std::string& bucket, const std::string& region,
                 const std::string& endpoint = "");
