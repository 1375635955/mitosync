#include "url_parser.h"
#include <cstring>

static bool starts_with(const std::string& str, const char* prefix) {
    size_t prefix_len = strlen(prefix);
    return str.size() >= prefix_len && str.compare(0, prefix_len, prefix) == 0;
}

bool parse_s3_url(const std::string& url, std::string& bucket, std::string& key, std::string& region) {
    // Must start with s3://
    if (!starts_with(url, "s3://")) return false;

    std::string rest = url.substr(5);  // Remove "s3://"

    // Check for @region suffix
    size_t at_pos = rest.rfind('@');
    if (at_pos != std::string::npos && at_pos > 0) {
        // Check if @ is after the bucket/key part (not in the key itself)
        // We consider @ valid for region only if it comes after a /
        size_t slash_pos = rest.find('/');
        if (at_pos > slash_pos) {
            region = rest.substr(at_pos + 1);
            rest = rest.substr(0, at_pos);
        }
    }

    // Find first slash to separate bucket from key
    size_t slash_pos = rest.find('/');
    if (slash_pos == std::string::npos || slash_pos == 0) {
        return false;  // No key specified or empty bucket
    }

    bucket = rest.substr(0, slash_pos);
    key = rest.substr(slash_pos + 1);

    if (key.empty()) return false;

    return true;
}

FileSource parse_source(const std::string& arg, const std::string& default_region, std::string& error) {
    FileSource source;
    error.clear();

    if (starts_with(arg, "s3://")) {
        source.type = SourceType::S3;
        // Leave region empty initially - parse_s3_url will set it only if explicitly specified
        // Caller can then auto-detect or use default for empty regions

        if (!parse_s3_url(arg, source.bucket, source.path, source.region)) {
            error = "Invalid S3 URL format: " + arg + " (expected s3://bucket/key or s3://bucket/key@region)";
        }

        // Apply default region only if not explicitly specified and default provided
        if (source.region.empty() && !default_region.empty()) {
            source.region = default_region;
        }
    } else {
        source.type = SourceType::Local;
        source.path = arg;
    }

    return source;
}
