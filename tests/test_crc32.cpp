#include <gtest/gtest.h>
#include <crc32_hw.h>
#include <zlib.h>
#include <vector>
#include <cstring>
#include <random>
#include <numeric>
#include <memory>
#include <set>
#include <thread>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <sys/mman.h>

// Standard CRC32 (IEEE 802.3 polynomial 0x04C11DB7) test vectors
// These are the canonical test vectors that any correct CRC32 implementation must pass

class CRC32Test : public ::testing::Test {
protected:
    // Helper to compute CRC32 using zlib (known correct implementation)
    static uint32_t zlib_crc32(const uint8_t* data, size_t length) {
        return ::crc32(0, data, static_cast<uInt>(length));
    }

    // Helper to create aligned buffer at specific offset
    static std::pair<std::unique_ptr<uint8_t[]>, uint8_t*>
    create_aligned_buffer(size_t size, size_t alignment_offset) {
        // Allocate extra space for alignment adjustment
        auto buffer = std::make_unique<uint8_t[]>(size + 64);
        // Calculate aligned base then add offset
        uintptr_t base = reinterpret_cast<uintptr_t>(buffer.get());
        uintptr_t aligned = (base + 15) & ~static_cast<uintptr_t>(15);
        uint8_t* ptr = reinterpret_cast<uint8_t*>(aligned) + alignment_offset;
        return {std::move(buffer), ptr};
    }

    // Helper to fill buffer with pattern
    static void fill_pattern(uint8_t* data, size_t size, uint8_t pattern) {
        std::memset(data, pattern, size);
    }

    // Helper to fill buffer with sequential values
    static void fill_sequential(uint8_t* data, size_t size, uint8_t start = 0) {
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<uint8_t>((start + i) & 0xFF);
        }
    }

    // Helper to fill buffer with pseudo-random data (reproducible)
    static void fill_random(uint8_t* data, size_t size, uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<uint8_t>(dist(rng));
        }
    }
};

// Test: Empty input should return 0
TEST_F(CRC32Test, EmptyInput) {
    uint32_t result = crc32_hw(nullptr, 0);
    // Empty CRC32 is 0 (initial value XOR'd with itself after final XOR)
    EXPECT_EQ(result, 0x00000000u);
}

// Test: The canonical "123456789" test vector
// This is THE standard CRC32 test - every implementation must pass this
TEST_F(CRC32Test, CanonicalTestVector) {
    const char* input = "123456789";
    uint32_t result = crc32_hw(reinterpret_cast<const uint8_t*>(input), 9);

    // The canonical CRC32 value for "123456789" is 0xCBF43926
    // If this fails, the implementation is using the wrong polynomial
    EXPECT_EQ(result, 0xCBF43926u)
        << "CRC32 of '123456789' should be 0xCBF43926 (IEEE polynomial)";
}

// Test: Single character
TEST_F(CRC32Test, SingleCharacter) {
    const char* input = "a";
    uint32_t result = crc32_hw(reinterpret_cast<const uint8_t*>(input), 1);
    uint32_t expected = zlib_crc32(reinterpret_cast<const uint8_t*>(input), 1);

    EXPECT_EQ(result, expected) << "Single character 'a' CRC32 mismatch";
    EXPECT_EQ(result, 0xE8B7BE43u);
}

// Test: Three characters "abc"
TEST_F(CRC32Test, ThreeCharacters) {
    const char* input = "abc";
    uint32_t result = crc32_hw(reinterpret_cast<const uint8_t*>(input), 3);
    uint32_t expected = zlib_crc32(reinterpret_cast<const uint8_t*>(input), 3);

    EXPECT_EQ(result, expected) << "CRC32 of 'abc' mismatch";
    EXPECT_EQ(result, 0x352441C2u);
}

// Test: All zeros (small buffer)
TEST_F(CRC32Test, AllZerosSmall) {
    std::vector<uint8_t> zeros(16, 0);
    uint32_t result = crc32_hw(zeros.data(), zeros.size());
    uint32_t expected = zlib_crc32(zeros.data(), zeros.size());

    EXPECT_EQ(result, expected) << "16 bytes of zeros CRC32 mismatch";
}

// Test: All zeros (64 bytes - triggers PCLMUL path on x86)
TEST_F(CRC32Test, AllZeros64Bytes) {
    std::vector<uint8_t> zeros(64, 0);
    uint32_t result = crc32_hw(zeros.data(), zeros.size());
    uint32_t expected = zlib_crc32(zeros.data(), zeros.size());

    EXPECT_EQ(result, expected)
        << "64 bytes of zeros CRC32 mismatch - PCLMUL path may be broken";
}

// Test: All zeros (large buffer - 1KB)
TEST_F(CRC32Test, AllZerosLarge) {
    std::vector<uint8_t> zeros(1024, 0);
    uint32_t result = crc32_hw(zeros.data(), zeros.size());
    uint32_t expected = zlib_crc32(zeros.data(), zeros.size());

    EXPECT_EQ(result, expected) << "1KB of zeros CRC32 mismatch";
}

// Test: All 0xFF bytes (small)
TEST_F(CRC32Test, AllOnesSmall) {
    std::vector<uint8_t> ones(16, 0xFF);
    uint32_t result = crc32_hw(ones.data(), ones.size());
    uint32_t expected = zlib_crc32(ones.data(), ones.size());

    EXPECT_EQ(result, expected) << "16 bytes of 0xFF CRC32 mismatch";
}

// Test: All 0xFF bytes (64 bytes - triggers PCLMUL path)
TEST_F(CRC32Test, AllOnes64Bytes) {
    std::vector<uint8_t> ones(64, 0xFF);
    uint32_t result = crc32_hw(ones.data(), ones.size());
    uint32_t expected = zlib_crc32(ones.data(), ones.size());

    EXPECT_EQ(result, expected)
        << "64 bytes of 0xFF CRC32 mismatch - PCLMUL path may be broken";
}

