#include <gtest/gtest.h>
#include "s3_mock.h"
#include "s3_interface.h"
#include "s3_utils.h"
#include "comparison_task.h"
#include "crc32_hw.h"
#include "constants.h"
#include "temp_test_path.h"

#include <unistd.h>

#include <limits>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <filesystem>
#include <set>
#include <future>
#include <thread>
#include <chrono>
#include <atomic>

// ============================================================================
// MockS3Client Basic Operations Tests
// ============================================================================

class MockS3ClientTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock;

    void SetUp() override {
        mock = std::make_shared<MockS3Client>();
    }
};

TEST_F(MockS3ClientTest, CreateBucket) {
    EXPECT_TRUE(mock->CreateBucket("test-bucket"));
    // Creating same bucket again should succeed (idempotent)
    EXPECT_TRUE(mock->CreateBucket("test-bucket"));
}

TEST_F(MockS3ClientTest, PutAndGetObjectSize) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(1024, 'X');
    EXPECT_TRUE(mock->PutObject("test-bucket", "test-key", data));

    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, 1024);
}

TEST_F(MockS3ClientTest, GetObjectSizeNonExistent) {
    mock->CreateBucket("test-bucket");
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "nonexistent"), -1);
    EXPECT_EQ(mock->GetObjectSize("nonexistent-bucket", "key"), -1);
}

TEST_F(MockS3ClientTest, PutObjectWithoutBucket) {
    std::vector<uint8_t> data(100, 'X');
    EXPECT_FALSE(mock->PutObject("nonexistent-bucket", "key", data));
}

TEST_F(MockS3ClientTest, GetObjectRange) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    mock->PutObject("test-bucket", "test-key", data);

    auto range = mock->GetObjectRange("test-bucket", "test-key", 2, 5);
    ASSERT_EQ(range.size(), 4u);
    EXPECT_EQ(range[0], 2);
    EXPECT_EQ(range[1], 3);
    EXPECT_EQ(range[2], 4);
    EXPECT_EQ(range[3], 5);
}

TEST_F(MockS3ClientTest, GetObjectRangeIntoMatchesTheInterfaceContract) {
    // The mock is what most suites test against, so where it disagrees with the
    // real adapter a bug passes here and fails in production. GetObjectRangeInto
    // used to return true for any reversed range - its guard was
    // `data.empty() && end >= start`, which a reversed range fails - and it left
    // the caller's previous bytes in the buffer on the failure path (issue #23).
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data = {0, 1, 2, 3, 4};
    mock->PutObject("test-bucket", "test-key", data);

    std::vector<uint8_t> buffer = {9, 9, 9, 9};

    EXPECT_FALSE(mock->GetObjectRangeInto("test-bucket", "test-key", 100, 50, buffer))
        << "a reversed range is a failure, as it is in the real adapter";
    EXPECT_TRUE(buffer.empty()) << "and it must not leave the previous read's bytes";

    buffer = {9, 9, 9, 9};
    EXPECT_FALSE(mock->GetObjectRangeInto("test-bucket", "test-key", 0, -1, buffer))
        << "the zero-byte-object shape is a failure too";
    EXPECT_TRUE(buffer.empty());

    buffer = {9, 9, 9, 9};
    EXPECT_FALSE(mock->GetObjectRangeInto("test-bucket", "test-key", 0, 99, buffer))
        << "past the end is a failure, not a clamp";
    EXPECT_TRUE(buffer.empty());

    buffer = {9, 9, 9, 9};
    ASSERT_TRUE(mock->GetObjectRangeInto("test-bucket", "test-key", 1, 3, buffer));
    ASSERT_EQ(buffer.size(), 3u) << "success means exactly the requested bytes";
    EXPECT_EQ(buffer[0], 1);
}

TEST_F(MockS3ClientTest, GetObjectRangeEdgeCases) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {0, 1, 2, 3, 4};
    mock->PutObject("test-bucket", "test-key", data);

    // Full range
    auto full = mock->GetObjectRange("test-bucket", "test-key", 0, 4);
    EXPECT_EQ(full.size(), 5u);

    // Past the end - rejected, not clamped. The mock used to answer this with
    // the two bytes it could reach, which made it the only implementation where
    // a short read counted as success. The real adapter fails one, and every
    // caller computes an exact in-bounds range from a size it already knows
    // (issue #23).
    auto past_end = mock->GetObjectRange("test-bucket", "test-key", 3, 100);
    EXPECT_TRUE(past_end.empty()) << "a range reaching past the last byte must fail";

    // One byte too far is still too far.
    EXPECT_TRUE(mock->GetObjectRange("test-bucket", "test-key", 0, 5).empty());
    EXPECT_EQ(mock->GetObjectRange("test-bucket", "test-key", 0, 4).size(), 5u)
        << "the last byte itself is in bounds";

    // Invalid start
    auto invalid = mock->GetObjectRange("test-bucket", "test-key", 10, 20);
    EXPECT_TRUE(invalid.empty());

    // Reversed and negative ranges
    EXPECT_TRUE(mock->GetObjectRange("test-bucket", "test-key", 3, 1).empty());
    EXPECT_TRUE(mock->GetObjectRange("test-bucket", "test-key", -1, 2).empty());
}

TEST_F(MockS3ClientTest, DeleteObject) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(100, 'X');
    mock->PutObject("test-bucket", "test-key", data);
    EXPECT_TRUE(mock->ObjectExists("test-bucket", "test-key"));

    EXPECT_TRUE(mock->DeleteObject("test-bucket", "test-key"));
    EXPECT_FALSE(mock->ObjectExists("test-bucket", "test-key"));
}

TEST_F(MockS3ClientTest, ListObjects) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(10, 'X');
    mock->PutObject("test-bucket", "file1.txt", data);
    mock->PutObject("test-bucket", "file2.txt", data);
    mock->PutObject("test-bucket", "folder/file3.txt", data);
    mock->PutObject("test-bucket", "folder/file4.txt", data);

    // List all
    auto result = mock->ListObjects("test-bucket", "", "", "", 1000);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects.size(), 4u);

    // List with prefix
    auto prefixed = mock->ListObjects("test-bucket", "folder/", "", "", 1000);
    EXPECT_TRUE(prefixed.success);
    EXPECT_EQ(prefixed.objects.size(), 2u);
}

TEST_F(MockS3ClientTest, ListObjectsWithDelimiter) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(10, 'X');
    mock->PutObject("test-bucket", "file1.txt", data);
    mock->PutObject("test-bucket", "folder1/file2.txt", data);
    mock->PutObject("test-bucket", "folder1/file3.txt", data);
    mock->PutObject("test-bucket", "folder2/file4.txt", data);

    auto result = mock->ListObjects("test-bucket", "", "/", "", 1000);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects.size(), 1u);  // file1.txt
    EXPECT_EQ(result.common_prefixes.size(), 2u);  // folder1/, folder2/
}

TEST_F(MockS3ClientTest, ListObjectsInvalidMaxKeys) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(10, 'X');
    mock->PutObject("test-bucket", "file1.txt", data);

    // max_keys = 0 should fail gracefully (not crash)
    auto result_zero = mock->ListObjects("test-bucket", "", "", "", 0);
    EXPECT_FALSE(result_zero.success);
    EXPECT_FALSE(result_zero.error_message.empty());

    // max_keys < 0 should also fail
    auto result_neg = mock->ListObjects("test-bucket", "", "", "", -1);
    EXPECT_FALSE(result_neg.success);
    EXPECT_FALSE(result_neg.error_message.empty());
}

TEST_F(MockS3ClientTest, ListObjectsPagination) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(10, 'X');
    // Create 5 objects with alphabetically sorted names
    mock->PutObject("test-bucket", "file1.txt", data);
    mock->PutObject("test-bucket", "file2.txt", data);
    mock->PutObject("test-bucket", "file3.txt", data);
    mock->PutObject("test-bucket", "file4.txt", data);
    mock->PutObject("test-bucket", "file5.txt", data);

    // First page: max 2 keys
    auto page1 = mock->ListObjects("test-bucket", "", "", "", 2);
    EXPECT_TRUE(page1.success);
    EXPECT_EQ(page1.objects.size(), 2u);
    EXPECT_TRUE(page1.is_truncated);
    EXPECT_FALSE(page1.next_continuation_token.empty());

    // Second page using continuation token
    auto page2 = mock->ListObjects("test-bucket", "", "", page1.next_continuation_token, 2);
    EXPECT_TRUE(page2.success);
    EXPECT_EQ(page2.objects.size(), 2u);
    EXPECT_TRUE(page2.is_truncated);

    // Third page - should have 1 remaining
    auto page3 = mock->ListObjects("test-bucket", "", "", page2.next_continuation_token, 2);
    EXPECT_TRUE(page3.success);
    EXPECT_EQ(page3.objects.size(), 1u);
    EXPECT_FALSE(page3.is_truncated);

    // Verify we got all 5 objects across all pages
    std::set<std::string> all_objects;
    for (const auto& obj : page1.objects) all_objects.insert(obj.key);
    for (const auto& obj : page2.objects) all_objects.insert(obj.key);
    for (const auto& obj : page3.objects) all_objects.insert(obj.key);
    EXPECT_EQ(all_objects.size(), 5u);
}

// ============================================================================
// MockS3Client CRC32 Computation Tests
// ============================================================================

