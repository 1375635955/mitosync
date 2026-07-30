#include "comparison_task.h"
#include "s3_utils.h"
#include "s3_interface.h"
#include "crc32_chunks.h"

#include <spdlog/spdlog.h>
#include <future>
#include <chrono>
#include <cmath>
#include <memory>
#include <cassert>
#include <fcntl.h>
#include <unistd.h>

bool should_reuse_s3_client(const FileSource& a, const FileSource& b) {
    return a.type == SourceType::S3 && b.type == SourceType::S3 &&
           a.region == b.region && a.profile == b.profile;
}

// Return type for async tasks - bundles CRCs with elapsed time
struct TaskResult {
    std::vector<uint32_t> crcs;
    double elapsed = 0.0;
};

// Get file size for any source type
static int64_t get_source_size(const FileSource& source, const std::shared_ptr<IS3Client>& s3_client, bool debug = false) {
    if (source.type == SourceType::Local) {
        return GetLocalFileSize(source.path);
    } else {
        if (!s3_client) return -1;
        int64_t size = s3_client->GetObjectSize(source.bucket, source.path);
        if (debug) {
            spdlog::debug("get_source_size: s3://{}/{} -> {} bytes", source.bucket, source.path, size);
        }
        return size;
    }
}

// Compute CRCs for any source type
static TaskResult compute_source_crcs(
    const FileSource& source,
    int64_t file_size,
    const std::vector<int64_t>& chunk_ids,
    std::function<void(double)> progress_cb,
    std::atomic<bool>& done_flag,
    std::atomic<bool>& cancelled,
    std::shared_ptr<IS3Client> s3_client,
    bool debug,
    int num_threads,
    bool ramp_up,
    int64_t chunk_size
) {
    TaskResult tr;
    if (cancelled) return tr;

    auto t0 = std::chrono::high_resolution_clock::now();

    if (source.type == SourceType::Local) {
        tr.crcs = compute_crc32_chunks_boost_asio(source.path, chunk_ids, progress_cb, chunk_size);
    } else {
        if (!s3_client) return tr;
        tr.crcs = s3_client->GetChunkCRC32s(source.bucket, source.path, file_size, chunk_ids, progress_cb, debug, num_threads, ramp_up, chunk_size);
    }

    done_flag = true;
    auto t1 = std::chrono::high_resolution_clock::now();
    tr.elapsed = std::chrono::duration<double>(t1 - t0).count();
    return tr;
}

