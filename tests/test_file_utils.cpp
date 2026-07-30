#include <gtest/gtest.h>
#include "s3_utils.h"
#include "constants.h"
#include "fd_limits.h"
#include "app_settings.h"
#include <sys/resource.h>
#include "dns_cache.h"
#include <aws/s3/S3Errors.h>
#include <aws/core/client/AWSError.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// GetLocalFileSize Tests
// ============================================================================

class LocalFileSizeTest : public ::testing::Test {
protected:
    std::string temp_dir;
    std::vector<std::string> temp_files;

    void SetUp() override {
        temp_dir = "/tmp/objiff_test_" + std::to_string(getpid());
        mkdir(temp_dir.c_str(), 0755);
    }

    void TearDown() override {
        // Clean up temp files
        for (const auto& file : temp_files) {
            std::remove(file.c_str());
        }
        rmdir(temp_dir.c_str());
    }

    std::string create_temp_file(size_t size, const std::string& name = "test.bin") {
        std::string path = temp_dir + "/" + name;
        std::ofstream out(path, std::ios::binary);
        if (size > 0) {
            std::vector<char> data(size, 'X');
            out.write(data.data(), data.size());
        }
        out.close();
        temp_files.push_back(path);
        return path;
    }
};

TEST_F(LocalFileSizeTest, EmptyFile) {
    std::string path = create_temp_file(0);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, 0);
}

TEST_F(LocalFileSizeTest, SmallFile) {
    std::string path = create_temp_file(100);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, 100);
}

TEST_F(LocalFileSizeTest, ExactChunkSize) {
    std::string path = create_temp_file(CHUNK_SIZE);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, CHUNK_SIZE);
}

TEST_F(LocalFileSizeTest, MultipleChunks) {
    size_t file_size = CHUNK_SIZE * 2 + 1234;
    std::string path = create_temp_file(file_size);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, static_cast<int64_t>(file_size));
}

TEST_F(LocalFileSizeTest, OneByte) {
    std::string path = create_temp_file(1);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, 1);
}

TEST_F(LocalFileSizeTest, OneKiB) {
    std::string path = create_temp_file(1024);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, 1024);
}

TEST_F(LocalFileSizeTest, OneMiB) {
    std::string path = create_temp_file(1024 * 1024);
    int64_t size = GetLocalFileSize(path);
    EXPECT_EQ(size, 1024 * 1024);
}

TEST_F(LocalFileSizeTest, NonExistentFile) {
    int64_t size = GetLocalFileSize("/nonexistent/path/to/file.bin");
    EXPECT_EQ(size, -1);
}

TEST_F(LocalFileSizeTest, DirectoryNotFile) {
    int64_t size = GetLocalFileSize(temp_dir);
    // Directories have a size, but it's not meaningful for our purposes
    // The function should still return something (implementation-dependent)
    // Main thing is it doesn't crash
    EXPECT_GE(size, 0);
}

TEST_F(LocalFileSizeTest, EmptyPath) {
    int64_t size = GetLocalFileSize("");
    EXPECT_EQ(size, -1);
}

// ============================================================================
// Constants Tests
// ============================================================================

TEST(ConstantsTest, ChunkSizeValue) {
    // Chunk size should be 8 MiB
    EXPECT_EQ(CHUNK_SIZE, 8 * 1024 * 1024);
}

TEST(ConstantsTest, ChunkSizeType) {
    // Chunk size should be int64_t to handle large files
    static_assert(std::is_same<decltype(CHUNK_SIZE), const int64_t>::value,
                  "CHUNK_SIZE should be int64_t");
}

TEST(ConstantsTest, ChunkSizePowerOfTwo) {
    // Verify chunk size is a power of 2 (important for alignment)
    int64_t size = CHUNK_SIZE;
    EXPECT_GT(size, 0);
    EXPECT_EQ(size & (size - 1), 0) << "CHUNK_SIZE should be a power of 2";
}

TEST(ConstantsTest, ChunkSizeReasonable) {
    // Chunk size should be between 1 MiB and 1 GiB
    EXPECT_GE(CHUNK_SIZE, 1024 * 1024);
    EXPECT_LE(CHUNK_SIZE, 1024 * 1024 * 1024);
}

// ============================================================================
// Chunk Calculation Tests
// ============================================================================

class ChunkCalculationTest : public ::testing::Test {
protected:
    // Helper to calculate number of chunks for a given file size
    static int64_t calc_num_chunks(int64_t file_size) {
        return (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    }
};

TEST_F(ChunkCalculationTest, EmptyFile) {
    EXPECT_EQ(calc_num_chunks(0), 0);
}

TEST_F(ChunkCalculationTest, OneByte) {
    EXPECT_EQ(calc_num_chunks(1), 1);
}

TEST_F(ChunkCalculationTest, ExactlyOneChunk) {
    EXPECT_EQ(calc_num_chunks(CHUNK_SIZE), 1);
}

TEST_F(ChunkCalculationTest, OneByteOverChunk) {
    EXPECT_EQ(calc_num_chunks(CHUNK_SIZE + 1), 2);
}

TEST_F(ChunkCalculationTest, TwoChunks) {
    EXPECT_EQ(calc_num_chunks(CHUNK_SIZE * 2), 2);
}

TEST_F(ChunkCalculationTest, TwoAndHalfChunks) {
    EXPECT_EQ(calc_num_chunks(CHUNK_SIZE * 2 + CHUNK_SIZE / 2), 3);
}

TEST_F(ChunkCalculationTest, LargeFile1GiB) {
    int64_t file_size = 1024LL * 1024 * 1024;  // 1 GiB
    int64_t expected_chunks = file_size / CHUNK_SIZE;  // 128 chunks
    EXPECT_EQ(calc_num_chunks(file_size), expected_chunks);
}

TEST_F(ChunkCalculationTest, LargeFile100GiB) {
    int64_t file_size = 100LL * 1024 * 1024 * 1024;  // 100 GiB
    int64_t expected_chunks = file_size / CHUNK_SIZE;  // 12800 chunks
    EXPECT_EQ(calc_num_chunks(file_size), expected_chunks);
}

TEST_F(ChunkCalculationTest, MaxInt64Chunks) {
    // Test with a very large (but reasonable) file size
    int64_t file_size = 1LL * 1024 * 1024 * 1024 * 1024;  // 1 TiB
    int64_t expected_chunks = file_size / CHUNK_SIZE;  // 131072 chunks
    EXPECT_EQ(calc_num_chunks(file_size), expected_chunks);
}

// ============================================================================
// Chunk Offset Calculation Tests
// ============================================================================

TEST(ChunkOffsetTest, FirstChunk) {
    int64_t chunk_id = 0;
    int64_t offset = chunk_id * CHUNK_SIZE;
    EXPECT_EQ(offset, 0);
}

TEST(ChunkOffsetTest, SecondChunk) {
    int64_t chunk_id = 1;
    int64_t offset = chunk_id * CHUNK_SIZE;
    EXPECT_EQ(offset, CHUNK_SIZE);
}

TEST(ChunkOffsetTest, HundredthChunk) {
    int64_t chunk_id = 99;
    int64_t offset = chunk_id * CHUNK_SIZE;
    EXPECT_EQ(offset, 99 * CHUNK_SIZE);
}

// ============================================================================
// Chunk Length Calculation Tests
// ============================================================================

class ChunkLengthTest : public ::testing::Test {
protected:
    // Calculate the length of a specific chunk
    static int64_t calc_chunk_length(int64_t file_size, int64_t chunk_id) {
        int64_t offset = chunk_id * CHUNK_SIZE;
        if (offset >= file_size) return 0;
        return std::min(CHUNK_SIZE, file_size - offset);
    }
};

TEST_F(ChunkLengthTest, SingleFullChunk) {
    int64_t file_size = CHUNK_SIZE;
    EXPECT_EQ(calc_chunk_length(file_size, 0), CHUNK_SIZE);
}

TEST_F(ChunkLengthTest, SinglePartialChunk) {
    int64_t file_size = CHUNK_SIZE / 2;
    EXPECT_EQ(calc_chunk_length(file_size, 0), CHUNK_SIZE / 2);
}

TEST_F(ChunkLengthTest, LastChunkPartial) {
    int64_t file_size = CHUNK_SIZE + 1000;
    EXPECT_EQ(calc_chunk_length(file_size, 0), CHUNK_SIZE);  // First chunk full
    EXPECT_EQ(calc_chunk_length(file_size, 1), 1000);        // Last chunk partial
}

TEST_F(ChunkLengthTest, LastChunkFull) {
    int64_t file_size = CHUNK_SIZE * 3;
    EXPECT_EQ(calc_chunk_length(file_size, 0), CHUNK_SIZE);
    EXPECT_EQ(calc_chunk_length(file_size, 1), CHUNK_SIZE);
    EXPECT_EQ(calc_chunk_length(file_size, 2), CHUNK_SIZE);
}

TEST_F(ChunkLengthTest, ChunkBeyondFile) {
    int64_t file_size = CHUNK_SIZE;
    EXPECT_EQ(calc_chunk_length(file_size, 1), 0);  // No second chunk
}

TEST_F(ChunkLengthTest, TinyFile) {
    int64_t file_size = 1;
    EXPECT_EQ(calc_chunk_length(file_size, 0), 1);
}

// ============================================================================
// S3ListResult Structure Tests
// ============================================================================

TEST(S3ListResultTest, DefaultValues) {
    S3ListResult result;
    EXPECT_TRUE(result.objects.empty());
    EXPECT_TRUE(result.common_prefixes.empty());
    EXPECT_TRUE(result.next_continuation_token.empty());
    EXPECT_FALSE(result.is_truncated);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.empty());
}

