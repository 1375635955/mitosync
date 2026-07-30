#pragma once

// Command-line argument parsing for the mito CLI.
//
// These parsers are pure: they read argc/argv and produce an options struct,
// touching no global state, no filesystem and no network. They live apart from
// main.cpp so they can be tested directly.

#include <string>

#include "comparison_task.h"  // FileSource, SourceType
#include "sync_types.h"       // SyncDirection

// Subcommand selected by argv[1], if any.
enum class SubCommand {
    None,       // Legacy mode (no subcommand)
    Diff,
    Sync,
    Rm,
    Stats,
    Leftovers,
};

// Options for the default/diff command.
struct CliOptions {
    bool help = false;
    bool error = false;
    bool debug = false;
    bool verbose = false;  // Show retry warnings
    bool quiet = false;  // Minimal output
    bool allow_unverified_ranges = false;  // Accept ranged reads without Content-Range
    bool ramp_up = false;  // Gradually ramp up concurrency
    bool directory_mode = false;  // Force directory comparison mode
    int num_threads = 1024;  // Default: 1024 max, concurrency adapts to throughput
    bool parallel_discovery = true;   // Use parallel BFS directory enumeration
    int parallel_discovery_workers = 128;  // Number of parallel discovery workers
    std::string output_file;  // Write results to this file
    FileSource source_a;
    FileSource source_b;
    std::string source_profile;   // --source-profile -> source_a
    std::string dest_profile;     // --dest-profile   -> source_b
    std::string endpoint_url;     // --endpoint-url, applied to every S3 source
    bool has_source_a = false;
    bool has_source_b = false;
    std::string error_message;
};

// Options for the sync command.
struct SyncCliOptions {
    bool help = false;
    bool error = false;
    bool debug = false;
    bool verbose = false;
    bool quiet = false;
    bool allow_unverified_ranges = false;  // Accept ranged reads without Content-Range
    bool delete_orphans = false;
    bool dry_run = false;
    int num_threads = 256;
    std::string local_path;
    FileSource s3_source;         // Source S3 (for download and S3-to-S3)
    FileSource s3_dest;           // Dest S3 (for S3-to-S3)
    std::string source_profile;   // --source-profile (S3 source side)
    std::string dest_profile;     // --dest-profile (S3 dest side)
    std::string endpoint_url;     // --endpoint-url, applied to every S3 side
    SyncDirection direction = SyncDirection::Upload;
    bool has_local = false;
    bool has_s3_source = false;
    bool has_s3_dest = false;
    std::string error_message;
};

// Options for the rm command.
struct RmCliOptions {
    bool help = false;
    bool error = false;
    bool recursive = false;
    bool force = false;
    bool quiet = false;
    bool verbose = false;
    bool batch = false;  // Use batch DeleteObjects API (faster but can hit rate limits)
    int num_threads = 256;
    std::string s3_url;
    std::string bucket;
    std::string prefix;
    std::string region;
    std::string endpoint_url;     // --endpoint-url
    std::string error_message;
};

// Inspect argv[1]. Anything unrecognised is SubCommand::None (legacy diff mode).
SubCommand detect_subcommand(int argc, char* argv[]);

// True when argv[1] is a request for the version (`--version` or `-V`).
//
// Only argv[1], the way `git --version` behaves. Accepting it in any position would put
// the flag in every subcommand parser, and would turn `mito rm --version s3://bucket/key`
// into a version request rather than the typo it is.
bool wants_version(int argc, char* argv[]);

// Parse the default/diff command. Reads from argv[1] onwards.
CliOptions parse_args(int argc, char* argv[]);

// Parse the sync command. Reads from argv[2] onwards, skipping the subcommand.
SyncCliOptions parse_sync_args(int argc, char* argv[]);

// Parse the rm command. Reads from argv[2] onwards, skipping the subcommand.
RmCliOptions parse_rm_args(int argc, char* argv[]);