ComparisonResult run_comparison(
    const ComparisonConfig& config,
    ComparisonProgress& progress,
    std::shared_ptr<IS3Client> injected_client_a,
    std::shared_ptr<IS3Client> injected_client_b
) {
    ComparisonResult result;

    try {
        // Create S3 clients - use injected ones for testing, or create real ones
        std::shared_ptr<IS3Client> client_a;
        std::shared_ptr<IS3Client> client_b;

        if (config.source_a.type == SourceType::S3) {
            client_a = injected_client_a ? injected_client_a : CreateS3Client(config.source_a.region, config.source_a.endpoint, config.max_connections, config.source_a.profile);
        }

        if (config.source_b.type == SourceType::S3) {
            if (injected_client_b) {
                client_b = injected_client_b;
            } else if (should_reuse_s3_client(config.source_a, config.source_b)) {
                // Same region and profile - reuse client (whether injected or created)
                client_b = client_a;
            } else {
                client_b = CreateS3Client(config.source_b.region, config.source_b.endpoint, config.max_connections, config.source_b.profile);
            }
        }

        // Get file sizes for both sources
        int64_t size_a = get_source_size(config.source_a, client_a, config.debug);
        if (size_a < 0) {
            result.error_message = "Failed to get size for source A";
            return result;
        }

        int64_t size_b = get_source_size(config.source_b, client_b, config.debug);
        if (size_b < 0) {
            result.error_message = "Failed to get size for source B";
            return result;
        }

        // Store both file sizes
        result.size_a = size_a;
        result.size_b = size_b;
        // Common size for comparison (used for block analysis on common chunks)
        result.file_size = std::min(size_a, size_b);

        // Run both tasks in parallel
        auto t_start = std::chrono::high_resolution_clock::now();

        // IMPORTANT: progress is captured by reference but this is safe because:
        // 1. All accessed fields (source_a_progress, source_b_done, cancelled) are std::atomic
        // 2. We block on future.get() below before run_comparison returns
        // DO NOT refactor to fire-and-forget without addressing lifetime issues.
        const auto source_a = config.source_a;
        const auto source_b = config.source_b;
        const auto chunk_ids = config.chunk_ids;
        const bool debug = config.debug;
        const int num_threads = config.num_threads;
        const bool ramp_up = config.ramp_up;
        const int64_t chunk_size = config.chunk_size;

        auto future_a = std::async(std::launch::async, [source_a, size_a, &progress, client_a, chunk_ids, debug, num_threads, ramp_up, chunk_size]() -> TaskResult {
            return compute_source_crcs(
                source_a, size_a, chunk_ids,
                [&](double pct) { progress.source_a_progress = pct; },
                progress.source_a_done, progress.cancelled,
                client_a, debug, num_threads, ramp_up, chunk_size
            );
        });

        auto future_b = std::async(std::launch::async, [source_b, size_b, &progress, client_b, chunk_ids, debug, num_threads, ramp_up, chunk_size]() -> TaskResult {
            return compute_source_crcs(
                source_b, size_b, chunk_ids,
                [&](double pct) { progress.source_b_progress = pct; },
                progress.source_b_done, progress.cancelled,
                client_b, debug, num_threads, ramp_up, chunk_size
            );
        });

        // Wait for both and extract results
        auto result_a = future_a.get();
        auto result_b = future_b.get();

        result.source_a_crcs = std::move(result_a.crcs);
        result.source_b_crcs = std::move(result_b.crcs);
        result.source_a_elapsed = result_a.elapsed;
        result.source_b_elapsed = result_b.elapsed;

        auto t_end = std::chrono::high_resolution_clock::now();
        result.total_elapsed = std::chrono::duration<double>(t_end - t_start).count();

        // Check for cancellation
        if (progress.cancelled) {
            result.error_message = "Cancelled";
            return result;
        }

        // Check for errors (empty CRCs are only an error if file size > 0)
        if (result.source_a_crcs.empty() && size_a > 0) {
            result.error_message = "Failed to compute CRC32 checksums for source A";
            return result;
        }
        if (result.source_b_crcs.empty() && size_b > 0) {
            result.error_message = "Failed to compute CRC32 checksums for source B";
            return result;
        }

        // Compare results
        result.all_match = true;
        size_t chunks_a = result.source_a_crcs.size();
        size_t chunks_b = result.source_b_crcs.size();
        size_t common_chunks = std::min(chunks_a, chunks_b);

        // Compare common chunks
        for (size_t i = 0; i < common_chunks; ++i) {
            if (result.source_a_crcs[i] != result.source_b_crcs[i]) {
                result.all_match = false;
                result.mismatched_chunks.push_back(i);
            }
        }

        // Record extra chunks in the larger file
        if (chunks_a > chunks_b) {
            result.all_match = false;
            for (size_t i = chunks_b; i < chunks_a; ++i) {
                result.extra_chunks_in_a.push_back(i);
            }
        } else if (chunks_b > chunks_a) {
            result.all_match = false;
            for (size_t i = chunks_a; i < chunks_b; ++i) {
                result.extra_chunks_in_b.push_back(i);
            }
        }

        result.success = true;

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }

    return result;
}

// Helper to read a chunk from a local file
static std::vector<uint8_t> read_local_chunk(const std::string& path, int64_t chunk_index, int64_t file_size, int64_t chunk_size) {
    int64_t start = chunk_index * chunk_size;
    int64_t length = std::min(chunk_size, file_size - start);
    if (length <= 0) return {};

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};

    std::vector<uint8_t> data(length);
    ssize_t read_bytes = pread(fd, data.data(), length, start);
    close(fd);

    if (read_bytes != length) return {};
    return data;
}

// Helper to read a chunk from S3
static std::vector<uint8_t> read_s3_chunk(
    IS3Client* client,
    const std::string& bucket,
    const std::string& key,
    int64_t chunk_index,
    int64_t file_size,
    int64_t chunk_size
) {
    int64_t start = chunk_index * chunk_size;
    if (start < 0 || start >= file_size) return {};  // Bounds check
    int64_t end = std::min(start + chunk_size - 1, file_size - 1);
    return client->GetObjectRange(bucket, key, start, end);
}