// Test: Sequential bytes 0-255
TEST_F(CRC32Test, SequentialBytes) {
    std::vector<uint8_t> seq(256);
    for (int i = 0; i < 256; ++i) {
        seq[i] = static_cast<uint8_t>(i);
    }
    uint32_t result = crc32_hw(seq.data(), seq.size());
    uint32_t expected = zlib_crc32(seq.data(), seq.size());

    EXPECT_EQ(result, expected) << "Sequential bytes 0-255 CRC32 mismatch";
}

// Test: Unaligned buffer (important for SIMD paths)
TEST_F(CRC32Test, UnalignedBuffer) {
    // Allocate extra space and offset by 1 to ensure unalignment
    std::vector<uint8_t> buffer(129);
    for (size_t i = 0; i < 128; ++i) {
        buffer[i + 1] = static_cast<uint8_t>(i);
    }

    uint32_t result = crc32_hw(buffer.data() + 1, 128);
    uint32_t expected = zlib_crc32(buffer.data() + 1, 128);

    EXPECT_EQ(result, expected) << "Unaligned 128-byte buffer CRC32 mismatch";
}

// Test: Large buffer (1MB) - stress test
TEST_F(CRC32Test, LargeBuffer) {
    std::vector<uint8_t> large(1024 * 1024);
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint32_t result = crc32_hw(large.data(), large.size());
    uint32_t expected = zlib_crc32(large.data(), large.size());

    EXPECT_EQ(result, expected) << "1MB buffer CRC32 mismatch";
}

// Test: Chunk size boundary (8MB - typical chunk size)
TEST_F(CRC32Test, ChunkSizeBoundary) {
    // Use smaller buffer for speed, but test boundary conditions
    std::vector<uint8_t> chunk(8192);
    for (size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<uint8_t>((i * 17) & 0xFF);
    }

    uint32_t result = crc32_hw(chunk.data(), chunk.size());
    uint32_t expected = zlib_crc32(chunk.data(), chunk.size());

    EXPECT_EQ(result, expected) << "8KB chunk CRC32 mismatch";
}

// Test: Verify hardware detection function works
TEST(CRC32HardwareTest, DetectionWorks) {
    // These should not crash
    bool has_hw = has_hw_crc32();
    const char* name = hw_crc32_name();

    EXPECT_NE(name, nullptr);

    // Log what hardware is available
    std::cout << "Hardware CRC32 available: " << (has_hw ? "yes" : "no") << std::endl;
    std::cout << "Hardware CRC32 name: " << name << std::endl;
}

// Test: Verify consistency across multiple calls
TEST_F(CRC32Test, Consistency) {
    const char* input = "The quick brown fox jumps over the lazy dog";
    size_t len = strlen(input);

    uint32_t first = crc32_hw(reinterpret_cast<const uint8_t*>(input), len);

    // Call multiple times - should always return the same value
    for (int i = 0; i < 100; ++i) {
        uint32_t result = crc32_hw(reinterpret_cast<const uint8_t*>(input), len);
        EXPECT_EQ(result, first) << "CRC32 not consistent on iteration " << i;
    }
}

// Test: Various sizes to catch off-by-one errors
TEST_F(CRC32Test, VariousSizes) {
    std::vector<uint8_t> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i);
    }

    // Test sizes 1 through 128
    for (size_t size = 1; size <= 128; ++size) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected) << "CRC32 mismatch at size " << size;
    }
}

// ============================================================================
// PCLMUL Boundary Tests (64-byte threshold on x86)
// ============================================================================

// Test: Sizes around the 64-byte PCLMUL threshold
TEST_F(CRC32Test, PCLMULBoundary) {
    std::vector<uint8_t> data(128);
    fill_sequential(data.data(), data.size());

    // Test sizes around the 64-byte boundary
    for (size_t size = 60; size <= 70; ++size) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected)
            << "CRC32 mismatch at PCLMUL boundary size " << size;
    }
}

// Test: Exact PCLMUL block sizes (multiples of 16)
TEST_F(CRC32Test, PCLMULBlockSizes) {
    std::vector<uint8_t> data(512);
    fill_random(data.data(), data.size(), 12345);

    // Test exact multiples of 16 (PCLMUL processes 16 bytes at a time)
    for (size_t size = 64; size <= 512; size += 16) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected)
            << "CRC32 mismatch at PCLMUL block size " << size;
    }
}

// ============================================================================
// Alignment Tests
// ============================================================================

// Test: All possible alignments (0-15 byte offsets)
TEST_F(CRC32Test, AllAlignments) {
    const size_t data_size = 256;

    for (size_t offset = 0; offset < 16; ++offset) {
        auto [buffer, ptr] = create_aligned_buffer(data_size, offset);
        fill_sequential(ptr, data_size);

        uint32_t result = crc32_hw(ptr, data_size);
        uint32_t expected = zlib_crc32(ptr, data_size);

        EXPECT_EQ(result, expected)
            << "CRC32 mismatch at alignment offset " << offset;
    }
}

// Test: Unaligned with various sizes around PCLMUL boundary
TEST_F(CRC32Test, UnalignedPCLMULBoundary) {
    for (size_t offset = 1; offset < 16; offset += 3) {
        for (size_t size = 60; size <= 70; ++size) {
            auto [buffer, ptr] = create_aligned_buffer(size, offset);
            fill_random(ptr, size, static_cast<uint32_t>(offset * 1000 + size));

            uint32_t result = crc32_hw(ptr, size);
            uint32_t expected = zlib_crc32(ptr, size);

            EXPECT_EQ(result, expected)
                << "CRC32 mismatch at offset " << offset << ", size " << size;
        }
    }
}