TEST(S3ListResultTest, SuccessResult) {
    S3ListResult result;
    result.success = true;
    result.objects = {{"key1", 100}, {"key2", 200}, {"key3", 300}};
    result.common_prefixes = {"folder1/", "folder2/"};

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects.size(), 3u);
    EXPECT_EQ(result.objects[0].key, "key1");
    EXPECT_EQ(result.objects[0].size, 100);
    EXPECT_EQ(result.common_prefixes.size(), 2u);
}

TEST(S3ListResultTest, PaginatedResult) {
    S3ListResult result;
    result.success = true;
    result.is_truncated = true;
    result.next_continuation_token = "token123";

    EXPECT_TRUE(result.is_truncated);
    EXPECT_EQ(result.next_continuation_token, "token123");
}

TEST(S3ListResultTest, ErrorResult) {
    S3ListResult result;
    result.success = false;
    result.error_message = "Access Denied";

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "Access Denied");
}

// ============================================================================
// Local CRC32 Computation Tests
// ============================================================================

#include "crc32_chunks.h"
#include "crc32_hw.h"
#include <filesystem>

class LocalCRC32Test : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/objiff_crc_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    std::string create_temp_file(const std::vector<uint8_t>& data, const std::string& name = "test.bin") {
        std::string path = temp_dir + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        return path;
    }
};

TEST_F(LocalCRC32Test, SmallFile) {
    std::vector<uint8_t> data(1000, 'A');
    std::string path = create_temp_file(data);

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    ASSERT_EQ(crcs.size(), 1u);

    // Verify CRC matches
    uint32_t expected = crc32_hw(data.data(), data.size());
    EXPECT_EQ(crcs[0], expected);
}

TEST_F(LocalCRC32Test, MultipleChunks) {
    int64_t file_size = CHUNK_SIZE * 2 + 1000;
    std::vector<uint8_t> data(file_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    std::string path = create_temp_file(data);

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    ASSERT_EQ(crcs.size(), 3u);

    // Verify each chunk's CRC
    uint32_t crc0 = crc32_hw(data.data(), CHUNK_SIZE);
    uint32_t crc1 = crc32_hw(data.data() + CHUNK_SIZE, CHUNK_SIZE);
    uint32_t crc2 = crc32_hw(data.data() + 2 * CHUNK_SIZE, 1000);

    EXPECT_EQ(crcs[0], crc0);
    EXPECT_EQ(crcs[1], crc1);
    EXPECT_EQ(crcs[2], crc2);
}

TEST_F(LocalCRC32Test, SpecificChunks) {
    int64_t file_size = CHUNK_SIZE * 5;
    std::vector<uint8_t> data(file_size, 'B');
    std::string path = create_temp_file(data);

    // Request only chunks 1 and 3
    std::vector<int64_t> chunk_ids = {1, 3};
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);
    ASSERT_EQ(crcs.size(), 2u);

    uint32_t expected_crc = crc32_hw(data.data(), CHUNK_SIZE);
    EXPECT_EQ(crcs[0], expected_crc);
    EXPECT_EQ(crcs[1], expected_crc);
}

TEST_F(LocalCRC32Test, WithProgressCallback) {
    int64_t file_size = CHUNK_SIZE * 4;
    std::vector<uint8_t> data(file_size, 'C');
    std::string path = create_temp_file(data);

    std::vector<double> progress_values;
    auto progress_cb = [&progress_values](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);
    ASSERT_EQ(crcs.size(), 4u);

    // Should have 4 progress callbacks
    ASSERT_EQ(progress_values.size(), 4u);
    EXPECT_DOUBLE_EQ(progress_values[3], 100.0);
}

TEST_F(LocalCRC32Test, NonExistentFile) {
    auto crcs = compute_crc32_chunks_boost_asio("/nonexistent/path/file.bin", {}, nullptr);
    EXPECT_TRUE(crcs.empty());
}

TEST_F(LocalCRC32Test, ExactlyOneChunk) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    std::string path = create_temp_file(data);

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    ASSERT_EQ(crcs.size(), 1u);

    uint32_t expected = crc32_hw(data.data(), CHUNK_SIZE);
    EXPECT_EQ(crcs[0], expected);
}

TEST_F(LocalCRC32Test, OneBytePastChunk) {
    std::vector<uint8_t> data(CHUNK_SIZE + 1, 'Y');
    std::string path = create_temp_file(data);

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    ASSERT_EQ(crcs.size(), 2u);
}

TEST_F(LocalCRC32Test, OutOfBoundsChunkIds) {
    int64_t file_size = CHUNK_SIZE * 2;
    std::vector<uint8_t> data(file_size, 'Z');
    std::string path = create_temp_file(data);

    // Chunk ID 5 is out of bounds (only 0 and 1 exist)
    std::vector<int64_t> chunk_ids = {0, 5};
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);

    // An out-of-bounds chunk cannot be checksummed, and 0 is a legitimate
    // CRC32 value, so the request fails rather than returning a fabricated
    // zero the caller would compare as real. The S3 mock rejects invalid chunk
    // ids outright; and since #26 the real client rejects the same range.
    EXPECT_TRUE(crcs.empty())
        << "an invalid chunk id must fail the request, not yield a zero CRC";
}

TEST_F(LocalCRC32Test, NegativeChunkIds) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'N');
    std::string path = create_temp_file(data);

    // A negative chunk id is a caller error, not a chunk whose CRC is 0.
    std::vector<int64_t> chunk_ids = {0, -1};
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);

    EXPECT_TRUE(crcs.empty())
        << "a negative chunk id must fail the request, not yield a zero CRC";
}

// ============================================================================
// Local-to-Local Comparison Tests
// ============================================================================

#include "comparison_task.h"

class LocalComparisonTest : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/objiff_local_cmp_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    std::string create_temp_file(const std::vector<uint8_t>& data, const std::string& name) {
        std::string path = temp_dir + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        return path;
    }
};

TEST_F(LocalComparisonTest, IdenticalFiles) {
    std::vector<uint8_t> data(CHUNK_SIZE + 1000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    std::string path_a = create_temp_file(data, "file_a.bin");
    std::string path_b = create_temp_file(data, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_TRUE(result.mismatched_chunks.empty());
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
}

TEST_F(LocalComparisonTest, DifferentFiles) {
    std::vector<uint8_t> data_a(CHUNK_SIZE * 2, 'A');
    std::vector<uint8_t> data_b(CHUNK_SIZE * 2, 'A');
    // Modify second chunk
    data_b[CHUNK_SIZE + 100] = 'B';

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    ASSERT_EQ(result.mismatched_chunks.size(), 1u);
    EXPECT_EQ(result.mismatched_chunks[0], 1u);
}

TEST_F(LocalComparisonTest, SizeMismatch) {
    // Both files fit in a single chunk (< 8MB), but have different sizes
    // CRCs differ because chunk sizes differ, even with same content
    std::vector<uint8_t> data_a(1000, 'A');
    std::vector<uint8_t> data_b(2000, 'A');

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    // Different sizes now succeed - both have 1 chunk but CRCs differ due to size
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.size_a, 1000);
    EXPECT_EQ(result.size_b, 2000);
    EXPECT_EQ(result.file_size, 1000);
    // Both files fit in 1 chunk, but chunk CRCs differ due to different sizes
    EXPECT_EQ(result.source_a_crcs.size(), 1u);
    EXPECT_EQ(result.source_b_crcs.size(), 1u);
    EXPECT_FALSE(result.mismatched_chunks.empty());  // Chunk 0 differs
    EXPECT_TRUE(result.extra_chunks_in_a.empty());   // Same number of chunks
    EXPECT_TRUE(result.extra_chunks_in_b.empty());   // Same number of chunks
}

TEST_F(LocalComparisonTest, SizeMismatchWithExtraChunks) {
    // Create files where B spans more chunks than A
    std::vector<uint8_t> data_a(1000, 'A');  // 1 chunk
    std::vector<uint8_t> data_b(CHUNK_SIZE + 1000, 'A');  // 2 chunks

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.size_a, 1000);
    EXPECT_EQ(result.size_b, CHUNK_SIZE + 1000);
    EXPECT_EQ(result.source_a_crcs.size(), 1u);
    EXPECT_EQ(result.source_b_crcs.size(), 2u);
    // Chunk 0 differs (different sizes), chunk 1 is extra in B
    EXPECT_FALSE(result.mismatched_chunks.empty());
    EXPECT_TRUE(result.extra_chunks_in_a.empty());
    ASSERT_EQ(result.extra_chunks_in_b.size(), 1u);
    EXPECT_EQ(result.extra_chunks_in_b[0], 1u);  // Chunk 1 is extra
}

