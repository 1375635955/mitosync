#pragma once

// AWS SDK Monitoring Interface implementation for tracking S3 API calls
// This hooks into the AWS SDK's built-in monitoring system to capture all API calls

#include <aws/core/monitoring/MonitoringInterface.h>
#include <aws/core/monitoring/MonitoringFactory.h>
#include <aws/core/monitoring/MonitoringManager.h>
#include <aws/core/monitoring/CoreMetrics.h>
#include <aws/core/monitoring/HttpClientMetrics.h>

#include "cloud_metrics.h"

// Context passed between monitoring callbacks
struct S3RequestContext {
    S3OperationType operation_type;
    std::chrono::steady_clock::time_point start_time;
    std::string service_name;
    std::string request_name;
};

// Custom monitoring implementation using AWS SDK interface
class S3MonitoringListener : public Aws::Monitoring::MonitoringInterface {
public:
    S3MonitoringListener() = default;
    ~S3MonitoringListener() override = default;

    // Called when a request starts - returns a context pointer
    void* OnRequestStarted(
        const Aws::String& serviceName,
        const Aws::String& requestName,
        const std::shared_ptr<const Aws::Http::HttpRequest>& request
    ) const override;

    // Called when a request succeeds
    void OnRequestSucceeded(
        const Aws::String& serviceName,
        const Aws::String& requestName,
        const std::shared_ptr<const Aws::Http::HttpRequest>& request,
        const Aws::Client::HttpResponseOutcome& outcome,
        const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
        void* context
    ) const override;

    // Called when a request fails
    void OnRequestFailed(
        const Aws::String& serviceName,
        const Aws::String& requestName,
        const std::shared_ptr<const Aws::Http::HttpRequest>& request,
        const Aws::Client::HttpResponseOutcome& outcome,
        const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
        void* context
    ) const override;

    // Called when a request is retried
    void OnRequestRetry(
        const Aws::String& serviceName,
        const Aws::String& requestName,
        const std::shared_ptr<const Aws::Http::HttpRequest>& request,
        void* context
    ) const override;

    // Called when request processing is complete (cleanup context)
    void OnFinish(
        const Aws::String& serviceName,
        const Aws::String& requestName,
        const std::shared_ptr<const Aws::Http::HttpRequest>& request,
        void* context
    ) const override;

private:
    // Extract latency from CoreMetrics (returns 0 if not available)
    static int64_t extractLatency(const Aws::Monitoring::CoreMetricsCollection& metrics);

    // Extract bytes transferred from request/response
    static uint64_t extractBytesUploaded(const std::shared_ptr<const Aws::Http::HttpRequest>& request);
    static uint64_t extractBytesDownloaded(const Aws::Client::HttpResponseOutcome& outcome);
};

// Factory for creating S3MonitoringListener instances
class S3MonitoringFactory : public Aws::Monitoring::MonitoringFactory {
public:
    S3MonitoringFactory() = default;
    ~S3MonitoringFactory() override = default;

    Aws::UniquePtr<Aws::Monitoring::MonitoringInterface> CreateMonitoringInstance() const override;
};

// Get the factory creation function for AWS SDK options
// Usage:
//   Aws::SDKOptions options;
//   options.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
//       GetS3MonitoringFactoryCreateFn()
//   );
//   Aws::InitAPI(options);
Aws::Monitoring::MonitoringFactoryCreateFunction GetS3MonitoringFactoryCreateFn();