// ============================================================================
// Pattern Tests
// ============================================================================

// Test: Alternating bit patterns
TEST_F(CRC32Test, AlternatingBits) {
    std::vector<uint8_t> data_aa(256, 0xAA);  // 10101010
    std::vector<uint8_t> data_55(256, 0x55);  // 01010101

    uint32_t result_aa = crc32_hw(data_aa.data(), data_aa.size());
    uint32_t expected_aa = zlib_crc32(data_aa.data(), data_aa.size());
    EXPECT_EQ(result_aa, expected_aa) << "0xAA pattern CRC32 mismatch";

    uint32_t result_55 = crc32_hw(data_55.data(), data_55.size());
    uint32_t expected_55 = zlib_crc32(data_55.data(), data_55.size());
    EXPECT_EQ(result_55, expected_55) << "0x55 pattern CRC32 mismatch";
}

// Test: Repeating patterns of various lengths
TEST_F(CRC32Test, RepeatingPatterns) {
    std::vector<uint8_t> data(1024);

    // Pattern: 0x12, 0x34
    for (size_t i = 0; i < data.size(); i += 2) {
        data[i] = 0x12;
        if (i + 1 < data.size()) data[i + 1] = 0x34;
    }
    uint32_t result1 = crc32_hw(data.data(), data.size());
    uint32_t expected1 = zlib_crc32(data.data(), data.size());
    EXPECT_EQ(result1, expected1) << "2-byte pattern CRC32 mismatch";

    // Pattern: 0xDE, 0xAD, 0xBE, 0xEF
    for (size_t i = 0; i < data.size(); i += 4) {
        data[i] = 0xDE;
        if (i + 1 < data.size()) data[i + 1] = 0xAD;
        if (i + 2 < data.size()) data[i + 2] = 0xBE;
        if (i + 3 < data.size()) data[i + 3] = 0xEF;
    }
    uint32_t result2 = crc32_hw(data.data(), data.size());
    uint32_t expected2 = zlib_crc32(data.data(), data.size());
    EXPECT_EQ(result2, expected2) << "4-byte pattern CRC32 mismatch";
}

// Test: Single bit set at various positions
TEST_F(CRC32Test, SingleBitSet) {
    std::vector<uint8_t> data(128, 0);

    for (size_t byte_pos = 0; byte_pos < 128; ++byte_pos) {
        for (int bit = 0; bit < 8; ++bit) {
            std::fill(data.begin(), data.end(), 0);
            data[byte_pos] = static_cast<uint8_t>(1 << bit);

            uint32_t result = crc32_hw(data.data(), data.size());
            uint32_t expected = zlib_crc32(data.data(), data.size());

            EXPECT_EQ(result, expected)
                << "CRC32 mismatch with single bit at byte " << byte_pos
                << ", bit " << bit;
        }
    }
}

// ============================================================================
// Large Buffer Tests
// ============================================================================

// Test: 8MB buffer (actual chunk size used by objiff)
TEST_F(CRC32Test, ChunkSize8MB) {
    const size_t chunk_size = 8 * 1024 * 1024;  // 8 MiB
    std::vector<uint8_t> data(chunk_size);
    fill_random(data.data(), data.size(), 0xDEADBEEF);

    uint32_t result = crc32_hw(data.data(), data.size());
    uint32_t expected = zlib_crc32(data.data(), data.size());

    EXPECT_EQ(result, expected) << "8MB chunk CRC32 mismatch";
}

// Test: Power of 2 sizes
TEST_F(CRC32Test, PowerOfTwoSizes) {
    std::vector<uint8_t> data(1024 * 1024);  // 1MB
    fill_sequential(data.data(), data.size());

    std::vector<size_t> sizes = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512,
                                  1024, 2048, 4096, 8192, 16384, 32768,
                                  65536, 131072, 262144, 524288, 1048576};

    for (size_t size : sizes) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected) << "CRC32 mismatch at power-of-2 size " << size;
    }
}

// Test: Prime number sizes (catch off-by-one and alignment issues)
TEST_F(CRC32Test, PrimeNumberSizes) {
    std::vector<uint8_t> data(10000);
    fill_random(data.data(), data.size(), 999);

    std::vector<size_t> primes = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37,
                                   41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
                                   89, 97, 101, 127, 131, 251, 509, 1021, 2039,
                                   4093, 8191};

    for (size_t size : primes) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected) << "CRC32 mismatch at prime size " << size;
    }
}

// ============================================================================
// Random Data Tests (with multiple seeds for coverage)
// ============================================================================

// Test: Multiple random data sets
TEST_F(CRC32Test, RandomDataMultipleSeeds) {
    const size_t size = 4096;
    std::vector<uint8_t> data(size);

    for (uint32_t seed = 0; seed < 100; ++seed) {
        fill_random(data.data(), size, seed);

        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);

        EXPECT_EQ(result, expected) << "CRC32 mismatch with seed " << seed;
    }
}

// Test: Random sizes with random data
TEST_F(CRC32Test, RandomSizesRandomData) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> size_dist(1, 10000);

    std::vector<uint8_t> data(10000);

    for (int i = 0; i < 50; ++i) {
        size_t size = size_dist(rng);
        fill_random(data.data(), size, static_cast<uint32_t>(i * 1000));

        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);

        EXPECT_EQ(result, expected)
            << "CRC32 mismatch at random size " << size << ", iteration " << i;
    }
}

// ============================================================================
// Text/ASCII Data Tests
// ============================================================================