TEST_F(LocalComparisonTest, BothFilesEmpty) {
    // Both files are zero-sized - should match
    std::vector<uint8_t> empty_data;
    std::string path_a = create_temp_file(empty_data, "empty_a.bin");
    std::string path_b = create_temp_file(empty_data, "empty_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.size_a, 0);
    EXPECT_EQ(result.size_b, 0);
    EXPECT_TRUE(result.source_a_crcs.empty());
    EXPECT_TRUE(result.source_b_crcs.empty());
    EXPECT_TRUE(result.mismatched_chunks.empty());
    EXPECT_TRUE(result.extra_chunks_in_a.empty());
    EXPECT_TRUE(result.extra_chunks_in_b.empty());
}

TEST_F(LocalComparisonTest, OneFileEmpty) {
    // One file is empty, other has content
    std::vector<uint8_t> empty_data;
    std::vector<uint8_t> data_b(1000, 'B');
    std::string path_a = create_temp_file(empty_data, "empty_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.size_a, 0);
    EXPECT_EQ(result.size_b, 1000);
    EXPECT_TRUE(result.source_a_crcs.empty());
    EXPECT_EQ(result.source_b_crcs.size(), 1u);
    EXPECT_TRUE(result.mismatched_chunks.empty());  // No common chunks
    EXPECT_TRUE(result.extra_chunks_in_a.empty());
    ASSERT_EQ(result.extra_chunks_in_b.size(), 1u);
    EXPECT_EQ(result.extra_chunks_in_b[0], 0u);  // Chunk 0 is extra in B
}

TEST_F(LocalComparisonTest, FileNotFound) {
    std::vector<uint8_t> data(1000, 'A');
    std::string path_a = create_temp_file(data, "file_a.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = "/nonexistent/path/file.bin";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size") != std::string::npos);
}

TEST_F(LocalComparisonTest, SpecificChunks) {
    int64_t file_size = CHUNK_SIZE * 5;
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Make chunks 1 and 3 different
    std::fill(data_b.begin() + CHUNK_SIZE, data_b.begin() + CHUNK_SIZE * 2, 'B');
    std::fill(data_b.begin() + CHUNK_SIZE * 3, data_b.begin() + CHUNK_SIZE * 4, 'B');

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    // Only compare chunks 0, 2, 4 (which should match)
    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;
    config.chunk_ids = {0, 2, 4};

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
}

TEST_F(LocalComparisonTest, ProgressTracking) {
    std::vector<uint8_t> data(CHUNK_SIZE * 4, 'X');
    std::string path_a = create_temp_file(data, "file_a.bin");
    std::string path_b = create_temp_file(data, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(progress.source_a_done);
    EXPECT_TRUE(progress.source_b_done);
    EXPECT_GE(progress.source_a_progress.load(), 99.0);
    EXPECT_GE(progress.source_b_progress.load(), 99.0);
}

TEST_F(LocalComparisonTest, SingleByteFiles) {
    std::vector<uint8_t> data_a = {'A'};
    std::vector<uint8_t> data_b = {'A'};

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 1u);
}

TEST_F(LocalComparisonTest, ExactlyOneChunk) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    std::string path_a = create_temp_file(data, "file_a.bin");
    std::string path_b = create_temp_file(data, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 1u);
}

TEST_F(LocalComparisonTest, SourceANotFound) {
    // Test when Source A file doesn't exist (covers lines 90-91)
    std::vector<uint8_t> data(1000, 'A');
    std::string path_b = create_temp_file(data, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = "/nonexistent/path/source_a.bin";
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size for source A") != std::string::npos);
}

// ============================================================================
// Empty File CRC Computation Tests
// ============================================================================

TEST_F(LocalCRC32Test, EmptyFile) {
    std::vector<uint8_t> empty_data;
    std::string path = create_temp_file(empty_data, "empty.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    // Empty files should return empty CRC list
    EXPECT_TRUE(crcs.empty());
}

TEST_F(LocalComparisonTest, AllChunksDifferent) {
    // Test case where all chunks are different
    int64_t file_size = CHUNK_SIZE * 3;
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'B');

    std::string path_a = create_temp_file(data_a, "file_a.bin");
    std::string path_b = create_temp_file(data_b, "file_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.mismatched_chunks.size(), 3u);
    EXPECT_EQ(result.mismatched_chunks[0], 0u);
    EXPECT_EQ(result.mismatched_chunks[1], 1u);
    EXPECT_EQ(result.mismatched_chunks[2], 2u);
}

// ============================================================================
// Debug Mode Local Comparison Tests
// ============================================================================

TEST_F(LocalComparisonTest, LocalComparisonWithDebugMode) {
    // Test that debug mode works correctly with local comparisons
    std::vector<uint8_t> data(CHUNK_SIZE * 2, 'D');
    std::string path_a = create_temp_file(data, "debug_a.bin");
    std::string path_b = create_temp_file(data, "debug_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;
    config.debug = true;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
}

// ============================================================================
// Mixed Differences Tests
// ============================================================================

TEST_F(LocalComparisonTest, OnlyFirstChunkDifferent) {
    // First chunk different, rest identical
    std::vector<uint8_t> data_a(CHUNK_SIZE * 3, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE * 3, 'X');
    data_a[0] = 'A';  // Modify first byte of first chunk

    std::string path_a = create_temp_file(data_a, "first_diff_a.bin");
    std::string path_b = create_temp_file(data_b, "first_diff_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    ASSERT_EQ(result.mismatched_chunks.size(), 1u);
    EXPECT_EQ(result.mismatched_chunks[0], 0u);
}

TEST_F(LocalComparisonTest, OnlyLastChunkDifferent) {
    // Last chunk different, rest identical
    std::vector<uint8_t> data_a(CHUNK_SIZE * 3, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE * 3, 'X');
    data_a[CHUNK_SIZE * 2 + 100] = 'Z';  // Modify byte in last chunk

    std::string path_a = create_temp_file(data_a, "last_diff_a.bin");
    std::string path_b = create_temp_file(data_b, "last_diff_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    ASSERT_EQ(result.mismatched_chunks.size(), 1u);
    EXPECT_EQ(result.mismatched_chunks[0], 2u);
}

TEST_F(LocalComparisonTest, AlternatingChunksDifferent) {
    // Chunks 0, 2 different, chunks 1, 3 identical
    std::vector<uint8_t> data_a(CHUNK_SIZE * 4, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE * 4, 'X');
    data_a[0] = 'A';           // Chunk 0 different
    data_a[CHUNK_SIZE * 2] = 'B';  // Chunk 2 different

    std::string path_a = create_temp_file(data_a, "alt_diff_a.bin");
    std::string path_b = create_temp_file(data_b, "alt_diff_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    ASSERT_EQ(result.mismatched_chunks.size(), 2u);
    EXPECT_EQ(result.mismatched_chunks[0], 0u);
    EXPECT_EQ(result.mismatched_chunks[1], 2u);
}

// ============================================================================
// Large File Chunk Tests
// ============================================================================

TEST_F(LocalCRC32Test, ManyChunks) {
    // Test with many chunks to exercise parallel processing
    std::vector<uint8_t> data(CHUNK_SIZE * 10);  // 10 chunks
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 17) % 256);
    }
    std::string path = create_temp_file(data, "many_chunks.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs.size(), 10u);

    // Verify consistency - computing again should give same results
    auto crcs2 = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs, crcs2);
}

TEST_F(LocalCRC32Test, FewChunksSequentialPath) {
    // Test with exactly 4 chunks - this triggers the sequential code path
    // (threshold is <= 4 chunks for sequential processing)
    std::vector<uint8_t> data(CHUNK_SIZE * 4);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 13) % 256);
    }
    std::string path = create_temp_file(data, "four_chunks.bin");

    std::vector<double> progress_values;
    auto progress_cb = [&](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);
    EXPECT_EQ(crcs.size(), 4u);

    // All CRCs should be valid (non-zero for non-trivial data)
    for (const auto& crc : crcs) {
        EXPECT_NE(crc, 0u);
    }

    // Verify consistency
    auto crcs2 = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs, crcs2);

    // Progress should have been called 4 times (once per chunk)
    EXPECT_EQ(progress_values.size(), 4u);
    // Last progress should be 100%
    EXPECT_NEAR(progress_values.back(), 100.0, 0.1);
}

