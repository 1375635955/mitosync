#pragma once

#include "sync_types.h"
#include "s3_interface.h"
#include <memory>

// Run sync operation from local directory to S3
SyncResult run_sync(
    const SyncConfig& config,
    SyncProgress& progress,
    std::shared_ptr<IS3Client> s3_client = nullptr
);