// Test: Common ASCII strings
TEST_F(CRC32Test, ASCIIStrings) {
    std::vector<std::string> test_strings = {
        "Hello, World!",
        "The quick brown fox jumps over the lazy dog",
        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG",
        "0123456789",
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "!@#$%^&*()_+-=[]{}|;':\",./<>?",
        std::string(100, 'A'),
        std::string(100, ' '),
        std::string(100, '\n'),
    };

    for (const auto& str : test_strings) {
        auto* data = reinterpret_cast<const uint8_t*>(str.data());
        size_t len = str.size();

        uint32_t result = crc32_hw(data, len);
        uint32_t expected = zlib_crc32(data, len);

        EXPECT_EQ(result, expected)
            << "CRC32 mismatch for string: \"" << str.substr(0, 50) << "...\"";
    }
}

// Test: JSON-like data
TEST_F(CRC32Test, JSONLikeData) {
    const char* json = R"({
        "name": "test",
        "value": 12345,
        "array": [1, 2, 3, 4, 5],
        "nested": {
            "key": "value",
            "number": 3.14159
        }
    })";

    auto* data = reinterpret_cast<const uint8_t*>(json);
    size_t len = strlen(json);

    uint32_t result = crc32_hw(data, len);
    uint32_t expected = zlib_crc32(data, len);

    EXPECT_EQ(result, expected) << "JSON-like data CRC32 mismatch";
}

// ============================================================================
// Edge Cases
// ============================================================================

// Test: Buffer with all same bytes (each possible byte value)
TEST_F(CRC32Test, AllSameByteValues) {
    std::vector<uint8_t> data(256);

    for (int byte_val = 0; byte_val < 256; ++byte_val) {
        std::fill(data.begin(), data.end(), static_cast<uint8_t>(byte_val));

        uint32_t result = crc32_hw(data.data(), data.size());
        uint32_t expected = zlib_crc32(data.data(), data.size());

        EXPECT_EQ(result, expected)
            << "CRC32 mismatch for all-" << byte_val << " buffer";
    }
}

// Test: Incrementing then decrementing pattern
TEST_F(CRC32Test, IncreasingDecreasingPattern) {
    std::vector<uint8_t> data(512);

    // First half increasing, second half decreasing
    for (size_t i = 0; i < 256; ++i) {
        data[i] = static_cast<uint8_t>(i);
        data[511 - i] = static_cast<uint8_t>(i);
    }

    uint32_t result = crc32_hw(data.data(), data.size());
    uint32_t expected = zlib_crc32(data.data(), data.size());

    EXPECT_EQ(result, expected) << "Increasing/decreasing pattern CRC32 mismatch";
}

// Test: High entropy data (simulates compressed/encrypted data)
TEST_F(CRC32Test, HighEntropyData) {
    std::vector<uint8_t> data(4096);

    // Use a good PRNG to generate high-entropy data
    std::mt19937_64 rng(0xCAFEBABE);
    for (size_t i = 0; i < data.size(); i += 8) {
        uint64_t val = rng();
        for (size_t j = 0; j < 8 && i + j < data.size(); ++j) {
            data[i + j] = static_cast<uint8_t>(val >> (j * 8));
        }
    }

    uint32_t result = crc32_hw(data.data(), data.size());
    uint32_t expected = zlib_crc32(data.data(), data.size());

    EXPECT_EQ(result, expected) << "High entropy data CRC32 mismatch";
}

// Test: Boundary at exact ARM64 processing sizes (8, 4, 2, 1 byte boundaries)
TEST_F(CRC32Test, ARM64ProcessingSizes) {
    std::vector<uint8_t> data(64);
    fill_random(data.data(), data.size(), 777);

    // Test sizes that exercise different ARM64 code paths
    std::vector<size_t> sizes = {
        1, 2, 3, 4, 5, 6, 7, 8,        // Single iterations
        9, 10, 11, 12, 13, 14, 15, 16, // Two iterations
        17, 24, 25, 32, 33, 40, 48, 56 // Multiple iterations
    };

    for (size_t size : sizes) {
        uint32_t result = crc32_hw(data.data(), size);
        uint32_t expected = zlib_crc32(data.data(), size);
        EXPECT_EQ(result, expected)
            << "CRC32 mismatch at ARM64 boundary size " << size;
    }
}

// ============================================================================
// Stress Tests
// ============================================================================

// Test: Many small buffers (throughput test)
TEST_F(CRC32Test, ManySmallBuffers) {
    std::vector<uint8_t> data(64);
    fill_sequential(data.data(), data.size());

    for (int i = 0; i < 10000; ++i) {
        // Modify data slightly each iteration
        data[i % 64] = static_cast<uint8_t>(i & 0xFF);

        uint32_t result = crc32_hw(data.data(), data.size());
        uint32_t expected = zlib_crc32(data.data(), data.size());

        EXPECT_EQ(result, expected) << "CRC32 mismatch at iteration " << i;
    }
}

// Test: Verify CRC32 is not affected by data after the specified length
TEST_F(CRC32Test, LengthBoundary) {
    std::vector<uint8_t> data1(128, 0xAA);
    std::vector<uint8_t> data2(128, 0xAA);

    // Make the last 64 bytes different
    std::fill(data2.begin() + 64, data2.end(), 0x55);

    // CRC of first 64 bytes should be identical
    uint32_t result1 = crc32_hw(data1.data(), 64);
    uint32_t result2 = crc32_hw(data2.data(), 64);

    EXPECT_EQ(result1, result2)
        << "CRC32 should only consider data within specified length";
}

// ============================================================================
// Known CRC32 Values (additional test vectors)
// ============================================================================