TEST_F(LocalCRC32Test, OneChunkSequentialPath) {
    // Test with exactly 1 chunk - sequential path
    std::vector<uint8_t> data(CHUNK_SIZE);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 11) % 256);
    }
    std::string path = create_temp_file(data, "one_chunk.bin");

    std::vector<double> progress_values;
    auto progress_cb = [&](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);
    EXPECT_EQ(crcs.size(), 1u);
    EXPECT_NE(crcs[0], 0u);

    // Progress should have been called once
    EXPECT_EQ(progress_values.size(), 1u);
    EXPECT_NEAR(progress_values.back(), 100.0, 0.1);
}

TEST_F(LocalCRC32Test, TwoChunksSequentialPath) {
    // Test with exactly 2 chunks - sequential path
    std::vector<uint8_t> data(CHUNK_SIZE * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7) % 256);
    }
    std::string path = create_temp_file(data, "two_chunks.bin");

    std::vector<double> progress_values;
    auto progress_cb = [&](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);
    EXPECT_EQ(crcs.size(), 2u);

    for (const auto& crc : crcs) {
        EXPECT_NE(crc, 0u);
    }

    // Progress should have been called twice (once per chunk)
    EXPECT_EQ(progress_values.size(), 2u);
    EXPECT_NEAR(progress_values[0], 50.0, 0.1);
    EXPECT_NEAR(progress_values[1], 100.0, 0.1);
}

TEST_F(LocalCRC32Test, ThreeChunksSequentialPath) {
    // Test with exactly 3 chunks - sequential path
    std::vector<uint8_t> data(CHUNK_SIZE * 3);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 5) % 256);
    }
    std::string path = create_temp_file(data, "three_chunks.bin");

    std::vector<double> progress_values;
    auto progress_cb = [&](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);
    EXPECT_EQ(crcs.size(), 3u);

    for (const auto& crc : crcs) {
        EXPECT_NE(crc, 0u);
    }

    // Progress should have been called three times
    EXPECT_EQ(progress_values.size(), 3u);
    EXPECT_NEAR(progress_values[0], 33.33, 0.1);
    EXPECT_NEAR(progress_values[1], 66.67, 0.1);
    EXPECT_NEAR(progress_values[2], 100.0, 0.1);

    // Verify consistency
    auto crcs2 = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs, crcs2);
}

TEST_F(LocalCRC32Test, SequentialPathWithInvalidChunkIds) {
    // Test sequential path with some invalid chunk IDs
    std::vector<uint8_t> data(CHUNK_SIZE * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    std::string path = create_temp_file(data, "seq_invalid.bin");

    // Request 3 chunks where one is out of bounds (sequential path: <= 4 chunks)
    std::vector<int64_t> chunk_ids = {0, 5, 1};  // Chunk 5 is invalid
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);

    // One bad id poisons the whole request on the sequential path too: a
    // partial vector with a zero in the middle would be compared position by
    // position against the other side as though every entry were real.
    EXPECT_TRUE(crcs.empty())
        << "one invalid chunk id must fail the whole request";
}

TEST_F(LocalCRC32Test, FiveChunksParallelPath) {
    // Test with exactly 5 chunks - this triggers the parallel code path
    // (threshold is <= 4 chunks for sequential, so 5+ uses thread pool)
    std::vector<uint8_t> data(CHUNK_SIZE * 5);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 13) % 256);
    }
    std::string path = create_temp_file(data, "five_chunks.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs.size(), 5u);

    // All CRCs should be valid
    for (const auto& crc : crcs) {
        EXPECT_NE(crc, 0u);
    }

    // Verify consistency - parallel processing should give same results
    auto crcs2 = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs, crcs2);
}

TEST_F(LocalCRC32Test, SequentialAndParallelPathsProduceSameResults) {
    // Create a file with exactly 5 chunks
    std::vector<uint8_t> data(CHUNK_SIZE * 5);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    }
    std::string path = create_temp_file(data, "threshold_test.bin");

    // Request 4 chunks (sequential path)
    std::vector<int64_t> four_chunks = {0, 1, 2, 3};
    auto crcs_sequential = compute_crc32_chunks_boost_asio(path, four_chunks, nullptr);

    // Request 5 chunks (parallel path)
    std::vector<int64_t> five_chunks = {0, 1, 2, 3, 4};
    auto crcs_parallel = compute_crc32_chunks_boost_asio(path, five_chunks, nullptr);

    // The first 4 chunks should match exactly
    ASSERT_EQ(crcs_sequential.size(), 4u);
    ASSERT_EQ(crcs_parallel.size(), 5u);

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(crcs_sequential[i], crcs_parallel[i])
            << "Chunk " << i << " CRC mismatch between sequential and parallel paths";
    }
}

TEST_F(LocalCRC32Test, SpecificNonContiguousChunks) {
    // Request non-contiguous chunks
    std::vector<uint8_t> data(CHUNK_SIZE * 5);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    std::string path = create_temp_file(data, "non_contig.bin");

    std::vector<int64_t> chunk_ids = {0, 2, 4};  // Skip chunks 1 and 3
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);

    EXPECT_EQ(crcs.size(), 3u);
}

// ============================================================================
// Partial Chunk Tests
// ============================================================================

TEST_F(LocalCRC32Test, LastChunkPartialSize) {
    // File size not aligned to chunk boundary
    int64_t partial_size = CHUNK_SIZE * 2 + 12345;  // 2 full chunks + partial
    std::vector<uint8_t> data(partial_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 3) % 256);
    }
    std::string path = create_temp_file(data, "partial_last.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs.size(), 3u);  // 2 full + 1 partial

    // Request just the partial chunk
    auto partial_crc = compute_crc32_chunks_boost_asio(path, {2}, nullptr);
    EXPECT_EQ(partial_crc.size(), 1u);
    EXPECT_EQ(partial_crc[0], crcs[2]);
}

TEST_F(LocalCRC32Test, SingleByteFile) {
    std::vector<uint8_t> data = {0x42};
    std::string path = create_temp_file(data, "single_byte.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);
    EXPECT_EQ(crcs.size(), 1u);
    EXPECT_NE(crcs[0], 0u);  // Should have a valid CRC
}

// ============================================================================
// Progress Callback Local Tests
// ============================================================================

TEST_F(LocalCRC32Test, ProgressCallbackMonotonicity) {
    std::vector<uint8_t> data(CHUNK_SIZE * 5);
    std::string path = create_temp_file(data, "progress_mono.bin");

    std::vector<double> progress_values;
    std::mutex progress_mutex;
    auto progress_cb = [&progress_values, &progress_mutex](double pct) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        progress_values.push_back(pct);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, progress_cb);

    EXPECT_EQ(crcs.size(), 5u);
    EXPECT_FALSE(progress_values.empty());

    bool saw_complete = std::any_of(progress_values.begin(), progress_values.end(), [](double pct) {
        return std::abs(pct - 100.0) <= 0.1;
    });
    EXPECT_TRUE(saw_complete);

    // Due to parallel processing, progress may not be strictly monotonic,
    // but the completion callback should still be reported.
}

// ============================================================================
// Source B Not Found Test
// ============================================================================

TEST_F(LocalComparisonTest, SourceBNotFound) {
    std::vector<uint8_t> data(1000, 'A');
    std::string path_a = create_temp_file(data, "found_a.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = "/nonexistent/path/source_b.bin";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size for source B") != std::string::npos);
}

// ============================================================================
// File Descriptor Limits Tests
// ============================================================================

TEST(FdLimitsTest, GetFdLimitsReturnsValidValues) {
    FdLimits limits = GetFdLimits(64);
    
    // Soft limit should be positive (or very large for "unlimited")
    EXPECT_GT(limits.soft_limit, 0);
    
    // Hard limit should be >= soft limit
    EXPECT_GE(limits.hard_limit, limits.soft_limit);
    
    // Max safe threads should be positive
    EXPECT_GT(limits.max_safe_threads, 0);
}

TEST(FdLimitsTest, LowThreadCountNotCapped) {
    // With a very low thread count, we should never be capped
    FdLimits limits = GetFdLimits(1);
    
    EXPECT_FALSE(limits.was_capped);
    EXPECT_GE(limits.max_safe_threads, 1);
}

