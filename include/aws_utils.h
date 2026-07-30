#pragma once

#include <aws/core/Aws.h>
#include "cached_dns_http_client.h"

// RAII guard to ensure AWS SDK is properly shut down
struct AwsShutdownGuard {
    Aws::SDKOptions& opts;
    ~AwsShutdownGuard() {
        // Prepare DNS cache for shutdown before AWS SDK cleanup.
        // This prevents curl handles from accessing stale DNS cache data.
        CachedDnsHttpClient::PrepareForShutdown();
        Aws::ShutdownAPI(opts);
    }
};
