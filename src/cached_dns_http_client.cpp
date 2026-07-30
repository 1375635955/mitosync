#include "cached_dns_http_client.h"
#include "dns_cache.h"
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <atomic>
#include <mutex>

// Custom deleter for curl_slist to use with shared_ptr
struct CurlSlistDeleter {
    void operator()(curl_slist* list) const {
        if (list) {
            curl_slist_free_all(list);
        }
    }
};

// Static member initialization
std::mutex CachedDnsHttpClient::s_mutex;
std::vector<std::string> CachedDnsHttpClient::s_resolveStrings;
std::shared_ptr<struct curl_slist> CachedDnsHttpClient::s_resolveList;

// Keep old lists alive indefinitely to prevent use-after-free from pooled curl handles.
//
// SAFETY MODEL:
// - curl_easy_setopt(CURLOPT_RESOLVE) stores a raw pointer to our curl_slist
// - Curl handles are pooled by AWS SDK and reused across requests
// - A pooled handle may use our list pointer long after OverrideOptionsOnConnectionHandle returns
// - We have no way to know when curl is done with a pointer
// - Therefore: NEVER free a curl_slist that was passed to CURLOPT_RESOLVE
//
// Memory impact is minimal: one list per unique hostname (typically 1-2 for S3 operations).
// Lists are only freed at process exit by the OS.
static std::vector<std::shared_ptr<struct curl_slist>> s_oldLists;

// Shutdown flag - once set, new requests won't use the DNS cache
static std::atomic<bool> s_shuttingDown{false};

CachedDnsHttpClient::CachedDnsHttpClient(const Aws::Client::ClientConfiguration& config)
    : CurlHttpClient(config) {
}

std::shared_ptr<struct curl_slist> CachedDnsHttpClient::BuildResolveList() {
    // Must be called with lock held
    curl_slist* raw_list = nullptr;
    for (const auto& entry : s_resolveStrings) {
        raw_list = curl_slist_append(raw_list, entry.c_str());
    }
    return std::shared_ptr<struct curl_slist>(raw_list, CurlSlistDeleter{});
}

void CachedDnsHttpClient::AddDnsEntry(const std::string& resolveString) {
    if (resolveString.empty()) return;

    std::lock_guard<std::mutex> lock(s_mutex);
    // Check if already exists
    for (const auto& entry : s_resolveStrings) {
        if (entry == resolveString) return;
    }
    s_resolveStrings.push_back(resolveString);
    // Preserve old list indefinitely - pooled curl handles may still reference it.
    // See SAFETY MODEL comment above. We never free these lists.
    if (s_resolveList) {
        s_oldLists.push_back(s_resolveList);
    }
    // Build a new list with all entries
    s_resolveList = BuildResolveList();
    spdlog::debug("Added DNS cache entry: {}", resolveString);
}

void CachedDnsHttpClient::PrepareForShutdown() {
    // Acquire lock to synchronize with OverrideOptionsOnConnectionHandle
    std::lock_guard<std::mutex> lock(s_mutex);

    // Mark as shutting down to prevent new requests from using the cache.
    // We intentionally do NOT free any curl_slist data - see SAFETY MODEL above.
    // Memory is reclaimed by the OS at process exit.
    s_shuttingDown.store(true);
    spdlog::debug("DNS cache prepared for shutdown");
}

void CachedDnsHttpClient::OverrideOptionsOnConnectionHandle(CURL* connectionHandle) const {
    // Call parent implementation first
    CurlHttpClient::OverrideOptionsOnConnectionHandle(connectionHandle);

    // Apply DNS cache entries under lock to avoid race with shutdown
    std::lock_guard<std::mutex> lock(s_mutex);

    // Don't apply DNS cache if shutting down - curl will do its own DNS lookup
    if (s_shuttingDown.load()) {
        return;
    }

    if (s_resolveList) {
        curl_easy_setopt(connectionHandle, CURLOPT_RESOLVE, s_resolveList.get());
        spdlog::trace("Applied CURLOPT_RESOLVE with {} entries", s_resolveStrings.size());
    }
}

// Factory implementation
std::shared_ptr<Aws::Http::HttpClient> CachedDnsHttpClientFactory::CreateHttpClient(
    const Aws::Client::ClientConfiguration& clientConfiguration) const {
    return std::make_shared<CachedDnsHttpClient>(clientConfiguration);
}

std::shared_ptr<Aws::Http::HttpRequest> CachedDnsHttpClientFactory::CreateHttpRequest(
    const Aws::String& uri,
    Aws::Http::HttpMethod method,
    const Aws::IOStreamFactory& streamFactory) const {
    auto request = std::make_shared<Aws::Http::Standard::StandardHttpRequest>(uri, method);
    request->SetResponseStreamFactory(streamFactory);
    return request;
}

std::shared_ptr<Aws::Http::HttpRequest> CachedDnsHttpClientFactory::CreateHttpRequest(
    const Aws::Http::URI& uri,
    Aws::Http::HttpMethod method,
    const Aws::IOStreamFactory& streamFactory) const {
    auto request = std::make_shared<Aws::Http::Standard::StandardHttpRequest>(uri, method);
    request->SetResponseStreamFactory(streamFactory);
    return request;
}

void InitCachedDnsHttpClient() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        // NOTE: This MUST be called BEFORE Aws::InitAPI()
        // The AWS SDK ignores SetHttpClientFactory after initialization
        Aws::Http::SetHttpClientFactory(std::make_shared<CachedDnsHttpClientFactory>());
        spdlog::debug("Initialized cached DNS HTTP client factory");
    });
}

bool WarmupS3Dns(const std::string& bucket, const std::string& region,
                 const std::string& endpoint) {
    // The warmup resolves an AWS hostname - bucket.s3.<region>.amazonaws.com -
    // and seeds it on port 443. With a custom endpoint both are wrong: no
    // request will ever go to that host, and the gateway need not even be
    // HTTPS. Resolving it anyway wastes a lookup and puts a misleading AWS
    // hostname in the log while the transfer goes somewhere else.
    //
    // Nothing is lost by skipping. The warmup exists to spread load across the
    // many addresses AWS returns for S3 under high concurrency; a gateway is
    // typically one host, and often a literal IP that needs no resolution.
    if (!endpoint.empty()) {
        spdlog::debug("DNS warmup skipped: using endpoint '{}'", endpoint);
        return true;
    }

    std::string hostname = GetS3Hostname(bucket, region);
    spdlog::info("DNS warmup: resolving '{}'", hostname);

    if (!DnsCache::Instance().Warmup(hostname)) {
        return false;
    }

    // S3 is always accessed over HTTPS
    std::string resolve443 = DnsCache::Instance().GetResolveString(hostname, 443);
    if (!resolve443.empty()) {
        spdlog::info("DNS cache entry: {}", resolve443);
        CachedDnsHttpClient::AddDnsEntry(resolve443);
    }

    return !resolve443.empty();
}