TEST(FdLimitsTest, MaxSafeThreadsCalculation) {
    FdLimits limits = GetFdLimits(1000);

    // When ulimit is "unlimited", soft_limit can be a huge value
    // In that case, max_safe_threads should be >= requested
    if (limits.soft_limit > 1000000) {
        // Unlimited case - should not be capped
        EXPECT_GE(limits.max_safe_threads, 1000);
        EXPECT_FALSE(limits.was_capped);
    } else {
        // Normal case - verify calculation
        int64_t expected = (limits.soft_limit - FDS_RESERVED) / FDS_PER_CONNECTION;
        if (expected < 1) expected = 1;
        EXPECT_EQ(limits.max_safe_threads, expected);
    }
}

TEST(FdLimitsTest, WasCappedWhenExceedsLimit) {
    // Request a very high thread count that would exceed any reasonable limit
    FdLimits limits = GetFdLimits(100000);
    
    // Unless ulimit is truly unlimited with a massive value, this should be capped
    // If soft_limit is large enough, was_capped might be false - that's fine
    if (limits.soft_limit < 300000) {  // 100000 * 3 fds per connection
        EXPECT_TRUE(limits.was_capped);
        EXPECT_LT(limits.max_safe_threads, 100000);
    }
}

TEST(FdLimitsTest, ZeroThreadsNotCapped) {
    // Zero threads (unbounded mode) should not trigger capping logic
    FdLimits limits = GetFdLimits(0);
    
    // With 0 requested, max_safe_threads is still calculated but was_capped
    // depends on whether 0 > max_safe_threads (it never is)
    EXPECT_FALSE(limits.was_capped);
}

TEST(FdLimitsTest, ConstantsAreReasonable) {
    // Sanity check the constants
    EXPECT_GT(FDS_PER_CONNECTION, 0);
    EXPECT_LE(FDS_PER_CONNECTION, 10);  // Shouldn't need more than 10 fds per connection

    EXPECT_GT(FDS_RESERVED, 0);
    EXPECT_LE(FDS_RESERVED, 100);  // Reserve reasonable amount
}

TEST(FdLimitsTest, TryRaiseFdLimitLowThreadCount) {
    // Should always succeed for a low thread count
    EXPECT_TRUE(TryRaiseFdLimit(1));
    EXPECT_TRUE(TryRaiseFdLimit(10));
}

TEST(FdLimitsTest, TryRaiseFdLimitAlreadyHighEnough) {
    // Get current limits
    struct rlimit rl;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl), 0);

    // Calculate a thread count that's safely within current limit
    int safe_threads = static_cast<int>((rl.rlim_cur - FDS_RESERVED - 50) / FDS_PER_CONNECTION);
    if (safe_threads > 0) {
        // Should succeed without needing to raise
        EXPECT_TRUE(TryRaiseFdLimit(safe_threads));
    }
}

TEST(FdLimitsTest, TryRaiseFdLimitRaisesWhenNeeded) {
    // Get current limits
    struct rlimit rl_before;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl_before), 0);

    // Skip if already unlimited
    if (rl_before.rlim_cur > 1000000) {
        GTEST_SKIP() << "Limit already effectively unlimited";
    }

    // Skip if soft == hard (can't raise)
    if (rl_before.rlim_cur >= rl_before.rlim_max) {
        GTEST_SKIP() << "Soft limit already at hard limit";
    }

    // Try to raise for a higher thread count
    int target_threads = static_cast<int>((rl_before.rlim_cur / FDS_PER_CONNECTION) + 10);
    bool raised = TryRaiseFdLimit(target_threads);

    if (raised) {
        // Verify the limit was actually raised
        struct rlimit rl_after;
        ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl_after), 0);
        EXPECT_GE(rl_after.rlim_cur, rl_before.rlim_cur);
    }
    // If raise failed, that's okay - hard limit might be too low
}

// ============================================================================
// IsRetryableS3Error Tests
// ============================================================================

// Helper to create S3Error with specific error type
static Aws::S3::S3Error MakeS3Error(Aws::S3::S3Errors error_type, const std::string& message = "") {
    return Aws::S3::S3Error(
        Aws::Client::AWSError<Aws::S3::S3Errors>(error_type, message.c_str(), message.c_str(), false)
    );
}

TEST(IsRetryableS3ErrorTest, NetworkConnectionError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::NETWORK_CONNECTION);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, RequestTimeoutError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::REQUEST_TIMEOUT);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, ThrottlingError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::THROTTLING);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, SlowDownError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::SLOW_DOWN);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, InternalFailureError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::INTERNAL_FAILURE);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, ServiceUnavailableError) {
    auto error = MakeS3Error(Aws::S3::S3Errors::SERVICE_UNAVAILABLE);
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, InternalErrorMessageLowercase) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "We encountered an internal error. Please try again.");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, InternalErrorMessageUppercase) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "Internal error occurred");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, PleaseRetryMessage) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "Service busy. Please try again later.");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, DnsResolutionFailedCurlCode6) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "curlCode: 6 - Could not resolve host");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, ConnectionFailedCurlCode7) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "curlCode: 7 - Failed to connect");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, TimeoutCurlCode28) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "curlCode: 28 - Operation timed out");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, CouldNotResolveMessage) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "Could not resolve host: bucket.s3.amazonaws.com");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, GetaddrinfoMessage) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "getaddrinfo failed");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, CouldNotConnectMessage) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "Could not connect to server");
    EXPECT_TRUE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, AccessDeniedNotRetryable) {
    auto error = MakeS3Error(Aws::S3::S3Errors::ACCESS_DENIED, "Access Denied");
    EXPECT_FALSE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, NoSuchKeyNotRetryable) {
    auto error = MakeS3Error(Aws::S3::S3Errors::NO_SUCH_KEY, "The specified key does not exist");
    EXPECT_FALSE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, NoSuchBucketNotRetryable) {
    auto error = MakeS3Error(Aws::S3::S3Errors::NO_SUCH_BUCKET, "The specified bucket does not exist");
    EXPECT_FALSE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, InvalidAccessKeyNotRetryable) {
    auto error = MakeS3Error(Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID, "Invalid access key");
    EXPECT_FALSE(IsRetryableS3Error(error));
}

TEST(IsRetryableS3ErrorTest, UnknownErrorWithoutRetryableMessage) {
    auto error = MakeS3Error(Aws::S3::S3Errors::UNKNOWN, "Some random error");
    EXPECT_FALSE(IsRetryableS3Error(error));
}

// ============================================================================
// GetS3Hostname Tests
// ============================================================================

TEST(GetS3HostnameTest, StandardBucketAndRegion) {
    EXPECT_EQ(GetS3Hostname("my-bucket", "us-east-1"), "my-bucket.s3.us-east-1.amazonaws.com");
}

TEST(GetS3HostnameTest, DifferentRegion) {
    EXPECT_EQ(GetS3Hostname("data-bucket", "eu-west-1"), "data-bucket.s3.eu-west-1.amazonaws.com");
}

TEST(GetS3HostnameTest, BucketWithDots) {
    EXPECT_EQ(GetS3Hostname("my.dotted.bucket", "ap-south-1"), "my.dotted.bucket.s3.ap-south-1.amazonaws.com");
}

TEST(GetS3HostnameTest, BucketWithHyphens) {
    EXPECT_EQ(GetS3Hostname("my-hyphenated-bucket", "us-west-2"), "my-hyphenated-bucket.s3.us-west-2.amazonaws.com");
}

// ============================================================================
// ResolveHostname Tests
// ============================================================================

// Helper to check if running under ThreadSanitizer
// Note: __has_feature is Clang-specific and must be checked in a nested #if
#if defined(__has_feature)
  #if __has_feature(thread_sanitizer)
    #define OBJIFF_TSAN_ENABLED 1
  #endif
#endif
#if defined(__SANITIZE_THREAD__)
  #define OBJIFF_TSAN_ENABLED 1
#endif

inline bool IsRunningUnderTSAN() {
#if defined(OBJIFF_TSAN_ENABLED)
    return true;
#else
    return false;
#endif
}

TEST(ResolveHostnameTest, LocalhostResolvesToLoopback) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    auto ips = ResolveHostname("localhost");
    ASSERT_FALSE(ips.empty());
    // Should contain 127.0.0.1 or ::1
    bool found_loopback = false;
    for (const auto& ip : ips) {
        if (ip == "127.0.0.1" || ip == "::1") {
            found_loopback = true;
            break;
        }
    }
    EXPECT_TRUE(found_loopback) << "Expected localhost to resolve to 127.0.0.1 or ::1";
}