TEST_F(MockS3ClientTest, GetChunkCRC32sSingleChunk) {
    mock->CreateBucket("test-bucket");

    // Create data smaller than CHUNK_SIZE
    std::vector<uint8_t> data(1000, 'A');
    mock->PutObject("test-bucket", "small-file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "small-file", 1000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 1u);

    // Verify CRC matches locally computed value
    uint32_t expected_crc = crc32_hw(data.data(), data.size());
    EXPECT_EQ(crcs[0], expected_crc);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sMultipleChunks) {
    mock->CreateBucket("test-bucket");

    // Create data spanning 3 chunks
    int64_t file_size = CHUNK_SIZE * 2 + 1000;
    std::vector<uint8_t> data(file_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    mock->PutObject("test-bucket", "large-file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "large-file", file_size, {}, nullptr, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 3u);

    // Verify each chunk's CRC
    uint32_t crc0 = crc32_hw(data.data(), CHUNK_SIZE);
    uint32_t crc1 = crc32_hw(data.data() + CHUNK_SIZE, CHUNK_SIZE);
    uint32_t crc2 = crc32_hw(data.data() + 2 * CHUNK_SIZE, 1000);

    EXPECT_EQ(crcs[0], crc0);
    EXPECT_EQ(crcs[1], crc1);
    EXPECT_EQ(crcs[2], crc2);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sSpecificChunks) {
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 5;
    std::vector<uint8_t> data(file_size, 'B');
    mock->PutObject("test-bucket", "file", data);

    // Request only chunks 1 and 3
    std::vector<int64_t> chunk_ids = {1, 3};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, chunk_ids, nullptr, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 2u);

    uint32_t expected_crc = crc32_hw(data.data(), CHUNK_SIZE);  // All chunks same content
    EXPECT_EQ(crcs[0], expected_crc);
    EXPECT_EQ(crcs[1], expected_crc);
}


TEST_F(MockS3ClientTest, GetChunkCRC32sRepeatsARequestedChunk) {
    // The shape all three implementations agree on: one entry per requested id,
    // in request order, duplicates included.
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 2;
    std::vector<uint8_t> data(file_size, 'D');
    mock->PutObject("test-bucket", "file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, {1, 1, 0},
                                     nullptr, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 3u);
    EXPECT_EQ(crcs[0], crcs[1]);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sWithProgress) {
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 4;
    std::vector<uint8_t> data(file_size, 'C');
    mock->PutObject("test-bucket", "file", data);

    std::vector<double> progress_values;
    auto progress_cb = [&progress_values](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, {}, progress_cb, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 4u);

    // Should have 4 progress callbacks (25%, 50%, 75%, 100%)
    ASSERT_EQ(progress_values.size(), 4u);
    EXPECT_DOUBLE_EQ(progress_values[3], 100.0);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sNegativeChunkIds) {
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 3;
    std::vector<uint8_t> data(file_size, 'X');
    mock->PutObject("test-bucket", "file", data);

    // Negative chunk_ids should return empty (error)
    std::vector<int64_t> invalid_ids = {0, -1, 2};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, invalid_ids, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_TRUE(crcs.empty());

    // Valid chunk_ids should still work
    std::vector<int64_t> valid_ids = {0, 1, 2};
    crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, valid_ids, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_EQ(crcs.size(), 3u);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sOutOfBoundsChunkIds) {
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 2;  // 2 chunks (ids 0 and 1)
    std::vector<uint8_t> data(file_size, 'X');
    mock->PutObject("test-bucket", "file", data);

    // Chunk ID 2 is out of bounds (only 0 and 1 exist)
    std::vector<int64_t> invalid_ids = {0, 2};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, invalid_ids, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_TRUE(crcs.empty());

    // Valid chunk_ids should work
    std::vector<int64_t> valid_ids = {0, 1};
    crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, valid_ids, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_EQ(crcs.size(), 2u);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sChunkIdThatOverflowsTheBoundsCheck) {
    // The bounds check used to be "chunk_id * chunk_size >= actual_size". For
    // an id this large that product overflows int64_t and wraps negative, so
    // it passed the check and then indexed the buffer at a negative offset -
    // a segfault under AddressSanitizer.
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 2;
    std::vector<uint8_t> data(file_size, 'X');
    mock->PutObject("test-bucket", "file", data);

    std::vector<int64_t> overflowing = {std::numeric_limits<int64_t>::max() / 2};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, overflowing,
                                     nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_TRUE(crcs.empty()) << "an id past the end must be rejected, not wrapped";
}

TEST_F(MockS3ClientTest, GetChunkCRC32sDuplicateChunkIds) {
    mock->CreateBucket("test-bucket");

    int64_t file_size = CHUNK_SIZE * 2;
    std::vector<uint8_t> data(file_size);
    // Fill chunk 0 with 'A', chunk 1 with 'B' to ensure different CRCs
    std::fill(data.begin(), data.begin() + CHUNK_SIZE, 'A');
    std::fill(data.begin() + CHUNK_SIZE, data.end(), 'B');
    mock->PutObject("test-bucket", "file", data);

    // Request chunk 0 twice - should return same CRC twice
    std::vector<int64_t> dup_ids = {0, 0, 1, 0};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", file_size, dup_ids, nullptr, false, 64, false, CHUNK_SIZE);
    ASSERT_EQ(crcs.size(), 4u);

    // All chunk 0 CRCs should be identical
    EXPECT_EQ(crcs[0], crcs[1]);
    EXPECT_EQ(crcs[0], crcs[3]);

    // Chunk 1 CRC should be different (different data)
    EXPECT_NE(crcs[0], crcs[2]);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sFileSizeMismatch) {
    mock->CreateBucket("test-bucket");

    std::vector<uint8_t> data(1000, 'X');
    mock->PutObject("test-bucket", "file", data);

    // Passing wrong file_size should return empty (error)
    auto crcs = mock->GetChunkCRC32s("test-bucket", "file", 2000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_TRUE(crcs.empty());

    // Correct file_size should work
    crcs = mock->GetChunkCRC32s("test-bucket", "file", 1000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_FALSE(crcs.empty());
}

// ============================================================================
// Integration Tests with run_comparison using MockS3Client
// ============================================================================

class MockS3ComparisonTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock;
    std::string temp_dir;

    void SetUp() override {
        mock = std::make_shared<MockS3Client>();
        mock->CreateBucket("test-bucket");
        temp_dir = "/tmp/objiff_s3_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
        // Ignore errors - best effort cleanup
    }

    std::string create_temp_file(const std::vector<uint8_t>& data, const std::string& name = "test.bin") {
        std::string path = temp_dir + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        return path;
    }
};

TEST_F(MockS3ComparisonTest, S3toS3SameData) {
    // Create identical data in two S3 "objects"
    std::vector<uint8_t> data(CHUNK_SIZE + 1000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    mock->PutObject("test-bucket", "file-a", data);
    mock->PutObject("test-bucket", "file-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_TRUE(result.mismatched_chunks.empty());
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
    EXPECT_EQ(result.source_b_crcs.size(), 2u);
}

TEST_F(MockS3ComparisonTest, S3toS3DifferentData) {
    std::vector<uint8_t> data_a(CHUNK_SIZE * 2, 'A');
    std::vector<uint8_t> data_b(CHUNK_SIZE * 2, 'A');
    // Modify second chunk
    data_b[CHUNK_SIZE + 100] = 'B';

    mock->PutObject("test-bucket", "file-a", data_a);
    mock->PutObject("test-bucket", "file-b", data_b);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    ASSERT_EQ(result.mismatched_chunks.size(), 1u);
    EXPECT_EQ(result.mismatched_chunks[0], 1u);  // Second chunk differs
}

TEST_F(MockS3ComparisonTest, S3toS3SizeMismatch) {
    // Both files fit in a single chunk (< 8MB), but have different sizes
    // CRCs differ because chunk sizes differ, even with same content
    std::vector<uint8_t> data_a(1000, 'A');
    std::vector<uint8_t> data_b(2000, 'A');

    mock->PutObject("test-bucket", "file-a", data_a);
    mock->PutObject("test-bucket", "file-b", data_b);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

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

TEST_F(MockS3ComparisonTest, S3toS3ObjectNotFound) {
    std::vector<uint8_t> data(1000, 'A');
    mock->PutObject("test-bucket", "file-a", data);
    // file-b doesn't exist

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size") != std::string::npos);
}

TEST_F(MockS3ComparisonTest, LocalToS3SameData) {
    // Create identical data locally and in S3
    std::vector<uint8_t> data(CHUNK_SIZE + 500);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    std::string local_path = create_temp_file(data);
    mock->PutObject("test-bucket", "s3-file", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = local_path;

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "s3-file";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
}

TEST_F(MockS3ComparisonTest, LocalToS3DifferentData) {
    std::vector<uint8_t> local_data(CHUNK_SIZE, 'L');
    std::vector<uint8_t> s3_data(CHUNK_SIZE, 'S');

    std::string local_path = create_temp_file(local_data);
    mock->PutObject("test-bucket", "s3-file", s3_data);

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = local_path;

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "s3-file";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.mismatched_chunks.size(), 1u);
}

TEST_F(MockS3ComparisonTest, S3ToLocalSameData) {
    // Test the reverse direction: S3 as source_a, Local as source_b
    std::vector<uint8_t> data(CHUNK_SIZE + 500);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    mock->PutObject("test-bucket", "s3-file", data);
    std::string local_path = create_temp_file(data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "s3-file";

    config.source_b.type = SourceType::Local;
    config.source_b.path = local_path;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
}

TEST_F(MockS3ComparisonTest, S3ToLocalDifferentData) {
    std::vector<uint8_t> s3_data(CHUNK_SIZE, 'S');
    std::vector<uint8_t> local_data(CHUNK_SIZE, 'L');

    mock->PutObject("test-bucket", "s3-file", s3_data);
    std::string local_path = create_temp_file(local_data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "s3-file";

    config.source_b.type = SourceType::Local;
    config.source_b.path = local_path;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.mismatched_chunks.size(), 1u);
}

TEST_F(MockS3ComparisonTest, SpecificChunksComparison) {
    // Create data with 5 chunks, chunks 0,2,4 same, chunks 1,3 different
    int64_t file_size = CHUNK_SIZE * 5;
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Make chunks 1 and 3 different
    std::fill(data_b.begin() + CHUNK_SIZE, data_b.begin() + CHUNK_SIZE * 2, 'B');
    std::fill(data_b.begin() + CHUNK_SIZE * 3, data_b.begin() + CHUNK_SIZE * 4, 'B');

    mock->PutObject("test-bucket", "file-a", data_a);
    mock->PutObject("test-bucket", "file-b", data_b);

    // Only compare chunks 0 and 2 (should match)
    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    config.chunk_ids = {0, 2, 4};

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);  // Only compared matching chunks
}

TEST_F(MockS3ComparisonTest, LargeFileComparison) {
    // 20 MB file (enough to test multi-chunk without excessive memory use)
    int64_t file_size = 20 * 1024 * 1024;
    std::vector<uint8_t> data(file_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    mock->PutObject("test-bucket", "large-a", data);
    mock->PutObject("test-bucket", "large-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "large-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "large-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);

    // Should have 3 chunks (20MB / 8MB = 2.5 -> 3)
    int64_t expected_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    EXPECT_EQ(result.source_a_crcs.size(), static_cast<size_t>(expected_chunks));
}

TEST_F(MockS3ComparisonTest, ProgressTracking) {
    std::vector<uint8_t> data(CHUNK_SIZE * 4, 'X');
    mock->PutObject("test-bucket", "file-a", data);
    mock->PutObject("test-bucket", "file-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(progress.source_a_done);
    EXPECT_TRUE(progress.source_b_done);
    EXPECT_GE(progress.source_a_progress.load(), 99.0);
    EXPECT_GE(progress.source_b_progress.load(), 99.0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(MockS3ComparisonTest, ExactlyOneChunk) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    mock->PutObject("test-bucket", "exact-a", data);
    mock->PutObject("test-bucket", "exact-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "exact-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "exact-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 1u);
}

TEST_F(MockS3ComparisonTest, OneBytePastChunk) {
    std::vector<uint8_t> data(CHUNK_SIZE + 1, 'X');
    mock->PutObject("test-bucket", "plus1-a", data);
    mock->PutObject("test-bucket", "plus1-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "plus1-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "plus1-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 2u);  // 2 chunks
}

TEST_F(MockS3ComparisonTest, FileSizeStoredInResult) {
    // Verify that file_size is correctly stored in ComparisonResult
    int64_t expected_size = CHUNK_SIZE * 2 + 12345;
    std::vector<uint8_t> data(expected_size, 'F');
    mock->PutObject("test-bucket", "filesize-a", data);
    mock->PutObject("test-bucket", "filesize-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "filesize-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "filesize-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.file_size, expected_size);
}

TEST_F(MockS3ComparisonTest, FileSizeStoredOnMismatch) {
    // Verify file_size is stored even when files don't match
    int64_t expected_size = CHUNK_SIZE + 5000;
    std::vector<uint8_t> data_a(expected_size, 'A');
    std::vector<uint8_t> data_b(expected_size, 'B');
    mock->PutObject("test-bucket", "mismatch-a", data_a);
    mock->PutObject("test-bucket", "mismatch-b", data_b);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "mismatch-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "mismatch-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.file_size, expected_size);
}

TEST_F(MockS3ComparisonTest, FileSizeZeroOnError) {
    // Verify file_size behavior when comparison fails
    // (file doesn't exist - should fail before setting file_size)
    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "nonexistent-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "nonexistent-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_FALSE(result.success);
    // file_size should remain at default (0) since we failed before setting it
    EXPECT_EQ(result.file_size, 0);
}

// ============================================================================
// Error Injection Tests
// ============================================================================

TEST_F(MockS3ClientTest, ErrorInjectionGetObjectSize) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(100, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Should work normally
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), 100);

    // Inject failure
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);

    // Clear failure
    mock->ClearFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), 100);
}

TEST_F(MockS3ClientTest, ErrorInjectionGetChunkCRC32s) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(1000, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Should work normally
    auto crcs = mock->GetChunkCRC32s("test-bucket", "test-key", 1000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_FALSE(crcs.empty());

    // Inject failure
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::GetChunkCRC32s);
    crcs = mock->GetChunkCRC32s("test-bucket", "test-key", 1000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_TRUE(crcs.empty());

    // Clear all failures
    mock->ClearAllFailures();
    crcs = mock->GetChunkCRC32s("test-bucket", "test-key", 1000, {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_FALSE(crcs.empty());
}

TEST_F(MockS3ComparisonTest, ErrorInjectionComparison) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    mock->PutObject("test-bucket", "file-a", data);
    mock->PutObject("test-bucket", "file-b", data);

    // Inject failure on file-b's CRC computation
    mock->SetFailure("test-bucket", "file-b", S3MockMethod::GetChunkCRC32s);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    // Should fail because file-b's CRCs couldn't be computed
    EXPECT_TRUE(result.source_b_crcs.empty());
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(MockS3ClientTest, ConcurrentReadsAndWrites) {
    mock->CreateBucket("test-bucket");

    // Pre-populate some objects
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
        mock->PutObject("test-bucket", "key" + std::to_string(i), data);
    }

    std::vector<std::future<bool>> futures;

    // Launch concurrent readers
    for (int i = 0; i < 10; ++i) {
        futures.push_back(std::async(std::launch::async, [this, i]() {
            for (int j = 0; j < 100; ++j) {
                auto size = mock->GetObjectSize("test-bucket", "key" + std::to_string(i));
                if (size != 100) return false;
            }
            return true;
        }));
    }

    // Launch concurrent writers
    for (int i = 10; i < 20; ++i) {
        futures.push_back(std::async(std::launch::async, [this, i]() {
            for (int j = 0; j < 50; ++j) {
                std::vector<uint8_t> data(100, static_cast<uint8_t>(i + j));
                if (!mock->PutObject("test-bucket", "key" + std::to_string(i), data)) {
                    return false;
                }
            }
            return true;
        }));
    }

    // All operations should succeed without crashes or data races
    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

TEST_F(MockS3ComparisonTest, ConcurrentComparisons) {
    // Create multiple file pairs
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> data(CHUNK_SIZE + 1000, static_cast<uint8_t>(i));
        mock->PutObject("test-bucket", "file-a-" + std::to_string(i), data);
        mock->PutObject("test-bucket", "file-b-" + std::to_string(i), data);
    }

    std::vector<std::future<bool>> futures;

    // Run multiple comparisons concurrently
    for (int i = 0; i < 5; ++i) {
        futures.push_back(std::async(std::launch::async, [this, i]() {
            ComparisonConfig config;
            config.source_a.type = SourceType::S3;
            config.source_a.bucket = "test-bucket";
            config.source_a.path = "file-a-" + std::to_string(i);

            config.source_b.type = SourceType::S3;
            config.source_b.bucket = "test-bucket";
            config.source_b.path = "file-b-" + std::to_string(i);

            ComparisonProgress progress;
            auto result = run_comparison(config, progress, mock, mock);

            return result.success && result.all_match;
        }));
    }

    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

// ============================================================================
// Cancellation Tests
// ============================================================================

TEST_F(MockS3ComparisonTest, CancellationDuringComparison) {
    // Create large files to ensure comparison takes some time
    std::vector<uint8_t> data(CHUNK_SIZE * 10, 'X');
    mock->PutObject("test-bucket", "large-a", data);
    mock->PutObject("test-bucket", "large-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "large-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "large-b";

    ComparisonProgress progress;

    // Start comparison in background and cancel quickly
    auto future = std::async(std::launch::async, [&]() {
        return run_comparison(config, progress, mock, mock);
    });

    // Give it a tiny bit of time to start, then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    progress.cancelled = true;

    auto result = future.get();

    // The comparison may or may not complete depending on timing
    // but it should not crash and the cancelled flag should be respected
    EXPECT_TRUE(progress.cancelled);
}

// ============================================================================
// Empty File Behavior
// ============================================================================

TEST_F(MockS3ComparisonTest, BothFilesEmpty) {
    // Empty files (0 bytes) should match - both have zero chunks
    std::vector<uint8_t> empty_data;
    mock->PutObject("test-bucket", "empty-a", empty_data);
    mock->PutObject("test-bucket", "empty-b", empty_data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "empty-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "empty-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    // Empty files succeed and match (both have zero chunks)
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

// ============================================================================
// S3MockMethodToString Tests
// ============================================================================

TEST(S3MockMethodToStringTest, AllMethods) {
    // Test all S3MockMethod enum values get proper string representation
    EXPECT_EQ(S3MockMethodToString(S3MockMethod::GetObjectSize), "GetObjectSize");
    EXPECT_EQ(S3MockMethodToString(S3MockMethod::GetChunkCRC32s), "GetChunkCRC32s");
    EXPECT_EQ(S3MockMethodToString(S3MockMethod::GetObjectRange), "GetObjectRange");
    EXPECT_EQ(S3MockMethodToString(S3MockMethod::PutObject), "PutObject");
    EXPECT_EQ(S3MockMethodToString(S3MockMethod::DeleteObject), "DeleteObject");
}

// ============================================================================
// Additional Error Injection Tests
// ============================================================================

TEST_F(MockS3ClientTest, ErrorInjectionGetObjectRange) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    mock->PutObject("test-bucket", "test-key", data);

    // Should work normally
    auto range = mock->GetObjectRange("test-bucket", "test-key", 0, 4);
    EXPECT_EQ(range.size(), 5u);

    // Inject failure
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::GetObjectRange);
    range = mock->GetObjectRange("test-bucket", "test-key", 0, 4);
    EXPECT_TRUE(range.empty());

    // Clear failure
    mock->ClearFailure("test-bucket", "test-key", S3MockMethod::GetObjectRange);
    range = mock->GetObjectRange("test-bucket", "test-key", 0, 4);
    EXPECT_EQ(range.size(), 5u);
}

TEST_F(MockS3ClientTest, ErrorInjectionPutObject) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(100, 'X');

    // Should work normally
    EXPECT_TRUE(mock->PutObject("test-bucket", "test-key", data));

    // Inject failure
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::PutObject);
    EXPECT_FALSE(mock->PutObject("test-bucket", "test-key", data));

    // Clear failure
    mock->ClearFailure("test-bucket", "test-key", S3MockMethod::PutObject);
    EXPECT_TRUE(mock->PutObject("test-bucket", "test-key", data));
}

TEST_F(MockS3ClientTest, ErrorInjectionDeleteObject) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(100, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Inject failure before delete
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::DeleteObject);
    EXPECT_FALSE(mock->DeleteObject("test-bucket", "test-key"));
    // Object should still exist
    EXPECT_TRUE(mock->ObjectExists("test-bucket", "test-key"));

    // Clear failure and delete
    mock->ClearFailure("test-bucket", "test-key", S3MockMethod::DeleteObject);
    EXPECT_TRUE(mock->DeleteObject("test-bucket", "test-key"));
    EXPECT_FALSE(mock->ObjectExists("test-bucket", "test-key"));
}

// ============================================================================
// Source A Not Found Tests (S3)
// ============================================================================

TEST_F(MockS3ComparisonTest, S3SourceANotFound) {
    // Test when source A doesn't exist (covers lines 90-91 in comparison_task.cpp)
    std::vector<uint8_t> data(1000, 'A');
    mock->PutObject("test-bucket", "file-b", data);
    // file-a doesn't exist

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size for source A") != std::string::npos);
}

TEST_F(MockS3ComparisonTest, S3SourceAErrorInjection) {
    // Test when GetObjectSize fails for source A
    std::vector<uint8_t> data(1000, 'A');
    mock->PutObject("test-bucket", "file-a", data);
    mock->PutObject("test-bucket", "file-b", data);

    // Inject failure on source A
    mock->SetFailure("test-bucket", "file-a", S3MockMethod::GetObjectSize);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "file-b";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Failed to get size for source A") != std::string::npos);
}

// ============================================================================
// GetObjectRange Edge Cases
// ============================================================================

TEST_F(MockS3ClientTest, GetObjectRangeNonExistentBucket) {
    auto range = mock->GetObjectRange("nonexistent-bucket", "key", 0, 10);
    EXPECT_TRUE(range.empty());
}

TEST_F(MockS3ClientTest, GetObjectRangeNonExistentKey) {
    mock->CreateBucket("test-bucket");
    auto range = mock->GetObjectRange("test-bucket", "nonexistent-key", 0, 10);
    EXPECT_TRUE(range.empty());
}

TEST_F(MockS3ClientTest, GetObjectRangeNegativeStart) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data = {0, 1, 2, 3, 4};
    mock->PutObject("test-bucket", "test-key", data);

    // Negative start should be handled gracefully
    auto range = mock->GetObjectRange("test-bucket", "test-key", -1, 3);
    // Implementation may vary, but shouldn't crash
    EXPECT_TRUE(range.empty());
}

// ============================================================================
// ListObjects Edge Cases
// ============================================================================

TEST_F(MockS3ClientTest, ListObjectsNonExistentBucket) {
    auto result = mock->ListObjects("nonexistent-bucket", "", "", "", 100);
    EXPECT_FALSE(result.success);
}

TEST_F(MockS3ClientTest, ListObjectsEmptyBucket) {
    mock->CreateBucket("empty-bucket");
    auto result = mock->ListObjects("empty-bucket", "", "", "", 100);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.objects.empty());
    EXPECT_FALSE(result.is_truncated);
}

TEST_F(MockS3ClientTest, ListObjectsNoMatchingPrefix) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(10, 'X');
    mock->PutObject("test-bucket", "folder/file.txt", data);

    auto result = mock->ListObjects("test-bucket", "other/", "", "", 100);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.objects.empty());
}

// ============================================================================
// Same Region S3 Client Reuse Tests
// ============================================================================

TEST_F(MockS3ComparisonTest, S3toS3SameRegionClientReuse) {
    // Test that when both S3 sources are in the same region,
    // the comparison still works correctly (exercises client reuse path)
    std::vector<uint8_t> data(CHUNK_SIZE + 100, 'R');
    mock->PutObject("test-bucket", "region-file-a", data);
    mock->PutObject("test-bucket", "region-file-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "region-file-a";
    config.source_a.region = "us-east-1";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "region-file-b";
    config.source_b.region = "us-east-1";  // Same region as source_a

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
}

TEST_F(MockS3ComparisonTest, S3toS3DifferentRegions) {
    // Test with explicitly different regions
    std::vector<uint8_t> data(CHUNK_SIZE, 'D');
    mock->PutObject("test-bucket", "diff-region-a", data);
    mock->PutObject("test-bucket", "diff-region-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "diff-region-a";
    config.source_a.region = "us-east-1";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "diff-region-b";
    config.source_b.region = "eu-west-1";  // Different region

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
}

// ============================================================================
// Debug Mode Tests
// ============================================================================

TEST_F(MockS3ComparisonTest, S3ComparisonWithDebugMode) {
    std::vector<uint8_t> data(CHUNK_SIZE * 2, 'D');
    mock->PutObject("test-bucket", "debug-file-a", data);
    mock->PutObject("test-bucket", "debug-file-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "debug-file-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "debug-file-b";

    config.debug = true;  // Enable debug mode

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    // Debug mode should not affect correctness
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
}

// ============================================================================
// Special Character Key Tests (URL Encoding)
// ============================================================================

TEST_F(MockS3ClientTest, KeysWithSpecialCharacters) {
    mock->CreateBucket("test-bucket");

    // Test keys with spaces and special characters
    std::vector<uint8_t> data(100, 'S');
    EXPECT_TRUE(mock->PutObject("test-bucket", "folder/file with spaces.txt", data));
    EXPECT_TRUE(mock->PutObject("test-bucket", "path/to/file+plus.bin", data));
    EXPECT_TRUE(mock->PutObject("test-bucket", "special/chars&equals=test.dat", data));

    // Verify we can retrieve them
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "folder/file with spaces.txt"), 100);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "path/to/file+plus.bin"), 100);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "special/chars&equals=test.dat"), 100);
}

TEST_F(MockS3ComparisonTest, S3ComparisonWithSpecialCharacterKeys) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'K');
    mock->PutObject("test-bucket", "folder/file with spaces.bin", data);
    mock->PutObject("test-bucket", "folder/file+plus.bin", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "folder/file with spaces.bin";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "folder/file+plus.bin";

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);  // Same data
}

