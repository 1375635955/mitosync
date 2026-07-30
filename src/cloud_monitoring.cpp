#include "cloud_monitoring.h"
#include <spdlog/spdlog.h>

// Helper to check if operation is a server-side copy type (same-bucket or cross-bucket).
static bool isServerSideCopyType(S3OperationType op) {
    return op == S3OperationType::UploadPartCopy ||
           op == S3OperationType::UploadPartCopyRemote ||
           op == S3OperationType::CopyObject ||
           op == S3OperationType::CopyObjectRemote;
}

// Extract bucket name from x-amz-copy-source header.
// Format: "/bucket/key" or "bucket/key" (URL-encoded)
static std::string extractSourceBucket(const std::string& copy_source) {
    if (copy_source.empty()) return "";

    std::string source = copy_source;

    // Remove leading slash if present
    size_t start = 0;
    if (source[0] == '/') {
        start = 1;
    }

    // Find the first slash after the bucket name
    size_t slash_pos = source.find('/', start);
    if (slash_pos == std::string::npos) {
        // No key, just bucket name
        return source.substr(start);
    }

    return source.substr(start, slash_pos - start);
}

// Extract destination bucket from request.
// Virtual-hosted style: bucket.s3.region.amazonaws.com or bucket.s3.amazonaws.com
// Path style: /bucket/key in URI
static std::string extractDestinationBucket(
    const std::shared_ptr<const Aws::Http::HttpRequest>& request
) {
    if (!request) return "";

    // Try Host header first (virtual-hosted style)
    if (request->HasHeader("Host")) {
        std::string host(request->GetHeaderValue("Host").c_str());

        // Pattern: bucket.s3.region.amazonaws.com or bucket.s3.amazonaws.com
        // Find ".s3." to locate bucket name
        size_t s3_pos = host.find(".s3.");
        if (s3_pos != std::string::npos) {
            return host.substr(0, s3_pos);
        }

        // Also check for ".s3-" (older style like bucket.s3-us-west-2.amazonaws.com)
        s3_pos = host.find(".s3-");
        if (s3_pos != std::string::npos) {
            return host.substr(0, s3_pos);
        }
    }

    // Fall back to path style: extract bucket from URI path
    const Aws::Http::URI& uri = request->GetUri();
    std::string path(uri.GetPath().c_str());

    if (!path.empty() && path[0] == '/') {
        size_t second_slash = path.find('/', 1);
        if (second_slash != std::string::npos) {
            return path.substr(1, second_slash - 1);
        } else if (path.length() > 1) {
            return path.substr(1);
        }
    }

    return "";
}

// Determine if a copy operation is same-bucket or cross-bucket.
// Returns true if cross-bucket (Remote), false if same-bucket.
static bool isCrossBucketCopy(
    const std::shared_ptr<const Aws::Http::HttpRequest>& request
) {
    if (!request) {
        // Can't determine, assume cross-bucket (safer for cost estimation)
        return true;
    }

    // Get source bucket from x-amz-copy-source header
    std::string source_bucket;
    if (request->HasHeader("x-amz-copy-source")) {
        source_bucket = extractSourceBucket(
            std::string(request->GetHeaderValue("x-amz-copy-source").c_str()));
    } else if (request->HasHeader("X-Amz-Copy-Source")) {
        source_bucket = extractSourceBucket(
            std::string(request->GetHeaderValue("X-Amz-Copy-Source").c_str()));
    }

    if (source_bucket.empty()) {
        // Can't determine source, assume cross-bucket
        return true;
    }

    // Get destination bucket
    std::string dest_bucket = extractDestinationBucket(request);
    if (dest_bucket.empty()) {
        // Can't determine destination, assume cross-bucket
        return true;
    }

    // URL-decode source bucket for comparison (handles %2F etc.)
    // Simple decode: just compare as-is since bucket names don't need encoding
    bool is_cross = (source_bucket != dest_bucket);

    spdlog::trace("Copy bucket check: source='{}' dest='{}' cross={}",
                  source_bucket, dest_bucket, is_cross);

    return is_cross;
}

