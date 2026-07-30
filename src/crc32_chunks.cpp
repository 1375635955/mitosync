#include "../include/crc32_chunks.h"
#include <atomic>
#include "../include/crc32_hw.h"
#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <cstdint>
#include <future>
#include <cerrno>
#include <cstring>
#include <algorithm>

std::vector<uint32_t> compute_crc32_chunks_boost_asio(
    const std::string& filepath,
    const std::vector<int64_t>& chunk_ids,
    std::function<void(double)> progress_cb,
    int64_t chunk_size
) {
    // Use default if not specified or invalid
    if (chunk_size <= 0) {
        chunk_size = DEFAULT_CHUNK_SIZE;
    }

    // Open the file once and share the fd across all tasks
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        spdlog::error("Failed to open '{}': {}", filepath, std::strerror(errno));
        return {};
    }
    int64_t filesize = lseek(fd, 0, SEEK_END);
    if (filesize < 0) {
        spdlog::error("Failed to get size of '{}': {}", filepath, std::strerror(errno));
        close(fd);
        return {};
    }
    if (filesize == 0) {
        // Empty file - no chunks to compute, return empty result (not an error)
        spdlog::debug("File '{}' is empty (0 bytes), no CRCs to compute", filepath);
        close(fd);
        return {};
    }
    lseek(fd, 0, SEEK_SET);
    int64_t num_chunks = (filesize + chunk_size - 1) / chunk_size;

    // Build list of chunks to process: all chunks if none specified, else use provided list
    std::vector<int64_t> chunks_to_process;
    if (chunk_ids.empty()) {
        chunks_to_process.reserve(static_cast<size_t>(num_chunks));
        for (int64_t i = 0; i < num_chunks; ++i) {
            chunks_to_process.push_back(i);
        }
    } else {
        chunks_to_process = chunk_ids;
    }

    // Results indexed by position in chunks_to_process
    std::vector<uint32_t> results(chunks_to_process.size(), 0);
    size_t total = chunks_to_process.size();

    // Any chunk that cannot be checksummed poisons the whole result. Leaving a
    // zero in its slot would be indistinguishable from a real CRC32 of 0, and
    // the caller compares these values position by position - so a chunk that
    // could not be read would silently read as "these chunks match" or "these
    // chunks differ" depending on the other side, with no error anywhere.
    // Callers already treat an empty vector as failure, which is also what the
    // S3 path returns.
    std::atomic<bool> chunk_failed{false};

    // Lambda to compute CRC for a single chunk
    auto compute_chunk_crc = [&](size_t idx, int64_t chunk_id) {
        if (chunk_id < 0 || chunk_id >= num_chunks) {
            // Detail for the first failure only: a large file whose every chunk
            // fails would otherwise emit one line per chunk - measured at ~40k
            // lines for a 40k-chunk request.
            if (!chunk_failed.exchange(true)) {
                spdlog::error("Invalid chunk id {} for a file of {} chunks", chunk_id, num_chunks);
            }
            return;
        }
        int64_t offset = chunk_id * chunk_size;
        int64_t length = (offset + chunk_size <= filesize) ? chunk_size : filesize - offset;
        if (length <= 0) {
            if (!chunk_failed.exchange(true)) {
                spdlog::error("Chunk {} of '{}' has no bytes to read", chunk_id, filepath);
            }
            return;
        }
        // Read rather than map. This used to mmap the chunk and checksum the
        // mapping, which dies on SIGBUS if the file shrinks in between: mmap
        // past EOF succeeds on Linux, and only the read faults, so there is no
        // return value to check and no way to catch it without a process-wide
        // signal handler. A file shrinking under a sync is ordinary - log
        // rotation, a build tree, an editor rewriting in place - and the whole
        // process died for it (issue #53). A short pread is an error value.
        //
        // Nothing is given up for that. A block small enough to stay in L2 is
        // copied in and checksummed while still hot, where an 8 MiB mapping
        // streams through memory twice. Measuring this step alone against the
        // mapping it replaces, best of five on 16 threads: +16% at 1 GiB and
        // +38% at 256 MiB with the file in page cache, and within noise when it
        // is not - the case that dominates, being bounded by the disk. 1 MiB
        // blocks are ~35% slower than these, which is the cache effect showing
        // itself rather than measurement noise.
        //
        // End to end it is a wash: a 1 GiB local diff takes 0.07s either way,
        // because checksumming was never what that time was spent on. The
        // reason to prefer this is the signal that no longer arrives.
        thread_local std::vector<uint8_t> buffer;
        constexpr int64_t kBlock = 256 * 1024;
        if (buffer.size() < static_cast<size_t>(kBlock)) buffer.resize(kBlock);

        uint32_t crc = 0;
        int64_t done = 0;
        while (done < length) {
            const int64_t want = std::min(kBlock, length - done);
            int64_t got = 0;
            while (got < want) {
                const ssize_t n = ::pread(fd, buffer.data() + got,
                                          static_cast<size_t>(want - got), offset + done + got);
                if (n <= 0) {
                    // A short read means the file no longer has these bytes -
                    // truncated under us, or an I/O error. Either way this
                    // chunk has no checksum, and a wrong one is worse than
                    // none: callers compare position by position.
                    if (!chunk_failed.exchange(true)) {
                        if (n == 0) {
                            spdlog::error("Chunk {} of '{}' ended early at offset {}: the file "
                                          "shrank while it was being read",
                                          chunk_id, filepath, offset + done + got);
                        } else {
                            spdlog::error("Failed to read chunk {} of '{}' at offset {}: {}",
                                          chunk_id, filepath, offset + done + got,
                                          std::strerror(errno));
                        }
                    }
                    return;
                }
                got += n;
            }
            // crc32_hw_update() chains across blocks; crc32_hw() over the whole
            // buffer would agree with it, and does in the tests.
            crc = crc32_hw_update(crc, buffer.data(), static_cast<size_t>(want));
            done += want;
        }
        results[idx] = crc;
    };

    // For few chunks, process sequentially to avoid thread pool overhead
    // This is critical when called from directory comparison which already has file-level parallelism
    if (chunks_to_process.size() <= 4) {
        for (size_t idx = 0; idx < chunks_to_process.size(); ++idx) {
            compute_chunk_crc(idx, chunks_to_process[idx]);
            if (progress_cb) progress_cb(100.0 * (idx + 1) / total);
        }
    } else {
        // Many chunks: use thread pool for parallel processing
        boost::asio::thread_pool pool(std::thread::hardware_concurrency());
        std::atomic<size_t> completed{0};

        for (size_t idx = 0; idx < chunks_to_process.size(); ++idx) {
            int64_t chunk_id = chunks_to_process[idx];
            boost::asio::post(pool, [&, idx, chunk_id]() {
                compute_chunk_crc(idx, chunk_id);
                size_t done = ++completed;
                if (progress_cb) progress_cb(100.0 * done / total);
            });
        }
        pool.join();
    }
    close(fd);

    // Return nothing rather than a vector with a fabricated zero in it.
    if (chunk_failed) {
        spdlog::error("CRC32 computation failed for '{}'; discarding all {} requested chunks",
                      filepath, results.size());
        return {};
    }

    return results;
}