TEST_F(CRC32Test, AdditionalKnownVectors) {
    // These are additional well-known CRC32 test vectors
    struct TestVector {
        const char* data;
        size_t len;
        uint32_t expected;
    };

    // Verify against zlib first, then check our implementation
    TestVector vectors[] = {
        {"", 0, 0x00000000},
        {"a", 1, 0xE8B7BE43},
        {"abc", 3, 0x352441C2},
        {"message digest", 14, 0x20159D7F},
        {"abcdefghijklmnopqrstuvwxyz", 26, 0x4C2750BD},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62, 0x1FC2E6D2},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80, 0x7CA94A72},
    };

    for (const auto& tv : vectors) {
        auto* data = reinterpret_cast<const uint8_t*>(tv.data);

        // First verify our expected value matches zlib
        uint32_t zlib_result = zlib_crc32(data, tv.len);
        EXPECT_EQ(zlib_result, tv.expected)
            << "Zlib CRC32 doesn't match expected for \"" << tv.data << "\"";

        // Then verify our implementation matches
        uint32_t result = crc32_hw(data, tv.len);
        EXPECT_EQ(result, tv.expected)
            << "CRC32 mismatch for known vector \"" << tv.data << "\""
            << " (got 0x" << std::hex << result
            << ", expected 0x" << tv.expected << ")";
    }
}

// ============================================================================
// CRC32 Mathematical Properties
// ============================================================================

// Test: CRC32 MUST detect any single bit flip (fundamental property)
TEST_F(CRC32Test, DetectsSingleBitFlip) {
    std::vector<uint8_t> original(256);
    fill_random(original.data(), original.size(), 12345);
    uint32_t original_crc = crc32_hw(original.data(), original.size());

    // Flip each bit - CRC must change every time
    for (size_t byte_pos = 0; byte_pos < original.size(); ++byte_pos) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> modified = original;
            modified[byte_pos] ^= (1 << bit);

            uint32_t modified_crc = crc32_hw(modified.data(), modified.size());
            EXPECT_NE(original_crc, modified_crc)
                << "CRC32 failed to detect bit flip at byte " << byte_pos
                << " bit " << bit;
        }
    }
}

// Test: CRC32 should detect burst errors (consecutive bit errors)
TEST_F(CRC32Test, DetectsBurstErrors) {
    std::vector<uint8_t> original(256);
    fill_random(original.data(), original.size(), 54321);
    uint32_t original_crc = crc32_hw(original.data(), original.size());

    // Test burst errors of various lengths at various positions
    std::vector<int> burst_lengths = {2, 3, 4, 8, 16, 32};

    for (int burst_len : burst_lengths) {
        for (size_t start_byte = 0; start_byte < 128; start_byte += 17) {
            std::vector<uint8_t> modified = original;

            // Flip 'burst_len' consecutive bits starting at start_byte
            for (int i = 0; i < burst_len; ++i) {
                size_t byte_idx = start_byte + (i / 8);
                int bit_idx = i % 8;
                if (byte_idx < modified.size()) {
                    modified[byte_idx] ^= (1 << bit_idx);
                }
            }

            uint32_t modified_crc = crc32_hw(modified.data(), modified.size());
            EXPECT_NE(original_crc, modified_crc)
                << "CRC32 failed to detect " << burst_len
                << "-bit burst error at byte " << start_byte;
        }
    }
}

// Test: Verify we're using IEEE CRC32, NOT CRC32-C (Castagnoli)
TEST_F(CRC32Test, NotCRC32C) {
    // The canonical test string "123456789" has different CRC values:
    // - CRC32 (IEEE):      0xCBF43926
    // - CRC32-C (Castagnoli): 0xE3069283

    const char* input = "123456789";
    uint32_t result = crc32_hw(reinterpret_cast<const uint8_t*>(input), 9);

    // MUST NOT be CRC32-C
    EXPECT_NE(result, 0xE3069283u)
        << "CRITICAL: Implementation is using CRC32-C polynomial, not IEEE CRC32! "
        << "This will cause mismatches with S3.";

    // MUST be IEEE CRC32
    EXPECT_EQ(result, 0xCBF43926u)
        << "CRC32 does not match IEEE polynomial result";
}

// Test: Verify polynomial by checking multiple known differences
TEST_F(CRC32Test, PolynomialVerification) {
    // Additional test vectors where CRC32 and CRC32-C differ significantly
    struct PolyTestVector {
        const char* data;
        size_t len;
        uint32_t crc32_ieee;   // What we want
        uint32_t crc32c;       // What we DON'T want
    };

    PolyTestVector vectors[] = {
        {"123456789", 9, 0xCBF43926, 0xE3069283},
        {"a", 1, 0xE8B7BE43, 0xC1D04330},
        {"test", 4, 0xD87F7E0C, 0x86A072C0},
    };

    for (const auto& tv : vectors) {
        auto* data = reinterpret_cast<const uint8_t*>(tv.data);
        uint32_t result = crc32_hw(data, tv.len);

        EXPECT_EQ(result, tv.crc32_ieee)
            << "Wrong polynomial for \"" << tv.data << "\"";
        EXPECT_NE(result, tv.crc32c)
            << "Using CRC32-C instead of IEEE for \"" << tv.data << "\"";
    }
}

// ============================================================================
// S3 Compatibility Tests
// ============================================================================

// Test: S3 returns CRC32 as Base64-encoded big-endian bytes
// Verify our CRC32 matches what S3 would return
TEST_F(CRC32Test, S3Base64Format) {
    // S3 encodes CRC32 as: Base64(BigEndian(crc32))
    // For "123456789", CRC32 = 0xCBF43926
    // Big-endian bytes: CB F4 39 26
    // Base64("CBF43926" as bytes) = "y/Q5Jg=="

    const char* input = "123456789";
    uint32_t crc = crc32_hw(reinterpret_cast<const uint8_t*>(input), 9);

    // Extract bytes in big-endian order (how S3 encodes it)
    uint8_t bytes[4];
    bytes[0] = (crc >> 24) & 0xFF;
    bytes[1] = (crc >> 16) & 0xFF;
    bytes[2] = (crc >> 8) & 0xFF;
    bytes[3] = crc & 0xFF;

    // Verify the bytes match expected big-endian representation
    EXPECT_EQ(bytes[0], 0xCB);
    EXPECT_EQ(bytes[1], 0xF4);
    EXPECT_EQ(bytes[2], 0x39);
    EXPECT_EQ(bytes[3], 0x26);
}