TEST_F(MockS3ClientTest, DeeplyNestedKeys) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(50, 'N');

    // Test deeply nested paths
    EXPECT_TRUE(mock->PutObject("test-bucket", "a/b/c/d/e/f/g/deep.txt", data));
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "a/b/c/d/e/f/g/deep.txt"), 50);

    // Test listing with deeply nested prefix
    auto result = mock->ListObjects("test-bucket", "a/b/c/", "/", "", 100);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.common_prefixes.empty());
}

// ============================================================================
// Chunk Count Mismatch Tests (via injected failure)
// ============================================================================

TEST_F(MockS3ComparisonTest, ChunkCountMismatchViaDifferentChunkIds) {
    // Create data that would have 3 chunks
    std::vector<uint8_t> data(CHUNK_SIZE * 3, 'M');
    mock->PutObject("test-bucket", "multi-a", data);
    mock->PutObject("test-bucket", "multi-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "multi-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "multi-b";

    // Request specific chunks only
    config.chunk_ids = {0, 2};  // Skip chunk 1

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    // Should only have 2 results (chunks 0 and 2)
    EXPECT_EQ(result.source_a_crcs.size(), 2u);
    EXPECT_EQ(result.source_b_crcs.size(), 2u);
}

// ============================================================================
// Progress Callback Edge Cases
// ============================================================================

TEST_F(MockS3ClientTest, GetChunkCRC32sProgressCallbackValues) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(CHUNK_SIZE * 4, 'P');  // 4 chunks
    mock->PutObject("test-bucket", "progress-test", data);

    std::vector<double> progress_values;
    auto progress_cb = [&progress_values](double pct) {
        progress_values.push_back(pct);
    };

    auto crcs = mock->GetChunkCRC32s("test-bucket", "progress-test",
                                      static_cast<int64_t>(data.size()),
                                      {}, progress_cb, false, 64, false, CHUNK_SIZE);

    EXPECT_EQ(crcs.size(), 4u);
    EXPECT_FALSE(progress_values.empty());
    // Last progress value should be 100%
    EXPECT_NEAR(progress_values.back(), 100.0, 0.1);
    // Progress should be monotonically increasing
    for (size_t i = 1; i < progress_values.size(); ++i) {
        EXPECT_GE(progress_values[i], progress_values[i-1]);
    }
}

