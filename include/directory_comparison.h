#pragma once

#include "comparison_task.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <memory>

// Forward declarations
class IS3Client;

// Represents a file or directory entry discovered during enumeration
struct DirectoryEntry {
    std::string relative_path;   // Path relative to root directory
    int64_t size = 0;            // File size in bytes (0 for directories)
    bool is_directory = false;   // True if this entry is a directory
    // Modification time, Unix epoch seconds. 0 means unknown, which callers
    // must treat as "cannot prove unchanged" rather than as an old timestamp.
    int64_t mtime = 0;
};

// Result of comparing a single file pair within a directory comparison
enum class FileCompareStatus {
    Match,           // Files exist in both and content matches
    Mismatch,        // Files exist in both but content differs
    OnlyInA,         // File exists only in source A
    OnlyInB,         // File exists only in source B
    Error            // Comparison failed
};

struct FileCompareResult {
    std::string relative_path;
    FileCompareStatus status = FileCompareStatus::Error;
    int64_t size_a = -1;             // Size in source A (-1 if not present)
    int64_t size_b = -1;             // Size in source B (-1 if not present)
    size_t total_chunks = 0;         // Total chunks compared (max of both sources)
    std::vector<size_t> mismatched_chunks;   // Indices of chunks that differ
    std::vector<size_t> extra_chunks_in_a;   // Chunks only in A (A is larger)
    std::vector<size_t> extra_chunks_in_b;   // Chunks only in B (B is larger)
    std::string error_message;       // Error details if status is Error

    // Block-level analysis for small files (when total_chunks <= 1)
    // Block size adapts based on file size for useful granularity
    bool has_block_analysis = false;
    int64_t block_size = 0;                  // Block size used (4-64 KiB)
    size_t total_blocks = 0;                 // Total blocks in common region
    std::vector<size_t> mismatched_blocks;   // Indices of blocks that differ
    std::vector<size_t> extra_blocks_in_a;   // Blocks only in A
    std::vector<size_t> extra_blocks_in_b;   // Blocks only in B
};

// Compute adaptive block size based on file size
// Returns block size from 4 KiB to 64 KiB for useful visualization granularity
int64_t compute_adaptive_block_size(int64_t file_size);

// Result of directory comparison
struct DirectoryComparisonResult {
    std::vector<FileCompareResult> files;
    size_t total_files = 0;
    size_t matching_files = 0;
    size_t mismatched_files = 0;
    size_t only_in_a = 0;
    size_t only_in_b = 0;
    size_t errors = 0;
    int64_t total_bytes_a = 0;       // Total bytes in source A (local only; 0 for S3)
    int64_t total_bytes_b = 0;       // Total bytes in source B (local only; 0 for S3)
    double total_elapsed = 0.0;
    double discovery_elapsed = 0.0;  // Time for Phase 1 (enumeration)
    double comparison_elapsed = 0.0; // Time for Phase 2+3 (comparison)
    bool success = false;
    std::string error_message;
};

// Progress tracking for directory comparison
struct DirectoryComparisonProgress {
    std::atomic<size_t> files_scanned_a{0};   // Files discovered in source A
    std::atomic<size_t> files_scanned_b{0};   // Files discovered in source B
    std::atomic<size_t> files_compared{0};    // Files compared so far
    std::atomic<size_t> total_files{0};       // Total files to process
    std::atomic<bool> scanning_done{false};   // Enumeration phase complete
    std::atomic<bool> cancelled{false};       // Cancellation requested
};

// Configuration for directory comparison
struct DirectoryComparisonConfig {
    FileSource source_a;         // Root of directory A
    FileSource source_b;         // Root of directory B
    bool recursive = true;       // Scan subdirectories
    bool debug = false;
    int num_threads = 1024;     // Max threads - adaptive concurrency scales based on throughput
    int max_connections = 256;  // Max concurrent S3 connections per client
    bool ramp_up = false;
    int64_t chunk_size = 8 * 1024 * 1024;  // Chunk size in bytes (default 8 MiB)
    int64_t block_size = 64 * 1024;        // Block size in bytes (default 64 KiB)

    // Parallel discovery settings
    bool parallel_discovery = true;       // Enable parallel directory enumeration
    int parallel_discovery_workers = 128;  // Number of parallel workers (1-128)
};

// Check if a FileSource represents a directory
// For local: checks if path is a directory via filesystem
// For S3: checks if path ends with '/'
bool is_directory_source(const FileSource& source);

// S3-specific: checks if S3 key exists as an object (makes API call)
// Returns true if the key does NOT exist as an object (so it's a prefix/directory)
bool is_s3_prefix(const FileSource& source);

// Enumerate files in a local directory
// Returns list of entries with paths relative to root_path
//
// out_complete: set to false if any part of the tree could not be read, so the
// returned list is partial. A partial list is indistinguishable from a genuinely
// empty one, and callers that plan deletions from it MUST check this.
std::vector<DirectoryEntry> enumerate_local_directory(
    const std::string& root_path,
    bool recursive,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    bool* out_complete = nullptr
);

// Enumerate files in an S3 prefix
// Returns list of entries with paths relative to prefix
// out_complete: see enumerate_local_directory.
std::vector<DirectoryEntry> enumerate_s3_prefix(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    bool* out_complete = nullptr
);

// Parallel enumeration using level-by-level BFS
// max_workers: number of concurrent workers (1-128)
// out_alias_omitted: set to true when a symlinked subtree was skipped because
// that target had already been enumerated under another name. The files are
// still in the listing, under the first alias, so a comparison is unaffected -
// but a caller that plans deletions from relative paths would see the second
// alias's paths as missing, and must treat this as an incomplete inventory.
std::vector<DirectoryEntry> parallel_enumerate_local_directory(
    const std::string& root_path,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    bool* out_complete = nullptr,
    bool* out_alias_omitted = nullptr
);

// include_directory_markers: keep the zero-byte objects whose keys end in "/"
// that other S3 clients create to represent folders, including the one named
// exactly by the prefix, which comes back with an empty relative_path.
//
// Comparison wants them left out: they are not files, and a local directory has
// no counterpart to compare them against. Deletion wants them in, because
// "remove everything under this prefix" includes them, and skipping them left
// them behind while reporting success (issue #71).
//
// Only honoured on the recursive path. A non-recursive call delegates to the
// single-level enumerator, which does not take this flag.
std::vector<DirectoryEntry> parallel_enumerate_s3_prefix(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    bool* out_complete = nullptr,
    bool include_directory_markers = false
);

// Callback type for streaming enumeration: receives batch of entries
// Return false to stop enumeration early
using EnumerateCallback = std::function<bool(std::vector<DirectoryEntry>&&)>;

// Streaming version - calls callback after each BFS level with new entries
// Allows deletions to start before enumeration completes
void parallel_enumerate_s3_prefix_streaming(
    const std::string& bucket,
    const std::string& prefix,
    bool recursive,
    int max_workers,
    std::atomic<size_t>& files_found,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> client,
    EnumerateCallback on_entries
);

// Run directory comparison
// Enumerates both sources, finds common/unique files, compares content
DirectoryComparisonResult run_directory_comparison(
    const DirectoryComparisonConfig& config,
    DirectoryComparisonProgress& progress,
    std::shared_ptr<IS3Client> s3_client_a = nullptr,
    std::shared_ptr<IS3Client> s3_client_b = nullptr
);
