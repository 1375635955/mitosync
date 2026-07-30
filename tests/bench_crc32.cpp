// Throughput benchmark for crc32_hw(), against zlib as the baseline.
//
// Not a test: nothing here fails a build, and ctest does not run it. It exists
// because issue #50 traded a hand-written CRC for a dependency on the strength
// of a throughput argument, and an argument like that should be re-runnable by
// whoever doubts it rather than quoted from a commit message.
//
//     ./build/mito_bench_crc32
//
// The default 8 MiB size is DEFAULT_CHUNK_SIZE: it is the buffer mito actually
// checksums, one chunk at a time, in crc32_chunks.cpp. The smaller sizes are
// there because a folding implementation has a fixed start-up cost, so the
// advantage narrows on short buffers, and the 64 MiB one because past the
// cache the memory system, not the CRC, sets the pace.
//
// Single-threaded on purpose. mito computes chunk CRCs across a thread pool,
// where throughput is bounded by memory bandwidth and the difference between
// two implementations compresses; this measures the implementations instead.
#include "crc32_hw.h"

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kMinSeconds = 0.5;

// Runs fn over the buffer often enough to time it honestly, and returns GB/s.
template <typename Fn>
double measure(const std::vector<uint8_t>& buf, Fn fn) {
    size_t iterations = 4;
    for (;;) {
        const auto start = std::chrono::steady_clock::now();
        volatile uint32_t sink = 0;
        for (size_t i = 0; i < iterations; ++i) {
            sink ^= fn(buf.data(), buf.size());
        }
        (void)sink;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds >= kMinSeconds) {
            return static_cast<double>(buf.size()) * iterations / seconds / 1e9;
        }
        // Scale up towards the target rather than doubling blindly.
        const double growth = seconds > 1e-9 ? (kMinSeconds * 1.2) / seconds : 1000.0;
        iterations = static_cast<size_t>(iterations * (growth < 2.0 ? 2.0 : growth)) + 1;
    }
}

uint32_t zlib_reference(const uint8_t* data, size_t length) {
    return static_cast<uint32_t>(::crc32_z(0, data, length));
}

}  // namespace

int main() {
    printf("crc32_hw: %s\n", hw_crc32_name());
    printf("baseline: zlib %s\n\n", zlibVersion());
    printf("%12s  %10s  %10s  %8s  %s\n", "size", "zlib GB/s", "mito GB/s", "speedup", "checksum");

    const size_t sizes[] = {4u << 10, 64u << 10, 1u << 20, 8u << 20, 64u << 20};
    int mismatches = 0;

    for (size_t size : sizes) {
        std::vector<uint8_t> buf(size);
        for (size_t i = 0; i < size; ++i) {
            buf[i] = static_cast<uint8_t>(i * 31 + 7);
        }

        const uint32_t mine = crc32_hw(buf.data(), buf.size());
        const uint32_t reference = zlib_reference(buf.data(), buf.size());

        const double zlib_rate = measure(buf, zlib_reference);
        const double mito_rate = measure(buf, crc32_hw);

        // A benchmark that does not check its answer measures the wrong thing.
        const char* verdict = "";
        if (mine != reference) {
            verdict = "  <-- DISAGREES WITH ZLIB";
            ++mismatches;
        }

        std::string label = std::to_string(size >> 10) + " KiB";
        if (size >= (1u << 20)) {
            label = std::to_string(size >> 20) + " MiB";
        }
        printf("%12s  %10.2f  %10.2f  %7.1fx  %08x%s\n", label.c_str(), zlib_rate, mito_rate,
               mito_rate / zlib_rate, mine, verdict);
    }

    if (mismatches > 0) {
        fprintf(stderr, "\n%d size(s) disagreed with zlib - the numbers above are meaningless\n",
                mismatches);
        return 1;
    }
    return 0;
}
