#pragma once
#include <cstdint>
#include <string>
constexpr int64_t CHUNK_SIZE = 8 * 1024 * 1024;
constexpr int64_t BLOCK_SIZE = 64 * 1024;  // 64 KB per block for detailed analysis
constexpr int64_t BLOCKS_PER_CHUNK = CHUNK_SIZE / BLOCK_SIZE;  // 128 blocks per chunk

// Suffix on the temporary file a sync writes before moving it into place.
//
// Enumeration deliberately does NOT hide these. Filtering the local side only
// makes a sync asymmetric - the same name on the remote side stays visible, so
// --delete reads the local absence as an orphan and deletes real remote data,
// and a download of a matching object never converges. Filtering both sides is
// worse still: S3 enumeration is shared with rm, so hidden objects would be
// undeletable. A temporary orphaned by a killed run therefore stays visible,
// which at least means it can be seen and removed.
inline constexpr const char* kTempSuffix = ".mito-tmp";

// True for a name produced by AtomicFileWriter, including one orphaned by a
// killed run.
inline bool is_sync_temp_name(const std::string& filename) {
    const std::string suffix = kTempSuffix;
    return filename.size() > suffix.size() && filename.front() == '.' &&
           filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}
