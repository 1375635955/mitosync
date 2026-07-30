#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include "byte_provider.h"
#include "temp_test_path.h"
#include <fstream>
#include <filesystem>

class LocalByteProviderTest : public ::testing::Test {
protected:
    std::string temp_file;

    void SetUp() override {
        temp_file = mito_test_temp_path("test_byte_provider").string() + ".bin";
        std::ofstream f(temp_file, std::ios::binary);
        // Write 256 bytes: 0x00, 0x01, ..., 0xFF
        for (int i = 0; i < 256; ++i) {
            char c = static_cast<char>(i);
            f.write(&c, 1);
        }
    }

    void TearDown() override {
        std::filesystem::remove(temp_file);
    }
};

TEST_F(LocalByteProviderTest, Size) {
    auto provider = CreateLocalByteProvider(temp_file);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->size(), 256);
}

TEST_F(LocalByteProviderTest, ReadFullFile) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    EXPECT_TRUE(provider->read(0, 256, buffer));
    EXPECT_EQ(buffer.size(), 256u);
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(buffer[i], static_cast<uint8_t>(i));
    }
}

TEST_F(LocalByteProviderTest, ReadPartial) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    EXPECT_TRUE(provider->read(16, 32, buffer));
    EXPECT_EQ(buffer.size(), 32u);
    EXPECT_EQ(buffer[0], 16);
    EXPECT_EQ(buffer[31], 47);
}

TEST_F(LocalByteProviderTest, ReadBeyondEnd) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Request beyond file end - should return only available bytes
    EXPECT_TRUE(provider->read(250, 20, buffer));
    EXPECT_EQ(buffer.size(), 6u);  // Only 6 bytes available
}

TEST_F(LocalByteProviderTest, IsLocal) {
    auto provider = CreateLocalByteProvider(temp_file);
    EXPECT_TRUE(provider->is_local());
}

TEST_F(LocalByteProviderTest, NonExistentFile) {
    auto provider = CreateLocalByteProvider("/nonexistent/path/file.bin");
    EXPECT_EQ(provider, nullptr);
}

TEST_F(LocalByteProviderTest, ReadZeroLength) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Zero-length read should succeed with empty buffer
    EXPECT_TRUE(provider->read(100, 0, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(LocalByteProviderTest, ReadAtEOF) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Read at exactly EOF should succeed with empty buffer
    EXPECT_TRUE(provider->read(256, 10, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(LocalByteProviderTest, ReadBeyondEOF) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Read beyond EOF should fail
    EXPECT_FALSE(provider->read(300, 10, buffer));
}

TEST_F(LocalByteProviderTest, ReadNegativeOffset) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Negative offset should fail
    EXPECT_FALSE(provider->read(-10, 10, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(LocalByteProviderTest, ReadNegativeLength) {
    auto provider = CreateLocalByteProvider(temp_file);
    std::vector<uint8_t> buffer;
    // Negative length should succeed with empty buffer (treated as 0)
    EXPECT_TRUE(provider->read(0, -5, buffer));
    EXPECT_TRUE(buffer.empty());
}

#include "s3_mock.h"

class S3ByteProviderTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_client;

    void SetUp() override {
        mock_client = std::make_shared<MockS3Client>();
        mock_client->CreateBucket("test-bucket");

        // Create test object with 256 bytes
        std::vector<uint8_t> data(256);
        for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
        mock_client->PutObject("test-bucket", "test.bin", data);
    }
};

TEST_F(S3ByteProviderTest, Size) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->size(), 256);
}

TEST_F(S3ByteProviderTest, ReadFullFile) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    EXPECT_TRUE(provider->read(0, 256, buffer));
    EXPECT_EQ(buffer.size(), 256u);
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(buffer[i], static_cast<uint8_t>(i));
    }
}

TEST_F(S3ByteProviderTest, ReadPartial) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    EXPECT_TRUE(provider->read(100, 50, buffer));
    EXPECT_EQ(buffer.size(), 50u);
    EXPECT_EQ(buffer[0], 100);
}

TEST_F(S3ByteProviderTest, IsNotLocal) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    EXPECT_FALSE(provider->is_local());
}

TEST_F(S3ByteProviderTest, NonExistentKey) {
    auto provider = CreateS3ByteProvider("test-bucket", "nonexistent.bin", mock_client);
    EXPECT_EQ(provider, nullptr);
}

