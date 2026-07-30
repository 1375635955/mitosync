#pragma once

#include <cstdint>
#include <spdlog/spdlog.h>

// File descriptors used per HTTPS connection (socket + SSL overhead)
constexpr int FDS_PER_CONNECTION = 3;
// Reserve some file descriptors for stdin/stdout/stderr, log files, etc.
constexpr int FDS_RESERVED = 50;

struct FdLimits {
    int64_t soft_limit;      // Current limit
    int64_t hard_limit;      // Maximum we can raise to
    int max_safe_threads;    // Safe thread count based on limits
    bool was_capped;         // True if requested threads were reduced
};

#ifdef _WIN32

// Windows: No practical fd limit concerns for our use case
// Windows uses handles, not file descriptors, and has different limits

inline bool TryRaiseFdLimit(int /*requested_threads*/) {
    return true;  // No-op on Windows
}

inline FdLimits GetFdLimits(int requested_threads) {
    // Windows doesn't have the same fd limits as Unix
    // Return uncapped results
    return FdLimits{
        INT64_MAX,  // soft_limit
        INT64_MAX,  // hard_limit
        requested_threads > 0 ? requested_threads : 1,  // max_safe_threads
        false       // was_capped
    };
}

inline void WarnIfFdLimitLow(const FdLimits& /*limits*/, int /*requested_threads*/) {
    // No-op on Windows
}

#else  // Unix/POSIX

#include <sys/resource.h>

// Try to raise the soft limit to accommodate requested threads
// Returns true if successful (or if already high enough)
inline bool TryRaiseFdLimit(int requested_threads) {
    // Sanity check - negative or zero threads don't need fd limit changes
    if (requested_threads <= 0) {
        return true;
    }

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return false;
    }

    // Cap to reasonable maximum to prevent overflow (100k threads is plenty)
    constexpr int MAX_REASONABLE_THREADS = 100000;
    int capped_threads = requested_threads > MAX_REASONABLE_THREADS ? MAX_REASONABLE_THREADS : requested_threads;

    // Calculate needed limit (use int64_t to avoid overflow, then check against rlim_t max)
    // Always request at least 4096 for headroom with connection pools and retries
    constexpr int64_t MIN_TARGET_FDS = 4096;
    int64_t needed_i64 = static_cast<int64_t>(capped_threads) * FDS_PER_CONNECTION + FDS_RESERVED + 50;
    if (needed_i64 < MIN_TARGET_FDS) {
        needed_i64 = MIN_TARGET_FDS;
    }
    rlim_t needed = static_cast<rlim_t>(needed_i64);

    // Already high enough
    if (rl.rlim_cur >= needed) {
        return true;
    }

    // Can't exceed hard limit
    if (needed > rl.rlim_max) {
        needed = rl.rlim_max;
    }

    // Try to raise
    rl.rlim_cur = needed;
    if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
        spdlog::info("Raised file descriptor limit to {}", needed);
        return true;
    }

    return false;
}

// Get current file descriptor limits and calculate safe thread count
inline FdLimits GetFdLimits(int requested_threads) {
    FdLimits result = {256, 256, requested_threads, false};  // Safe defaults

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        result.soft_limit = static_cast<int64_t>(rl.rlim_cur);
        result.hard_limit = static_cast<int64_t>(rl.rlim_max);
    }

    // Handle "unlimited" case (RLIM_INFINITY is typically a very large value)
    constexpr int64_t EFFECTIVELY_UNLIMITED = 1000000;
    if (result.soft_limit > EFFECTIVELY_UNLIMITED) {
        // No practical limit - allow any requested thread count
        result.max_safe_threads = requested_threads > 0 ? requested_threads : 1;
        result.was_capped = false;
        return result;
    }

    // Calculate max safe threads: (available_fds) / fds_per_connection
    int64_t available_fds = result.soft_limit - FDS_RESERVED;
    if (available_fds < 0) available_fds = 0;

    result.max_safe_threads = static_cast<int>(available_fds / FDS_PER_CONNECTION);
    if (result.max_safe_threads < 1) result.max_safe_threads = 1;

    // Check if we need to cap the requested threads
    if (requested_threads > result.max_safe_threads) {
        result.was_capped = true;
    }

    return result;
}

// Print warning and instructions if thread count was capped
inline void WarnIfFdLimitLow(const FdLimits& limits, int requested_threads) {
    if (!limits.was_capped) return;

    // Sanity check
    if (requested_threads <= 0) return;

    // Calculate recommended limit (use int64_t to avoid overflow)
    int64_t recommended = static_cast<int64_t>(requested_threads) * FDS_PER_CONNECTION + FDS_RESERVED + 50;

    spdlog::warn("Reducing threads from {} to {} due to file descriptor limit. "
                 "To use {} threads, run: ulimit -n {}",
                 requested_threads, limits.max_safe_threads, requested_threads, recommended);
}

#endif  // _WIN32