// Test: Simulate S3 multipart chunk CRCs
TEST_F(CRC32Test, S3MultipartChunks) {
    // Simulate a file split into 8MB chunks like S3 multipart
    const size_t chunk_size = 8 * 1024 * 1024;
    const size_t num_chunks = 3;

    std::vector<uint8_t> full_file(chunk_size * num_chunks);
    fill_random(full_file.data(), full_file.size(), 0x535353);

    // Compute CRC32 for each chunk (like S3 does)
    std::vector<uint32_t> chunk_crcs;
    for (size_t i = 0; i < num_chunks; ++i) {
        uint32_t chunk_crc = crc32_hw(
            full_file.data() + (i * chunk_size),
            chunk_size
        );
        uint32_t expected = zlib_crc32(
            full_file.data() + (i * chunk_size),
            chunk_size
        );
        EXPECT_EQ(chunk_crc, expected)
            << "Chunk " << i << " CRC32 mismatch";
        chunk_crcs.push_back(chunk_crc);
    }

    // Verify chunks are different (sanity check)
    EXPECT_NE(chunk_crcs[0], chunk_crcs[1]);
    EXPECT_NE(chunk_crcs[1], chunk_crcs[2]);
}

// Test: Last chunk smaller than chunk size (common in real files)
TEST_F(CRC32Test, S3LastChunkPartial) {
    const size_t chunk_size = 8 * 1024 * 1024;
    const size_t last_chunk_size = 1234567;  // Partial last chunk
    const size_t total_size = chunk_size * 2 + last_chunk_size;

    std::vector<uint8_t> file_data(total_size);
    fill_random(file_data.data(), file_data.size(), 0xFA271A1);

    // Full chunks
    for (size_t i = 0; i < 2; ++i) {
        uint32_t result = crc32_hw(file_data.data() + (i * chunk_size), chunk_size);
        uint32_t expected = zlib_crc32(file_data.data() + (i * chunk_size), chunk_size);
        EXPECT_EQ(result, expected) << "Full chunk " << i << " CRC32 mismatch";
    }

    // Partial last chunk
    uint32_t last_result = crc32_hw(file_data.data() + (2 * chunk_size), last_chunk_size);
    uint32_t last_expected = zlib_crc32(file_data.data() + (2 * chunk_size), last_chunk_size);
    EXPECT_EQ(last_result, last_expected) << "Partial last chunk CRC32 mismatch";
}

// ============================================================================
// Incremental CRC32 Tests
// ============================================================================

// Test: Verify zlib's incremental CRC32 for reference
// (Our crc32_hw doesn't support incremental, but zlib does)
TEST_F(CRC32Test, ZlibIncrementalReference) {
    std::vector<uint8_t> data(1024);
    fill_sequential(data.data(), data.size());

    // Compute full CRC
    uint32_t full_crc = zlib_crc32(data.data(), data.size());

    // Compute incrementally with zlib
    uint32_t incremental_crc = 0;
    incremental_crc = ::crc32(incremental_crc, data.data(), 256);
    incremental_crc = ::crc32(incremental_crc, data.data() + 256, 256);
    incremental_crc = ::crc32(incremental_crc, data.data() + 512, 256);
    incremental_crc = ::crc32(incremental_crc, data.data() + 768, 256);

    EXPECT_EQ(full_crc, incremental_crc)
        << "Incremental CRC32 should match full computation";

    // Verify our implementation matches the full CRC
    uint32_t our_result = crc32_hw(data.data(), data.size());
    EXPECT_EQ(our_result, full_crc);
}

// Test: CRC32 combine property (for parallel computation)
TEST_F(CRC32Test, CRC32CombineProperty) {
    // zlib provides crc32_combine(crc1, crc2, len2) which computes
    // the CRC of data1 || data2 given CRC(data1), CRC(data2), and len(data2)

    std::vector<uint8_t> part1(512);
    std::vector<uint8_t> part2(768);
    fill_random(part1.data(), part1.size(), 111);
    fill_random(part2.data(), part2.size(), 222);

    // Combine into one buffer
    std::vector<uint8_t> combined(part1.size() + part2.size());
    std::copy(part1.begin(), part1.end(), combined.begin());
    std::copy(part2.begin(), part2.end(), combined.begin() + part1.size());

    // CRC of combined
    uint32_t combined_crc = crc32_hw(combined.data(), combined.size());
    uint32_t expected = zlib_crc32(combined.data(), combined.size());
    EXPECT_EQ(combined_crc, expected);

    // CRCs of parts
    uint32_t crc1 = zlib_crc32(part1.data(), part1.size());
    uint32_t crc2 = zlib_crc32(part2.data(), part2.size());

    // Use zlib's combine function
    uint32_t computed_combined = ::crc32_combine(crc1, crc2, part2.size());
    EXPECT_EQ(computed_combined, combined_crc)
        << "CRC32 combine should produce same result as direct computation";
}

// ============================================================================
// Endianness and Byte Order Tests
// ============================================================================