TEST_F(S3ByteProviderTest, ReadZeroLength) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    // Zero-length read should succeed with empty buffer
    EXPECT_TRUE(provider->read(100, 0, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(S3ByteProviderTest, ReadAtEOF) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    // Read at exactly EOF should succeed with empty buffer
    EXPECT_TRUE(provider->read(256, 10, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(S3ByteProviderTest, ReadBeyondEOF) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    // Read beyond EOF (invalid offset) should fail
    EXPECT_FALSE(provider->read(300, 10, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(S3ByteProviderTest, ReadBeyondEnd) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    // Request beyond end - should return available bytes (clamped)
    EXPECT_TRUE(provider->read(250, 20, buffer));
    EXPECT_EQ(buffer.size(), 6u);  // Only 6 bytes available
}

TEST_F(S3ByteProviderTest, ReadNegativeOffset) {
    auto provider = CreateS3ByteProvider("test-bucket", "test.bin", mock_client);
    std::vector<uint8_t> buffer;
    // Negative offset should fail
    EXPECT_FALSE(provider->read(-10, 10, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST_F(S3ByteProviderTest, NullClient) {
    auto provider = CreateS3ByteProvider("bucket", "key", nullptr);
    EXPECT_EQ(provider, nullptr);
}

TEST_F(S3ByteProviderTest, NonExistentBucket) {
    auto provider = CreateS3ByteProvider("nonexistent-bucket", "test.bin", mock_client);
    EXPECT_EQ(provider, nullptr);
}

TEST_F(LocalByteProviderTest, OffsetPastEndOfFileClearsTheBuffer) {
    // offset > size takes the invalid-offset branch, which must empty the
    // caller's buffer rather than leaving stale bytes behind.
    auto provider = CreateLocalByteProvider(temp_file);
    ASSERT_NE(provider, nullptr);
    std::vector<uint8_t> buffer{0xAA, 0xBB, 0xCC};
    EXPECT_FALSE(provider->read(provider->size() + 1, 4, buffer));
    EXPECT_TRUE(buffer.empty());
}

// ============================================================================
// Range arithmetic (issue #20)
// ============================================================================

class ByteProviderRangeTest : public ::testing::Test {
protected:
    std::string temp_file;
    std::shared_ptr<MockS3Client> mock;

    void SetUp() override {
        temp_file = mito_test_temp_path("byte_provider_range").string() + ".bin";
        std::vector<uint8_t> data(256);
        for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
        {
            std::ofstream f(temp_file, std::ios::binary);
            f.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
        }
        mock = std::make_shared<MockS3Client>();
        mock->CreateBucket("b");
        mock->PutObject("b", "k", data);
    }

    void TearDown() override { std::filesystem::remove(temp_file); }
};

TEST_F(ByteProviderRangeTest, AHugeLengthIsClampedRatherThanOverflowed) {
    // The S3 provider computed "offset + length - 1" and clamped afterwards.
    // For a large length that addition overflows int64_t - undefined behaviour,
    // and in practice a negative end, which becomes a backwards range request.
    // Reproduced under UBSan at src/byte_provider.cpp:90.
    auto s3 = CreateS3ByteProvider("b", "k", mock);
    ASSERT_NE(s3, nullptr);

    // Not offset 0 or 1: at those two the wraps cancel - 1 + INT64_MAX wraps to
    // INT64_MIN, minus 1 wraps back to INT64_MAX - and the broken code lands on
    // the right answer by accident. It only misbehaves from offset 2 on, and
    // only where the compiler exploits the UB, so a test pinned to offset 1
    // passes against the bug at -O0.
    for (int64_t offset : {2, 128, 255}) {
        std::vector<uint8_t> buf;
        ASSERT_TRUE(s3->read(offset, std::numeric_limits<int64_t>::max(), buf))
            << "offset " << offset;
        EXPECT_EQ(buf.size(), static_cast<size_t>(256 - offset)) << "offset " << offset;
        EXPECT_EQ(buf.front(), static_cast<uint8_t>(offset)) << "offset " << offset;
        EXPECT_EQ(buf.back(), 255) << "offset " << offset;
    }
}

TEST_F(ByteProviderRangeTest, TheTwoProvidersAgreeOnEveryEdgeOfTheRange) {
    // A caller picks a provider by whether the source is local or S3 and then
    // uses it identically, so any disagreement here is a bug in whichever one
    // the caller happens not to have been tested against.
    auto local = CreateLocalByteProvider(temp_file);
    auto s3 = CreateS3ByteProvider("b", "k", mock);
    ASSERT_NE(local, nullptr);
    ASSERT_NE(s3, nullptr);
    ASSERT_EQ(local->size(), s3->size());

    struct Case { int64_t offset; int64_t length; const char* what; };
    // How many bytes a correct provider returns, given a 256-byte object.
    auto expected_bytes = [](int64_t offset, int64_t length) -> size_t {
        if (offset < 0 || offset > 256) return 0;
        if (length <= 0 || offset == 256) return 0;
        return static_cast<size_t>(std::min<int64_t>(length, 256 - offset));
    };
    const Case cases[] = {
        {0, 256, "the whole object"},
        {0, 257, "one past the end"},
        {0, std::numeric_limits<int64_t>::max(), "a length that would overflow"},
        {1, std::numeric_limits<int64_t>::max(), "offset 1, overflowing length"},
        {2, std::numeric_limits<int64_t>::max(), "offset 2, overflowing length"},
        {128, std::numeric_limits<int64_t>::max(), "mid-object, overflowing length"},
        {255, std::numeric_limits<int64_t>::max(), "last byte, overflowing length"},
        {255, 1, "the last byte"},
        {256, 1, "at EOF"},
        {256, std::numeric_limits<int64_t>::max(), "at EOF, overflowing length"},
        {0, 0, "zero length"},
        {0, -1, "negative length"},
        {128, 64, "an ordinary middle range"},
    };

    for (const auto& c : cases) {
        std::vector<uint8_t> lbuf, sbuf;
        const bool lok = local->read(c.offset, c.length, lbuf);
        const bool sok = s3->read(c.offset, c.length, sbuf);
        EXPECT_EQ(lok, sok) << c.what << " (offset " << c.offset << ")";
        EXPECT_EQ(lbuf, sbuf) << c.what << ": local returned " << lbuf.size()
                              << " bytes, S3 returned " << sbuf.size();
        EXPECT_EQ(lbuf.size(), expected_bytes(c.offset, c.length))
            << c.what << ": agreeing on the wrong answer is still wrong";
    }
}

TEST_F(ByteProviderRangeTest, AnOffsetPastTheEndFails) {
    auto local = CreateLocalByteProvider(temp_file);
    auto s3 = CreateS3ByteProvider("b", "k", mock);
    std::vector<uint8_t> buf;
    EXPECT_FALSE(local->read(257, 1, buf));
    EXPECT_FALSE(s3->read(257, 1, buf));
    EXPECT_FALSE(local->read(-1, 1, buf));
    EXPECT_FALSE(s3->read(-1, 1, buf));
}
