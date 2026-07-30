#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdint>

struct LeftoversConfig {
    std::string bucket;
    std::string region;
    std::string endpoint;  // S3-compatible endpoint (empty = AWS)
    std::string prefix;
    std::chrono::seconds older_than{0};  // 0 = no filter
    bool abort_uploads = false;
    bool verbose = false;
};

struct LeftoversProgress {
    std::atomic<size_t> uploads_found{0};
    std::atomic<size_t> uploads_aborted{0};
    std::atomic<size_t> abort_failures{0};
    std::atomic<bool> cancelled{false};
};

struct LeftoversResult {
    bool success = false;
    size_t uploads_listed = 0;
    size_t uploads_aborted = 0;
    size_t abort_failures = 0;
    double elapsed_seconds = 0.0;
    std::string error_message;
};
