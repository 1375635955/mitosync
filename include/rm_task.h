#pragma once

#include "rm_types.h"
#include "s3_interface.h"
#include <memory>

// Run rm operation on S3 prefix
RmResult run_rm(
    const RmConfig& config,
    RmProgress& progress,
    std::shared_ptr<IS3Client> s3_client = nullptr
);
