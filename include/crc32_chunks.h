#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <functional>

// Default chunk size (8 MiB) - used when chunk_size parameter is 0 or not specified
constexpr int64_t DEFAULT_CHUNK_SIZE = 8 * 1024 * 1024;

// Default block size (64 KiB) - used for detailed diff analysis
constexpr int64_t DEFAULT_BLOCK_SIZE = 64 * 1024;

// Compute CRC32 checksums for chunks of a local file
// chunk_size: size of each chunk in bytes (default 8 MiB)
// Returns one CRC32 per requested chunk, in the order requested.
//
// Returns an EMPTY vector if any chunk could not be checksummed - an invalid
// chunk id, or a chunk that could not be mapped. A zero is never substituted
// for a failed chunk, because 0 is a value a real chunk can produce and the
// caller compares these positionally against another source. This matches
// IS3Client::GetChunkCRC32s, which also returns empty on error.
std::vector<uint32_t> compute_crc32_chunks_boost_asio(
    const std::string& filepath,
    const std::vector<int64_t>& chunk_ids = {},
    std::function<void(double)> progress_cb = nullptr,
    int64_t chunk_size = DEFAULT_CHUNK_SIZE
);