TEST(ResolveHostnameTest, InvalidHostnameReturnsEmpty) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    auto ips = ResolveHostname("this-hostname-definitely-does-not-exist.invalid");
    EXPECT_TRUE(ips.empty());
}

TEST(ResolveHostnameTest, EmptyHostnameReturnsEmpty) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    auto ips = ResolveHostname("");
    EXPECT_TRUE(ips.empty());
}

// ============================================================================
// DnsCache Tests
// ============================================================================

TEST(DnsCacheTest, WarmupLocalhost) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    // Clear any previous state
    DnsCache::Instance().Clear();

    EXPECT_TRUE(DnsCache::Instance().Warmup("localhost"));
}

TEST(DnsCacheTest, WarmupInvalidHostname) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();

    EXPECT_FALSE(DnsCache::Instance().Warmup("this-hostname-definitely-does-not-exist.invalid"));
}

TEST(DnsCacheTest, ResolveReturnsCachedResult) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();

    // First resolve
    auto ips1 = DnsCache::Instance().Resolve("localhost");
    ASSERT_FALSE(ips1.empty());

    // Second resolve should return same result (from cache)
    auto ips2 = DnsCache::Instance().Resolve("localhost");
    EXPECT_EQ(ips1, ips2);
}

TEST(DnsCacheTest, GetResolveStringFormat) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();

    // Warmup localhost first
    ASSERT_TRUE(DnsCache::Instance().Warmup("localhost"));

    std::string resolve_str = DnsCache::Instance().GetResolveString("localhost", 443);
    ASSERT_FALSE(resolve_str.empty());

    // Format should be "hostname:port:ip1,ip2,..."
    EXPECT_EQ(resolve_str.substr(0, 14), "localhost:443:");

    // Should contain at least one IP after the port
    size_t colon_pos = resolve_str.rfind(':');
    ASSERT_NE(colon_pos, std::string::npos);
    std::string ips_part = resolve_str.substr(colon_pos + 1);
    EXPECT_FALSE(ips_part.empty());
}

TEST(DnsCacheTest, GetResolveStringDifferentPorts) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();
    DnsCache::Instance().Warmup("localhost");

    std::string str_443 = DnsCache::Instance().GetResolveString("localhost", 443);
    std::string str_80 = DnsCache::Instance().GetResolveString("localhost", 80);

    // Both should have content
    ASSERT_FALSE(str_443.empty());
    ASSERT_FALSE(str_80.empty());

    // Port should be different
    EXPECT_NE(str_443.find(":443:"), std::string::npos);
    EXPECT_NE(str_80.find(":80:"), std::string::npos);
}

TEST(DnsCacheTest, GetResolveStringUnresolvedHostname) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();

    // Don't warmup - just try to get resolve string for unknown host
    std::string resolve_str = DnsCache::Instance().GetResolveString("unknown-host.invalid", 443);
    EXPECT_TRUE(resolve_str.empty());
}

TEST(DnsCacheTest, ClearRemovesCachedEntries) {
    if (IsRunningUnderTSAN()) {
        GTEST_SKIP() << "Skipping DNS test under ThreadSanitizer - macOS getaddrinfo has known TSAN false positives";
    }
    DnsCache::Instance().Clear();

    // Warmup and verify it's cached
    ASSERT_TRUE(DnsCache::Instance().Warmup("localhost"));
    auto ips = DnsCache::Instance().Resolve("localhost");
    ASSERT_FALSE(ips.empty());

    // Clear the cache
    DnsCache::Instance().Clear();

    // GetResolveString should now do a fresh resolve (which will work for localhost)
    // But if we check for a non-existent cached entry, it won't be there
    // This is tricky to test - let's verify clear doesn't crash and warmup still works
    EXPECT_TRUE(DnsCache::Instance().Warmup("localhost"));
}

// ============================================================================
// Block Analysis Tests
// ============================================================================

class BlockAnalysisTest : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/objiff_block_analysis_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    std::string create_temp_file(const std::vector<uint8_t>& data, const std::string& name) {
        std::string path = temp_dir + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        return path;
    }
};

TEST_F(BlockAnalysisTest, IdenticalChunks) {
    // Two identical files - block analysis should show no differences
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    std::string path_a = create_temp_file(data, "identical_a.bin");
    std::string path_b = create_temp_file(data, "identical_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_EQ(result.chunk_index, 0);
    EXPECT_EQ(result.chunk_size, CHUNK_SIZE);
    EXPECT_EQ(result.total_blocks, BLOCKS_PER_CHUNK);
    EXPECT_EQ(result.blocks_different, 0);
    EXPECT_EQ(result.bytes_different, 0);
    EXPECT_DOUBLE_EQ(result.percentage_different, 0.0);
    EXPECT_EQ(result.first_diff_offset, -1);
    EXPECT_EQ(result.last_diff_offset, -1);

    // All blocks should match
    for (bool matches : result.block_matches) {
        EXPECT_TRUE(matches);
    }
}

TEST_F(BlockAnalysisTest, CompletelyDifferentChunks) {
    // Two completely different files
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'A');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'B');
    std::string path_a = create_temp_file(data_a, "diff_a.bin");
    std::string path_b = create_temp_file(data_b, "diff_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_EQ(result.chunk_size, CHUNK_SIZE);
    EXPECT_EQ(result.total_blocks, BLOCKS_PER_CHUNK);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK);  // All blocks different
    EXPECT_EQ(result.bytes_different, CHUNK_SIZE);  // All bytes different
    EXPECT_DOUBLE_EQ(result.percentage_different, 100.0);
    EXPECT_EQ(result.first_diff_offset, 0);
    EXPECT_EQ(result.last_diff_offset, CHUNK_SIZE - 1);

    // All blocks should be different
    for (bool matches : result.block_matches) {
        EXPECT_FALSE(matches);
    }
}

