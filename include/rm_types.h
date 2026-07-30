#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

struct RmConfig {
    std::string bucket;
    std::string prefix;
    std::string region;
    std::string endpoint;  // S3-compatible endpoint (empty = AWS)
    bool recursive = false;
    bool force = false;
    bool quiet = false;
    bool verbose = false;
    bool batch = false;  // Use batch DeleteObjects API (can hit rate limits)
    int max_threads = 256;
    int max_retries = 5;
};

struct RmProgress {
    std::atomic<size_t> objects_found{0};
    std::atomic<size_t> objects_deleted{0};
    std::atomic<size_t> objects_failed{0};
    std::atomic<size_t> bytes_freed{0};
    std::atomic<bool> enumeration_done{false};
    std::atomic<bool> cancelled{false};
};

struct RmResult {
    bool success = false;
    size_t objects_deleted = 0;
    size_t objects_failed = 0;
    size_t bytes_freed = 0;
    double elapsed_seconds = 0.0;
    std::vector<std::string> failed_keys;
    std::string error_message;

    // Things the run did not do, that the user asked for or might assume it
    // did. Not errors: the operation succeeded, and something adjacent to it
    // was deliberately left alone. Carried here rather than logged so the CLI
    // prints it with the result and tests can assert it.
    std::vector<std::string> warnings;
};