TEST_F(MockS3ClientTest, GetChunkCRC32sNullProgressCallback) {
    mock->CreateBucket("test-bucket");
    std::vector<uint8_t> data(CHUNK_SIZE * 2, 'N');
    mock->PutObject("test-bucket", "null-progress", data);

    // Should work fine with nullptr callback
    auto crcs = mock->GetChunkCRC32s("test-bucket", "null-progress",
                                      static_cast<int64_t>(data.size()),
                                      {}, nullptr, false, 64, false, CHUNK_SIZE);
    EXPECT_EQ(crcs.size(), 2u);
}

// ============================================================================
// UploadRegistry Tests
// ============================================================================
//
// Note: These tests use nullptr for the S3 client because:
// 1. The AWS SDK's S3Client isn't designed for easy mocking
// 2. abort_all() gracefully handles null clients (logs warning, skips)
// 3. The actual AbortMultipartUpload network calls require integration tests
//
// These unit tests verify registry logic (register/unregister/count/threading).
// Integration tests with localstack/minio should verify actual abort behavior.
// ============================================================================

class UploadRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear any uploads from previous tests
        UploadRegistry::instance().abort_all();
    }

    void TearDown() override {
        // Clean up after each test
        UploadRegistry::instance().abort_all();
    }
};

TEST_F(UploadRegistryTest, SingletonInstance) {
    // Verify we always get the same instance
    auto& instance1 = UploadRegistry::instance();
    auto& instance2 = UploadRegistry::instance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(UploadRegistryTest, RegisterAndCount) {
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);

    // Register an upload (client can be nullptr for this test)
    ActiveUpload upload1;
    upload1.client = nullptr;
    upload1.bucket = "test-bucket";
    upload1.key = "test-key-1";
    upload1.upload_id = "upload-id-1";

    UploadRegistry::instance().register_upload(upload1);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    // Register another upload
    ActiveUpload upload2;
    upload2.client = nullptr;
    upload2.bucket = "test-bucket";
    upload2.key = "test-key-2";
    upload2.upload_id = "upload-id-2";

    UploadRegistry::instance().register_upload(upload2);
    EXPECT_EQ(UploadRegistry::instance().count(), 2u);
}

TEST_F(UploadRegistryTest, UnregisterUpload) {
    // Register two uploads
    ActiveUpload upload1;
    upload1.client = nullptr;
    upload1.bucket = "bucket";
    upload1.key = "key1";
    upload1.upload_id = "id-1";

    ActiveUpload upload2;
    upload2.client = nullptr;
    upload2.bucket = "bucket";
    upload2.key = "key2";
    upload2.upload_id = "id-2";

    UploadRegistry::instance().register_upload(upload1);
    UploadRegistry::instance().register_upload(upload2);
    EXPECT_EQ(UploadRegistry::instance().count(), 2u);

    // Unregister first upload
    UploadRegistry::instance().unregister_upload("id-1");
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    // Unregister second upload
    UploadRegistry::instance().unregister_upload("id-2");
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, UnregisterNonexistentUpload) {
    // Unregistering a non-existent upload should not crash
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
    UploadRegistry::instance().unregister_upload("nonexistent-id");
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);

    // Register one, then try to unregister a different one
    ActiveUpload upload;
    upload.client = nullptr;
    upload.bucket = "bucket";
    upload.key = "key";
    upload.upload_id = "real-id";

    UploadRegistry::instance().register_upload(upload);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    UploadRegistry::instance().unregister_upload("wrong-id");
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);  // Still 1
}

TEST_F(UploadRegistryTest, AbortAllClearsRegistry) {
    // Register multiple uploads with nullptr clients
    // (abort_all will try to call abort but gracefully handle nullptr)
    for (int i = 0; i < 5; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket-" + std::to_string(i);
        upload.key = "key-" + std::to_string(i);
        upload.upload_id = "id-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }
    EXPECT_EQ(UploadRegistry::instance().count(), 5u);

    // Abort all should clear the registry
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, AbortAllOnEmptyRegistry) {
    // Should not crash when aborting an empty registry
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, DuplicateUploadIdOverwrites) {
    // If same upload_id is registered twice, it should overwrite
    ActiveUpload upload1;
    upload1.client = nullptr;
    upload1.bucket = "bucket-1";
    upload1.key = "key-1";
    upload1.upload_id = "same-id";

    ActiveUpload upload2;
    upload2.client = nullptr;
    upload2.bucket = "bucket-2";
    upload2.key = "key-2";
    upload2.upload_id = "same-id";  // Same ID

    UploadRegistry::instance().register_upload(upload1);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    UploadRegistry::instance().register_upload(upload2);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);  // Still 1, overwrote

    // Unregister should remove it
    UploadRegistry::instance().unregister_upload("same-id");
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, ConcurrentRegistration) {
    // Test thread safety of register/unregister
    const int num_threads = 10;
    const int uploads_per_thread = 100;

    std::vector<std::future<void>> futures;

    // Spawn threads that register uploads
    for (int t = 0; t < num_threads; ++t) {
        futures.push_back(std::async(std::launch::async, [t]() {
            for (int i = 0; i < 100; ++i) {
                ActiveUpload upload;
                upload.client = nullptr;
                upload.bucket = "bucket";
                upload.key = "key";
                upload.upload_id = "thread-" + std::to_string(t) + "-upload-" + std::to_string(i);
                UploadRegistry::instance().register_upload(upload);
            }
        }));
    }

    // Wait for all threads
    for (auto& f : futures) {
        f.get();
    }

    // Should have all uploads registered
    EXPECT_EQ(UploadRegistry::instance().count(),
              static_cast<size_t>(num_threads * uploads_per_thread));
}