// Test: CRC32 with specific byte patterns to verify endianness handling
TEST_F(CRC32Test, EndiannessPatterns) {
    // Test patterns that would fail if endianness is handled incorrectly
    uint8_t pattern1[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t pattern2[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};

    uint32_t crc1 = crc32_hw(pattern1, sizeof(pattern1));
    uint32_t crc2 = crc32_hw(pattern2, sizeof(pattern2));

    uint32_t expected1 = zlib_crc32(pattern1, sizeof(pattern1));
    uint32_t expected2 = zlib_crc32(pattern2, sizeof(pattern2));

    EXPECT_EQ(crc1, expected1);
    EXPECT_EQ(crc2, expected2);
    EXPECT_NE(crc1, crc2) << "Reversed bytes should have different CRC";
}

// Test: 64-bit aligned data (important for ARM64 __crc32d)
TEST_F(CRC32Test, Aligned64BitData) {
    alignas(8) uint8_t data[64];
    for (int i = 0; i < 64; ++i) {
        data[i] = static_cast<uint8_t>(i * 3);
    }

    uint32_t result = crc32_hw(data, sizeof(data));
    uint32_t expected = zlib_crc32(data, sizeof(data));

    EXPECT_EQ(result, expected) << "64-bit aligned data CRC32 mismatch";
}

// ============================================================================
// Collision Resistance (Sanity Checks)
// ============================================================================

// Test: Different data should (almost always) produce different CRCs
TEST_F(CRC32Test, CollisionResistance) {
    std::set<uint32_t> seen_crcs;
    std::vector<uint8_t> data(64);

    // Generate 10000 different random inputs
    for (int i = 0; i < 10000; ++i) {
        fill_random(data.data(), data.size(), static_cast<uint32_t>(i));
        uint32_t crc = crc32_hw(data.data(), data.size());
        seen_crcs.insert(crc);
    }

    // With 10000 random inputs and 2^32 possible CRCs, collisions are very unlikely
    // Allow for a tiny number of collisions (birthday paradox gives ~0.01 expected)
    EXPECT_GE(seen_crcs.size(), 9990u)
        << "Too many CRC32 collisions - possible implementation bug";
}

// Test: Similar data should produce different CRCs
TEST_F(CRC32Test, SimilarDataDifferentCRCs) {
    std::vector<uint8_t> data1(256, 0x00);
    std::vector<uint8_t> data2(256, 0x00);

    // Change just one byte
    data2[128] = 0x01;

    uint32_t crc1 = crc32_hw(data1.data(), data1.size());
    uint32_t crc2 = crc32_hw(data2.data(), data2.size());

    EXPECT_NE(crc1, crc2) << "Single byte change must produce different CRC";

    // Verify the difference isn't trivial
    uint32_t diff = crc1 ^ crc2;
    int bits_different = __builtin_popcount(diff);
    EXPECT_GE(bits_different, 8)
        << "CRC32 should have good avalanche effect (many bits changed)";
}

// ============================================================================
// Robustness Tests
// ============================================================================

// Test: Repeated computation gives same result (no state leakage)
TEST_F(CRC32Test, NoStateLeakage) {
    std::vector<uint8_t> data1(128);
    std::vector<uint8_t> data2(256);
    fill_random(data1.data(), data1.size(), 1);
    fill_random(data2.data(), data2.size(), 2);

    // Compute CRC of data1
    uint32_t crc1_first = crc32_hw(data1.data(), data1.size());

    // Compute CRC of data2 (should not affect data1's result)
    crc32_hw(data2.data(), data2.size());

    // Compute CRC of data1 again
    uint32_t crc1_second = crc32_hw(data1.data(), data1.size());

    EXPECT_EQ(crc1_first, crc1_second)
        << "CRC32 function has internal state leakage!";
}

// Test: Concurrent CRC32 computation (thread safety)
TEST_F(CRC32Test, ThreadSafety) {
    const int num_threads = 8;
    const int iterations = 1000;

    std::vector<std::vector<uint8_t>> data(num_threads);
    std::vector<uint32_t> expected(num_threads);

    // Prepare data and expected results
    for (int i = 0; i < num_threads; ++i) {
        data[i].resize(1024);
        fill_random(data[i].data(), data[i].size(), static_cast<uint32_t>(i * 1000));
        expected[i] = zlib_crc32(data[i].data(), data[i].size());
    }

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < iterations && !failed; ++i) {
                uint32_t result = crc32_hw(data[t].data(), data[t].size());
                if (result != expected[t]) {
                    failed = true;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(failed) << "CRC32 computation is not thread-safe!";
}

// ============================================================================
// Hardware path agreement (issue #18)
// ============================================================================
//
// The removed x86 PCLMUL path was wrong for 16-byte-aligned buffers of 64 bytes
// or more.
//
// The tests above would have caught it - glibc malloc returns 16-byte-aligned
// pointers, so their std::vector buffers do reach the aligned branch, and 33 of
// them fail when the broken code is compiled in. The gap was never test
// coverage: it was that no build enables __PCLMUL__, so the path was never
// compiled and never ran. That is what mito_tests_crc32_native addresses.
//
// These add explicit intent rather than new detection power: alignment is
// guaranteed rather than incidental, and the sweep covers the sizes and offsets
// where a vectorised implementation changes strategy.

class CrcHardwarePath : public CRC32Test {
protected:
    // 16-byte aligned, which is what a PCLMUL/SSE path requires.
    alignas(16) uint8_t buf_[4096];

    void fill(size_t n, int seed) {
        for (size_t i = 0; i < n; ++i) {
            buf_[i] = static_cast<uint8_t>((i * 31 + seed * 7 + 11) & 0xFF);
        }
    }
};

TEST_F(CrcHardwarePath, MatchesZlibOnAlignedBuffers) {
    ASSERT_EQ(reinterpret_cast<uintptr_t>(buf_) % 16, 0u)
        << "the buffer must be aligned or this test cannot exercise a vector path";

    // Dense sweep across the 64-byte threshold, then block multiples and
    // deliberately awkward tails.
    std::vector<size_t> sizes;
    for (size_t n = 0; n <= 200; ++n) sizes.push_back(n);
    for (size_t n : {255u, 256u, 257u, 511u, 512u, 513u, 1023u, 1024u,
                     1025u, 4095u, 4096u}) {
        sizes.push_back(n);
    }

    for (size_t n : sizes) {
        fill(n, static_cast<int>(n));
        uint32_t got = crc32_hw(buf_, n);
        uint32_t want = zlib_crc32(buf_, n);
        ASSERT_EQ(got, want) << "crc32_hw disagrees with zlib at aligned length " << n;
    }
}

TEST_F(CrcHardwarePath, MatchesZlibAtEveryAlignmentOffset) {
    // A vectorised path typically branches on alignment; walk every offset
    // within a 16-byte window so none of those branches goes unchecked.
    for (size_t offset = 0; offset < 16; ++offset) {
        // Past 129 too: a fold-by-4 or VPCLMUL path typically does not change
        // strategy until 256+ bytes, so a short sweep would miss its branches.
        for (size_t n : {63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u,
                         511u, 512u, 1023u, 1024u, 4000u}) {
            fill(n + offset, static_cast<int>(offset));
            const uint8_t* p = buf_ + offset;
            uint32_t got = crc32_hw(p, n);
            uint32_t want = zlib_crc32(p, n);
            ASSERT_EQ(got, want)
                << "crc32_hw disagrees with zlib at offset " << offset << " length " << n;
        }
    }
}

TEST_F(CrcHardwarePath, ReportedNameMatchesWhetherAPathExists) {
    // Guards the reporting against drifting apart from the implementation.
    // Both answers now come from runtime CPU detection rather than from what
    // the compiler was told, which is what made the old report claim "x86
    // PCLMUL" on a build that had compiled no PCLMUL path at all (issue #22).
    const std::string name = hw_crc32_name();
    EXPECT_NE(name.find("zlib-ng"), std::string::npos)
        << "the name should say which library computes the CRC: " << name;

    const bool names_an_isa = name.find("PCLMULQDQ") != std::string::npos ||
                              name.find("CRC32") != std::string::npos;
    if (has_hw_crc32()) {
        EXPECT_TRUE(names_an_isa)
            << "an accelerated CPU was detected but the name does not say which: " << name;
        EXPECT_EQ(name.find("portable C"), std::string::npos)
            << "claiming acceleration while naming the portable path: " << name;
    } else {
        EXPECT_FALSE(names_an_isa)
            << "no accelerated instruction set here, but the name advertises one: " << name;
    }
}

TEST_F(CrcHardwarePath, HardwareClaimMatchesTheKernelsView) {
    // The test above compares the report against itself: both answers come from
    // the same predicate, so it cannot fail. This one asks something that does
    // not know about mito - the kernel's own CPU feature list - which is the
    // only independent oracle available here, since zlib-ng exposes no API for
    // which implementation its dispatch table ended up holding.
#if !(defined(__x86_64__) || defined(__i386__)) || !defined(__linux__)
    GTEST_SKIP() << "no independent CPU feature oracle on this platform";
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) {
        GTEST_SKIP() << "/proc/cpuinfo is not readable";
    }

    bool kernel_says_pclmulqdq = false;
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("flags", 0) != 0) {
            continue;
        }
        // Match the whole token: "pclmulqdq" must not be found inside another.
        std::istringstream fields(line);
        std::string field;
        while (fields >> field) {
            if (field == "pclmulqdq") {
                kernel_says_pclmulqdq = true;
            }
        }
        break;
    }

    EXPECT_EQ(has_hw_crc32(), kernel_says_pclmulqdq)
        << "mito reports " << (has_hw_crc32() ? "" : "no ") << "acceleration ("
        << hw_crc32_name() << ") while the kernel reports "
        << (kernel_says_pclmulqdq ? "" : "no ") << "pclmulqdq";
