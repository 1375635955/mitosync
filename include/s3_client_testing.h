#pragma once

// Test-only seam for the real S3 adapter.
//
// S3ClientImpl normally builds its own Aws::S3::S3Client from a region and
// credentials, which makes it unreachable from tests. This factory hands it a
// client the caller already owns, so a subclass of Aws::S3::S3Client returning
// canned outcomes can drive the adapter's request building, response mapping
// and retry loops without touching the network.
//
// Production code should use CreateS3Client from s3_interface.h instead.

#include <memory>
#include <string>

#include <aws/s3/S3Client.h>

#include "s3_interface.h"

// Wrap an existing SDK client in the production IS3Client implementation.
//
// region         reported back by the adapter; does not re-configure the client
// use_path_style path-style addressing, as set for LocalStack/MinIO endpoints
// max_retries    retry budget per operation; 0 means attempt once and give up,
//                which keeps tests fast and makes retry counts explicit
std::shared_ptr<IS3Client> CreateS3ClientForTesting(
    std::shared_ptr<Aws::S3::S3Client> client,
    const std::string& region = "us-east-1",
    bool use_path_style = false,
    int max_retries = 0
);