TEST_F(UploadRegistryTest, ConcurrentUnregistration) {
    // Pre-register uploads
    const int num_uploads = 100;
    for (int i = 0; i < num_uploads; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket";
        upload.key = "key";
        upload.upload_id = "upload-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }
    EXPECT_EQ(UploadRegistry::instance().count(), static_cast<size_t>(num_uploads));

    // Spawn threads that unregister uploads
    std::vector<std::future<void>> futures;
    for (int i = 0; i < num_uploads; ++i) {
        futures.push_back(std::async(std::launch::async, [i]() {
            UploadRegistry::instance().unregister_upload("upload-" + std::to_string(i));
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, ConcurrentMixedOperations) {
    // Test concurrent register, unregister, and count operations
    std::atomic<bool> stop{false};
    std::vector<std::future<void>> futures;

    // Thread that continuously registers
    futures.push_back(std::async(std::launch::async, [&stop]() {
        int counter = 0;
        while (!stop) {
            ActiveUpload upload;
            upload.client = nullptr;
            upload.bucket = "bucket";
            upload.key = "key";
            upload.upload_id = "reg-" + std::to_string(counter++);
            UploadRegistry::instance().register_upload(upload);
            if (counter > 1000) counter = 0;  // Wrap around
        }
    }));

    // Thread that continuously unregisters
    futures.push_back(std::async(std::launch::async, [&stop]() {
        int counter = 0;
        while (!stop) {
            UploadRegistry::instance().unregister_upload("reg-" + std::to_string(counter++));
            if (counter > 1000) counter = 0;
        }
    }));

    // Thread that continuously reads count
    futures.push_back(std::async(std::launch::async, [&stop]() {
        while (!stop) {
            [[maybe_unused]] auto count = UploadRegistry::instance().count();
            // Just reading, no assertion - testing for crashes
        }
    }));

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;

    for (auto& f : futures) {
        f.get();
    }

    // No assertions - just verifying no crashes or deadlocks
}

// ============================================================================
// Early Exit / Cleanup Simulation Tests
// ============================================================================

TEST_F(UploadRegistryTest, SimulatedEarlyExitWithPendingUploads) {
    // Simulate the scenario where uploads are registered but process exits
    // before normal cleanup (unregister) can occur

    // Register several uploads as if operations started
    for (int i = 0; i < 3; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket-" + std::to_string(i);
        upload.key = "key-" + std::to_string(i);
        upload.upload_id = "pending-upload-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }

    // Verify uploads are pending (simulating in-progress operations)
    EXPECT_EQ(UploadRegistry::instance().count(), 3u);

    // Simulate early exit cleanup (what signal handler would do)
    UploadRegistry::instance().abort_all();

    // Verify all uploads were cleaned up
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, AbortAllIdempotent) {
    // Verify abort_all can be called multiple times safely
    // (important if atexit and signal handler both fire)

    ActiveUpload upload;
    upload.client = nullptr;
    upload.bucket = "bucket";
    upload.key = "key";
    upload.upload_id = "test-upload";
    UploadRegistry::instance().register_upload(upload);

    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    // First abort
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);

    // Second abort (should be no-op, not crash)
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);

    // Third abort
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, PartialOperationCleanup) {
    // Simulate scenario where some operations complete normally
    // and others are interrupted

    // Register 5 uploads
    for (int i = 0; i < 5; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket";
        upload.key = "key-" + std::to_string(i);
        upload.upload_id = "upload-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }
    EXPECT_EQ(UploadRegistry::instance().count(), 5u);

    // Simulate 2 operations completing normally (they would unregister)
    UploadRegistry::instance().unregister_upload("upload-0");
    UploadRegistry::instance().unregister_upload("upload-2");
    EXPECT_EQ(UploadRegistry::instance().count(), 3u);

    // Simulate early exit - remaining 3 should be cleaned up
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, ConcurrentAbortAll) {
    // Test that concurrent abort_all calls don't cause issues
    // (e.g., multiple threads calling cleanup simultaneously)

    // Register many uploads
    for (int i = 0; i < 100; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket";
        upload.key = "key";
        upload.upload_id = "upload-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }

    // Launch multiple threads calling abort_all simultaneously
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(std::async(std::launch::async, []() {
            UploadRegistry::instance().abort_all();
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    // Should be empty and no crashes
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, RegisterDuringAbortAll) {
    // Test behavior when new uploads are registered while abort_all is running
    // This simulates a race between cleanup and new operations

    std::atomic<bool> abort_started{false};
    std::atomic<bool> abort_done{false};

    // Pre-register some uploads
    for (int i = 0; i < 50; ++i) {
        ActiveUpload upload;
        upload.client = nullptr;
        upload.bucket = "bucket";
        upload.key = "key";
        upload.upload_id = "initial-" + std::to_string(i);
        UploadRegistry::instance().register_upload(upload);
    }

    // Thread that calls abort_all
    auto abort_future = std::async(std::launch::async, [&]() {
        abort_started = true;
        UploadRegistry::instance().abort_all();
        abort_done = true;
    });

    // Thread that tries to register new uploads during abort
    auto register_future = std::async(std::launch::async, [&]() {
        while (!abort_started) {
            std::this_thread::yield();
        }
        // Try to register while abort might be running
        for (int i = 0; i < 10; ++i) {
            ActiveUpload upload;
            upload.client = nullptr;
            upload.bucket = "bucket";
            upload.key = "key";
            upload.upload_id = "during-abort-" + std::to_string(i);
            UploadRegistry::instance().register_upload(upload);
        }
    });

    abort_future.get();
    register_future.get();

    // Some uploads registered during/after abort may still be present
    // The important thing is no crashes or deadlocks occurred
    // Clean up any remaining
    UploadRegistry::instance().abort_all();
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

// ============================================================================
// UploadGuard Tests
// ============================================================================

TEST_F(UploadRegistryTest, GuardReleaseUnregisters) {
    // Test that calling release() unregisters the upload
    ActiveUpload upload;
    upload.client = nullptr;
    upload.bucket = "bucket";
    upload.key = "key";
    upload.upload_id = "guard-test-id";

    UploadRegistry::instance().register_upload(upload);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    {
        UploadGuard guard("guard-test-id");
        EXPECT_EQ(UploadRegistry::instance().count(), 1u);
        guard.release();
        EXPECT_EQ(UploadRegistry::instance().count(), 0u);
    }
    // After scope exit, count should still be 0
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

TEST_F(UploadRegistryTest, GuardDestructorWithoutReleaseLeavesRegistered) {
    // Test that destructor without release() leaves upload registered
    // This is the key exception safety behavior
    ActiveUpload upload;
    upload.client = nullptr;
    upload.bucket = "bucket";
    upload.key = "key";
    upload.upload_id = "exception-test-id";

    UploadRegistry::instance().register_upload(upload);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    {
        UploadGuard guard("exception-test-id");
        EXPECT_EQ(UploadRegistry::instance().count(), 1u);
        // Intentionally NOT calling release() - simulating exception path
    }
    // After scope exit without release(), upload should still be registered
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);
}

TEST_F(UploadRegistryTest, GuardDoubleReleaseIsSafe) {
    // Test that calling release() twice is safe (idempotent)
    ActiveUpload upload;
    upload.client = nullptr;
    upload.bucket = "bucket";
    upload.key = "key";
    upload.upload_id = "double-release-id";

    UploadRegistry::instance().register_upload(upload);
    EXPECT_EQ(UploadRegistry::instance().count(), 1u);

    {
        UploadGuard guard("double-release-id");
        guard.release();
        EXPECT_EQ(UploadRegistry::instance().count(), 0u);
        guard.release();  // Second release should be no-op
        EXPECT_EQ(UploadRegistry::instance().count(), 0u);
    }
    EXPECT_EQ(UploadRegistry::instance().count(), 0u);
}

// ============================================================================
// Block Analysis with Mock S3 Tests
// ============================================================================

class MockS3BlockAnalysisTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_a;
    std::shared_ptr<MockS3Client> mock_b;
    std::string temp_dir;

    void SetUp() override {
        mock_a = std::make_shared<MockS3Client>();
        mock_b = std::make_shared<MockS3Client>();
        mock_a->CreateBucket("bucket-a");
        mock_b->CreateBucket("bucket-b");

        temp_dir = "/tmp/objiff_mock_block_" + std::to_string(getpid());
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

TEST_F(MockS3BlockAnalysisTest, S3toS3IdenticalChunks) {
    // Create identical data in two S3 buckets
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_b->PutObject("bucket-b", "key-b", data);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_EQ(result.blocks_different, 0);
    EXPECT_EQ(result.bytes_different, 0);
}

TEST_F(MockS3BlockAnalysisTest, S3toS3DifferentChunks) {
    // Create different data in two S3 buckets
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'A');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'B');
    mock_a->PutObject("bucket-a", "key-a", data_a);
    mock_b->PutObject("bucket-b", "key-b", data_b);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK);
    EXPECT_EQ(result.bytes_different, CHUNK_SIZE);
    EXPECT_DOUBLE_EQ(result.percentage_different, 100.0);
}

TEST_F(MockS3BlockAnalysisTest, S3toS3PartialDifference) {
    // First block different, rest identical
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');
    std::fill(data_b.begin(), data_b.begin() + BLOCK_SIZE, 'Y');  // First block different

    mock_a->PutObject("bucket-a", "key-a", data_a);
    mock_b->PutObject("bucket-b", "key-b", data_b);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, BLOCK_SIZE);
    EXPECT_FALSE(result.block_matches[0]);
    for (size_t i = 1; i < result.block_matches.size(); ++i) {
        EXPECT_TRUE(result.block_matches[i]);
    }
}

TEST_F(MockS3BlockAnalysisTest, LocalToS3BlockAnalysis) {
    // Local file vs S3 object
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'X');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'X');
    data_b[CHUNK_SIZE / 2] = 'Z';  // Single byte different in middle

    std::string local_path = create_temp_file(data_a, "local.bin");
    mock_b->PutObject("bucket-b", "key-b", data_b);

    FileSource source_a{SourceType::Local, local_path, "", ""};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, nullptr, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, 1);
    EXPECT_EQ(result.first_diff_offset, CHUNK_SIZE / 2);
    EXPECT_EQ(result.last_diff_offset, CHUNK_SIZE / 2);
}

TEST_F(MockS3BlockAnalysisTest, S3ToLocalBlockAnalysis) {
    // S3 object vs local file
    std::vector<uint8_t> data_a(CHUNK_SIZE, 'A');
    std::vector<uint8_t> data_b(CHUNK_SIZE, 'B');

    mock_a->PutObject("bucket-a", "key-a", data_a);
    std::string local_path = create_temp_file(data_b, "local.bin");

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::Local, local_path, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, nullptr);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK);
    EXPECT_EQ(result.bytes_different, CHUNK_SIZE);
}

TEST_F(MockS3BlockAnalysisTest, S3ObjectNotFound) {
    // Source A exists, Source B doesn't
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    mock_a->PutObject("bucket-a", "key-a", data);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "nonexistent-key", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(MockS3BlockAnalysisTest, S3MultiChunkAnalyzeMiddle) {
    // Multi-chunk file, analyze the middle chunk
    int64_t file_size = CHUNK_SIZE * 3;
    std::vector<uint8_t> data_a(file_size, 'X');
    std::vector<uint8_t> data_b(file_size, 'X');

    // Make the second chunk different
    std::fill(data_b.begin() + CHUNK_SIZE, data_b.begin() + CHUNK_SIZE * 2, 'Y');

    mock_a->PutObject("bucket-a", "key-a", data_a);
    mock_b->PutObject("bucket-b", "key-b", data_b);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    // Analyze chunk 1 (second chunk, 0-indexed)
    auto result = analyze_mismatched_chunk(source_a, source_b, 1, file_size, mock_a, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_index, 1);
    EXPECT_EQ(result.chunk_size, CHUNK_SIZE);
    EXPECT_EQ(result.blocks_different, BLOCKS_PER_CHUNK);  // All blocks in this chunk differ
}

TEST_F(MockS3BlockAnalysisTest, S3PartialLastChunk) {
    // File with partial last chunk
    int64_t file_size = CHUNK_SIZE + 5000;  // 1 full chunk + 5000 bytes
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Modify one byte in the partial chunk
    data_b[CHUNK_SIZE + 100] = 'Z';

    mock_a->PutObject("bucket-a", "key-a", data_a);
    mock_b->PutObject("bucket-b", "key-b", data_b);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    // Analyze chunk 1 (partial chunk)
    auto result = analyze_mismatched_chunk(source_a, source_b, 1, file_size, mock_a, mock_b);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_index, 1);
    EXPECT_EQ(result.chunk_size, 5000);  // Partial chunk
    EXPECT_EQ(result.bytes_different, 1);
    EXPECT_EQ(result.first_diff_offset, 100);
}

TEST_F(MockS3BlockAnalysisTest, S3WithProgressCallback) {
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_b->PutObject("bucket-b", "key-b", data);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    std::vector<double> progress_values;
    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b,
        [&progress_values](double pct) { progress_values.push_back(pct); });

    EXPECT_TRUE(result.computed);
    EXPECT_FALSE(progress_values.empty());
    EXPECT_NEAR(progress_values.back(), 100.0, 0.1);
}

TEST_F(MockS3BlockAnalysisTest, S3ChunkIndexOutOfBounds) {
    // Test S3 block analysis with chunk index beyond file size
    std::vector<uint8_t> data(1000, 'A');  // Small file
    mock_a->PutObject("bucket-a", "small-a", data);
    mock_b->PutObject("bucket-b", "small-b", data);

    FileSource source_a{SourceType::S3, "small-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "small-b", "bucket-b", "us-east-1"};

    // Try to analyze chunk 5 of a 1000 byte file
    auto result = analyze_mismatched_chunk(source_a, source_b, 5, 1000, mock_a, mock_b);

    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(MockS3BlockAnalysisTest, S3NegativeChunkIndex) {
    // Test S3 block analysis with negative chunk index
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_b->PutObject("bucket-b", "key-b", data);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, -1, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_FALSE(result.computed);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(MockS3BlockAnalysisTest, S3SameClientReusedForSameRegion) {
    // Test that when both sources are in the same region and no clients provided,
    // the function creates clients correctly
    std::vector<uint8_t> data(CHUNK_SIZE, 'X');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_a->PutObject("bucket-a", "key-b", data);  // Same mock, simulating same region

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-a", "us-east-1"};  // Same region

    // Pass the same mock for both to simulate same-region behavior
    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_a);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.blocks_different, 0);  // Same data
}

TEST_F(MockS3BlockAnalysisTest, S3ErrorOnSourceAFetch) {
    // Test error handling when fetching from source A fails
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_b->PutObject("bucket-b", "key-b", data);

    // Inject failure on source A
    mock_a->SetFailure("bucket-a", "key-a", S3MockMethod::GetObjectRange);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_FALSE(result.computed);
    EXPECT_TRUE(result.error_message.find("source A") != std::string::npos);
}

TEST_F(MockS3BlockAnalysisTest, S3ErrorOnSourceBFetch) {
    // Test error handling when fetching from source B fails
    std::vector<uint8_t> data(CHUNK_SIZE, 'A');
    mock_a->PutObject("bucket-a", "key-a", data);
    mock_b->PutObject("bucket-b", "key-b", data);

    // Inject failure on source B
    mock_b->SetFailure("bucket-b", "key-b", S3MockMethod::GetObjectRange);

    FileSource source_a{SourceType::S3, "key-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "key-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE, mock_a, mock_b);

    EXPECT_FALSE(result.computed);
    EXPECT_TRUE(result.error_message.find("source B") != std::string::npos);
}

// ============================================================================
// Retry Behavior Tests
// Tests for transient failure handling and retry logic in GetObjectSize.
// These tests verify behavior that mirrors S3ClientImpl::GetObjectSize.
// ============================================================================

class MockS3RetryTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock;

    void SetUp() override {
        mock = std::make_shared<MockS3Client>();
        mock->CreateBucket("test-bucket");
    }
};

TEST_F(MockS3RetryTest, TransientFailureSucceedsAfterRetries) {
    // Object exists with known size
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Set up to fail 3 times, then succeed (retryable)
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 3, true);

    // Mock doesn't retry internally - caller must retry
    // First 3 calls should fail
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);

    // 4th call should succeed (transient failures exhausted)
    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, 1024);

    // Should have made 4 attempts (3 failures + 1 success)
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 4);
}