// Compare two data buffers and populate BlockAnalysis
// Returns false if buffers have mismatched sizes (sets error_message)
static bool analyze_buffers(
    const std::vector<uint8_t>& data_a,
    const std::vector<uint8_t>& data_b,
    BlockAnalysis& result,
    int64_t block_size
) {
    if (data_a.size() != data_b.size()) {
        result.error_message = "Buffer size mismatch: A=" + std::to_string(data_a.size()) +
                               ", B=" + std::to_string(data_b.size());
        return false;
    }
    int64_t data_size = static_cast<int64_t>(data_a.size());
    result.chunk_size = data_size;
    result.block_size = block_size;

    // Calculate number of blocks in this chunk
    int64_t num_blocks = (data_size + block_size - 1) / block_size;
    result.total_blocks = num_blocks;
    result.block_matches.resize(num_blocks, true);

    result.bytes_different = 0;
    result.blocks_different = 0;
    result.first_diff_offset = -1;
    result.last_diff_offset = -1;

    // Compare block by block
    for (int64_t block = 0; block < num_blocks; ++block) {
        int64_t block_start = block * block_size;
        int64_t block_end = std::min(block_start + block_size, data_size);

        bool block_matches = true;
        for (int64_t i = block_start; i < block_end; ++i) {
            if (data_a[i] != data_b[i]) {
                block_matches = false;
                ++result.bytes_different;

                if (result.first_diff_offset < 0) {
                    result.first_diff_offset = i;
                }
                result.last_diff_offset = i;
            }
        }

        result.block_matches[block] = block_matches;
        if (!block_matches) {
            ++result.blocks_different;
        }
    }

    result.percentage_different = data_size > 0
        ? (100.0 * result.bytes_different / data_size)
        : 0.0;

    return true;
}

BlockAnalysis analyze_mismatched_chunk(
    const FileSource& source_a,
    const FileSource& source_b,
    int64_t chunk_index,
    int64_t file_size,
    std::shared_ptr<IS3Client> s3_client_a,
    std::shared_ptr<IS3Client> s3_client_b,
    std::function<void(double)> progress_cb,
    int64_t chunk_size,
    int64_t block_size
) {
    BlockAnalysis result;
    result.chunk_index = chunk_index;

    // Use defaults if not specified
    if (chunk_size <= 0) chunk_size = DEFAULT_CHUNK_SIZE;
    if (block_size <= 0) block_size = DEFAULT_BLOCK_SIZE;

    try {
        if (progress_cb) progress_cb(0.0);

        // Create S3 clients if needed and not provided
        std::shared_ptr<IS3Client> client_a = s3_client_a;
        std::shared_ptr<IS3Client> client_b = s3_client_b;

        if (source_a.type == SourceType::S3 && !client_a) {
            client_a = CreateS3Client(source_a.region, source_a.endpoint, 128, source_a.profile);
            if (!client_a) {
                result.error_message = "Failed to create S3 client for source A (region: " + source_a.region + ")";
                return result;
            }
        }
        if (source_b.type == SourceType::S3 && !client_b) {
            if (should_reuse_s3_client(source_a, source_b)) {
                client_b = client_a;
            } else {
                client_b = CreateS3Client(source_b.region, source_b.endpoint, 128, source_b.profile);
                if (!client_b) {
                    result.error_message = "Failed to create S3 client for source B (region: " + source_b.region + ")";
                    return result;
                }
            }
        }

        // Fetch chunk data from both sources in parallel
        std::vector<uint8_t> data_a, data_b;

        auto future_a = std::async(std::launch::async, [&]() -> std::vector<uint8_t> {
            if (source_a.type == SourceType::Local) {
                return read_local_chunk(source_a.path, chunk_index, file_size, chunk_size);
            } else {
                return read_s3_chunk(client_a.get(), source_a.bucket, source_a.path, chunk_index, file_size, chunk_size);
            }
        });

        auto future_b = std::async(std::launch::async, [&]() -> std::vector<uint8_t> {
            if (source_b.type == SourceType::Local) {
                return read_local_chunk(source_b.path, chunk_index, file_size, chunk_size);
            } else {
                return read_s3_chunk(client_b.get(), source_b.bucket, source_b.path, chunk_index, file_size, chunk_size);
            }
        });

        if (progress_cb) progress_cb(25.0);
        data_a = future_a.get();
        if (progress_cb) progress_cb(50.0);
        data_b = future_b.get();
        if (progress_cb) progress_cb(75.0);

        // Validate data
        if (data_a.empty()) {
            result.error_message = "Failed to read chunk from source A";
            return result;
        }
        if (data_b.empty()) {
            result.error_message = "Failed to read chunk from source B";
            return result;
        }
        if (data_a.size() != data_b.size()) {
            result.error_message = "Chunk sizes don't match: A=" + std::to_string(data_a.size()) +
                                   ", B=" + std::to_string(data_b.size());
            return result;
        }

        // Analyze the buffers
        if (!analyze_buffers(data_a, data_b, result, block_size)) {
            return result;  // error_message already set by analyze_buffers
        }
        result.computed = true;

        if (progress_cb) progress_cb(100.0);

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
    }

    return result;
}
