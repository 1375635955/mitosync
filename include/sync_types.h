#pragma once

#include "url_parser.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>

// Action to take for each file during sync
enum class SyncAction {
    Skip,           // File unchanged, no action needed
    Upload,         // Local -> S3: new file or small file, full upload
    UploadDiff,     // Local -> S3: large file, differential upload (reuse unchanged parts)
    Download,       // S3 -> Local: new file or small file, full download
    DownloadDiff,   // S3 -> Local: large file, differential download (reuse unchanged parts)
    Copy,           // S3 -> S3: server-side copy (no client bandwidth used)
    Delete          // File only in destination, delete if --delete flag set
};

// Direction of sync operation
enum class SyncDirection {
    Upload,      // Local filesystem -> S3 bucket
    Download,    // S3 bucket -> Local filesystem
    S3ToS3,      // S3 bucket -> S3 bucket (server-side, cross-bucket/cross-region supported)
    LocalToLocal // Local filesystem -> Local filesystem
};

// Information about a single file to sync
//
// Field semantics vary by SyncDirection:
//
//   Upload (Local -> S3):
//     local_size = local file size (-1 if doesn't exist)
//     s3_size    = S3 destination size (-1 if doesn't exist)
//     dest_size  = unused
//
//   Download (S3 -> Local):
//     local_size = local file size (-1 if doesn't exist)
//     s3_size    = S3 source size (-1 if doesn't exist)
//     dest_size  = unused
//
//   S3ToS3 (S3 -> S3):
//     local_size = unused (-1)
//     s3_size    = S3 source size (-1 if doesn't exist)
//     dest_size  = S3 destination size (-1 if doesn't exist)
//
//   LocalToLocal (Local -> Local):
//     local_size = source file size (-1 if doesn't exist)
//     s3_size    = unused (-1)
//     dest_size  = destination file size (-1 if doesn't exist)
//
struct SyncFileEntry {
    std::string relative_path;
    SyncAction action = SyncAction::Skip;
    int64_t local_size = -1;    // Local file size; -1 = doesn't exist (unused for S3ToS3)
    int64_t s3_size = -1;       // S3 size: destination for Upload, source for Download/S3ToS3
    int64_t dest_size = -1;     // S3 destination size for S3ToS3 only (-1 = doesn't exist)
    int64_t local_mtime = 0;    // Local file modification time (seconds since epoch)
};

// Configuration for sync operation
struct SyncConfig {
    SyncDirection direction = SyncDirection::Upload;

    // For Upload: local_path is source, destination is S3
    // For Download: source is S3, local_path is destination
    // For LocalToLocal: local_path is source, dest_local_path is destination
    std::string local_path;
    std::string dest_local_path;  // Destination for LocalToLocal sync
    FileSource source;          // S3 source (for Download direction)
    FileSource destination;     // S3 destination (for Upload direction)

    bool delete_orphans = false;
    bool dry_run = false;
    int max_threads = 256;      // Higher default for small file performance
    bool debug = false;
    bool quiet = false;
    bool verbose = false;
};

// Maps CLI --source-profile/--dest-profile onto the correct S3 FileSource(s)
// based on sync direction. s3_source is overloaded: for Upload it is the S3
// destination, for Download it is the S3 source, for S3->S3 it is the source.
// Empty flag strings leave the existing profile untouched.
void apply_sync_profiles(SyncDirection direction,
                         const std::string& source_profile,
                         const std::string& dest_profile,
                         FileSource& s3_source,
                         FileSource& s3_dest);

// Progress tracking for sync operation
struct SyncProgress {
    std::atomic<size_t> files_scanned_local{0};
    std::atomic<size_t> files_scanned_s3{0};
    std::atomic<size_t> files_scanned_dest{0};  // Destination S3 for S3-to-S3 sync
    std::atomic<bool> scanning_done{false};

    std::atomic<size_t> files_total{0};
    std::atomic<size_t> files_processed{0};
    std::atomic<size_t> files_in_flight{0};  // Currently uploading
    std::atomic<size_t> bytes_transferred{0};           // Actual client-side bytes transferred
    std::atomic<size_t> bytes_copied_server_side{0};    // Server-side copy bytes (S3-to-S3)

    std::atomic<bool> cancelled{false};

    // Current file being processed (for display) - protected by current_file_mutex
    mutable std::mutex current_file_mutex;
    std::string current_file;

    // Thread-safe setters/getters for current_file
    void set_current_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(current_file_mutex);
        current_file = path;
    }

    std::string get_current_file() const {
        std::lock_guard<std::mutex> lock(current_file_mutex);
        return current_file;
    }
};

// Result of sync operation
struct SyncResult {
    bool success = false;
    size_t files_uploaded = 0;
    size_t files_diff_uploaded = 0;
    size_t files_downloaded = 0;
    size_t files_diff_downloaded = 0;
    size_t files_copied = 0;        // S3 -> S3 copies
    size_t files_skipped = 0;
    size_t files_deleted = 0;
    size_t files_failed = 0;

    size_t bytes_transferred = 0;           // Actual client-side bytes transferred
    size_t bytes_copied_server_side = 0;    // Server-side copy bytes (S3-to-S3)
    size_t bytes_saved = 0;                 // Bytes avoided via diff upload/download

    double elapsed_seconds = 0.0;
    std::string error_message;

    // Detailed file list (for dry-run display)
    std::vector<SyncFileEntry> files;
};
