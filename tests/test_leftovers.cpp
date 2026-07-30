#include <gtest/gtest.h>
#include "leftovers_task.h"
#include "s3_mock.h"
#include "duration_parse.h"

class LeftoversTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_client = std::make_shared<MockS3Client>();
        mock_client->CreateBucket("test-bucket");
    }

    std::shared_ptr<MockS3Client> mock_client;
};

TEST_F(LeftoversTest, EmptyBucketReturnsNoUploads) {
    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 0u);
    EXPECT_EQ(result.uploads_aborted, 0u);
}

TEST_F(LeftoversTest, ListsActiveUploads) {
    // Create some multipart uploads
    std::string upload1 = mock_client->CreateMultipartUpload("test-bucket", "file1.bin");
    std::string upload2 = mock_client->CreateMultipartUpload("test-bucket", "file2.bin");
    ASSERT_FALSE(upload1.empty());
    ASSERT_FALSE(upload2.empty());

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 2u);
    EXPECT_EQ(progress.uploads_found.load(), 2u);
}

TEST_F(LeftoversTest, PrefixFilterWorks) {
    mock_client->CreateMultipartUpload("test-bucket", "backups/file1.bin");
    mock_client->CreateMultipartUpload("test-bucket", "backups/file2.bin");
    mock_client->CreateMultipartUpload("test-bucket", "data/file3.bin");

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.prefix = "backups/";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 2u);
}

// An endpoint that accepts a prefix and ignores it.
//
// MinIO answers a prefix-filtered ListMultipartUploads with nothing unless the
// prefix is the entire key, so passing the prefix through reported a clean
// bucket while an upload sat in it. MockS3Client filters correctly, which is
// why the suite could not see that: a mock doing the right thing hides a
// caller that depends on the service doing it (issue #104).
class PrefixIgnoringClient : public MockS3Client {
public:
    mutable std::string last_prefix_received;

    S3ListMultipartUploadsResult ListMultipartUploads(
        const std::string& bucket, const std::string& prefix = "",
        const std::string& key_marker = "", const std::string& upload_id_marker = "",
        int max_uploads = 1000) override {
        last_prefix_received = prefix;
        // Everything, whatever was asked for.
        return MockS3Client::ListMultipartUploads(bucket, "", key_marker,
                                                  upload_id_marker, max_uploads);
    }
};

TEST_F(LeftoversTest, PrefixIsAppliedEvenWhenTheServiceIgnoresIt) {
    auto client = std::make_shared<PrefixIgnoringClient>();
    client->CreateBucket("test-bucket");
    client->CreateMultipartUpload("test-bucket", "backups/file1.bin");
    client->CreateMultipartUpload("test-bucket", "backups/file2.bin");
    client->CreateMultipartUpload("test-bucket", "data/file3.bin");

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.prefix = "backups/";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 2u)
        << "the prefix must be honoured by mito, not left to the endpoint";
}

TEST_F(LeftoversTest, AbortWithAPrefixLeavesOtherPrefixesAlone) {
    // The consequence of getting the filtering wrong in the other direction:
    // listing bucket-wide and forgetting to filter would abort an unrelated
    // upload that the user never named.
    auto client = std::make_shared<PrefixIgnoringClient>();
    client->CreateBucket("test-bucket");
    client->CreateMultipartUpload("test-bucket", "backups/keep-me.bin");
    const std::string other = client->CreateMultipartUpload("test-bucket", "data/keep-me.bin");

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.prefix = "backups/";
    config.abort_uploads = true;

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_aborted, 1u);
    EXPECT_EQ(result.abort_failures, 0u);

    // The unrelated upload is still live.
    auto remaining = client->ListMultipartUploads("test-bucket", "", "", "", 1000);
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.uploads.size(), 1u)
        << "an upload outside the prefix was aborted";
    EXPECT_EQ(remaining.uploads[0].key, "data/keep-me.bin");
    EXPECT_EQ(remaining.uploads[0].upload_id, other);
}

TEST_F(LeftoversTest, OlderThanFilterWorks) {
    auto now = std::chrono::system_clock::now();
    auto two_days_ago = now - std::chrono::hours(48);
    auto one_hour_ago = now - std::chrono::hours(1);

    std::string old_upload = mock_client->CreateMultipartUpload("test-bucket", "old.bin");
    std::string new_upload = mock_client->CreateMultipartUpload("test-bucket", "new.bin");

    mock_client->SetUploadInitiatedTime(old_upload, two_days_ago);
    mock_client->SetUploadInitiatedTime(new_upload, one_hour_ago);

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.older_than = std::chrono::hours(24);  // 1 day

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 1u);  // Only the old one
}

TEST_F(LeftoversTest, AbortRemovesUploads) {
    std::string upload1 = mock_client->CreateMultipartUpload("test-bucket", "file1.bin");
    std::string upload2 = mock_client->CreateMultipartUpload("test-bucket", "file2.bin");

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.abort_uploads = true;

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 2u);
    EXPECT_EQ(result.uploads_aborted, 2u);
    EXPECT_EQ(progress.uploads_aborted.load(), 2u);

    // Verify uploads are actually gone
    auto list_result = mock_client->ListMultipartUploads("test-bucket");
    EXPECT_EQ(list_result.uploads.size(), 0u);
}