TEST_F(MockS3RetryTest, TransientFailureExhaustsRetries) {
    // Object exists
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Set up to fail 10 times
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 10, true);

    // Mock doesn't retry internally - each call decrements the failure counter
    // If caller only retries 5 times (6 total attempts), they'll still see failures
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);
    }

    // Should have made 6 attempts, all failed (4 more failures remaining)
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 6);
}

TEST_F(MockS3RetryTest, NonRetryableFailureFailsImmediately) {
    // Object exists
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Set up non-retryable failure (like ACCESS_DENIED)
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 3, false);

    // Should fail immediately without retrying
    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, -1);

    // Should have made only 1 attempt
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 1);
}

TEST_F(MockS3RetryTest, PermanentFailureDoesNotRetry) {
    // Object exists
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Set permanent failure
    mock->SetFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize);

    // Should fail immediately
    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, -1);

    // Only 1 call - no retries for permanent failures
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 1);
}

TEST_F(MockS3RetryTest, ZeroTransientFailuresSucceedsFirstTry) {
    std::vector<uint8_t> data(512, 'A');
    mock->PutObject("test-bucket", "test-key", data);

    // Zero failures = immediate success
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 0, true);

    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, 512);

    // Should succeed on first try
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 1);
}

TEST_F(MockS3RetryTest, TransientFailureAtMaxRetryBoundary) {
    std::vector<uint8_t> data(2048, 'B');
    mock->PutObject("test-bucket", "test-key", data);

    // Fail exactly 5 times - should succeed on the 6th attempt
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 5, true);

    // Mock doesn't retry internally - caller must retry
    // First 5 calls should fail
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);
    }

    // 6th call should succeed
    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, 2048);

    // 5 failures + 1 success = 6 attempts
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 6);
}

TEST_F(MockS3RetryTest, TransientFailureJustOverMaxRetries) {
    std::vector<uint8_t> data(2048, 'C');
    mock->PutObject("test-bucket", "test-key", data);

    // Fail 6 times
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 6, true);

    // Mock doesn't retry internally - if caller only makes 6 attempts, all fail
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), -1);
    }

    // 6 attempts, all failed
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 6);

    // 7th call would succeed since transient failures exhausted
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "test-key"), 2048);
}

TEST_F(MockS3RetryTest, ClearFailureResetsState) {
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Set transient failure
    mock->SetTransientFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize, 10, true);

    // Clear it
    mock->ClearFailure("test-bucket", "test-key", S3MockMethod::GetObjectSize);

    // Should succeed immediately
    int64_t size = mock->GetObjectSize("test-bucket", "test-key");
    EXPECT_EQ(size, 1024);
    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 1);
}

TEST_F(MockS3RetryTest, ClearAllFailuresResetsEverything) {
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "key1", data);
    mock->PutObject("test-bucket", "key2", data);

    mock->SetTransientFailure("test-bucket", "key1", S3MockMethod::GetObjectSize, 10, true);
    mock->SetFailure("test-bucket", "key2", S3MockMethod::GetObjectSize);

    // Both should fail
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key1"), -1);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key2"), -1);

    // Clear all
    mock->ClearAllFailures();

    // Both should now succeed
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key1"), 1024);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key2"), 1024);
}

TEST_F(MockS3RetryTest, CallCountTracksAllAttempts) {
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "test-key", data);

    // Call multiple times without failures
    mock->GetObjectSize("test-bucket", "test-key");
    mock->GetObjectSize("test-bucket", "test-key");
    mock->GetObjectSize("test-bucket", "test-key");

    EXPECT_EQ(mock->GetCallCount("test-bucket", "test-key", S3MockMethod::GetObjectSize), 3);
}

TEST_F(MockS3RetryTest, DifferentKeysHaveIndependentFailures) {
    std::vector<uint8_t> data(1024, 'X');
    mock->PutObject("test-bucket", "key1", data);
    mock->PutObject("test-bucket", "key2", data);

    // Only key1 has transient failures (2 failures, then success)
    mock->SetTransientFailure("test-bucket", "key1", S3MockMethod::GetObjectSize, 2, true);

    // Mock doesn't retry internally - caller must retry
    // key1 first 2 calls fail
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key1"), -1);
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key1"), -1);
    // key1 3rd call succeeds
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key1"), 1024);
    EXPECT_EQ(mock->GetCallCount("test-bucket", "key1", S3MockMethod::GetObjectSize), 3);

    // key2 should succeed immediately (no failures configured)
    EXPECT_EQ(mock->GetObjectSize("test-bucket", "key2"), 1024);
    EXPECT_EQ(mock->GetCallCount("test-bucket", "key2", S3MockMethod::GetObjectSize), 1);
}

// ============================================================================
// Custom Chunk Size Tests
// Tests for configurable chunk_size parameter in GetChunkCRC32s.
// ============================================================================