void* S3MonitoringListener::OnRequestStarted(
    const Aws::String& serviceName,
    const Aws::String& requestName,
    const std::shared_ptr<const Aws::Http::HttpRequest>& request
) const {
    // Log ALL requests at trace level to diagnose monitoring issues
    spdlog::trace("Monitoring callback: service='{}' request='{}'",
                  std::string(serviceName.c_str()), std::string(requestName.c_str()));

    // Only track S3 service calls
    if (serviceName != "s3" && serviceName != "S3") {
        return nullptr;
    }

    // Create context for this request
    auto* ctx = new S3RequestContext();
    ctx->operation_type = ParseS3OperationType(std::string(requestName.c_str()));
    ctx->start_time = std::chrono::steady_clock::now();
    ctx->service_name = std::string(serviceName.c_str());
    ctx->request_name = std::string(requestName.c_str());

    // Log unknown operations so we can add them to the parser
    if (ctx->operation_type == S3OperationType::Unknown) {
        spdlog::debug("Unknown S3 operation: '{}' (service: {})", ctx->request_name, ctx->service_name);
    }

    // For copy operations, determine if same-bucket or cross-bucket.
    // Same-bucket copies are free (no data transfer charges).
    // Cross-bucket copies may incur cross-region transfer charges.
    if (ctx->operation_type == S3OperationType::UploadPartCopy) {
        if (isCrossBucketCopy(request)) {
            ctx->operation_type = S3OperationType::UploadPartCopyRemote;
            spdlog::debug("Cross-bucket copy: {} -> UploadPartCopyRemote", ctx->request_name);
        } else {
            spdlog::debug("Same-bucket copy: {} -> UploadPartCopy", ctx->request_name);
        }
    } else if (ctx->operation_type == S3OperationType::CopyObject) {
        if (isCrossBucketCopy(request)) {
            ctx->operation_type = S3OperationType::CopyObjectRemote;
            spdlog::debug("Cross-bucket copy: {} -> CopyObjectRemote", ctx->request_name);
        } else {
            spdlog::debug("Same-bucket copy: {} -> CopyObject", ctx->request_name);
        }
    }

    // Record the call start
    CloudMetrics::instance().recordStart(ctx->operation_type);

    spdlog::trace("S3 request started: {} -> {} ({})",
                  ctx->request_name, S3OperationTypeName(ctx->operation_type), ctx->service_name);

    return ctx;
}

void S3MonitoringListener::OnRequestSucceeded(
    const Aws::String& /* serviceName */,
    const Aws::String& /* requestName */,
    const std::shared_ptr<const Aws::Http::HttpRequest>& request,
    const Aws::Client::HttpResponseOutcome& outcome,
    const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
    void* context
) const {
    if (!context) return;

    auto* ctx = static_cast<S3RequestContext*>(context);

    // Calculate latency
    auto now = std::chrono::steady_clock::now();
    int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ctx->start_time).count();

    // Try to get more precise latency from core metrics
    int64_t core_latency = extractLatency(metricsFromCore);
    if (core_latency > 0) {
        latency_ms = core_latency;
    }

    // Extract bytes transferred
    uint64_t bytes_up = extractBytesUploaded(request);
    uint64_t bytes_down = 0;
    uint64_t bytes_server_side = 0;

    // Only GetObject responses contain actual downloaded data.
    // All other operations return small XML/metadata responses that shouldn't
    // count as billable egress. This includes ListObjectsV2, HeadObject,
    // CreateMultipartUpload, UploadPart, etc.
    if (ctx->operation_type == S3OperationType::GetObject) {
        bytes_down = extractBytesDownloaded(outcome);
    }

    // Server-side copies (UploadPartCopyRemote, CopyObjectRemote) don't transfer
    // data through our connection - only XML metadata. Track the copied bytes separately.
    if (isServerSideCopyType(ctx->operation_type)) {
        // Try to determine the copied byte count from available headers
        if (request) {
            Aws::String range_header;

            // Method 1: x-amz-copy-source-range header (e.g., "bytes=0-8388607")
            // This is present for ranged copies (UploadPartCopy with specific byte range)
            if (request->HasHeader("x-amz-copy-source-range")) {
                range_header = request->GetHeaderValue("x-amz-copy-source-range");
            } else if (request->HasHeader("X-Amz-Copy-Source-Range")) {
                range_header = request->GetHeaderValue("X-Amz-Copy-Source-Range");
            }

            if (!range_header.empty()) {
                // Parse "bytes=START-END" format
                // Note: Open-ended ranges (bytes=100-) are not valid for copy-source-range
                std::string range_str(range_header.c_str());
                size_t eq_pos = range_str.find('=');
                if (eq_pos != std::string::npos && eq_pos + 1 < range_str.size()) {
                    size_t dash_pos = range_str.find('-', eq_pos + 1);
                    if (dash_pos != std::string::npos && dash_pos > eq_pos + 1 &&
                        dash_pos + 1 < range_str.size()) {
                        try {
                            int64_t start = std::stoll(range_str.substr(eq_pos + 1, dash_pos - eq_pos - 1));
                            int64_t end = std::stoll(range_str.substr(dash_pos + 1));
                            if (start >= 0 && end >= start) {
                                bytes_server_side = static_cast<uint64_t>(end - start + 1);
                            } else {
                                spdlog::debug("Invalid range values in copy-source-range: start={} end={}", start, end);
                            }
                        } catch (const std::exception& e) {
                            spdlog::debug("Failed to parse copy-source-range header '{}': {}", range_str, e.what());
                        }
                    } else {
                        spdlog::debug("Malformed copy-source-range header (missing end): {}", range_str);
                    }
                }
            }

            // Method 2: For full-object CopyObject, try Content-Length from response
            // Note: This is typically the XML response size, not the copied data size.
            // Full-object copy sizes cannot be determined from the response alone;
            // the caller would need to know the source object size beforehand.
            if (bytes_server_side == 0 && ctx->operation_type == S3OperationType::CopyObjectRemote) {
                // For CopyObject without range, we can't determine the size from headers.
                // The Content-Length in the response is just the XML metadata size.
                // Log at trace level since this is expected for full-object copies.
                spdlog::trace("CopyObject without range: copied bytes unknown (size tracking unavailable)");
            }
        }
        // Zero out any request body size (server-side copies have no upload)
        bytes_up = 0;
    }

    // Record success with the (possibly updated) operation type
    CloudMetrics::instance().recordSuccess(ctx->operation_type, latency_ms, bytes_up, bytes_down, bytes_server_side);

    spdlog::trace("S3 request succeeded: {} ({}) latency={}ms up={}B down={}B",
                  ctx->request_name, S3OperationTypeName(ctx->operation_type),
                  latency_ms, bytes_up, bytes_down);
}