TEST(DurationParseTest, ParsesHours) {
    auto result = parse_duration("1h");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().count(), 3600);
}

TEST(DurationParseTest, ParsesDays) {
    auto result = parse_duration("7d");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().count(), 7 * 86400);
}

TEST(DurationParseTest, ParsesMinutes) {
    auto result = parse_duration("30m");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().count(), 1800);
}

TEST(DurationParseTest, ParsesSeconds) {
    auto result = parse_duration("30s");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value().count(), 30);
}

TEST(DurationParseTest, InvalidFormatReturnsEmpty) {
    EXPECT_FALSE(parse_duration("abc").has_value());
    EXPECT_FALSE(parse_duration("").has_value());
    EXPECT_FALSE(parse_duration("5x").has_value());
}

TEST(DurationParseTest, OverflowReturnsEmpty) {
    // Very large number that would overflow
    EXPECT_FALSE(parse_duration("99999999999999999999d").has_value());
    // Large but valid-looking number that overflows on multiplication
    EXPECT_FALSE(parse_duration("999999999999999999d").has_value());
}

TEST_F(LeftoversTest, PaginationReturnsAllUploads) {
    // Create 5 uploads
    for (int i = 0; i < 5; ++i) {
        mock_client->CreateMultipartUpload("test-bucket", "file" + std::to_string(i) + ".bin");
    }

    // Test pagination with small page size (2)
    std::vector<S3MultipartUploadInfo> all_uploads;
    std::string key_marker;
    std::string upload_id_marker;

    int pages = 0;
    do {
        auto page = mock_client->ListMultipartUploads(
            "test-bucket", "", key_marker, upload_id_marker, 2);

        EXPECT_TRUE(page.success);
        all_uploads.insert(all_uploads.end(), page.uploads.begin(), page.uploads.end());

        key_marker = page.next_key_marker;
        upload_id_marker = page.next_upload_id_marker;
        pages++;

        if (!page.is_truncated) break;
    } while (pages < 10);  // Safety limit

    EXPECT_EQ(all_uploads.size(), 5u);
    EXPECT_EQ(pages, 3);  // 2 + 2 + 1 = 5 uploads in 3 pages
}

TEST_F(LeftoversTest, CancellationStopsProcessing) {
    // Create several uploads
    for (int i = 0; i < 5; ++i) {
        mock_client->CreateMultipartUpload("test-bucket", "file" + std::to_string(i) + ".bin");
    }

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    LeftoversProgress progress;
    progress.cancelled.store(true);  // Cancel before starting

    auto result = run_leftovers(config, progress, mock_client);

    // Should stop early due to cancellation
    EXPECT_EQ(result.error_message, "Cancelled");
    EXPECT_EQ(result.uploads_listed, 0u);
}

TEST_F(LeftoversTest, ApiErrorReturnsFailure) {
    LeftoversConfig config;
    config.bucket = "nonexistent-bucket";  // Bucket doesn't exist
    config.region = "us-east-1";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.error_message.find("Bucket not found") != std::string::npos);
}

TEST_F(LeftoversTest, CombinedPrefixAndOlderThanFilters) {
    auto now = std::chrono::system_clock::now();
    auto two_days_ago = now - std::chrono::hours(48);
    auto one_hour_ago = now - std::chrono::hours(1);

    // Create uploads with different prefixes and ages
    std::string old_backup = mock_client->CreateMultipartUpload("test-bucket", "backups/old.bin");
    std::string new_backup = mock_client->CreateMultipartUpload("test-bucket", "backups/new.bin");
    std::string old_data = mock_client->CreateMultipartUpload("test-bucket", "data/old.bin");
    std::string new_data = mock_client->CreateMultipartUpload("test-bucket", "data/new.bin");

    mock_client->SetUploadInitiatedTime(old_backup, two_days_ago);
    mock_client->SetUploadInitiatedTime(new_backup, one_hour_ago);
    mock_client->SetUploadInitiatedTime(old_data, two_days_ago);
    mock_client->SetUploadInitiatedTime(new_data, one_hour_ago);

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";
    config.prefix = "backups/";
    config.older_than = std::chrono::hours(24);  // 1 day

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 1u);  // Only old_backup matches both filters
}

TEST_F(LeftoversTest, EndToEndPaginationViaRunLeftovers) {
    // Create many uploads to test that run_leftovers handles pagination correctly
    // Note: run_leftovers uses max_uploads=1000, so we test with a reasonable number
    for (int i = 0; i < 10; ++i) {
        mock_client->CreateMultipartUpload("test-bucket", "file" + std::to_string(i) + ".bin");
    }

    LeftoversConfig config;
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.uploads_listed, 10u);
    EXPECT_EQ(progress.uploads_found.load(), 10u);
}

TEST(DurationParseTest, TrailingCharactersAfterTheUnitAreRejected) {
    // The unit must be the final character: "7dd" and "1hx" are not durations.
    EXPECT_FALSE(parse_duration("7dd").has_value());
    EXPECT_FALSE(parse_duration("1hx").has_value());
    EXPECT_FALSE(parse_duration("30mm").has_value());
}