TEST_F(MockS3ClientTest, GetChunkCRC32sCustomChunkSize1MiB) {
    mock->CreateBucket("test-bucket");

    // Use 1 MiB chunks instead of default 8 MiB
    constexpr int64_t CUSTOM_CHUNK = 1024 * 1024;  // 1 MiB

    // Create data spanning 3.5 chunks with custom size
    int64_t file_size = CUSTOM_CHUNK * 3 + CUSTOM_CHUNK / 2;
    std::vector<uint8_t> data(file_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    mock->PutObject("test-bucket", "custom-chunk-file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "custom-chunk-file", file_size,
                                      {}, nullptr, false, 64, false, CUSTOM_CHUNK);

    // Should have 4 chunks with 1 MiB chunk size
    ASSERT_EQ(crcs.size(), 4u);

    // Verify CRCs match locally computed values
    uint32_t crc0 = crc32_hw(data.data(), CUSTOM_CHUNK);
    uint32_t crc1 = crc32_hw(data.data() + CUSTOM_CHUNK, CUSTOM_CHUNK);
    uint32_t crc2 = crc32_hw(data.data() + 2 * CUSTOM_CHUNK, CUSTOM_CHUNK);
    uint32_t crc3 = crc32_hw(data.data() + 3 * CUSTOM_CHUNK, CUSTOM_CHUNK / 2);

    EXPECT_EQ(crcs[0], crc0);
    EXPECT_EQ(crcs[1], crc1);
    EXPECT_EQ(crcs[2], crc2);
    EXPECT_EQ(crcs[3], crc3);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sCustomChunkSize16MiB) {
    mock->CreateBucket("test-bucket");

    // Use 16 MiB chunks (larger than default 8 MiB)
    constexpr int64_t CUSTOM_CHUNK = 16 * 1024 * 1024;  // 16 MiB

    // Create data spanning 2.5 chunks with custom size
    int64_t file_size = CUSTOM_CHUNK * 2 + CUSTOM_CHUNK / 2;
    std::vector<uint8_t> data(file_size, 'X');
    mock->PutObject("test-bucket", "large-chunk-file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "large-chunk-file", file_size,
                                      {}, nullptr, false, 64, false, CUSTOM_CHUNK);

    // Should have 3 chunks with 16 MiB chunk size
    ASSERT_EQ(crcs.size(), 3u);

    // All chunks have same content ('X'), verify CRCs
    uint32_t expected_full_crc = crc32_hw(data.data(), CUSTOM_CHUNK);
    EXPECT_EQ(crcs[0], expected_full_crc);
    EXPECT_EQ(crcs[1], expected_full_crc);
    // Last chunk is half size, different CRC
    uint32_t expected_last_crc = crc32_hw(data.data() + 2 * CUSTOM_CHUNK, CUSTOM_CHUNK / 2);
    EXPECT_EQ(crcs[2], expected_last_crc);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sChunkSizeLargerThanFile) {
    mock->CreateBucket("test-bucket");

    // Chunk size larger than file
    constexpr int64_t CUSTOM_CHUNK = 64 * 1024 * 1024;  // 64 MiB

    std::vector<uint8_t> data(1000, 'S');  // 1000 bytes
    mock->PutObject("test-bucket", "small-file", data);

    auto crcs = mock->GetChunkCRC32s("test-bucket", "small-file", 1000,
                                      {}, nullptr, false, 64, false, CUSTOM_CHUNK);

    // Should have exactly 1 chunk
    ASSERT_EQ(crcs.size(), 1u);

    uint32_t expected_crc = crc32_hw(data.data(), 1000);
    EXPECT_EQ(crcs[0], expected_crc);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sSpecificChunksWithCustomSize) {
    mock->CreateBucket("test-bucket");

    constexpr int64_t CUSTOM_CHUNK = 512 * 1024;  // 512 KiB

    // Create file with 10 chunks at 512 KiB each = 5 MiB
    int64_t file_size = CUSTOM_CHUNK * 10;
    std::vector<uint8_t> data(file_size);
    // Fill each chunk with different data
    for (int64_t i = 0; i < 10; ++i) {
        std::fill(data.begin() + i * CUSTOM_CHUNK,
                  data.begin() + (i + 1) * CUSTOM_CHUNK,
                  static_cast<uint8_t>(i));
    }
    mock->PutObject("test-bucket", "multi-chunk-file", data);

    // Request only chunks 2, 5, and 8
    std::vector<int64_t> chunk_ids = {2, 5, 8};
    auto crcs = mock->GetChunkCRC32s("test-bucket", "multi-chunk-file", file_size,
                                      chunk_ids, nullptr, false, 64, false, CUSTOM_CHUNK);

    ASSERT_EQ(crcs.size(), 3u);

    // Verify each returned CRC matches expected chunk
    uint32_t crc2 = crc32_hw(data.data() + 2 * CUSTOM_CHUNK, CUSTOM_CHUNK);
    uint32_t crc5 = crc32_hw(data.data() + 5 * CUSTOM_CHUNK, CUSTOM_CHUNK);
    uint32_t crc8 = crc32_hw(data.data() + 8 * CUSTOM_CHUNK, CUSTOM_CHUNK);

    EXPECT_EQ(crcs[0], crc2);
    EXPECT_EQ(crcs[1], crc5);
    EXPECT_EQ(crcs[2], crc8);
}

TEST_F(MockS3ClientTest, GetChunkCRC32sZeroChunkSizeUsesDefault) {
    mock->CreateBucket("test-bucket");

    // Zero or negative chunk_size should use default (8 MiB)
    int64_t file_size = CHUNK_SIZE + 1000;  // Just over 1 default chunk
    std::vector<uint8_t> data(file_size, 'D');
    mock->PutObject("test-bucket", "default-chunk-file", data);

    // Pass 0 for chunk_size - should use default
    auto crcs = mock->GetChunkCRC32s("test-bucket", "default-chunk-file", file_size,
                                      {}, nullptr, false, 64, false, 0);

    // Should have 2 chunks with default 8 MiB size
    ASSERT_EQ(crcs.size(), 2u);
}

// ============================================================================
// Custom Chunk Size Integration Tests (run_comparison)
// Tests for configurable chunk_size in end-to-end comparison.
// ============================================================================

TEST_F(MockS3ComparisonTest, ComparisonWithCustomChunkSize) {
    constexpr int64_t CUSTOM_CHUNK = 1024 * 1024;  // 1 MiB

    // Create identical data in both sources
    int64_t file_size = CUSTOM_CHUNK * 4 + 500;  // 4.5 chunks at 1 MiB
    std::vector<uint8_t> data(file_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 3) % 256);
    }

    mock->PutObject("test-bucket", "custom-a", data);
    mock->PutObject("test-bucket", "custom-b", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "custom-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "custom-b";

    config.chunk_size = CUSTOM_CHUNK;  // Use custom chunk size

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    // Should have 5 chunks with 1 MiB size
    EXPECT_EQ(result.source_a_crcs.size(), 5u);
    EXPECT_EQ(result.source_b_crcs.size(), 5u);
}

TEST_F(MockS3ComparisonTest, ComparisonWithCustomChunkSizeMismatch) {
    constexpr int64_t CUSTOM_CHUNK = 512 * 1024;  // 512 KiB

    int64_t file_size = CUSTOM_CHUNK * 6;  // Exactly 6 chunks
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Make chunks 2 and 4 different
    std::fill(data_b.begin() + 2 * CUSTOM_CHUNK,
              data_b.begin() + 3 * CUSTOM_CHUNK, 'B');
    std::fill(data_b.begin() + 4 * CUSTOM_CHUNK,
              data_b.begin() + 5 * CUSTOM_CHUNK, 'B');

    mock->PutObject("test-bucket", "mismatch-a", data_a);
    mock->PutObject("test-bucket", "mismatch-b", data_b);

    ComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "mismatch-a";

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "mismatch-b";

    config.chunk_size = CUSTOM_CHUNK;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, mock, mock);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 6u);
    ASSERT_EQ(result.mismatched_chunks.size(), 2u);
    EXPECT_EQ(result.mismatched_chunks[0], 2u);
    EXPECT_EQ(result.mismatched_chunks[1], 4u);
}

TEST_F(MockS3ComparisonTest, LocalToS3WithCustomChunkSize) {
    constexpr int64_t CUSTOM_CHUNK = 2 * 1024 * 1024;  // 2 MiB

    int64_t file_size = CUSTOM_CHUNK * 3;
    std::vector<uint8_t> data(file_size, 'M');

    std::string local_path = create_temp_file(data);
    mock->PutObject("test-bucket", "s3-custom", data);

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = local_path;

    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "s3-custom";

    config.chunk_size = CUSTOM_CHUNK;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 3u);
}

// ============================================================================
// Custom Block Size Tests (analyze_mismatched_chunk)
// Tests for configurable block_size parameter in block analysis.
// ============================================================================

TEST_F(MockS3BlockAnalysisTest, BlockAnalysisWithCustomBlockSize) {
    constexpr int64_t CUSTOM_CHUNK = 1024 * 1024;  // 1 MiB chunks
    constexpr int64_t CUSTOM_BLOCK = 4 * 1024;     // 4 KiB blocks

    std::vector<uint8_t> data_a(CUSTOM_CHUNK, 'X');
    std::vector<uint8_t> data_b(CUSTOM_CHUNK, 'X');

    // Make first block different
    std::fill(data_b.begin(), data_b.begin() + CUSTOM_BLOCK, 'Y');

    mock_a->PutObject("bucket-a", "block-test-a", data_a);
    mock_b->PutObject("bucket-b", "block-test-b", data_b);

    FileSource source_a{SourceType::S3, "block-test-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "block-test-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CUSTOM_CHUNK,
                                           mock_a, mock_b, nullptr,
                                           CUSTOM_CHUNK, CUSTOM_BLOCK);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_size, CUSTOM_CHUNK);
    EXPECT_EQ(result.block_size, CUSTOM_BLOCK);

    // 1 MiB / 4 KiB = 256 blocks
    int64_t expected_blocks = CUSTOM_CHUNK / CUSTOM_BLOCK;
    EXPECT_EQ(result.total_blocks, expected_blocks);
    EXPECT_EQ(static_cast<int64_t>(result.block_matches.size()), expected_blocks);

    // Only first block should be different
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, CUSTOM_BLOCK);
    EXPECT_FALSE(result.block_matches[0]);
    for (size_t i = 1; i < result.block_matches.size(); ++i) {
        EXPECT_TRUE(result.block_matches[i]);
    }
}

