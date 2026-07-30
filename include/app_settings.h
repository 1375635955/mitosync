#pragma once

#include <cstdint>
#include <string>

// Get platform-specific application data directory
// Creates the directory if it doesn't exist
// Returns:
//   macOS:   ~/Library/Application Support/MitoSync/
//   Linux:   ~/.local/share/mitosync/ (or $XDG_DATA_HOME/mitosync/)
//   Windows: %APPDATA%/MitoSync/
std::string GetAppDataDirectory();

// Application-wide settings for configurable parameters
// These are set by the GUI and read by various components
struct AppSettings {
    // Comparison settings
    int chunk_size_mib = 8;      // MiB - size of chunks for CRC comparison
    int block_size_kib = 64;     // KiB - size of blocks for detailed diff analysis

    // Parallelism - adaptive concurrency scales based on throughput up to this limit
    int num_threads = 1024;

    // Parallel discovery - BFS-based parallel enumeration of directories
    bool parallel_discovery = true;
    int parallel_discovery_workers = 128;  // 1-128

    // Network
    int max_connections = 256;   // Max concurrent S3 connections per client
    int connect_timeout_s = 10;
    int request_timeout_s = 30;
    int max_retries = 5;

    // Accept a ranged GET whose response carries no Content-Range header.
    //
    // Off by default: without that header nothing shows which bytes came back,
    // and an endpoint that ignores Range returns the start of the object for a
    // request for the middle of it - the right length at the wrong offset,
    // which a comparison reads as a difference in the wrong place (issue #76).
    // A header that names a *different* range is still rejected either way;
    // this only tolerates its absence, for an endpoint that omits it.
    bool allow_unverified_ranges = false;

    // Computed values
    int64_t chunk_size_bytes() const { return static_cast<int64_t>(chunk_size_mib) * 1024 * 1024; }
    int64_t block_size_bytes() const { return static_cast<int64_t>(block_size_kib) * 1024; }
    int connect_timeout_ms() const { return connect_timeout_s * 1000; }
    int request_timeout_ms() const { return request_timeout_s * 1000; }
};

// Global settings instance - defined in app_settings.cpp
extern AppSettings g_app_settings;

// Global verbose flag for CLI mode - controls retry warning visibility
// When false (default), retry warnings are suppressed
extern bool g_verbose;
