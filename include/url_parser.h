#pragma once
#include <string>
#include "comparison_task.h"

// Parse an S3 URL: s3://bucket/key or s3://bucket/key@region
// Returns true on success, false on failure
// On success, bucket, key, and optionally region are populated
// If region is not specified in URL, it is left unchanged (caller should set default)
bool parse_s3_url(const std::string& url, std::string& bucket, std::string& key, std::string& region);

// Parse a source argument into a FileSource
// Handles both local paths and S3 URLs
// On error, error string is populated and FileSource may be partially filled
FileSource parse_source(const std::string& arg, const std::string& default_region, std::string& error);
