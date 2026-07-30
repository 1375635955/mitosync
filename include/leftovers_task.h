#pragma once

#include "leftovers_types.h"
#include "s3_interface.h"
#include <memory>

// Run leftovers operation on S3 bucket
LeftoversResult run_leftovers(
    const LeftoversConfig& config,
    LeftoversProgress& progress,
    std::shared_ptr<IS3Client> s3_client = nullptr
);