TEST_F(BlockAnalysisTest, SingleBlockDifferent) {
    // Only the first block (64 KiB) is different
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');

    // Modify only the first block
    std::fill(data_b.begin(), data_b.begin() + BLOCK_SIZE, 'Y');

    std::string path_a = create_temp_file(data_a, "single_block_a.bin");
    std::string path_b = create_temp_file(data_b, "single_block_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, BLOCK_SIZE);
    EXPECT_NEAR(result.percentage_different, 100.0 * BLOCK_SIZE / CHUNK_SIZE, 0.01);
    EXPECT_EQ(result.first_diff_offset, 0);
    EXPECT_EQ(result.last_diff_offset, BLOCK_SIZE - 1);

    // First block should be different, rest should match
    ASSERT_EQ(result.block_matches.size(), static_cast<size_t>(BLOCKS_PER_CHUNK));
    EXPECT_FALSE(result.block_matches[0]);
    for (size_t i = 1; i < result.block_matches.size(); ++i) {
        EXPECT_TRUE(result.block_matches[i]) << "Block " << i << " should match";
    }
}

TEST_F(BlockAnalysisTest, LastBlockDifferent) {
    // Only the last block is different
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');

    // Modify only the last block
    int64_t last_block_start = (BLOCKS_PER_CHUNK - 1) * BLOCK_SIZE;
    std::fill(data_b.begin() + last_block_start, data_b.end(), 'Z');

    std::string path_a = create_temp_file(data_a, "last_block_a.bin");
    std::string path_b = create_temp_file(data_b, "last_block_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, BLOCK_SIZE);
    EXPECT_EQ(result.first_diff_offset, last_block_start);
    EXPECT_EQ(result.last_diff_offset, CHUNK_SIZE - 1);

    // Last block should be different, rest should match
    ASSERT_EQ(result.block_matches.size(), static_cast<size_t>(BLOCKS_PER_CHUNK));
    for (size_t i = 0; i < result.block_matches.size() - 1; ++i) {
        EXPECT_TRUE(result.block_matches[i]) << "Block " << i << " should match";
    }
    EXPECT_FALSE(result.block_matches.back());
}

TEST_F(BlockAnalysisTest, AlternatingBlocksDifferent) {
    // Every other block is different
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');

    // Modify even-numbered blocks (0, 2, 4, ...)
    for (int block = 0; block < BLOCKS_PER_CHUNK; block += 2) {
        int64_t block_start = block * BLOCK_SIZE;
        int64_t block_end = block_start + BLOCK_SIZE;
        std::fill(data_b.begin() + block_start, data_b.begin() + block_end, 'Y');
    }

    std::string path_a = create_temp_file(data_a, "alt_blocks_a.bin");
    std::string path_b = create_temp_file(data_b, "alt_blocks_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK / 2);  // Half the blocks
    EXPECT_EQ(result.bytes_different, CHUNK_SIZE / 2);
    EXPECT_NEAR(result.percentage_different, 50.0, 0.01);

    // Check alternating pattern
    ASSERT_EQ(result.block_matches.size(), static_cast<size_t>(BLOCKS_PER_CHUNK));
    for (size_t i = 0; i < result.block_matches.size(); ++i) {
        if (i % 2 == 0) {
            EXPECT_FALSE(result.block_matches[i]) << "Block " << i << " should differ";
        } else {
            EXPECT_TRUE(result.block_matches[i]) << "Block " << i << " should match";
        }
    }
}

TEST_F(BlockAnalysisTest, SingleByteDifferent) {
    // Only a single byte is different
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');

    // Modify only byte at offset 12345
    data_b[12345] = 'Z';

    std::string path_a = create_temp_file(data_a, "single_byte_a.bin");
    std::string path_b = create_temp_file(data_b, "single_byte_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, 1);
    EXPECT_NEAR(result.percentage_different, 100.0 / CHUNK_SIZE, 0.001);
    EXPECT_EQ(result.first_diff_offset, 12345);
    EXPECT_EQ(result.last_diff_offset, 12345);
}

TEST_F(BlockAnalysisTest, PartialChunk) {
    // Test with a partial chunk (smaller than CHUNK_SIZE)
    int64_t file_size = CHUNK_SIZE + 1000;  // Partial second chunk
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Modify some bytes in the partial chunk
    data_b[CHUNK_SIZE + 100] = 'B';
    data_b[CHUNK_SIZE + 200] = 'B';

    std::string path_a = create_temp_file(data_a, "partial_a.bin");
    std::string path_b = create_temp_file(data_b, "partial_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    // Analyze the second (partial) chunk
    auto result = analyze_mismatched_chunk(source_a, source_b, 1, file_size);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_index, 1);
    EXPECT_EQ(result.chunk_size, 1000);  // Partial chunk size

    // Partial chunk has fewer blocks
    int64_t expected_blocks = (1000 + BLOCK_SIZE - 1) / BLOCK_SIZE;
    EXPECT_EQ(result.total_blocks, expected_blocks);

    EXPECT_EQ(result.bytes_different, 2);
    EXPECT_EQ(result.first_diff_offset, 100);  // Relative to chunk start
    EXPECT_EQ(result.last_diff_offset, 200);
}

TEST_F(BlockAnalysisTest, MultiChunkFileAnalyzeSecondChunk) {
    // Test analyzing a middle chunk in a multi-chunk file
    int64_t file_size = CHUNK_SIZE * 3;
    std::vector<uint8_t> data_a(file_size, 'X');
    std::vector<uint8_t> data_b(file_size, 'X');

    // Modify only the second chunk
    std::fill(data_b.begin() + CHUNK_SIZE, data_b.begin() + CHUNK_SIZE * 2, 'Y');

    std::string path_a = create_temp_file(data_a, "multi_a.bin");
    std::string path_b = create_temp_file(data_b, "multi_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    // Analyze chunk 1 (second chunk, zero-indexed)
    auto result = analyze_mismatched_chunk(source_a, source_b, 1, file_size);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_index, 1);
    EXPECT_EQ(result.chunk_size, CHUNK_SIZE);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK);  // All blocks in this chunk differ
    EXPECT_EQ(result.bytes_different, CHUNK_SIZE);
}

TEST_F(BlockAnalysisTest, SourceANotFound) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    std::string path_b = create_temp_file(data, "exists_b.bin");

    FileSource source_a{SourceType::Local, "/nonexistent/path/file.bin", "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.error_message.find("source A") != std::string::npos);
}

TEST_F(BlockAnalysisTest, SourceBNotFound) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    std::string path_a = create_temp_file(data, "exists_a.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, "/nonexistent/path/file.bin", "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);

    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.error_message.find("source B") != std::string::npos);
}

TEST_F(BlockAnalysisTest, ProgressCallback) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    std::string path_a = create_temp_file(data, "progress_a.bin");
    std::string path_b = create_temp_file(data, "progress_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    std::vector<double> progress_values;
    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, nullptr, nullptr,
        [&progress_values](double pct) { progress_values.push_back(pct); });

    EXPECT_TRUE(result.computed);
    EXPECT_FALSE(progress_values.empty());
    // Should reach 100%
    EXPECT_NEAR(progress_values.back(), 100.0, 0.1);
}

TEST_F(BlockAnalysisTest, SmallFile) {
    // Test with a file smaller than one block
    std::vector<uint8_t> data_a(1000, 'A');
    std::vector<uint8_t> data_b(1000, 'A');
    data_b[500] = 'B';  // One byte different

    std::string path_a = create_temp_file(data_a, "small_a.bin");
    std::string path_b = create_temp_file(data_b, "small_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, 1000);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_size, 1000);
    EXPECT_EQ(result.total_blocks, 1);  // Only one block (partial)
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, 1);
    EXPECT_EQ(result.first_diff_offset, 500);
    EXPECT_EQ(result.last_diff_offset, 500);
}

TEST_F(BlockAnalysisTest, BlockAnalysisStructDefaults) {
    BlockAnalysis analysis;

    EXPECT_EQ(analysis.chunk_index, -1);
    EXPECT_EQ(analysis.chunk_size, 0);
    EXPECT_TRUE(analysis.block_matches.empty());
    EXPECT_EQ(analysis.total_blocks, 0);
    EXPECT_EQ(analysis.blocks_different, 0);
    EXPECT_EQ(analysis.bytes_different, 0);
    EXPECT_DOUBLE_EQ(analysis.percentage_different, 0.0);
    EXPECT_EQ(analysis.first_diff_offset, -1);
    EXPECT_EQ(analysis.last_diff_offset, -1);
    EXPECT_FALSE(analysis.computed);
    EXPECT_TRUE(analysis.error_message.empty());
}

TEST(BlockConstantsTest, BlockSizeValue) {
    // Block size should be 64 KiB
    EXPECT_EQ(BLOCK_SIZE, 64 * 1024);
}

TEST(BlockConstantsTest, BlocksPerChunk) {
    // Should be CHUNK_SIZE / BLOCK_SIZE = 8 MiB / 64 KiB = 128
    EXPECT_EQ(BLOCKS_PER_CHUNK, 128);
    EXPECT_EQ(BLOCKS_PER_CHUNK, CHUNK_SIZE / BLOCK_SIZE);
}

TEST(BlockConstantsTest, BlockSizePowerOfTwo) {
    // Block size should be a power of 2
    int64_t size = BLOCK_SIZE;
    EXPECT_GT(size, 0);
    EXPECT_EQ(size & (size - 1), 0) << "BLOCK_SIZE should be a power of 2";
}