TEST_F(MockS3BlockAnalysisTest, BlockAnalysisWithLargeBlockSize) {
    constexpr int64_t CUSTOM_CHUNK = 1024 * 1024;  // 1 MiB
    constexpr int64_t CUSTOM_BLOCK = 256 * 1024;   // 256 KiB blocks

    std::vector<uint8_t> data_a(CUSTOM_CHUNK, 'A');
    std::vector<uint8_t> data_b(CUSTOM_CHUNK, 'A');

    // Make blocks 1 and 3 different (of 4 total)
    std::fill(data_b.begin() + CUSTOM_BLOCK,
              data_b.begin() + 2 * CUSTOM_BLOCK, 'B');
    std::fill(data_b.begin() + 3 * CUSTOM_BLOCK,
              data_b.begin() + 4 * CUSTOM_BLOCK, 'B');

    mock_a->PutObject("bucket-a", "large-block-a", data_a);
    mock_b->PutObject("bucket-b", "large-block-b", data_b);

    FileSource source_a{SourceType::S3, "large-block-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "large-block-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CUSTOM_CHUNK,
                                           mock_a, mock_b, nullptr,
                                           CUSTOM_CHUNK, CUSTOM_BLOCK);

    EXPECT_TRUE(result.computed);
    // 1 MiB / 256 KiB = 4 blocks
    EXPECT_EQ(result.total_blocks, 4);
    EXPECT_EQ(result.blocks_different, 2);
    EXPECT_EQ(result.bytes_different, 2 * CUSTOM_BLOCK);

    EXPECT_TRUE(result.block_matches[0]);   // Block 0: match
    EXPECT_FALSE(result.block_matches[1]);  // Block 1: different
    EXPECT_TRUE(result.block_matches[2]);   // Block 2: match
    EXPECT_FALSE(result.block_matches[3]);  // Block 3: different
}

TEST_F(MockS3BlockAnalysisTest, BlockAnalysisWithSmallBlockSize) {
    constexpr int64_t CUSTOM_CHUNK = 64 * 1024;   // 64 KiB (smaller chunk for test speed)
    constexpr int64_t CUSTOM_BLOCK = 1024;        // 1 KiB blocks

    std::vector<uint8_t> data_a(CUSTOM_CHUNK, 'Z');
    std::vector<uint8_t> data_b(CUSTOM_CHUNK, 'Z');

    // Single byte difference in block 10
    data_b[10 * CUSTOM_BLOCK + 500] = 'X';

    mock_a->PutObject("bucket-a", "small-block-a", data_a);
    mock_b->PutObject("bucket-b", "small-block-b", data_b);

    FileSource source_a{SourceType::S3, "small-block-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "small-block-b", "bucket-b", "us-east-1"};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CUSTOM_CHUNK,
                                           mock_a, mock_b, nullptr,
                                           CUSTOM_CHUNK, CUSTOM_BLOCK);

    EXPECT_TRUE(result.computed);
    // 64 KiB / 1 KiB = 64 blocks
    EXPECT_EQ(result.total_blocks, 64);
    EXPECT_EQ(result.blocks_different, 1);
    EXPECT_EQ(result.bytes_different, 1);
    EXPECT_EQ(result.first_diff_offset, 10 * CUSTOM_BLOCK + 500);
    EXPECT_EQ(result.last_diff_offset, 10 * CUSTOM_BLOCK + 500);

    // Only block 10 should be different
    for (int i = 0; i < 64; ++i) {
        if (i == 10) {
            EXPECT_FALSE(result.block_matches[i]);
        } else {
            EXPECT_TRUE(result.block_matches[i]);
        }
    }
}

TEST_F(MockS3BlockAnalysisTest, BlockAnalysisPartialLastChunkCustomSizes) {
    constexpr int64_t CUSTOM_CHUNK = 256 * 1024;  // 256 KiB
    constexpr int64_t CUSTOM_BLOCK = 32 * 1024;   // 32 KiB blocks

    // File is 1.5 chunks
    int64_t file_size = CUSTOM_CHUNK + CUSTOM_CHUNK / 2;
    std::vector<uint8_t> data_a(file_size, 'P');
    std::vector<uint8_t> data_b(file_size, 'P');

    // Make the last block of chunk 1 different
    int64_t last_block_start = CUSTOM_CHUNK + CUSTOM_CHUNK / 2 - CUSTOM_BLOCK;
    data_b[last_block_start + 100] = 'Q';

    mock_a->PutObject("bucket-a", "partial-a", data_a);
    mock_b->PutObject("bucket-b", "partial-b", data_b);

    FileSource source_a{SourceType::S3, "partial-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "partial-b", "bucket-b", "us-east-1"};

    // Analyze chunk 1 (the partial chunk)
    auto result = analyze_mismatched_chunk(source_a, source_b, 1, file_size,
                                           mock_a, mock_b, nullptr,
                                           CUSTOM_CHUNK, CUSTOM_BLOCK);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.chunk_index, 1);
    // Partial chunk is 128 KiB (half of 256 KiB)
    EXPECT_EQ(result.chunk_size, CUSTOM_CHUNK / 2);
    EXPECT_EQ(result.block_size, CUSTOM_BLOCK);

    // 128 KiB / 32 KiB = 4 blocks in partial chunk
    EXPECT_EQ(result.total_blocks, 4);
    EXPECT_EQ(result.blocks_different, 1);
}

TEST_F(MockS3BlockAnalysisTest, BlockAnalysisDefaultsWhenZeroPassed) {
    // When chunk_size or block_size is 0, should use defaults
    std::vector<uint8_t> data(CHUNK_SIZE, 'D');
    mock_a->PutObject("bucket-a", "defaults-a", data);
    mock_b->PutObject("bucket-b", "defaults-b", data);

    FileSource source_a{SourceType::S3, "defaults-a", "bucket-a", "us-east-1"};
    FileSource source_b{SourceType::S3, "defaults-b", "bucket-b", "us-east-1"};

    // Pass 0 for both - should use defaults
    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CHUNK_SIZE,
                                           mock_a, mock_b, nullptr, 0, 0);

    EXPECT_TRUE(result.computed);
    // Default chunk size is 8 MiB
    EXPECT_EQ(result.chunk_size, CHUNK_SIZE);
    // Default block size is 64 KiB
    EXPECT_EQ(result.block_size, BLOCK_SIZE);
    EXPECT_EQ(result.total_blocks, BLOCKS_PER_CHUNK);
}

// ============================================================================
// Local File Custom Chunk Size Tests
// Tests for configurable chunk_size with local files.
// ============================================================================

TEST_F(MockS3ComparisonTest, LocalFilesWithCustomChunkSize) {
    constexpr int64_t CUSTOM_CHUNK = 512 * 1024;  // 512 KiB

    int64_t file_size = CUSTOM_CHUNK * 5;
    std::vector<uint8_t> data(file_size, 'L');

    std::string path_a = create_temp_file(data, "local_a.bin");
    std::string path_b = create_temp_file(data, "local_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;

    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    config.chunk_size = CUSTOM_CHUNK;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 5u);
}

TEST_F(MockS3ComparisonTest, LocalFilesWithCustomChunkSizeMismatch) {
    constexpr int64_t CUSTOM_CHUNK = 256 * 1024;  // 256 KiB

    int64_t file_size = CUSTOM_CHUNK * 4;
    std::vector<uint8_t> data_a(file_size, 'A');
    std::vector<uint8_t> data_b(file_size, 'A');

    // Modify chunk 1 in data_b
    std::fill(data_b.begin() + CUSTOM_CHUNK,
              data_b.begin() + 2 * CUSTOM_CHUNK, 'B');

    std::string path_a = create_temp_file(data_a, "local_mismatch_a.bin");
    std::string path_b = create_temp_file(data_b, "local_mismatch_b.bin");

    ComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = path_a;

    config.source_b.type = SourceType::Local;
    config.source_b.path = path_b;

    config.chunk_size = CUSTOM_CHUNK;

    ComparisonProgress progress;
    auto result = run_comparison(config, progress, nullptr, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.all_match);
    EXPECT_EQ(result.source_a_crcs.size(), 4u);
    ASSERT_EQ(result.mismatched_chunks.size(), 1u);
    EXPECT_EQ(result.mismatched_chunks[0], 1u);
}

TEST_F(MockS3BlockAnalysisTest, LocalBlockAnalysisWithCustomSizes) {
    constexpr int64_t CUSTOM_CHUNK = 128 * 1024;  // 128 KiB
    constexpr int64_t CUSTOM_BLOCK = 8 * 1024;    // 8 KiB

    std::vector<uint8_t> data_a(CUSTOM_CHUNK, 'X');
    std::vector<uint8_t> data_b(CUSTOM_CHUNK, 'X');

    // Make blocks 2, 5, 10 different
    std::fill(data_b.begin() + 2 * CUSTOM_BLOCK,
              data_b.begin() + 3 * CUSTOM_BLOCK, 'Y');
    std::fill(data_b.begin() + 5 * CUSTOM_BLOCK,
              data_b.begin() + 6 * CUSTOM_BLOCK, 'Y');
    std::fill(data_b.begin() + 10 * CUSTOM_BLOCK,
              data_b.begin() + 11 * CUSTOM_BLOCK, 'Y');

    std::string path_a = temp_dir + "/block_local_a.bin";
    std::string path_b = temp_dir + "/block_local_b.bin";

    std::ofstream(path_a, std::ios::binary).write(
        reinterpret_cast<const char*>(data_a.data()), data_a.size());
    std::ofstream(path_b, std::ios::binary).write(
        reinterpret_cast<const char*>(data_b.data()), data_b.size());

    FileSource source_a{SourceType::Local, path_a, "", ""};
    FileSource source_b{SourceType::Local, path_b, "", ""};

    auto result = analyze_mismatched_chunk(source_a, source_b, 0, CUSTOM_CHUNK,
                                           nullptr, nullptr, nullptr,
                                           CUSTOM_CHUNK, CUSTOM_BLOCK);

    EXPECT_TRUE(result.computed);
    EXPECT_EQ(result.block_size, CUSTOM_BLOCK);
    // 128 KiB / 8 KiB = 16 blocks
    EXPECT_EQ(result.total_blocks, 16);
    EXPECT_EQ(result.blocks_different, 3);
    EXPECT_EQ(result.bytes_different, 3 * CUSTOM_BLOCK);

    // Verify specific blocks
    EXPECT_FALSE(result.block_matches[2]);
    EXPECT_FALSE(result.block_matches[5]);
    EXPECT_FALSE(result.block_matches[10]);
}

// The mock and the real client have to agree about what counts as a legal
// request. S3ClientImpl::PutObjectFromFile refuses a file over the single-PUT
// limit before it reads a byte; a mock that accepted one would let a sync-level
// test pass on a path that cannot work against real S3 (issue #92).
TEST(MockS3ClientUpload, PutObjectFromFileRefusesAFileTooLargeForASinglePut) {
    MockS3Client mock;
    mock.CreateBucket("test-bucket");

    const std::string path = mito_test_temp_path("mito_mock_put_huge").string();
    {
        std::ofstream o(path, std::ios::binary);
        ASSERT_TRUE(o.good());
    }
    // Sparse, so the size is declared without the bytes ever existing on disk.
    if (::truncate(path.c_str(), static_cast<off_t>(kMaxSinglePutBytes) + 1) != 0) {
        std::remove(path.c_str());
        GTEST_SKIP() << "filesystem does not support sparse files this large";
    }

    EXPECT_FALSE(mock.PutObjectFromFile("test-bucket", "huge", path));
    std::remove(path.c_str());

    EXPECT_EQ(mock.GetObjectSize("test-bucket", "huge"), -1)
        << "a rejected upload must not leave an object behind";
}