void S3MonitoringListener::OnRequestFailed(
    const Aws::String& /* serviceName */,
    const Aws::String& /* requestName */,
    const std::shared_ptr<const Aws::Http::HttpRequest>& /* request */,
    const Aws::Client::HttpResponseOutcome& /* outcome */,
    const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
    void* context
) const {
    if (!context) return;

    auto* ctx = static_cast<S3RequestContext*>(context);

    // Calculate latency
    auto now = std::chrono::steady_clock::now();
    int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ctx->start_time).count();

    // Try to get more precise latency from core metrics
    int64_t core_latency = extractLatency(metricsFromCore);
    if (core_latency > 0) {
        latency_ms = core_latency;
    }

    // Record failure
    CloudMetrics::instance().recordFailure(ctx->operation_type, latency_ms);

    spdlog::trace("S3 request failed: {} latency={}ms", ctx->request_name, latency_ms);
}

void S3MonitoringListener::OnRequestRetry(
    const Aws::String& /* serviceName */,
    const Aws::String& /* requestName */,
    const std::shared_ptr<const Aws::Http::HttpRequest>& /* request */,
    void* context
) const {
    if (!context) return;

    auto* ctx = static_cast<S3RequestContext*>(context);

    // Record retry
    CloudMetrics::instance().recordRetry(ctx->operation_type);

    spdlog::trace("S3 request retry: {}", ctx->request_name);
}

void S3MonitoringListener::OnFinish(
    const Aws::String& /* serviceName */,
    const Aws::String& /* requestName */,
    const std::shared_ptr<const Aws::Http::HttpRequest>& /* request */,
    void* context
) const {
    if (!context) return;

    // Clean up context
    auto* ctx = static_cast<S3RequestContext*>(context);
    delete ctx;
}

int64_t S3MonitoringListener::extractLatency(
    const Aws::Monitoring::CoreMetricsCollection& metrics
) {
    // Try to get RequestLatency from HTTP client metrics
    const auto& httpMetrics = metrics.httpClientMetrics;

    // Look for RequestLatency metric
    auto it = httpMetrics.find(Aws::Monitoring::GetHttpClientMetricNameByType(
        Aws::Monitoring::HttpClientMetricsType::RequestLatency));

    if (it != httpMetrics.end()) {
        return it->second;
    }

    return 0;
}

uint64_t S3MonitoringListener::extractBytesUploaded(
    const std::shared_ptr<const Aws::Http::HttpRequest>& request
) {
    if (!request) return 0;

    // Get content length from request headers
    if (request->HasHeader("content-length")) {
        try {
            return std::stoull(request->GetHeaderValue("content-length").c_str());
        } catch (const std::exception& e) {
            spdlog::debug("Failed to parse upload content-length: {}", e.what());
            return 0;
        }
    }

    return 0;
}

uint64_t S3MonitoringListener::extractBytesDownloaded(
    const Aws::Client::HttpResponseOutcome& outcome
) {
    if (!outcome.IsSuccess()) return 0;

    const auto& response = outcome.GetResult();
    if (!response) return 0;

    // Get content length from response headers
    if (response->HasHeader("content-length")) {
        try {
            return std::stoull(response->GetHeader("content-length").c_str());
        } catch (const std::exception& e) {
            spdlog::debug("Failed to parse download content-length: {}", e.what());
            return 0;
        }
    }

    return 0;
}

// Factory implementation
Aws::UniquePtr<Aws::Monitoring::MonitoringInterface>
S3MonitoringFactory::CreateMonitoringInstance() const {
    spdlog::debug("S3MonitoringFactory: Creating monitoring instance");
    return Aws::MakeUnique<S3MonitoringListener>("S3MonitoringListener");
}

// Get factory creation function
Aws::Monitoring::MonitoringFactoryCreateFunction GetS3MonitoringFactoryCreateFn() {
    return []() -> Aws::UniquePtr<Aws::Monitoring::MonitoringFactory> {
        return Aws::MakeUnique<S3MonitoringFactory>("S3MonitoringFactory");
    };
}
