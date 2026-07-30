#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>

// Forward declaration for dependency injection
class IS3Client;

// Source type for comparison
enum class SourceType { Local, S3 };

// Represents a file source (local or S3)
struct FileSource {
    SourceType type = SourceType::Local;
    std::string path;      // Local path or S3 key
    std::string bucket;    // Only for S3
    std::string region;    // Only for S3 (empty = auto-detect)
    std::string profile;   // Only for S3 (empty = default AWS credential chain)
    std::string endpoint;  // Only for S3 (empty = AWS; set for S3-compatible gateways)
};

// True if two S3 sources can share one S3 client (same region AND same profile).
bool should_reuse_s3_client(const FileSource& a, const FileSource& b);

// Configuration for a comparison task
struct ComparisonConfig {
    FileSource source_a;
    FileSource source_b;
    std::vector<int64_t> chunk_ids = {};  // Empty = all chunks
    bool debug = false;  // Enable debug logging
    int num_threads = 128;  // Number of threads for S3 requests (0 = unbounded/std::async)
    int max_connections = 128;  // Max concurrent S3 connections per client
    bool ramp_up = false;  // Gradually ramp up concurrency
    int64_t chunk_size = 8 * 1024 * 1024;  // Chunk size in bytes (default 8 MiB)
    int64_t block_size = 64 * 1024;        // Block size in bytes (default 64 KiB)
};

// Progress state for monitoring
struct ComparisonProgress {
    std::atomic<double> source_a_progress{0.0};
    std::atomic<double> source_b_progress{0.0};
    std::atomic<bool> source_a_done{false};
    std::atomic<bool> source_b_done{false};
    std::atomic<bool> cancelled{false};
};

// Result of a comparison
struct ComparisonResult {
    std::vector<uint32_t> source_a_crcs;
    std::vector<uint32_t> source_b_crcs;
    double source_a_elapsed = 0.0;
    double source_b_elapsed = 0.0;
    double total_elapsed = 0.0;
    std::string error_message;
    bool success = false;
    bool all_match = false;
    std::vector<size_t> mismatched_chunks;      // Chunks where content differs
    std::vector<size_t> extra_chunks_in_a;      // Chunks only in A (A is larger)
    std::vector<size_t> extra_chunks_in_b;      // Chunks only in B (B is larger)
    int64_t size_a = 0;                         // Size of source A
    int64_t size_b = 0;                         // Size of source B
    int64_t file_size = 0;                      // Common size for comparison (min of both)
};

// Detailed block-level analysis of a mismatched chunk
struct BlockAnalysis {
    int64_t chunk_index = -1;
    int64_t chunk_size = 0;                // Actual size of this chunk (may be smaller for last chunk)
    int64_t block_size = 64 * 1024;        // Block size used for analysis (default 64 KiB)
    std::vector<bool> block_matches;       // Per-block match status (true = match, false = mismatch)
    int64_t total_blocks = 0;              // Total number of blocks in this chunk
    int64_t blocks_different = 0;          // Count of mismatched blocks
    int64_t bytes_different = 0;           // Total differing bytes
    double percentage_different = 0.0;     // Percentage of chunk that differs
    int64_t first_diff_offset = -1;        // First byte offset with difference (relative to chunk start)
    int64_t last_diff_offset = -1;         // Last byte offset with difference (relative to chunk start)
    bool computed = false;                 // Whether analysis has been computed
    std::string error_message;             // Error if analysis failed
};

// Analyze a mismatched chunk in detail
// Fetches chunk data from both sources and compares block-by-block
// For S3 sources, downloads the chunk on-demand
// chunk_size: size of each chunk (default 8 MiB)
// block_size: size of blocks for detailed diff (default 64 KiB)
BlockAnalysis analyze_mismatched_chunk(
    const FileSource& source_a,
    const FileSource& source_b,
    int64_t chunk_index,
    int64_t file_size,
    std::shared_ptr<IS3Client> s3_client_a = nullptr,
    std::shared_ptr<IS3Client> s3_client_b = nullptr,
    std::function<void(double)> progress_cb = nullptr,
    int64_t chunk_size = 8 * 1024 * 1024,
    int64_t block_size = 64 * 1024
);

// Run a comparison task
// Returns result when complete
// Progress can be monitored via the progress parameter
// Optional s3_client parameters allow dependency injection for testing
ComparisonResult run_comparison(
    const ComparisonConfig& config,
    ComparisonProgress& progress,
    std::shared_ptr<IS3Client> s3_client_a = nullptr,
    std::shared_ptr<IS3Client> s3_client_b = nullptr
);