#endif
}

// ============================================================================
// Length is not truncated to 32 bits (issues #48, #21)
// ============================================================================

TEST_F(CRC32Test, DoesNotTruncateLengthAtFourGiB) {
    // crc32_hw used to hand the length to zlib through a uInt cast, so a buffer
    // of 4 GiB or more was checksummed over length mod 2^32 bytes - here, 100
    // bytes instead of 4 GiB + 100. In PutObjectFromFile that checksum goes to
    // S3 as x-amz-checksum-crc32, so S3 was validating an upload against the
    // wrong bytes.
    if (sizeof(size_t) < 8) {
        GTEST_SKIP() << "no buffer this large can exist on a 32-bit size_t";
    }

    const size_t kOverflow = (static_cast<size_t>(1) << 32) + 100;

    // Read-only anonymous pages all resolve to the shared zero page, so this
    // maps 4 GiB while costing a couple of megabytes of RSS.
    void* mapping = mmap(nullptr, kOverflow, PROT_READ,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mapping == MAP_FAILED) {
        GTEST_SKIP() << "could not map " << kOverflow << " bytes: " << std::strerror(errno);
    }
    const uint8_t* data = static_cast<const uint8_t*>(mapping);

    // zlib's own 64-bit-length entry point is the reference. If the length were
    // truncated the result would instead equal the CRC of the first 100 bytes,
    // so the two expectations below cannot both hold by accident.
    const uint32_t expected = static_cast<uint32_t>(::crc32_z(0, data, kOverflow));
    const uint32_t truncated = static_cast<uint32_t>(::crc32_z(0, data, 100));

    const uint32_t got = crc32_hw(data, kOverflow);
    munmap(mapping, kOverflow);

    EXPECT_EQ(got, expected) << "a 4 GiB + 100 byte buffer must be checksummed in full";
    EXPECT_NE(got, truncated) << "length was truncated to 32 bits";
}