TEST_F(BlockAnalysisTest, NegativeChunkIndex) {
    // Negative chunk index should result in an error (empty data read)
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    std::string path_a = create_temp_file(data, "neg_chunk_a.bin");
    std::string path_b = create_temp_file(data, "neg_chunk_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, -1, CHUNK_SIZE);

    // Should fail gracefully - negative offset causes pread to fail
    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(BlockAnalysisTest, ChunkIndexOutOfBounds) {
    // Chunk index beyond file size should result in an error
    std::vector<uint8_t> data(1000, 'A');  // Small file, less than one chunk
    std::string path_a = create_temp_file(data, "oob_a.bin");
    std::string path_b = create_temp_file(data, "oob_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    // Try to analyze chunk 5 (offset 5*8MB) of a 1000 byte file
    auto result = analyze_mismatched_chunk(source_a, source_b, 5, 1000);

    // Should fail - chunk is beyond file bounds
    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(BlockAnalysisTest, ChunkIndexExactlyAtEnd) {
    // Chunk index exactly at the boundary (file_size / CHUNK_SIZE)
    // For a file of exactly CHUNK_SIZE bytes, chunk 1 is out of bounds
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    std::string path_a = create_temp_file(data, "boundary_a.bin");
    std::string path_b = create_temp_file(data, "boundary_b.bin");

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    // Chunk 0 should work
    auto result0 = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE);
    EXPECT_TRUE(result0.computed);

    // Chunk 1 is out of bounds for exactly CHUNK_SIZE file
    auto result1 = analyze_mismatched_chunk(source_a, source_b, 1, CHUNK_SIZE);
    EXPECT_FALSE(result1.computed);
    EXPECT_FALSE(result1.error_message.empty());
}

// ============================================================================
// fd_limits: capping arithmetic under a controlled descriptor limit
// ============================================================================
//
// The existing FdLimits tests run at whatever limit the machine happens to
// have, which is usually high enough that the capping branch never executes.
// Lowering RLIMIT_NOFILE for the duration of a test exercises it deterministically.
// Lowering the soft limit is always permitted; the guard restores it.

namespace {
class FdLimitScope {
public:
    explicit FdLimitScope(rlim_t soft) : ok_(false) {
        if (getrlimit(RLIMIT_NOFILE, &saved_) != 0) return;
        struct rlimit r = saved_;
        if (soft > saved_.rlim_max) return;   // cannot exceed the hard limit
        r.rlim_cur = soft;
        ok_ = (setrlimit(RLIMIT_NOFILE, &r) == 0);
    }
    ~FdLimitScope() { if (ok_) setrlimit(RLIMIT_NOFILE, &saved_); }
    bool active() const { return ok_; }
private:
    struct rlimit saved_;
    bool ok_;
};
}  // namespace

TEST(FdLimitsCappingTest, DerivesSafeThreadsFromTheSoftLimit) {
    FdLimitScope scope(512);
    if (!scope.active()) GTEST_SKIP() << "could not lower RLIMIT_NOFILE";

    FdLimits limits = GetFdLimits(1000);
    EXPECT_EQ(limits.soft_limit, 512);
    // (512 - 50 reserved) / 3 per connection = 154
    EXPECT_EQ(limits.max_safe_threads, (512 - FDS_RESERVED) / FDS_PER_CONNECTION);
    EXPECT_TRUE(limits.was_capped) << "1000 threads must not fit in 512 descriptors";
}

TEST(FdLimitsCappingTest, RequestWithinBudgetIsNotCapped) {
    FdLimitScope scope(512);
    if (!scope.active()) GTEST_SKIP() << "could not lower RLIMIT_NOFILE";

    FdLimits limits = GetFdLimits(10);
    EXPECT_FALSE(limits.was_capped);
    EXPECT_GE(limits.max_safe_threads, 10);
}

TEST(FdLimitsCappingTest, VeryLowLimitStillLeavesAtLeastOneThread) {
    // Below the reserve, available_fds clamps to 0 and the count floors at 1
    // rather than going to zero, which would deadlock the pool.
    FdLimitScope scope(20);
    if (!scope.active()) GTEST_SKIP() << "could not lower RLIMIT_NOFILE";

    FdLimits limits = GetFdLimits(64);
    EXPECT_GE(limits.max_safe_threads, 1);
    EXPECT_TRUE(limits.was_capped);
}

TEST(FdLimitsCappingTest, BoundaryAtExactlyTheReserve) {
    FdLimitScope scope(FDS_RESERVED);
    if (!scope.active()) GTEST_SKIP() << "could not lower RLIMIT_NOFILE";

    FdLimits limits = GetFdLimits(8);
    EXPECT_EQ(limits.max_safe_threads, 1) << "zero available descriptors must floor at 1";
}

TEST(FdLimitsCappingTest, WarnOnlyFiresWhenCapped) {
    // Exercises both early returns and the warning body. The call logs; the
    // assertion is that it is total and does not throw for any of these inputs.
    FdLimits not_capped{4096, 4096, 512, false};
    EXPECT_NO_THROW(WarnIfFdLimitLow(not_capped, 128));

    FdLimits capped{256, 4096, 68, true};
    EXPECT_NO_THROW(WarnIfFdLimitLow(capped, 1024));
    EXPECT_NO_THROW(WarnIfFdLimitLow(capped, 0));    // sanity-check branch
    EXPECT_NO_THROW(WarnIfFdLimitLow(capped, -1));
}

TEST(FdLimitsCappingTest, TryRaiseIsANoOpForNonPositiveThreadCounts) {
    EXPECT_TRUE(TryRaiseFdLimit(0));
    EXPECT_TRUE(TryRaiseFdLimit(-5));
}

TEST(FdLimitsCappingTest, TryRaiseSucceedsWhenTheLimitAlreadySuffices) {
    // One thread needs 3 + 50 descriptors, which any usable limit exceeds.
    EXPECT_TRUE(TryRaiseFdLimit(1));
}

// ============================================================================
// app_settings: derived timeout accessors
// ============================================================================

TEST(AppSettingsTest, TimeoutAccessorsConvertSecondsToMilliseconds) {
    AppSettings s;
    s.connect_timeout_s = 7;
    s.request_timeout_s = 11;
    EXPECT_EQ(s.connect_timeout_ms(), 7000);
    EXPECT_EQ(s.request_timeout_ms(), 11000);
}

// ============================================================================
// A chunk that cannot be read must not yield a zero CRC (issue #19)
// ============================================================================

TEST_F(LocalCRC32Test, ZeroIsAValidCrcSoFailureMustNotBeEncodedAsZero) {
    // The premise of the fix, demonstrated rather than asserted: these four
    // bytes really do hash to 0 under IEEE CRC-32, so a zero in the result
    // vector cannot mean "this chunk failed" - it is a value a real chunk can
    // legitimately produce, and the caller compares it as real either way.
    std::vector<uint8_t> data = {0x9d, 0x0a, 0xd9, 0x6d};
    ASSERT_EQ(crc32_hw(data.data(), data.size()), 0u)
        << "this input is chosen precisely because its CRC32 is zero";

    std::string path = create_temp_file(data);
    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr);

    // The request succeeded, so the zero must be reported as the real value it
    // is - not suppressed, and not confused with a failure.
    ASSERT_EQ(crcs.size(), 1u) << "a readable one-chunk file must produce one CRC";
    EXPECT_EQ(crcs[0], 0u) << "a genuine CRC32 of zero must survive the round trip";
}

TEST_F(LocalCRC32Test, AnUnalignedChunkSizeIsChecksummedCorrectly) {
    // This used to fail the whole request, and that was the best available
    // answer at the time: mmap needs a page-aligned offset, so every chunk
    // after the first failed to map, and returning nothing beat returning
    // zeros that compare as real values. Reading with pread has no alignment
    // requirement, so an odd chunk size is now simply honoured (issues #17,
    // #53).
    std::vector<uint8_t> data(3000);
    for (size_t k = 0; k < data.size(); ++k) data[k] = static_cast<uint8_t>((k * 7 + 1) & 0xFF);
    std::string path = create_temp_file(data, "unaligned_chunks.bin");

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, nullptr, /*chunk_size=*/1000);

    ASSERT_EQ(crcs.size(), 3u) << "an unaligned chunk size must not fail the request";
    // Checked against an independent computation over the same ranges, so this
    // asserts the values are right rather than merely present.
    for (size_t c = 0; c < 3; ++c) {
        EXPECT_EQ(crcs[c], crc32_hw(data.data() + c * 1000, 1000)) << "chunk " << c;
    }
}

TEST_F(LocalCRC32Test, AFileTruncatedMidReadFailsInsteadOfKillingTheProcess) {
    // Issue #53. The chunks were mmap'd and then read, and mmap past EOF
    // succeeds on Linux - only touching the pages faults. So a file shrinking
    // between the size check and the read raised SIGBUS and killed mito
    // outright: "Bus error (core dumped)", exit 135. There was no return value
    // to check and no way to catch it short of a process-wide signal handler.
    //
    // Truncate from inside the progress callback, which runs after each chunk
    // on the sequential path, so the shrink lands exactly in that window.
    // Four chunks keeps it sequential and therefore deterministic.
    std::vector<uint8_t> data(static_cast<size_t>(CHUNK_SIZE) * 4, 'T');
    std::string path = create_temp_file(data, "truncated_midread.bin");

    bool truncated = false;
    auto shrink_after_first_chunk = [&](double) {
        if (truncated) return;
        truncated = true;
        ASSERT_EQ(::truncate(path.c_str(), CHUNK_SIZE), 0)
            << "could not truncate: " << std::strerror(errno);
    };

    auto crcs = compute_crc32_chunks_boost_asio(path, {}, shrink_after_first_chunk);

    // Surviving to this line is the point of the test. The result must also be
    // empty rather than holding fabricated values for the vanished chunks.
    EXPECT_TRUE(truncated) << "the callback never ran, so nothing was exercised";
    EXPECT_TRUE(crcs.empty())
        << "chunks whose bytes no longer exist must not be reported as checksums";
}

TEST_F(LocalCRC32Test, ParallelPathAlsoFailsTheWholeRequest) {
    // More than four chunks takes the thread-pool path, where the failure flag
    // is written by workers and read after the pool drains. All the other
    // failure tests use three or fewer ids and so only cover the sequential
    // branch.
    std::vector<uint8_t> data(CHUNK_SIZE * 8, 'P');
    std::string path = create_temp_file(data, "parallel_invalid.bin");

    std::vector<int64_t> chunk_ids = {0, 1, 2, 3, 4, 5, 99};   // 99 is out of range
    auto crcs = compute_crc32_chunks_boost_asio(path, chunk_ids, nullptr);

    EXPECT_TRUE(crcs.empty())
        << "one bad chunk must fail the request on the parallel path too";
}
