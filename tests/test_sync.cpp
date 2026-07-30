#include <gtest/gtest.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include "sync_task.h"
#include "s3_mock.h"
#include "constants.h"
#include "crc32_chunks.h"
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

// Set a file's mtime to a fixed Unix timestamp so timestamp comparisons in
// tests are deterministic rather than dependent on wall-clock ordering.
inline void set_mtime(const fs::path& p, int64_t unix_seconds) {
    struct ::timespec times[2];
    times[0].tv_sec = unix_seconds; times[0].tv_nsec = 0;   // atime
    times[1].tv_sec = unix_seconds; times[1].tv_nsec = 0;   // mtime
    ASSERT_EQ(::utimensat(AT_FDCWD, p.c_str(), times, 0), 0)
        << "utimensat failed for " << p << ": " << std::strerror(errno);
}

pid_t find_dead_pid() {
    const pid_t self = ::getpid();
    for (pid_t pid = 4194304; pid > 1; --pid) {
        if (pid == self) continue;
        errno = 0;
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            return pid;
        }
    }
    ADD_FAILURE() << "could not find a PID that kill(pid, 0) reports as dead";
    return -1;
}

std::string unique_test_dir_name(const char* prefix) {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(prefix) + "-" + std::to_string(::getpid());
    if (info) {
        name += "-";
        name += info->test_suite_name();
        name += "-";
        name += info->name();
    }
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            c = '_';
        }
    }
    return name;
}

class ListCreatesLocalTempClient : public MockS3Client {
public:
    explicit ListCreatesLocalTempClient(fs::path temp_path)
        : temp_path_(std::move(temp_path)) {}

    S3ListResult ListObjects(
        const std::string& bucket,
        const std::string& prefix,
        const std::string& delimiter,
        const std::string& continuation_token,
        int max_keys
    ) override {
        if (!created_.exchange(true)) {
            std::ofstream(temp_path_) << "concurrent sync temp";
        }
        return MockS3Client::ListObjects(bucket, prefix, delimiter, continuation_token, max_keys);
    }

private:
    fs::path temp_path_;
    std::atomic<bool> created_{false};
};


class SyncTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / unique_test_dir_name("mito_sync_test");
        // A previous killed run may have left a mode-000 directory here. Make
        // everything traversable again before removing it, otherwise the
        // throwing remove_all below fails and poisons every later test.
        restore_permissions();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        restore_permissions();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    // Non-throwing walk: increment through the error_code overload so an
    // unreadable directory cannot throw out of SetUp/TearDown.
    void restore_permissions() {
        std::error_code ec;
        if (!fs::exists(temp_dir_, ec)) return;
        fs::permissions(temp_dir_, fs::perms::owner_all, ec);
        for (fs::recursive_directory_iterator it(
                 temp_dir_, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); break; }
            std::error_code sec;
            if (it->is_directory(sec)) fs::permissions(it->path(), fs::perms::owner_all, sec);
        }
    }

    fs::path temp_dir_;
};

TEST_F(SyncTaskTest, EmptyDirectoryReturnsSuccess) {
    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 0);
    EXPECT_EQ(result.files_skipped, 0);
}

TEST_F(SyncTaskTest, ClassifiesNewFilesAsUpload) {
    // Create a small local file
    fs::path test_file = temp_dir_ / "small.txt";
    std::ofstream(test_file) << "hello world";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.dry_run = true;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    ASSERT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].relative_path, "small.txt");
    EXPECT_EQ(result.files[0].action, SyncAction::Upload);
}

TEST_F(SyncTaskTest, ClassifiesLargeFilesAsDiffUpload) {
    // Create a large local file (>8MB)
    fs::path test_file = temp_dir_ / "large.bin";
    {
        std::ofstream out(test_file, std::ios::binary);
        std::vector<char> data(9 * 1024 * 1024, 'x');  // 9 MB
        out.write(data.data(), data.size());
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.dry_run = true;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_diff_uploaded, 1);
    ASSERT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].action, SyncAction::UploadDiff);
}

TEST_F(SyncTaskTest, ExecutesSmallFileUpload) {
    // Create a small local file
    fs::path test_file = temp_dir_ / "upload.txt";
    std::ofstream(test_file) << "test content";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;  // Actually execute

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify file was uploaded to mock
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/upload.txt"));
}

TEST_F(SyncTaskTest, ReapsOrphanedSyncTempBeforeUploadEnumeration) {
    std::ofstream(temp_dir_ / "real.txt") << "real data";

    pid_t dead_pid = find_dead_pid();
    ASSERT_GT(dead_pid, 0);
    fs::path orphan = temp_dir_ / (".upload.txt-" + std::to_string(dead_pid) +
                                  "-0" + kTempSuffix);
    std::ofstream(orphan) << "partial data from killed sync";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/real.txt"));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/" + orphan.filename().string()));
}

TEST_F(SyncTaskTest, DryRunDoesNotReapOrphanedSyncTemp) {
    std::ofstream(temp_dir_ / "real.txt") << "real data";

    pid_t dead_pid = find_dead_pid();
    ASSERT_GT(dead_pid, 0);
    fs::path orphan = temp_dir_ / (".upload.txt-" + std::to_string(dead_pid) +
                                  "-0" + kTempSuffix);
    std::ofstream(orphan) << "partial data from killed sync";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = true;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("orphaned temporary sync file present"), std::string::npos);
    EXPECT_TRUE(fs::exists(orphan));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/real.txt"));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/" + orphan.filename().string()));
}

TEST_F(SyncTaskTest, UploadRefusesSyncTempCreatedDuringEnumeration) {
    std::ofstream(temp_dir_ / "real.txt") << "real data";
    fs::path late_temp = temp_dir_ / (".upload.txt-" + std::to_string(::getpid()) +
                                     "-0" + kTempSuffix);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    auto mock_client = std::make_shared<ListCreatesLocalTempClient>(late_temp);
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("temporary sync file"), std::string::npos);
    EXPECT_TRUE(fs::exists(late_temp));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/real.txt"));
}

TEST_F(SyncTaskTest, DownloadRefusesSyncTempCreatedDuringEnumeration) {
    fs::path late_temp = temp_dir_ / (".download.txt-" + std::to_string(::getpid()) +
                                     "-0" + kTempSuffix);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();

    SyncProgress progress;
    auto mock_client = std::make_shared<ListCreatesLocalTempClient>(late_temp);
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("temporary sync file"), std::string::npos);
    EXPECT_TRUE(fs::exists(late_temp));
}

TEST_F(SyncTaskTest, ActiveSyncTempStopsUploadEnumeration) {
    std::ofstream(temp_dir_ / "real.txt") << "real data";
    fs::path active = temp_dir_ / (".upload.txt-" + std::to_string(::getpid()) +
                                  "-0" + kTempSuffix);
    std::ofstream(active) << "active write";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("temporary sync file"), std::string::npos);
    EXPECT_TRUE(fs::exists(active));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/real.txt"));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/" + active.filename().string()));
}

TEST_F(SyncTaskTest, UnrecognizedSyncTempNameStopsUploadEnumeration) {
    std::ofstream(temp_dir_ / "real.txt") << "real data";
    fs::path unknown = temp_dir_ / (std::string(".unknown") + kTempSuffix);
    std::ofstream(unknown) << "not produced by AtomicFileWriter";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("unrecognized temporary sync file"), std::string::npos);
    EXPECT_TRUE(fs::exists(unknown));
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/real.txt"));
}

TEST_F(SyncTaskTest, DeletesOrphanedS3Objects) {
    // Empty local dir, but S3 has a file
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");  // Create bucket first
    mock_client->PutObject("test-bucket", "prefix/orphan.txt",
                          std::vector<uint8_t>{'t', 'e', 's', 't'});

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;
    config.dry_run = false;

    SyncProgress progress;

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_deleted, 1);
    EXPECT_FALSE(mock_client->ObjectExists("test-bucket", "prefix/orphan.txt"));
}

TEST_F(SyncTaskTest, DiffUploadNewLargeFile) {
    // Create a large local file (>8MB) with no S3 counterpart
    // This tests that diff_upload falls back to full upload for new files
    constexpr size_t FILE_SIZE = 9 * 1024 * 1024;  // 9 MB
    fs::path test_file = temp_dir_ / "new_large.bin";
    {
        std::ofstream out(test_file, std::ios::binary);
        std::vector<char> data(FILE_SIZE, 'x');
        out.write(data.data(), data.size());
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_diff_uploaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify file was uploaded to mock
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/new_large.bin"));
    EXPECT_EQ(mock_client->GetObjectSize("test-bucket", "prefix/new_large.bin"), FILE_SIZE);
}

TEST_F(SyncTaskTest, DiffUploadWithExistingFilePartialChange) {
    // Create a large file in S3, then modify only part of it locally
    // This tests the differential upload with UploadPartCopy
    constexpr size_t CHUNK_SIZE = 8 * 1024 * 1024;  // 8 MB
    constexpr size_t S3_FILE_SIZE = CHUNK_SIZE * 2;     // 16 MB (2 chunks)
    constexpr size_t LOCAL_FILE_SIZE = CHUNK_SIZE * 3;  // 24 MB (3 chunks) - different size to trigger UploadDiff

    // Create existing S3 object - fill with 'A' for first chunk, 'B' for second
    std::vector<uint8_t> s3_data(S3_FILE_SIZE);
    std::fill(s3_data.begin(), s3_data.begin() + CHUNK_SIZE, 'A');
    std::fill(s3_data.begin() + CHUNK_SIZE, s3_data.end(), 'B');

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/large.bin", s3_data);

    // Create local file with same first chunk, different second chunk, and new third chunk
    // Size differs from S3, so it gets classified as UploadDiff
    fs::path test_file = temp_dir_ / "large.bin";
    {
        std::ofstream out(test_file, std::ios::binary);
        std::vector<char> local_data(LOCAL_FILE_SIZE);
        std::fill(local_data.begin(), local_data.begin() + CHUNK_SIZE, 'A');  // Same as S3 chunk 1
        std::fill(local_data.begin() + CHUNK_SIZE, local_data.begin() + CHUNK_SIZE * 2, 'C');  // Different from S3 chunk 2
        std::fill(local_data.begin() + CHUNK_SIZE * 2, local_data.end(), 'D');  // New chunk 3
        out.write(local_data.data(), local_data.size());
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_diff_uploaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify the uploaded file has the correct content
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/large.bin"));

    // Check that bytes_saved > 0 (chunk 1 was copied from S3, not uploaded)
    EXPECT_GT(result.bytes_saved, 0);

    // Verify only the changed chunks were transferred
    // bytes_transferred should be ~2*CHUNK_SIZE (chunks 2 and 3 uploaded)
    EXPECT_GT(result.bytes_transferred, 0);
    EXPECT_LT(result.bytes_transferred, LOCAL_FILE_SIZE);
}

TEST_F(SyncTaskTest, CancellationDuringSync) {
    // Create multiple files to sync
    for (int i = 0; i < 10; ++i) {
        fs::path test_file = temp_dir_ / ("file" + std::to_string(i) + ".txt");
        std::ofstream(test_file) << "content " << i;
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Cancel immediately
    progress.cancelled = true;

    SyncResult result = run_sync(config, progress, mock_client);

    // Should return early with cancellation message
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

TEST_F(SyncTaskTest, UploadFailureCountsAsFailed) {
    // Create a file that will fail to upload
    fs::path test_file = temp_dir_ / "fail.txt";
    std::ofstream(test_file) << "will fail";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Set permanent failure for this file
    mock_client->SetFailure("test-bucket", "prefix/fail.txt", S3MockMethod::PutObject);

    SyncResult result = run_sync(config, progress, mock_client);

    // Sync completes, but the failed file makes the run unsuccessful.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_EQ(result.files_uploaded, 0);
}

TEST_F(SyncTaskTest, TransientFailureSucceedsWithRetry) {
    // Create a file
    fs::path test_file = temp_dir_ / "retry.txt";
    std::ofstream(test_file) << "will succeed on retry";

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Set transient failure - fail 2 times then succeed
    mock_client->SetTransientFailure("test-bucket", "prefix/retry.txt",
                                     S3MockMethod::PutObject, 2, true);

    SyncResult result = run_sync(config, progress, mock_client);

    // Should succeed after retries
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_EQ(result.files_failed, 0);
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/retry.txt"));
}

TEST_F(SyncTaskTest, DownloadTransientFailureSucceedsWithRetry) {
    // S3 has a file that fails to download twice, then succeeds
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    std::vector<uint8_t> data{'h', 'e', 'l', 'l', 'o'};
    mock_client->PutObject("test-bucket", "prefix/retry.txt", data);

    // Set transient failure for GetObjectRange - fail 2 times then succeed
    mock_client->SetTransientFailure("test-bucket", "prefix/retry.txt",
                                     S3MockMethod::GetObjectRange, 2, true);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = temp_dir_.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // Should succeed after retries
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify retry happened (call count > 1)
    int call_count = mock_client->GetCallCount("test-bucket", "prefix/retry.txt",
                                                S3MockMethod::GetObjectRange);
    EXPECT_GE(call_count, 3);  // 2 failures + 1 success = at least 3 calls
}

TEST_F(SyncTaskTest, DeleteFailureCountsAsFailed) {
    // Empty local dir, but S3 has a file we can't delete
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/nodelete.txt",
                          std::vector<uint8_t>{'t', 'e', 's', 't'});

    // Set permanent failure for delete
    mock_client->SetFailure("test-bucket", "prefix/nodelete.txt", S3MockMethod::DeleteObject);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;
    config.dry_run = false;

    SyncProgress progress;

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_EQ(result.files_deleted, 0);
    // Object should still exist
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/nodelete.txt"));
}

TEST_F(SyncTaskTest, ClassifiesS3OnlyFilesAsDownload) {
    // S3 has a file, local directory is empty
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data(100, 'x');
    mock_client->PutObject("test-bucket", "prefix/remote.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);
    ASSERT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].relative_path, "remote.txt");
    EXPECT_EQ(result.files[0].action, SyncAction::Download);
}

TEST_F(SyncTaskTest, ClassifiesNewLargeS3FilesAsDownload) {
    // New files should always use Download, not DownloadDiff
    // (DownloadDiff requires existing local file to compare CRC chunks)
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Create 9MB file in S3 (no local file exists)
    std::vector<uint8_t> data(9 * 1024 * 1024, 'x');
    mock_client->PutObject("test-bucket", "prefix/large.bin", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);  // Full download, not diff
    ASSERT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].action, SyncAction::Download);
}

TEST_F(SyncTaskTest, DownloadSkipsUnchangedFiles) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};
    mock_client->PutObject("test-bucket", "prefix/same.txt", data);
    mock_client->SetObjectMtime("test-bucket", "prefix/same.txt", 1000);

    // Create matching local file, downloaded after the object was written.
    fs::path local_file = temp_dir_ / "same.txt";
    std::ofstream(local_file) << "hello";
    set_mtime(local_file, 2000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_skipped, 1);
    ASSERT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].action, SyncAction::Skip);
}

TEST_F(SyncTaskTest, ExecutesSmallFileDownload) {
    // S3 has a file, local directory is empty - actually execute download
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
    mock_client->PutObject("test-bucket", "prefix/hello.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;  // Actually execute

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);
    EXPECT_EQ(result.files_failed, 0);
    EXPECT_EQ(result.bytes_transferred, 11);

    // Verify the file was actually downloaded
    fs::path local_file = temp_dir_ / "hello.txt";
    EXPECT_TRUE(fs::exists(local_file));
    std::ifstream in(local_file, std::ios::binary);
    std::vector<uint8_t> local_data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    EXPECT_EQ(local_data, data);
}

TEST_F(SyncTaskTest, DownloadCreatesParentDirectories) {
    // S3 has a file in a nested path, local directory is empty
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("test-bucket", "prefix/subdir/nested/file.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);

    // Verify the nested file was created
    fs::path local_file = temp_dir_ / "subdir" / "nested" / "file.txt";
    EXPECT_TRUE(fs::exists(local_file));
}

TEST_F(SyncTaskTest, DiffDownloadWithPartialChange) {
    // Test differential download with a large file where only some chunks differ
    constexpr size_t CHUNK_SIZE = 8 * 1024 * 1024;  // 8 MB
    constexpr size_t S3_FILE_SIZE = CHUNK_SIZE * 3;     // 24 MB (3 chunks) - S3 is larger
    constexpr size_t LOCAL_FILE_SIZE = CHUNK_SIZE * 2;  // 16 MB (2 chunks) - local is smaller

    // Create S3 object - chunk 1: 'A', chunk 2: 'B', chunk 3: 'D'
    std::vector<uint8_t> s3_data(S3_FILE_SIZE);
    std::fill(s3_data.begin(), s3_data.begin() + CHUNK_SIZE, 'A');
    std::fill(s3_data.begin() + CHUNK_SIZE, s3_data.begin() + CHUNK_SIZE * 2, 'B');
    std::fill(s3_data.begin() + CHUNK_SIZE * 2, s3_data.end(), 'D');

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/large.bin", s3_data);

    // Create local file with same chunk 1, different chunk 2
    // Size differs from S3, so it gets classified as DownloadDiff
    fs::path local_file = temp_dir_ / "large.bin";
    {
        std::ofstream out(local_file, std::ios::binary);
        std::vector<char> local_data(LOCAL_FILE_SIZE);
        std::fill(local_data.begin(), local_data.begin() + CHUNK_SIZE, 'A');  // Same as S3 chunk 1
        std::fill(local_data.begin() + CHUNK_SIZE, local_data.end(), 'C');    // Different from S3 chunk 2
        out.write(local_data.data(), local_data.size());
    }

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_diff_downloaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify bytes_saved > 0 (chunk 1 was reused)
    EXPECT_GT(result.bytes_saved, 0);

    // Verify only the changed chunks were transferred
    EXPECT_GT(result.bytes_transferred, 0);
    EXPECT_LT(result.bytes_transferred, S3_FILE_SIZE);

    // Verify the downloaded file has the correct content
    std::ifstream in(local_file, std::ios::binary);
    std::vector<uint8_t> downloaded((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    EXPECT_EQ(downloaded, s3_data);
}

TEST_F(SyncTaskTest, DownloadNewLargeFileUsesFullDownload) {
    // Test that new large files use full download (not diff, since no local file)
    constexpr size_t FILE_SIZE = 9 * 1024 * 1024;  // 9 MB (>= threshold)

    std::vector<uint8_t> s3_data(FILE_SIZE, 'X');

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/new_large.bin", s3_data);

    // No local file - should use full download
    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);  // Full download, not diff
    EXPECT_EQ(result.bytes_transferred, FILE_SIZE);

    // Verify file was downloaded
    fs::path local_file = temp_dir_ / "new_large.bin";
    EXPECT_TRUE(fs::exists(local_file));
    EXPECT_EQ(fs::file_size(local_file), FILE_SIZE);
}

TEST_F(SyncTaskTest, DownloadDeletesOrphanedLocalFiles) {
    // Test that --delete removes local files not in S3
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> keep_data = {'k', 'e', 'e', 'p'};
    mock_client->PutObject("test-bucket", "prefix/keep.txt", keep_data);

    // Local has two files: one matches S3, one is orphan
    std::ofstream(temp_dir_ / "keep.txt") << "keep";
    std::ofstream(temp_dir_ / "orphan.txt") << "delete me";

    EXPECT_TRUE(fs::exists(temp_dir_ / "keep.txt"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "orphan.txt"));

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.delete_orphans = true;
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_deleted, 1);
    EXPECT_TRUE(fs::exists(temp_dir_ / "keep.txt"));
    EXPECT_FALSE(fs::exists(temp_dir_ / "orphan.txt"));
}

TEST_F(SyncTaskTest, DownloadsEmptyFile) {
    // Test that empty (0 byte) files download correctly
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Create empty file in S3
    std::vector<uint8_t> empty_data;
    mock_client->PutObject("test-bucket", "prefix/empty.txt", empty_data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_downloaded, 1);

    // Verify empty file was created
    fs::path local_file = temp_dir_ / "empty.txt";
    EXPECT_TRUE(fs::exists(local_file));
    EXPECT_EQ(fs::file_size(local_file), 0);
}

// ============================================================================
// Path Traversal Attack Tests
// ============================================================================

TEST_F(SyncTaskTest, RejectsParentDirectoryTraversal) {
    // Test that S3 keys with ".." are rejected to prevent path traversal attacks
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    // Malicious S3 key attempting to escape to /etc/passwd
    std::vector<uint8_t> malicious_data = {'h', 'a', 'c', 'k', 'e', 'd'};
    mock_client->PutObject("test-bucket", "prefix/../../../etc/passwd", malicious_data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // The file should be rejected and make the run unsuccessful.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_downloaded, 0);
    EXPECT_EQ(result.files_failed, 1);

    // Verify no file was created outside temp_dir
    EXPECT_FALSE(fs::exists("/etc/passwd_test_marker"));
    // And no file inside temp_dir either (the path is rejected entirely)
    EXPECT_FALSE(fs::exists(temp_dir_ / "passwd"));
}

TEST_F(SyncTaskTest, RejectsAbsolutePathInS3Key) {
    // Test that S3 keys starting with "/" are rejected
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("test-bucket", "prefix//tmp/evil.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // File with empty component (consecutive slashes) should be rejected.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_downloaded, 0);
    EXPECT_EQ(result.files_failed, 1);
}

TEST_F(SyncTaskTest, RejectsCurrentDirectoryReference) {
    // Test that S3 keys with "." components are rejected
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("test-bucket", "prefix/./hidden.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // File with "." component should be rejected.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_downloaded, 0);
    EXPECT_EQ(result.files_failed, 1);
}

TEST_F(SyncTaskTest, RejectsBackslashTraversal) {
    // Test that Windows-style path traversal is also rejected
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("test-bucket", "prefix/..\\..\\etc\\passwd", data);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // Backslash traversal should be rejected.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_downloaded, 0);
    EXPECT_EQ(result.files_failed, 1);
}

// ============================================================================
// Permission and Disk Error Tests
// ============================================================================

TEST_F(SyncTaskTest, DownloadFailsOnReadOnlyDirectory) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    // Test that download fails gracefully when directory is not writable
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("test-bucket", "prefix/file.txt", data);

    // Create a read-only subdirectory
    fs::path readonly_dir = temp_dir_ / "readonly";
    fs::create_directories(readonly_dir);
    fs::permissions(readonly_dir, fs::perms::owner_read | fs::perms::owner_exec);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.local_path = readonly_dir.string();
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // Restore permissions for cleanup
    fs::permissions(readonly_dir, fs::perms::owner_all);

    // Sync should complete, but the failed file makes the run unsuccessful.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_EQ(result.files_downloaded, 0);
}

TEST_F(SyncTaskTest, UploadFailsOnUnreadableFile) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    // Test that upload fails gracefully when file is not readable
    fs::path test_file = temp_dir_ / "unreadable.txt";
    std::ofstream(test_file) << "secret content";

    // Make file unreadable
    fs::permissions(test_file, fs::perms::none);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    // Restore permissions for cleanup if the file is still present.
    std::error_code ec;
    fs::permissions(test_file, fs::perms::owner_all, ec);

    // Sync should complete, but the failed file makes the run unsuccessful.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_EQ(result.files_uploaded, 0);
}

// ============================================================================
// S3-to-S3 Sync Tests
// ============================================================================

TEST_F(SyncTaskTest, S3ToS3CopiesNewFiles) {
    // Source bucket has files, dest bucket is empty
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};
    mock_client->PutObject("source-bucket", "prefix/file.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify file was copied
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/file.txt"));
}

TEST_F(SyncTaskTest, S3ToS3SkipsIdenticalFiles) {
    // Both buckets have the same file (same size)
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};
    mock_client->PutObject("source-bucket", "prefix/same.txt", data);
    mock_client->PutObject("dest-bucket", "prefix/same.txt", data);
    // The destination copy was made after the source object was written.
    mock_client->SetObjectMtime("source-bucket", "prefix/same.txt", 1000);
    mock_client->SetObjectMtime("dest-bucket", "prefix/same.txt", 2000);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_skipped, 1);
    EXPECT_EQ(result.files_copied, 0);
}

TEST_F(SyncTaskTest, S3ToS3CopiesWhenSizeDiffers) {
    // Same key but different sizes
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> source_data = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    std::vector<uint8_t> dest_data = {'h', 'i'};
    mock_client->PutObject("source-bucket", "prefix/file.txt", source_data);
    mock_client->PutObject("dest-bucket", "prefix/file.txt", dest_data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);

    // Verify the dest file now has source content
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/file.txt"), 11);
}

TEST_F(SyncTaskTest, S3ToS3DeletesOrphans) {
    // Source is empty, dest has orphan file
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'o', 'r', 'p', 'h', 'a', 'n'};
    mock_client->PutObject("dest-bucket", "prefix/orphan.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_deleted, 1);
    EXPECT_FALSE(mock_client->ObjectExists("dest-bucket", "prefix/orphan.txt"));
}

TEST_F(SyncTaskTest, S3ToS3DryRun) {
    // Dry run should not actually copy
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("source-bucket", "prefix/file.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);  // Counted as would-copy

    // Verify file was NOT actually copied
    EXPECT_FALSE(mock_client->ObjectExists("dest-bucket", "prefix/file.txt"));
}

TEST_F(SyncTaskTest, S3ToS3RetryOnError) {
    // Copy fails twice, then succeeds
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("source-bucket", "prefix/retry.txt", data);

    // Set transient failure for CopyObject
    mock_client->SetTransientFailure("dest-bucket", "prefix/retry.txt",
                                     S3MockMethod::CopyObject, 2, true);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/retry.txt"));
}

TEST_F(SyncTaskTest, S3ToS3Cancellation) {
    // Cancel during S3-to-S3 sync
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>('a' + i));
        mock_client->PutObject("source-bucket", "prefix/file" + std::to_string(i) + ".txt", data);
    }

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    progress.cancelled = true;  // Cancel immediately

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

TEST_F(SyncTaskTest, S3ToS3CrossRegionCopy) {
    // Test that cross-region copies work (different source/dest regions)
    // Note: MockS3Client doesn't truly simulate regions, but this validates
    // that different region strings are handled correctly by the code
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'c', 'r', 'o', 's', 's', '-', 'r', 'e', 'g', 'i', 'o', 'n'};
    mock_client->PutObject("source-bucket", "prefix/cross-region.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";  // Source in Virginia
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "eu-west-1";  // Dest in Ireland
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/cross-region.txt"));

    // Verify bytes_copied_server_side is tracked correctly
    EXPECT_EQ(result.bytes_copied_server_side, 12);  // Length of "cross-region"
}

TEST_F(SyncTaskTest, S3ToS3LargeFileClassification) {
    // Test that large files (>5GB) are classified correctly
    // Note: AWS SDK CopyObject handles multipart copy automatically for >5GB objects
    // This test validates classification logic; actual AWS SDK behavior is SDK's responsibility
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    // We can't actually test 5GB files in a unit test, but we verify the classification
    // logic works for any size file. The design doc notes: "The AWS SDK's CopyObject
    // automatically uses multipart copy for objects exceeding the 5GB single-operation limit."
    // This test ensures our code correctly identifies files needing copy.
    std::vector<uint8_t> large_data(1024 * 1024, 'X');  // 1 MiB file (scaled down)
    mock_client->PutObject("source-bucket", "prefix/large-file.bin", large_data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify large file was copied correctly
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/large-file.bin"), 1024 * 1024);
    EXPECT_EQ(result.bytes_copied_server_side, 1024 * 1024);
}

TEST_F(SyncTaskTest, S3ToS3PermanentCopyFailure) {
    // Test that permanent copy failures are counted correctly
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data = {'f', 'a', 'i', 'l'};
    mock_client->PutObject("source-bucket", "prefix/will-fail.txt", data);

    // Set permanent failure for the copy destination
    mock_client->SetFailure("dest-bucket", "prefix/will-fail.txt", S3MockMethod::CopyObject);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_copied, 0);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_FALSE(mock_client->ObjectExists("dest-bucket", "prefix/will-fail.txt"));
}

TEST_F(SyncTaskTest, S3ToS3EmptyFileCopy) {
    // Test that empty files are copied correctly
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    // Create empty file
    std::vector<uint8_t> empty_data;
    mock_client->PutObject("source-bucket", "prefix/empty.txt", empty_data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify empty file exists in dest
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/empty.txt"));
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/empty.txt"), 0);

    // bytes_copied_server_side should be 0 for empty file
    EXPECT_EQ(result.bytes_copied_server_side, 0);
}

TEST_F(SyncTaskTest, S3ToS3SameBucketDifferentPrefix) {
    // Test copying within the same bucket but different prefixes
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("my-bucket");

    std::vector<uint8_t> data = {'s', 'a', 'm', 'e', '-', 'b', 'u', 'c', 'k', 'e', 't'};
    mock_client->PutObject("my-bucket", "source/file.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "my-bucket";
    config.source.path = "source";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "my-bucket";
    config.destination.path = "dest";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 1);
    EXPECT_EQ(result.files_failed, 0);

    // Verify file exists in both locations
    EXPECT_TRUE(mock_client->ObjectExists("my-bucket", "source/file.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("my-bucket", "dest/file.txt"));
}

TEST_F(SyncTaskTest, S3ToS3DifferentPrefixDepths) {
    // Test copying when source and dest prefixes have different depths
    // e.g., source: "foo/" -> dest: "foo/bar/baz/"
    // This ensures path construction correctly handles prefix depth differences
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");

    // Source has shallow prefix with nested files
    std::vector<uint8_t> data1 = {'f', 'i', 'l', 'e', '1'};
    std::vector<uint8_t> data2 = {'f', 'i', 'l', 'e', '2'};
    mock_client->PutObject("src-bucket", "foo/file1.txt", data1);
    mock_client->PutObject("src-bucket", "foo/subdir/file2.txt", data2);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "src-bucket";
    config.source.path = "foo";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dst-bucket";
    config.destination.path = "foo/bar/baz";  // Deeper prefix
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 2);
    EXPECT_EQ(result.files_failed, 0);

    // Verify files are at correct destination paths
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "foo/bar/baz/file1.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "foo/bar/baz/subdir/file2.txt"));

    // Verify source is unchanged
    EXPECT_TRUE(mock_client->ObjectExists("src-bucket", "foo/file1.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("src-bucket", "foo/subdir/file2.txt"));
}

TEST_F(SyncTaskTest, S3ToS3SpecialCharactersInKeys) {
    // Test copying files with special characters in keys (spaces, unicode, etc.)
    // S3 CopySource header requires URL encoding
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("src-bucket", "prefix/file with spaces.txt", data);
    mock_client->PutObject("src-bucket", "prefix/special!@#$%chars.txt", data);
    mock_client->PutObject("src-bucket", "prefix/path/to/deep file.txt", data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "src-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dst-bucket";
    config.destination.path = "dest";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 3);
    EXPECT_EQ(result.files_failed, 0);

    // Verify files with special characters are copied correctly
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "dest/file with spaces.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "dest/special!@#$%chars.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "dest/path/to/deep file.txt"));
}

TEST_F(SyncTaskTest, MockMultipartCopyFlow) {
    // Test that the mock's multipart copy methods work correctly.
    // This exercises the same flow that S3ClientImpl::CopyObjectMultipart uses for files >5GB.
    // The actual S3ClientImpl::CopyObjectMultipart is tested via integration tests against
    // LocalStack/MinIO since we can't store 5GB+ files in unit tests.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    // Create source object with 3 parts worth of data (simulating a large file)
    constexpr int64_t PART_SIZE = 8 * 1024 * 1024;  // 8 MiB
    std::vector<uint8_t> part1_data(PART_SIZE, 'A');
    std::vector<uint8_t> part2_data(PART_SIZE, 'B');
    std::vector<uint8_t> part3_data(PART_SIZE / 2, 'C');  // Last part smaller

    std::vector<uint8_t> full_data;
    full_data.insert(full_data.end(), part1_data.begin(), part1_data.end());
    full_data.insert(full_data.end(), part2_data.begin(), part2_data.end());
    full_data.insert(full_data.end(), part3_data.begin(), part3_data.end());
    mock_client->PutObject("source-bucket", "large-file.bin", full_data);

    // Step 1: Create multipart upload
    std::string upload_id = mock_client->CreateMultipartUpload("dest-bucket", "large-file.bin");
    ASSERT_FALSE(upload_id.empty()) << "CreateMultipartUpload should return upload ID";

    // Step 2: Copy each part using UploadPartCopy
    std::vector<std::pair<int, S3PartResult>> completed_parts;
    int64_t source_size = full_data.size();
    int64_t num_parts = (source_size + PART_SIZE - 1) / PART_SIZE;
    EXPECT_EQ(num_parts, 3);

    for (int64_t i = 0; i < num_parts; ++i) {
        int part_number = static_cast<int>(i + 1);
        int64_t start_byte = i * PART_SIZE;
        int64_t end_byte = std::min(start_byte + PART_SIZE - 1, source_size - 1);

        S3PartResult part = mock_client->UploadPartCopy(
            "dest-bucket", "large-file.bin", upload_id, part_number,
            "source-bucket", "large-file.bin", start_byte, end_byte
        );
        ASSERT_TRUE(part.ok()) << "UploadPartCopy should return ETag for part " << part_number;
        EXPECT_FALSE(part.checksum_crc32.empty())
            << "a copied part must report its checksum, or completion cannot name it";
        completed_parts.emplace_back(part_number, part);
    }

    // Step 3: Complete multipart upload
    bool complete_success = mock_client->CompleteMultipartUpload(
        "dest-bucket", "large-file.bin", upload_id, completed_parts
    );
    EXPECT_TRUE(complete_success) << "CompleteMultipartUpload should succeed";

    // Verify the copied object matches the source
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "large-file.bin"));
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "large-file.bin"),
              static_cast<int64_t>(full_data.size()));
}

TEST_F(SyncTaskTest, MockMultipartCopyAbortOnFailure) {
    // Test that multipart copy can be aborted, which is what CopyObjectMultipart does on error.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    std::vector<uint8_t> data(1024, 'X');
    mock_client->PutObject("source-bucket", "file.bin", data);

    // Start multipart upload
    std::string upload_id = mock_client->CreateMultipartUpload("dest-bucket", "file.bin");
    ASSERT_FALSE(upload_id.empty());

    // Copy one part
    S3PartResult part = mock_client->UploadPartCopy(
        "dest-bucket", "file.bin", upload_id, 1,
        "source-bucket", "file.bin", 0, 1023
    );
    EXPECT_TRUE(part.ok());

    // Abort instead of completing (simulating error path)
    bool abort_success = mock_client->AbortMultipartUpload("dest-bucket", "file.bin", upload_id);
    EXPECT_TRUE(abort_success) << "AbortMultipartUpload should succeed";

    // Object should NOT exist since we aborted
    EXPECT_FALSE(mock_client->ObjectExists("dest-bucket", "file.bin"));
}

TEST_F(SyncTaskTest, CopyObjectRespectsCancel) {
    // Test that CopyObject respects the cancellation flag
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");

    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("src-bucket", "file.txt", data);

    // Create a cancelled flag that's already set
    std::atomic<bool> cancelled{true};

    // CopyObject should return false immediately due to cancellation
    bool result = mock_client->CopyObject(
        "src-bucket", "file.txt",
        "dst-bucket", "file.txt",
        -1,  // source_size (unknown)
        &cancelled
    );

    EXPECT_FALSE(result) << "CopyObject should return false when cancelled";
    EXPECT_FALSE(mock_client->ObjectExists("dst-bucket", "file.txt"))
        << "Object should not be copied when cancelled";
}

TEST_F(SyncTaskTest, CopyObjectWithKnownSizeZero) {
    // Test that CopyObject works correctly with source_size = 0 (empty file)
    // This validates the edge case where >= 0 means "skip HeadObject"
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");

    // Create empty file
    std::vector<uint8_t> empty_data;
    mock_client->PutObject("src-bucket", "empty.txt", empty_data);

    // CopyObject with source_size = 0 should work
    bool result = mock_client->CopyObject(
        "src-bucket", "empty.txt",
        "dst-bucket", "empty.txt",
        0,  // source_size = 0 (known empty file)
        nullptr
    );

    EXPECT_TRUE(result) << "CopyObject should succeed with source_size=0";
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "empty.txt"));
    EXPECT_EQ(mock_client->GetObjectSize("dst-bucket", "empty.txt"), 0);
}

TEST_F(SyncTaskTest, CopyObjectWithKnownSizePositive) {
    // Test that CopyObject works correctly with known positive source_size
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");

    std::vector<uint8_t> data(1024, 'X');
    mock_client->PutObject("src-bucket", "file.bin", data);

    // CopyObject with known size should work
    bool result = mock_client->CopyObject(
        "src-bucket", "file.bin",
        "dst-bucket", "file.bin",
        1024,  // source_size = 1024 (known)
        nullptr
    );

    EXPECT_TRUE(result) << "CopyObject should succeed with known source_size";
    EXPECT_TRUE(mock_client->ObjectExists("dst-bucket", "file.bin"));
    EXPECT_EQ(mock_client->GetObjectSize("dst-bucket", "file.bin"), 1024);
}

TEST_F(SyncTaskTest, S3ToS3PassesKnownSizeToCopyObject) {
    // Test that S3-to-S3 sync passes the known file size to CopyObject
    // This is an integration test ensuring the optimization is wired correctly
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("source-bucket");
    mock_client->CreateBucket("dest-bucket");

    // Create files of various sizes including 0-byte
    std::vector<uint8_t> empty_data;
    std::vector<uint8_t> small_data(100, 'S');
    std::vector<uint8_t> medium_data(10000, 'M');

    mock_client->PutObject("source-bucket", "prefix/empty.txt", empty_data);
    mock_client->PutObject("source-bucket", "prefix/small.txt", small_data);
    mock_client->PutObject("source-bucket", "prefix/medium.bin", medium_data);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "prefix";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_copied, 3);
    EXPECT_EQ(result.files_failed, 0);

    // Verify all files were copied correctly including empty file
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/empty.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/small.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("dest-bucket", "prefix/medium.bin"));

    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/empty.txt"), 0);
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/small.txt"), 100);
    EXPECT_EQ(mock_client->GetObjectSize("dest-bucket", "prefix/medium.bin"), 10000);

    // bytes_copied_server_side should include all bytes
    EXPECT_EQ(result.bytes_copied_server_side, 0 + 100 + 10000);
}

// ============================================================================
// LocalToLocal Sync Tests
// ============================================================================

class LocalToLocalSyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp directories for source and destination
        base_dir_ = fs::temp_directory_path() / unique_test_dir_name("mito_local_sync_test");
        source_dir_ = base_dir_ / "source";
        dest_dir_ = base_dir_ / "dest";
        std::error_code ec;
        fs::remove_all(base_dir_, ec);   // a killed run under a since-reused pid
        fs::create_directories(source_dir_);
        fs::create_directories(dest_dir_);
    }

    void TearDown() override {
        fs::remove_all(base_dir_);
    }

    fs::path base_dir_;
    fs::path source_dir_;
    fs::path dest_dir_;
};

TEST_F(LocalToLocalSyncTest, EmptyDirectoryReturnsSuccess) {
    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 0);
    EXPECT_EQ(result.files_skipped, 0);
}

TEST_F(LocalToLocalSyncTest, CopiesNewFiles) {
    // Create files in source
    std::ofstream(source_dir_ / "file1.txt") << "content1";
    std::ofstream(source_dir_ / "file2.txt") << "content2";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 2);  // Upload action used for local copy
    EXPECT_EQ(result.files_skipped, 0);

    // Verify files were copied
    EXPECT_TRUE(fs::exists(dest_dir_ / "file1.txt"));
    EXPECT_TRUE(fs::exists(dest_dir_ / "file2.txt"));

    // Verify content
    std::ifstream ifs1(dest_dir_ / "file1.txt");
    std::string content1((std::istreambuf_iterator<char>(ifs1)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content1, "content1");
}

TEST_F(LocalToLocalSyncTest, SkipsIdenticalFiles) {
    // Create same file in both source and destination
    std::ofstream(source_dir_ / "same.txt") << "identical";
    std::ofstream(dest_dir_ / "same.txt") << "identical";
    // The destination copy is newer, as it would be after a real sync.
    set_mtime(source_dir_ / "same.txt", 1000);
    set_mtime(dest_dir_ / "same.txt", 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 0);
    EXPECT_EQ(result.files_skipped, 1);
}

TEST_F(LocalToLocalSyncTest, CopiesFilesWithDifferentSize) {
    // Create files with different sizes
    std::ofstream(source_dir_ / "diff.txt") << "longer content here";
    std::ofstream(dest_dir_ / "diff.txt") << "short";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);

    // Verify content was overwritten
    std::ifstream ifs(dest_dir_ / "diff.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "longer content here");
}

TEST_F(LocalToLocalSyncTest, HandlesNestedDirectories) {
    // Create nested structure
    fs::create_directories(source_dir_ / "subdir" / "deep");
    std::ofstream(source_dir_ / "subdir" / "file.txt") << "nested";
    std::ofstream(source_dir_ / "subdir" / "deep" / "file.txt") << "deeply nested";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 2);

    // Verify nested files were copied
    EXPECT_TRUE(fs::exists(dest_dir_ / "subdir" / "file.txt"));
    EXPECT_TRUE(fs::exists(dest_dir_ / "subdir" / "deep" / "file.txt"));
}

TEST_F(LocalToLocalSyncTest, DeletesOrphansWhenEnabled) {
    // Create file only in destination
    std::ofstream(dest_dir_ / "orphan.txt") << "orphan";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_deleted, 1);
    EXPECT_FALSE(fs::exists(dest_dir_ / "orphan.txt"));
}

TEST_F(LocalToLocalSyncTest, KeepsOrphansWhenDisabled) {
    // Create file only in destination
    std::ofstream(dest_dir_ / "orphan.txt") << "orphan";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    config.delete_orphans = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_deleted, 0);
    EXPECT_TRUE(fs::exists(dest_dir_ / "orphan.txt"));
}

TEST_F(LocalToLocalSyncTest, DryRunDoesNotModify) {
    // Create file in source
    std::ofstream(source_dir_ / "file.txt") << "content";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    config.dry_run = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_FALSE(fs::exists(dest_dir_ / "file.txt"));  // Should not be created
}

TEST_F(LocalToLocalSyncTest, RejectsSameSourceAndDestination) {
    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = source_dir_.string();  // Same as source!

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(LocalToLocalSyncTest, HandlesEmptyFile) {
    // Create empty file in source
    std::ofstream(source_dir_ / "empty.txt");

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_TRUE(fs::exists(dest_dir_ / "empty.txt"));
    EXPECT_EQ(fs::file_size(dest_dir_ / "empty.txt"), 0);
}

TEST_F(LocalToLocalSyncTest, TracksBytesTransferred) {
    // Create files with known sizes
    std::string content(1000, 'x');
    std::ofstream(source_dir_ / "file1.txt") << content;
    std::ofstream(source_dir_ / "file2.txt") << content;

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytes_transferred, 2000);
}

TEST_F(LocalToLocalSyncTest, CancellationStopsSync) {
    // Create multiple files to increase chance of hitting cancellation
    for (int i = 0; i < 10; ++i) {
        std::ofstream(source_dir_ / ("file" + std::to_string(i) + ".txt")) << "content " << i;
    }

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    progress.cancelled = true;  // Cancel immediately

    SyncResult result = run_sync(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(LocalToLocalSyncTest, HandlesUnreadableSourceFile) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    fs::path test_file = source_dir_ / "unreadable.txt";
    std::ofstream(test_file) << "content";
    fs::permissions(test_file, fs::perms::none);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    std::error_code ec;
    fs::permissions(test_file, fs::perms::owner_all, ec);  // Restore for cleanup

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
}

TEST_F(LocalToLocalSyncTest, HandlesUnwritableDestDirectory) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    std::ofstream(source_dir_ / "file.txt") << "content";
    fs::permissions(dest_dir_, fs::perms::owner_read | fs::perms::owner_exec);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    fs::permissions(dest_dir_, fs::perms::owner_all);  // Restore for cleanup

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
}

TEST_F(LocalToLocalSyncTest, CopiesLargeFile) {
    // Create file > 1MB to exercise chunked copy
    constexpr size_t LARGE_SIZE = 2 * 1024 * 1024;  // 2 MB
    std::vector<char> data(LARGE_SIZE, 'x');

    fs::path large_file = source_dir_ / "large.bin";
    {
        std::ofstream out(large_file, std::ios::binary);
        out.write(data.data(), data.size());
    }

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_EQ(result.bytes_transferred, LARGE_SIZE);
    EXPECT_EQ(fs::file_size(dest_dir_ / "large.bin"), LARGE_SIZE);
}

TEST_F(LocalToLocalSyncTest, HandlesNonexistentSourceDirectory) {
    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = "/nonexistent/path/that/does/not/exist";
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    // A source that cannot be read is not an empty source. Treating it as empty
    // is what made a transient failure look like "everything was deleted at the
    // source", so this now fails instead of reporting a successful no-op sync.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_uploaded, 0);
    EXPECT_NE(result.error_message.find("Source enumeration failed"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(LocalToLocalSyncTest, CreatesNestedDestDirectories) {
    // Create file in nested source directory
    fs::create_directories(source_dir_ / "a" / "b" / "c");
    std::ofstream(source_dir_ / "a" / "b" / "c" / "deep.txt") << "deeply nested content";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 1);
    EXPECT_TRUE(fs::exists(dest_dir_ / "a" / "b" / "c" / "deep.txt"));

    // Verify content
    std::ifstream ifs(dest_dir_ / "a" / "b" / "c" / "deep.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "deeply nested content");
}

TEST_F(LocalToLocalSyncTest, RejectsDestinationInsideSource) {
    // Destination inside source would cause infinite recursion
    fs::path nested_dest = source_dir_ / "backup";
    fs::create_directories(nested_dest);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = nested_dest.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("inside source") != std::string::npos);
}

TEST_F(LocalToLocalSyncTest, RejectsSourceInsideDestination) {
    // Source inside destination could cause data loss
    fs::path nested_source = dest_dir_ / "data";
    fs::create_directories(nested_source);
    std::ofstream(nested_source / "file.txt") << "content";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = nested_source.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("inside destination") != std::string::npos);
}

TEST_F(LocalToLocalSyncTest, HandlesSpecialCharactersInFilenames) {
    // Create files with spaces and special characters
    std::ofstream(source_dir_ / "file with spaces.txt") << "spaces";
    std::ofstream(source_dir_ / "file-with-dashes.txt") << "dashes";
    std::ofstream(source_dir_ / "file_with_underscores.txt") << "underscores";
    std::ofstream(source_dir_ / "file.multiple.dots.txt") << "dots";

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_uploaded, 4);
    EXPECT_TRUE(fs::exists(dest_dir_ / "file with spaces.txt"));
    EXPECT_TRUE(fs::exists(dest_dir_ / "file-with-dashes.txt"));
    EXPECT_TRUE(fs::exists(dest_dir_ / "file_with_underscores.txt"));
    EXPECT_TRUE(fs::exists(dest_dir_ / "file.multiple.dots.txt"));
}

TEST_F(LocalToLocalSyncTest, DeleteOrphanFailureCountsAsFailed) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    // Create file in destination that cannot be deleted
    fs::path orphan_dir = dest_dir_ / "protected";
    fs::create_directories(orphan_dir);
    std::ofstream(orphan_dir / "orphan.txt") << "orphan";
    // Make directory read-only to prevent file deletion
    fs::permissions(orphan_dir, fs::perms::owner_read | fs::perms::owner_exec);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    // Restore permissions for cleanup
    fs::permissions(orphan_dir, fs::perms::owner_all);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_TRUE(fs::exists(orphan_dir / "orphan.txt"));  // File should still exist
}

TEST_F(LocalToLocalSyncTest, HandlesSymlinkToFileByNotFollowing) {
    // Create a real file and a symlink to it
    std::ofstream(source_dir_ / "real.txt") << "real content";
    fs::create_symlink(source_dir_ / "real.txt", source_dir_ / "link.txt");

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    // The real file should be copied
    EXPECT_TRUE(fs::exists(dest_dir_ / "real.txt"));
    // The symlink is enumerated like any other entry, and the copy opens the
    // source with O_NOFOLLOW, so it fails rather than duplicating the target
    // through the link - and that failure has to reach the caller.
    EXPECT_EQ(result.files_failed, 1);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(fs::exists(dest_dir_ / "link.txt"));
}

// ---- should_reuse_s3_client (per-side profile reuse predicate) ----
TEST(ShouldReuseS3Client, SameRegionSameProfileReuses) {
    FileSource a{SourceType::S3, "k", "bucket", "us-east-1", ""};
    FileSource b{SourceType::S3, "k2", "bucket", "us-east-1", ""};
    EXPECT_TRUE(should_reuse_s3_client(a, b));
}
TEST(ShouldReuseS3Client, SameRegionDifferentProfileDoesNotReuse) {
    FileSource a{SourceType::S3, "k", "bucket", "us-east-1", "acct-a"};
    FileSource b{SourceType::S3, "k2", "bucket", "us-east-1", "acct-b"};
    EXPECT_FALSE(should_reuse_s3_client(a, b));
}
TEST(ShouldReuseS3Client, DifferentRegionDoesNotReuse) {
    FileSource a{SourceType::S3, "k", "bucket", "us-east-1", "p"};
    FileSource b{SourceType::S3, "k2", "bucket", "eu-west-1", "p"};
    EXPECT_FALSE(should_reuse_s3_client(a, b));
}
TEST(ShouldReuseS3Client, NonS3DoesNotReuse) {
    FileSource a{SourceType::Local, "/x", "", "", ""};
    FileSource b{SourceType::Local, "/y", "", "", ""};
    EXPECT_FALSE(should_reuse_s3_client(a, b));
}

// ---- apply_sync_profiles (direction-aware CLI profile mapping) ----
TEST(ApplySyncProfiles, S3ToS3MapsBothSides) {
    FileSource src{SourceType::S3, "k", "b1", "us-east-1", ""};
    FileSource dst{SourceType::S3, "k", "b2", "us-east-1", ""};
    apply_sync_profiles(SyncDirection::S3ToS3, "acct-a", "acct-b", src, dst);
    EXPECT_EQ(src.profile, "acct-a");
    EXPECT_EQ(dst.profile, "acct-b");
}
TEST(ApplySyncProfiles, UploadUsesDestProfileOnS3Side) {
    // Upload: s3_source holds the S3 *destination*.
    FileSource s3{SourceType::S3, "k", "b", "us-east-1", ""};
    FileSource unused{};
    apply_sync_profiles(SyncDirection::Upload, "ignored", "acct-dst", s3, unused);
    EXPECT_EQ(s3.profile, "acct-dst");
}
TEST(ApplySyncProfiles, DownloadUsesSourceProfileOnS3Side) {
    FileSource s3{SourceType::S3, "k", "b", "us-east-1", ""};
    FileSource unused{};
    apply_sync_profiles(SyncDirection::Download, "acct-src", "ignored", s3, unused);
    EXPECT_EQ(s3.profile, "acct-src");
}
TEST(ApplySyncProfiles, EmptyFlagsLeaveProfileUnset) {
    FileSource s3{SourceType::S3, "k", "b", "us-east-1", ""};
    FileSource unused{};
    apply_sync_profiles(SyncDirection::Upload, "", "", s3, unused);
    EXPECT_EQ(s3.profile, "");
}

// ---- additional coverage: edge cases flagged in review ----
TEST(ApplySyncProfiles, LocalToLocalIsNoOp) {
    FileSource a{SourceType::Local, "/src", "", "", ""};
    FileSource b{SourceType::Local, "/dst", "", "", ""};
    apply_sync_profiles(SyncDirection::LocalToLocal, "acct-a", "acct-b", a, b);
    EXPECT_EQ(a.profile, "");
    EXPECT_EQ(b.profile, "");
}
TEST(ApplySyncProfiles, EmptyFlagPreservesExistingProfile) {
    // An empty flag must leave an already-set profile untouched (not clear it).
    FileSource s3{SourceType::S3, "k", "b", "us-east-1", "preset"};
    FileSource unused{};
    apply_sync_profiles(SyncDirection::Upload, "", "", s3, unused);
    EXPECT_EQ(s3.profile, "preset");
}
TEST(ApplySyncProfiles, S3ToS3OnlySourceFlagLeavesDestUntouched) {
    FileSource src{SourceType::S3, "k", "b1", "us-east-1", ""};
    FileSource dst{SourceType::S3, "k", "b2", "us-east-1", "dst-preset"};
    apply_sync_profiles(SyncDirection::S3ToS3, "acct-a", "", src, dst);
    EXPECT_EQ(src.profile, "acct-a");
    EXPECT_EQ(dst.profile, "dst-preset");  // empty dest flag leaves it as-is
}
TEST(ShouldReuseS3Client, MixedS3AndLocalDoesNotReuse) {
    FileSource a{SourceType::S3, "k", "bucket", "us-east-1", ""};
    FileSource b{SourceType::Local, "/y", "", "us-east-1", ""};
    EXPECT_FALSE(should_reuse_s3_client(a, b));
    EXPECT_FALSE(should_reuse_s3_client(b, a));
}

// ============================================================================
// Enumeration completeness (issue #34)
// ============================================================================
//
// A failed listing used to return an empty vector, indistinguishable from an
// empty prefix. With delete_orphans that made every destination object look
// orphaned, so a transient listing error could delete data that still existed
// at the source. Sync must refuse to plan from a partial inventory.

TEST_F(SyncTaskTest, UnreadableLocalSourceAbortsUploadInsteadOfDeletingEverything) {
    // The dangerous direction: an unreadable local source looks like an empty
    // tree, so every destination object appears orphaned and is deleted.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/keep-me.txt",
                           std::vector<uint8_t>{'d', 'a', 't', 'a'});

    SyncConfig config;
    config.local_path = "/nonexistent/source/path";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;
    config.dry_run = false;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success) << "a partial source inventory must not be acted on";
    EXPECT_EQ(result.files_deleted, 0) << "nothing may be deleted from a failed listing";
    EXPECT_NE(result.error_message.find("Source enumeration failed"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/keep-me.txt"))
        << "the destination object must survive an unreadable source";
}

namespace {

// Restores permissions even if an assertion fails or the test throws. Without
// this a mode-000 directory survives in the shared temp dir and poisons every
// later test, including TearDown's throwing remove_all.
class PermGuard {
public:
    PermGuard(const fs::path& p, fs::perms mode) : path_(p) {
        std::error_code ec;
        old_ = fs::status(p, ec).permissions();
        fs::permissions(p, mode, ec);
    }
    ~PermGuard() {
        std::error_code ec;
        fs::permissions(path_, old_, ec);
    }
private:
    fs::path path_;
    fs::perms old_ = fs::perms::owner_all;
};
}  // namespace

// An unreadable directory is the realistic cause of a partial local listing:
// an ACL change, a root-owned subtree, or an NFS mount returning EACCES. It is
// NOT caught by the "is this a directory" precheck, because an unreadable
// directory is still a directory.
TEST_F(SyncTaskTest, UnreadableSourceSubdirectoryAbortsInsteadOfDeleting) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/locked/secret.txt",
                           std::vector<uint8_t>{'s'});
    mock_client->PutObject("test-bucket", "prefix/visible.txt",
                           std::vector<uint8_t>{'v'});

    // Both files exist at the source; one lives under a directory we cannot read.
    { std::ofstream(temp_dir_ / "visible.txt") << "v"; }
    fs::path locked = temp_dir_ / "locked";
    fs::create_directories(locked);
    { std::ofstream(locked / "secret.txt") << "s"; }
    PermGuard guard(locked, fs::perms::none);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success) << "an unreadable subtree must not look like an empty one";
    EXPECT_EQ(result.files_deleted, 0);
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/locked/secret.txt"))
        << "the object exists at the source and must not be deleted";
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/visible.txt"));
}

TEST_F(SyncTaskTest, UnreadableSourceRootAbortsInsteadOfWipingDestination) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/a.txt", std::vector<uint8_t>{'a'});
    mock_client->PutObject("test-bucket", "prefix/b.txt", std::vector<uint8_t>{'b'});

    fs::path src = temp_dir_ / "src_root";
    fs::create_directories(src);
    { std::ofstream(src / "a.txt") << "a"; }
    { std::ofstream(src / "b.txt") << "b"; }
    PermGuard guard(src, fs::perms::none);

    SyncConfig config;
    config.local_path = src.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_deleted, 0) << "the whole destination would otherwise be wiped";
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/a.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/b.txt"));
}

TEST_F(SyncTaskTest, IncompleteDestinationListingIsNotFatal) {
    // Only the source gates the sync. A destination listing that fails can lead
    // to redundant uploads and missed orphan removal, but never to deleting data
    // that exists at the source, so it must not abort the run.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->SetFailure("test-bucket", "prefix/", S3MockMethod::ListObjects);

    {
        std::ofstream out(temp_dir_ / "a.txt", std::ios::binary);
        out << "a";
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // The run must COMPLETE - this is the assertion that distinguishes
    // "destination failure tolerated" from "aborted", which files_deleted == 0
    // cannot, since an aborted run also deletes nothing.
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_uploaded, 1) << "the source file must still be uploaded";
    EXPECT_EQ(result.files_deleted, 0);
}

TEST_F(SyncTaskTest, FailedSourceListingAbortsDownload) {
    // Download direction: the S3 source is what fails. With delete_orphans an
    // empty source list would delete the entire local destination.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->SetFailure("test-bucket", "prefix/", S3MockMethod::ListObjects);

    fs::path local_file = temp_dir_ / "existing.txt";
    {
        std::ofstream out(local_file, std::ios::binary);
        out << "precious";
    }

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = temp_dir_.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_deleted, 0);
    EXPECT_TRUE(fs::exists(local_file)) << "local data must survive a failed source listing";
}

TEST_F(SyncTaskTest, SuccessfulListingStillSyncsNormally) {
    // Guard against the fix over-firing: a clean listing must behave as before.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/orphan.txt",
                           std::vector<uint8_t>{'x'});

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_deleted, 1);
}

TEST_F(SyncTaskTest, FileSymlinkWithUnreadableTargetIsNotTreatedAsBroken) {
    // entry.status() follows the link, so it fails with EACCES just as readily
    // as with ENOENT. Treating every stat failure on a symlink as "dangling"
    // dropped a real file from the inventory and deleted its destination copy.
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/linked.txt", std::vector<uint8_t>{'l'});

    // The target must live OUTSIDE the scanned root. Inside it, the unreadable
    // directory itself trips the directory-open check and the test would pass
    // even with the stat-failure discrimination reverted.
    fs::path hidden = fs::temp_directory_path() / unique_test_dir_name("mito_sync_hidden_target");
    std::error_code rec; fs::remove_all(hidden, rec);
    fs::create_directories(hidden);
    { std::ofstream(hidden / "real.txt") << "l"; }
    fs::create_symlink(hidden / "real.txt", temp_dir_ / "linked.txt");
    PermGuard guard(hidden, fs::perms::none);   // target unreadable, link still present

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_EQ(result.files_deleted, 0) << "a file we could not stat is not an orphan";
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/linked.txt"));

    std::error_code cec;
    fs::permissions(hidden, fs::perms::owner_all, cec);
    fs::remove_all(hidden, cec);
}

TEST_F(SyncTaskTest, SecondSymlinkToTheSameDirectoryIsNotDeletedAsAPhantomOrphan) {
    // Two links to one shared directory is not a cycle. Treating it as one
    // dropped the second subtree, so its destination copies looked orphaned.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/linkA/s1.txt", std::vector<uint8_t>{'s'});
    mock_client->PutObject("test-bucket", "prefix/linkB/s1.txt", std::vector<uint8_t>{'s'});

    fs::path shared = temp_dir_ / "shared";
    fs::create_directories(shared);
    { std::ofstream(shared / "s1.txt") << "s"; }
    fs::create_directory_symlink(shared, temp_dir_ / "linkA");
    fs::create_directory_symlink(shared, temp_dir_ / "linkB");

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    // The second link's subtree is not followed (that is what terminates mutual
    // cycles), so the listing is incomplete and the sync must refuse rather than
    // treat the un-enumerated files as orphans. Either way the invariant that
    // matters is the same: nothing that exists at the source may be deleted.
    EXPECT_EQ(result.files_deleted, 0);
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/linkA/s1.txt"));
    EXPECT_TRUE(mock_client->ObjectExists("test-bucket", "prefix/linkB/s1.txt"))
        << "the second link's subtree must not be deleted as a phantom orphan";
}

TEST_F(SyncTaskTest, DanglingSymlinkDoesNotAbortTheSync) {
    // ENOENT is safe to skip: a dangling link is not a file. This must not
    // trip the new incompleteness gate.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    { std::ofstream(temp_dir_ / "real.txt") << "r"; }
    fs::create_symlink(temp_dir_ / "no_such_target", temp_dir_ / "dangling.txt");

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_uploaded, 1);
}

TEST_F(SyncTaskTest, UnresolvableSymlinksDoNotAbortTheSync) {
    // ELOOP, ENOTDIR and ENAMETOOLONG all mean the target cannot exist, so no
    // file was dropped and the sync must proceed. Flagging them made a single
    // `ln -s .. up` anywhere in the tree refuse the whole run.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");

    { std::ofstream(temp_dir_ / "real.txt") << "r"; }
    // ELOOP: two links pointing at each other
    fs::create_symlink(temp_dir_ / "loop_b", temp_dir_ / "loop_a");
    fs::create_symlink(temp_dir_ / "loop_a", temp_dir_ / "loop_b");
    // ENOTDIR: a path that walks through a regular file
    fs::create_symlink(temp_dir_ / "real.txt" / "nope", temp_dir_ / "through_file");
    // ENAMETOOLONG
    fs::create_symlink(temp_dir_ / std::string(300, 'x'), temp_dir_ / "too_long");

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_uploaded, 1) << "the one real file must still be uploaded";
}

// ============================================================================
// Same-size content changes (issue #33)
// ============================================================================
//
// sync classified equal sizes as unchanged, so an in-place edit that preserves
// length was never transferred and the run reported success over stale data.
// The predicate is now "equal size AND the destination is provably no older
// than the source".

TEST_F(SyncTaskTest, SameSizeLocalEditIsUploadedNotSkipped) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/edited.txt",
                           std::vector<uint8_t>{'A', 'A', 'A', 'A'});
    // The object was written well before the local edit.
    mock_client->SetObjectMtime("test-bucket", "prefix/edited.txt", 1000);

    // Same length, different content, edited after the upload.
    { std::ofstream(temp_dir_ / "edited.txt") << "BBBB"; }
    set_mtime(temp_dir_ / "edited.txt", 2000);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_skipped, 0) << "an in-place edit must not be skipped";
    EXPECT_EQ(result.files_uploaded, 1);

    auto stored = mock_client->GetObjectRange("test-bucket", "prefix/edited.txt", 0, 3);
    ASSERT_EQ(stored.size(), 4u);
    EXPECT_EQ(std::string(stored.begin(), stored.end()), "BBBB")
        << "the destination must now hold the edited content";
}

TEST_F(SyncTaskTest, SameSizeUnchangedFileIsStillSkipped) {
    // The counterpart: when the destination really is newer, equal size still
    // means skip. Without this the fix would re-upload the world every run.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/same.txt",
                           std::vector<uint8_t>{'A', 'A', 'A', 'A'});
    mock_client->SetObjectMtime("test-bucket", "prefix/same.txt", 5000);

    { std::ofstream(temp_dir_ / "same.txt") << "AAAA"; }
    set_mtime(temp_dir_ / "same.txt", 1000);   // older than the upload

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_skipped, 1);
    EXPECT_EQ(result.files_uploaded, 0);
}

TEST_F(SyncTaskTest, SameSizeNewerS3ObjectIsDownloaded) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/f.txt",
                           std::vector<uint8_t>{'N', 'E', 'W', '!'});
    mock_client->SetObjectMtime("test-bucket", "prefix/f.txt", 9000);

    { std::ofstream(temp_dir_ / "f.txt") << "old!"; }
    set_mtime(temp_dir_ / "f.txt", 1000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = temp_dir_.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_skipped, 0);

    std::ifstream in(temp_dir_ / "f.txt");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "NEW!") << "the newer remote content must replace the local copy";
}

TEST_F(SyncTaskTest, UnknownTimestampTransfersRatherThanAssumingUnchanged) {
    // A listing that reports no LastModified proves nothing. Erring towards a
    // transfer costs bandwidth; erring towards skip leaves stale data.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/u.txt",
                           std::vector<uint8_t>{'A', 'A', 'A', 'A'});
    mock_client->SetObjectMtime("test-bucket", "prefix/u.txt", 0);   // unknown

    { std::ofstream(temp_dir_ / "u.txt") << "BBBB"; }
    set_mtime(temp_dir_ / "u.txt", 2000);

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_skipped, 0) << "an unknown timestamp is not evidence of freshness";
    EXPECT_EQ(result.files_uploaded, 1);
}

// ============================================================================
// Convergence: a second run over unchanged data must transfer nothing
// ============================================================================
//
// The mtime predicate is only viable if the destination reliably ends up newer
// than the source after a transfer. If it does not, every scheduled run
// re-transfers the whole tree - a far worse regression than the bug being
// fixed, and one that no single-run test can detect.

TEST_F(SyncTaskTest, UploadConvergesOnASecondRun) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    { std::ofstream(temp_dir_ / "a.txt") << "hello"; }
    set_mtime(temp_dir_ / "a.txt", 1000);   // safely older than "now"

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress p1;
    SyncResult r1 = run_sync(config, p1, mock_client);
    ASSERT_TRUE(r1.success) << r1.error_message;
    EXPECT_EQ(r1.files_uploaded, 1);

    SyncProgress p2;
    SyncResult r2 = run_sync(config, p2, mock_client);
    ASSERT_TRUE(r2.success) << r2.error_message;
    EXPECT_EQ(r2.files_uploaded, 0) << "unchanged data must not be re-uploaded";
    EXPECT_EQ(r2.files_skipped, 1);
}

TEST_F(SyncTaskTest, DownloadConvergesOnASecondRun) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/a.txt",
                           std::vector<uint8_t>{'h', 'i'});
    mock_client->SetObjectMtime("test-bucket", "prefix/a.txt", 1000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = temp_dir_.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "test-bucket";
    config.source.path = "prefix/";
    config.source.region = "us-east-1";

    SyncProgress p1;
    SyncResult r1 = run_sync(config, p1, mock_client);
    ASSERT_TRUE(r1.success) << r1.error_message;

    SyncProgress p2;
    SyncResult r2 = run_sync(config, p2, mock_client);
    ASSERT_TRUE(r2.success) << r2.error_message;
    EXPECT_EQ(r2.files_downloaded, 0) << "unchanged data must not be re-downloaded";
    EXPECT_EQ(r2.files_skipped, 1);
}

TEST_F(SyncTaskTest, S3ToS3ConvergesOnASecondRun) {
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("src-bucket");
    mock_client->CreateBucket("dst-bucket");
    mock_client->PutObject("src-bucket", "a/f.txt", std::vector<uint8_t>{'x'});
    mock_client->SetObjectMtime("src-bucket", "a/f.txt", 1000);

    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "src-bucket";
    config.source.path = "a/";
    config.source.region = "us-east-1";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dst-bucket";
    config.destination.path = "b/";
    config.destination.region = "us-east-1";

    SyncProgress p1;
    SyncResult r1 = run_sync(config, p1, mock_client);
    ASSERT_TRUE(r1.success) << r1.error_message;

    SyncProgress p2;
    SyncResult r2 = run_sync(config, p2, mock_client);
    ASSERT_TRUE(r2.success) << r2.error_message;
    EXPECT_EQ(r2.files_skipped, 1) << "an unchanged object must not be re-copied every run";
}

TEST_F(SyncTaskTest, LocalToLocalConvergesOnASecondRun) {
    fs::path src = temp_dir_ / "src";
    fs::path dst = temp_dir_ / "dst";
    fs::create_directories(src);
    { std::ofstream(src / "a.txt") << "data"; }
    set_mtime(src / "a.txt", 1000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = src.string();
    config.dest_local_path = dst.string();

    SyncProgress p1;
    SyncResult r1 = run_sync(config, p1);
    ASSERT_TRUE(r1.success) << r1.error_message;

    SyncProgress p2;
    SyncResult r2 = run_sync(config, p2);
    ASSERT_TRUE(r2.success) << r2.error_message;
    EXPECT_EQ(r2.files_skipped, 1) << "an unchanged file must not be re-copied every run";
}

TEST_F(SyncTaskTest, SameSecondEditIsNotSkippedForever) {
    // Both timestamps are whole seconds. With a >= comparison an edit made in
    // the same second as the upload compares equal and is skipped on every
    // future run, because neither stamp ever changes again.
    auto mock_client = std::make_shared<MockS3Client>();
    mock_client->CreateBucket("test-bucket");
    mock_client->PutObject("test-bucket", "prefix/e.txt",
                           std::vector<uint8_t>{'A', 'A', 'A', 'A'});
    mock_client->SetObjectMtime("test-bucket", "prefix/e.txt", 4242);

    { std::ofstream(temp_dir_ / "e.txt") << "BBBB"; }
    set_mtime(temp_dir_ / "e.txt", 4242);   // identical second

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock_client);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.files_skipped, 0) << "an equal timestamp is not proof of equal content";

    auto stored = mock_client->GetObjectRange("test-bucket", "prefix/e.txt", 0, 3);
    EXPECT_EQ(std::string(stored.begin(), stored.end()), "BBBB");
}

// ============================================================================
// --delete must not delete outside the destination tree (issue #45)
// ============================================================================
//
// The destination is enumerated following directory symlinks, so files behind a
// symlink are recorded with paths relative to the destination root. Those files
// are absent from the source, so they were classified as orphans and deleted
// through the symlink - destroying data the user never pointed the tool at.
//
// The containment check that was supposed to prevent this compared strings
// only: dst/photos/x lexically starts with dst/, so it passed, and fs::remove
// then followed the symlink.

TEST_F(SyncTaskTest, DeleteDoesNotFollowDestinationDirectorySymlinks) {
    fs::path dst = temp_dir_ / "dst";
    fs::path outside = temp_dir_ / "PRECIOUS";
    fs::create_directories(dst);
    fs::create_directories(outside);
    { std::ofstream(outside / "family_photo.jpg") << "irreplaceable"; }
    { std::ofstream(dst / "a.txt") << "a"; }
    // A directory symlink in the destination pointing out of the tree.
    fs::create_directory_symlink(outside, dst / "photos");

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/a.txt", std::vector<uint8_t>{'a'});
    mock->SetObjectMtime("bkt", "src/a.txt", 1000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    EXPECT_TRUE(fs::exists(outside / "family_photo.jpg"))
        << "a file outside the destination tree must never be deleted";
    // The symlink itself is left in place; only its contents were ever at risk.
    EXPECT_TRUE(fs::is_symlink(dst / "photos"));
}

TEST_F(SyncTaskTest, DeleteStillRemovesOrphansInsideTheDestination) {
    // Guard against over-firing: ordinary orphans, and orphans behind a symlink
    // that stays inside the destination, must still be deleted.
    fs::path dst = temp_dir_ / "dst2";
    fs::create_directories(dst / "real");
    { std::ofstream(dst / "orphan.txt") << "gone"; }
    { std::ofstream(dst / "real" / "inner.txt") << "gone too"; }
    fs::create_directory_symlink(dst / "real", dst / "alias");

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";
    config.delete_orphans = true;

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    EXPECT_FALSE(fs::exists(dst / "orphan.txt"))
        << "an ordinary orphan inside the destination must still be deleted";
    // The nested case matters more: it is the branch that compares a prefix and
    // a separator, which is where an over-firing containment bug would live.
    // The top-level file only exercises the parent == base equality branch.
    EXPECT_FALSE(fs::exists(dst / "real" / "inner.txt"))
        << "a nested orphan must still be deleted";
    // The symlinked alias makes the enumerator report inner.txt twice, so the
    // second removal is a no-op that counts as a failure. Pre-existing
    // double-enumeration behaviour, recorded here so a future change to it is
    // visible rather than silent.
    EXPECT_GE(result.files_deleted, 2u);
}

// ============================================================================
// Writes must not escape the destination tree either (issue #54)
// ============================================================================
//
// Same root cause as #45, but it fires without --delete: the destination is
// enumerated following directory symlinks, and O_NOFOLLOW guards only the final
// path component, so a symlinked parent directory was still traversed and
// written through.

TEST_F(SyncTaskTest, DownloadDoesNotOverwriteThroughADestinationSymlink) {
    fs::path dst = temp_dir_ / "dst";
    fs::path outside = temp_dir_ / "OUTSIDE";
    fs::create_directories(dst);
    fs::create_directories(outside);
    { std::ofstream(outside / "PRECIOUS") << "irreplaceable"; }
    fs::create_directory_symlink(outside, dst / "photos");

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    // A key whose leading component matches the symlink name.
    mock->PutObject("bkt", "src/photos/PRECIOUS",
                    std::vector<uint8_t>{'A','T','T','A','C','K'});

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    std::ifstream in(outside / "PRECIOUS");
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "irreplaceable")
        << "a file outside the destination must not be overwritten";
}

TEST_F(SyncTaskTest, DownloadDoesNotCreateFilesThroughADestinationSymlink) {
    fs::path dst = temp_dir_ / "dst_create";
    fs::path outside = temp_dir_ / "OUTSIDE_create";
    fs::create_directories(dst);
    fs::create_directories(outside);
    fs::create_directory_symlink(outside, dst / "photos");

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/photos/planted.txt", std::vector<uint8_t>{'x'});

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    EXPECT_FALSE(fs::exists(outside / "planted.txt"))
        << "no file may be created outside the destination tree";
}

TEST_F(SyncTaskTest, DownloadStillWritesNormallyWithoutSymlinks) {
    // Guard against over-firing: an ordinary nested download must still work,
    // including creating the directories it needs.
    fs::path dst = temp_dir_ / "dst_ok";
    fs::create_directories(dst);

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/a/b/c.txt", std::vector<uint8_t>{'o','k'});

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(fs::exists(dst / "a" / "b" / "c.txt"))
        << "an ordinary nested download must still be written";
}

TEST_F(SyncTaskTest, LocalToLocalCopyDoesNotEscapeThroughADestinationSymlink) {
    fs::path src = temp_dir_ / "l2l_src";
    fs::path dst = temp_dir_ / "l2l_dst";
    fs::path outside = temp_dir_ / "l2l_OUTSIDE";
    fs::create_directories(src / "photos");
    fs::create_directories(dst);
    fs::create_directories(outside);
    { std::ofstream(src / "photos" / "PRECIOUS") << "attacker"; }
    { std::ofstream(outside / "PRECIOUS") << "irreplaceable"; }
    fs::create_directory_symlink(outside, dst / "photos");

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = src.string();
    config.dest_local_path = dst.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    std::ifstream in(outside / "PRECIOUS");
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "irreplaceable")
        << "a local-to-local copy must not write outside the destination";
}

TEST_F(SyncTaskTest, FirstSyncIntoANonExistentDestinationStillWorks) {
    // The containment helper returns true when the base does not exist, because
    // nothing inside it can be a symlink to traverse. That carve-out is the
    // riskiest branch: without it every first run is refused, and this is the
    // guard for it.
    fs::path dst = temp_dir_ / "brand" / "new" / "dest";   // none of it exists

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/a/b.txt", std::vector<uint8_t>{'n','e','w'});

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    fs::create_directories(dst);   // as the CLI does before run_sync
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(fs::exists(dst / "a" / "b.txt"));
}

TEST_F(SyncTaskTest, DestinationReachedThroughASymlinkIsNotRefused) {
    // The destination itself being a symlink is legitimate and common - macOS
    // resolves /tmp to /private/tmp, so every test on that platform takes this
    // path. Both sides are canonicalised, so it must not be mistaken for an
    // escape. A Linux CI would not catch a regression here without this test.
    fs::path real_dst = temp_dir_ / "real_dest";
    fs::path link_dst = temp_dir_ / "linked_dest";
    fs::create_directories(real_dst);
    fs::create_directory_symlink(real_dst, link_dst);

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/deep/f.txt", std::vector<uint8_t>{'y'});

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = link_dst.string();   // the symlink, not the real path
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, mock);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(fs::exists(real_dst / "deep" / "f.txt"))
        << "a destination reached through a symlink must still be written";
}

// ============================================================================
// Destination durability under a failed write (issue #58)
// ============================================================================

#include <sys/resource.h>
#include <csignal>

// Caps how large a file this process may write, so a write partway through a
// large file fails with EFBIG. SIGXFSZ has to be ignored or the process dies
// instead of the write returning an error.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes) {
        ::getrlimit(RLIMIT_FSIZE, &saved_);
        old_handler_ = ::signal(SIGXFSZ, SIG_IGN);
        struct rlimit r = saved_;
        r.rlim_cur = bytes;
        applied_ = ::setrlimit(RLIMIT_FSIZE, &r) == 0;
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &saved_);
        ::signal(SIGXFSZ, old_handler_);
    }
    bool applied() const { return applied_; }
private:
    struct rlimit saved_{};
    void (*old_handler_)(int) = nullptr;
    bool applied_ = false;
};

TEST_F(SyncTaskTest, FailedDiffDownloadClearsMtimeSoNextSyncRepairs) {
    constexpr size_t CHUNK_SIZE_BYTES = CHUNK_SIZE;
    constexpr size_t FILE_SIZE = CHUNK_SIZE_BYTES * 3;

    std::vector<uint8_t> remote(FILE_SIZE);
    std::fill(remote.begin(), remote.begin() + CHUNK_SIZE_BYTES, 'A');
    std::fill(remote.begin() + CHUNK_SIZE_BYTES,
              remote.begin() + CHUNK_SIZE_BYTES * 2, 'X');
    std::fill(remote.begin() + CHUNK_SIZE_BYTES * 2, remote.end(), 'Y');

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/large.bin", remote);
    mock->SetObjectMtime("bkt", "src/large.bin", 2000);

    fs::path local_file = temp_dir_ / "large.bin";
    {
        std::ofstream out(local_file, std::ios::binary);
        std::vector<char> local(FILE_SIZE);
        std::fill(local.begin(), local.begin() + CHUNK_SIZE_BYTES, 'A');
        std::fill(local.begin() + CHUNK_SIZE_BYTES,
                  local.begin() + CHUNK_SIZE_BYTES * 2, 'B');
        std::fill(local.begin() + CHUNK_SIZE_BYTES * 2, local.end(), 'C');
        out.write(local.data(), local.size());
    }
    set_mtime(local_file, 1000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src";
    config.source.region = "us-east-1";
    config.local_path = temp_dir_.string();

    {
        FileSizeLimit limit(CHUNK_SIZE_BYTES + CHUNK_SIZE_BYTES / 2);
        ASSERT_TRUE(limit.applied());

        SyncProgress progress;
        SyncResult result = run_sync(config, progress, mock);

        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.files_failed, 1);
        EXPECT_EQ(result.files_diff_downloaded, 0);
    }

    struct ::stat st {};
    if (::stat(local_file.c_str(), &st) == 0) {
        EXPECT_EQ(st.st_mtime, 0)
            << "a failed in-place patch must not look newer than the remote object";
    }

    SyncProgress retry_progress;
    SyncResult retry = run_sync(config, retry_progress, mock);

    ASSERT_TRUE(retry.success) << retry.error_message;
    EXPECT_EQ(retry.files_downloaded + retry.files_diff_downloaded, 1);
    EXPECT_EQ(retry.files_failed, 0);

    std::ifstream in(local_file, std::ios::binary);
    std::vector<uint8_t> repaired((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    EXPECT_EQ(repaired, remote);
}

// The mtime is cleared to say "this file is a half-applied patch". A diff
// download that never wrote a byte has not made that true, and clearing it
// anyway is not a harmless over-correction: an epoch mtime reads as "unknown"
// to every direction, so the next upload of the same pair stops skipping this
// file and pushes it over a newer remote object.
TEST_F(SyncTaskTest, DiffDownloadThatNeverWritesLeavesTheLocalFileAlone) {
    constexpr size_t CHUNK_SIZE_BYTES = CHUNK_SIZE;
    constexpr size_t FILE_SIZE = CHUNK_SIZE_BYTES * 3;

    std::vector<uint8_t> remote(FILE_SIZE);
    std::fill(remote.begin(), remote.begin() + CHUNK_SIZE_BYTES, 'A');
    std::fill(remote.begin() + CHUNK_SIZE_BYTES,
              remote.begin() + CHUNK_SIZE_BYTES * 2, 'X');
    std::fill(remote.begin() + CHUNK_SIZE_BYTES * 2, remote.end(), 'Y');

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/large.bin", remote);
    mock->SetObjectMtime("bkt", "src/large.bin", 2000);

    // Same size, older, and sharing its first chunk with the remote object, so
    // the plan is a differential download rather than a full one.
    std::vector<uint8_t> original(FILE_SIZE);
    std::fill(original.begin(), original.begin() + CHUNK_SIZE_BYTES, 'A');
    std::fill(original.begin() + CHUNK_SIZE_BYTES,
              original.begin() + CHUNK_SIZE_BYTES * 2, 'B');
    std::fill(original.begin() + CHUNK_SIZE_BYTES * 2, original.end(), 'C');

    fs::path local_file = temp_dir_ / "large.bin";
    {
        std::ofstream out(local_file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(original.data()), original.size());
    }
    set_mtime(local_file, 1000);

    // Every range request fails, so not one byte reaches the file.
    mock->SetFailure("bkt", "src/large.bin", S3MockMethod::GetObjectRange);

    SyncConfig download;
    download.direction = SyncDirection::Download;
    download.source.type = SourceType::S3;
    download.source.bucket = "bkt";
    download.source.path = "src";
    download.source.region = "us-east-1";
    download.local_path = temp_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(download, progress, mock);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);

    {
        std::ifstream in(local_file, std::ios::binary);
        std::vector<uint8_t> after((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        ASSERT_EQ(after, original) << "nothing should have been written";
    }

    struct ::stat st {};
    ASSERT_EQ(::stat(local_file.c_str(), &st), 0);
    EXPECT_EQ(st.st_mtime, 1000)
        << "a file we never wrote to must keep the timestamp it came with";

    // The damage the timestamp protects against: push the same tree back up.
    // The remote object is newer and the same size, so it must be left alone.
    mock->ClearFailure("bkt", "src/large.bin", S3MockMethod::GetObjectRange);

    SyncConfig upload;
    upload.direction = SyncDirection::Upload;
    upload.destination.type = SourceType::S3;
    upload.destination.bucket = "bkt";
    upload.destination.path = "src";
    upload.destination.region = "us-east-1";
    upload.local_path = temp_dir_.string();

    SyncProgress upload_progress;
    SyncResult uploaded = run_sync(upload, upload_progress, mock);

    ASSERT_TRUE(uploaded.success) << uploaded.error_message;
    EXPECT_EQ(uploaded.files_skipped, 1);
    EXPECT_EQ(uploaded.files_uploaded, 0);
    EXPECT_EQ(uploaded.files_diff_uploaded, 0);
    EXPECT_EQ(mock->GetObjectRange("bkt", "src/large.bin", 0,
                                   static_cast<int64_t>(FILE_SIZE) - 1),
              remote)
        << "the newer remote object must survive the failed download";
}

TEST_F(LocalToLocalSyncTest, FailedCopyLeavesTheExistingDestinationIntact) {
    fs::path src = source_dir_ / "f.bin";
    fs::path dst = dest_dir_ / "f.bin";

    const std::string original = "ORIGINAL GOOD CONTENT";
    { std::ofstream o(dst, std::ios::binary); o << original; }
    set_mtime(dst, 1000);

    // Newer and far larger than the limit below, so the write fails partway.
    { std::ofstream o(src, std::ios::binary); o << std::string(4 * 1024 * 1024, 'N'); }
    set_mtime(src, 2000);

    FileSizeLimit limit(64 * 1024);
    ASSERT_TRUE(limit.applied());

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1);
    ASSERT_TRUE(fs::exists(dst)) << "a failed update must not delete the old copy";
    std::ifstream i(dst, std::ios::binary);
    std::string after((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, original)
        << "the destination must hold either the old contents or the complete new ones";
}

TEST_F(LocalToLocalSyncTest, FailedCopyLeavesNoTemporaryFileBehind) {
    fs::path src = source_dir_ / "f.bin";
    fs::path dst = dest_dir_ / "f.bin";
    { std::ofstream o(dst, std::ios::binary); o << "old"; }
    set_mtime(dst, 1000);
    { std::ofstream o(src, std::ios::binary); o << std::string(4 * 1024 * 1024, 'N'); }
    set_mtime(src, 2000);

    {
        FileSizeLimit limit(64 * 1024);
        ASSERT_TRUE(limit.applied());
        SyncConfig config;
        config.direction = SyncDirection::LocalToLocal;
        config.local_path = source_dir_.string();
        config.dest_local_path = dest_dir_.string();
        SyncProgress progress;
        run_sync(config, progress, nullptr);
    }

    ASSERT_TRUE(fs::exists(dst)) << "the destination itself must still be there";
    std::vector<std::string> leftovers;
    for (const auto& e : fs::directory_iterator(dest_dir_)) {
        if (e.path().filename().string() != "f.bin") {
            leftovers.push_back(e.path().filename().string());
        }
    }
    EXPECT_TRUE(leftovers.empty())
        << "a discarded write left " << leftovers.size() << " stray file(s), first: "
        << (leftovers.empty() ? "" : leftovers.front());
}

TEST_F(LocalToLocalSyncTest, AtomicReplacePreservesTheDestinationPermissions) {
    fs::path src = source_dir_ / "f.txt";
    fs::path dst = dest_dir_ / "f.txt";
    { std::ofstream o(dst); o << "old"; }
    set_mtime(dst, 1000);
    // Deliberately not 0600: that is the mode the temporary file is created
    // with, so this test could not tell a preserved mode from the default.
    ASSERT_EQ(::chmod(dst.c_str(), 0640), 0);

    { std::ofstream o(src); o << "new content"; }
    set_mtime(src, 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);
    ASSERT_EQ(result.files_failed, 0);

    struct ::stat sb {};
    ASSERT_EQ(::stat(dst.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 07777, 0640u)
        << "replacing a file must not widen its permissions";

    std::ifstream i(dst);
    std::string after((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, "new content");
}

TEST_F(SyncTaskTest, FailedDownloadLeavesTheExistingLocalFileIntact) {
    // The other half of issue #58: a download over an existing local file used
    // to truncate it up front and remove it when the write failed, so a
    // disk-full or EIO mid-update destroyed the copy the user already had.
    fs::path dst = temp_dir_ / "dst";
    fs::create_directories(dst);
    fs::path existing = dst / "big.bin";

    const std::string original = "ORIGINAL GOOD CONTENT";
    { std::ofstream o(existing, std::ios::binary); o << original; }
    set_mtime(existing, 1000);

    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "src/big.bin", std::vector<uint8_t>(4 * 1024 * 1024, 'N'));
    mock->SetObjectMtime("bkt", "src/big.bin", 2000);

    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.local_path = dst.string();
    config.source.type = SourceType::S3;
    config.source.bucket = "bkt";
    config.source.path = "src/";
    config.source.region = "us-east-1";

    SyncResult result;
    {
        FileSizeLimit limit(64 * 1024);
        ASSERT_TRUE(limit.applied());
        SyncProgress progress;
        result = run_sync(config, progress, mock);
    }

    EXPECT_EQ(result.files_downloaded, 0);
    ASSERT_TRUE(fs::exists(existing)) << "a failed download must not delete the old copy";
    std::ifstream i(existing, std::ios::binary);
    std::string after((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, original);

    std::vector<std::string> leftovers;
    for (const auto& e : fs::directory_iterator(dst)) {
        if (e.path().filename().string() != "big.bin") {
            leftovers.push_back(e.path().filename().string());
        }
    }
    EXPECT_TRUE(leftovers.empty())
        << "stray temporary file left behind: " << (leftovers.empty() ? "" : leftovers.front());
}

TEST_F(LocalToLocalSyncTest, InFlightTemporaryIsNeverWiderThanTheFileItReplaces) {
    // The temporary file holds the complete new contents. Creating it 0644 and
    // narrowing only at the end would publish a private file's contents to
    // every local user for the length of the transfer.
    fs::path src = source_dir_ / "secret.bin";
    fs::path dst = dest_dir_ / "secret.bin";
    { std::ofstream o(dst, std::ios::binary); o << "old secret"; }
    set_mtime(dst, 1000);
    ASSERT_EQ(::chmod(dst.c_str(), 0600), 0);
    { std::ofstream o(src, std::ios::binary); o << std::string(8 * 1024 * 1024, 'S'); }
    set_mtime(src, 2000);

    // Watch the destination directory while the copy runs.
    std::atomic<bool> done{false};
    std::atomic<mode_t> widest{0};
    std::atomic<int> sightings{0};
    std::thread watcher([&] {
        while (!done) {
            std::error_code ec;
            for (fs::directory_iterator it(dest_dir_, ec), end; it != end && !ec; it.increment(ec)) {
                if (!is_sync_temp_name(it->path().filename().string())) continue;
                struct ::stat sb {};
                if (::stat(it->path().c_str(), &sb) == 0) {
                    ++sightings;
                    mode_t m = sb.st_mode & 0777;
                    mode_t prev = widest.load();
                    if ((m & 077) > (prev & 077)) widest = m;
                }
            }
        }
    });

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);
    done = true;
    watcher.join();

    ASSERT_EQ(result.files_failed, 0);
    ASSERT_GT(sightings.load(), 0)
        << "the watcher never saw the temporary file, so it proved nothing";
    EXPECT_EQ(widest.load() & 077, 0u)
        << "the temporary file was readable by group/other (mode "
        << std::oct << widest.load() << ")";

    struct ::stat sb {};
    ASSERT_EQ(::stat(dst.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 0777, 0600u);
}

TEST_F(LocalToLocalSyncTest, SetuidIsNotCarriedOntoReplacedContent) {
    // The kernel strips setuid when an unprivileged process writes a file, so
    // the old in-place path cleared it. Preserving the mode must not put it
    // back on contents that just arrived from the sync source.
    fs::path src = source_dir_ / "tool";
    fs::path dst = dest_dir_ / "tool";
    { std::ofstream o(dst); o << "old"; }
    set_mtime(dst, 1000);
    ASSERT_EQ(::chmod(dst.c_str(), 06755), 0);
    struct ::stat before {};
    ASSERT_EQ(::stat(dst.c_str(), &before), 0);
    if ((before.st_mode & 06000) == 0) GTEST_SKIP() << "filesystem does not keep setuid bits";

    { std::ofstream o(src); o << "new content from the source"; }
    set_mtime(src, 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);
    ASSERT_EQ(result.files_failed, 0);

    struct ::stat sb {};
    ASSERT_EQ(::stat(dst.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 06000, 0u) << "setuid/setgid must not survive a replace";
    EXPECT_EQ(sb.st_mode & 0777, 0755u) << "the ordinary permission bits still carry across";
}

TEST_F(LocalToLocalSyncTest, AStaleTemporaryNameDoesNotBlockTheFileForever) {
    // A killed run leaves temporaries behind. The next sync reaps them before
    // enumeration so they do not get copied as user data or block the real file.
    fs::path src = source_dir_ / "f.txt";
    fs::path dst = dest_dir_ / "f.txt";
    { std::ofstream o(src); o << "new content"; }
    set_mtime(src, 2000);

    pid_t dead_pid = find_dead_pid();
    ASSERT_GT(dead_pid, 0);
    for (int i = 0; i < 4; ++i) {
        std::ofstream o(dest_dir_ / ("." + std::string("f.txt") + "-" +
                                     std::to_string(dead_pid) + "-" +
                                     std::to_string(i) + kTempSuffix));
        o << "stale";
    }

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);

    EXPECT_EQ(result.files_failed, 0) << "a stale temporary must not block the real file";
    ASSERT_TRUE(fs::exists(dst));
    std::ifstream i(dst);
    std::string after((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, "new content");
}

TEST_F(LocalToLocalSyncTest, AVeryLongFilenameStillSyncs) {
    // The temporary name is derived from the destination name. Appending to a
    // name already near NAME_MAX made the open fail, so files that used to
    // sync stopped syncing.
    const std::string long_name(250, 'x');
    { std::ofstream o(source_dir_ / long_name); o << "content"; }
    set_mtime(source_dir_ / long_name, 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);

    EXPECT_EQ(result.files_failed, 0);
    EXPECT_TRUE(fs::exists(dest_dir_ / long_name));
}

TEST_F(LocalToLocalSyncTest, CancellingMidCopyLeavesTheExistingDestinationIntact) {
    // Ctrl-C partway through an update is the scenario issue #58 names first:
    // the old code had already truncated the destination and removed it on the
    // way out.
    const std::string original = "ORIGINAL GOOD CONTENT";
    for (int i = 0; i < 40; ++i) {
        fs::path d = dest_dir_ / ("f" + std::to_string(i) + ".bin");
        { std::ofstream o(d, std::ios::binary); o << original; }
        set_mtime(d, 1000);
        fs::path srcp = source_dir_ / ("f" + std::to_string(i) + ".bin");
        { std::ofstream o(srcp, std::ios::binary); o << std::string(2 * 1024 * 1024, 'N'); }
        set_mtime(srcp, 2000);
    }

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();

    SyncProgress progress;
    std::thread canceller([&] {
        while (progress.bytes_transferred.load() == 0) std::this_thread::yield();
        progress.cancelled = true;
    });
    SyncResult result = run_sync(config, progress, nullptr);
    canceller.join();

    // Every destination file must hold either the old contents or the complete
    // new ones - never nothing, and never a partial write.
    int intact = 0, replaced = 0;
    for (int i = 0; i < 40; ++i) {
        fs::path d = dest_dir_ / ("f" + std::to_string(i) + ".bin");
        ASSERT_TRUE(fs::exists(d)) << d << " was destroyed by a cancelled sync";
        std::ifstream in(d, std::ios::binary);
        std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (after == original) ++intact;
        else if (after == std::string(2 * 1024 * 1024, 'N')) ++replaced;
        else FAIL() << d << " holds " << after.size() << " bytes: neither the old nor the new contents";
    }
    EXPECT_EQ(intact + replaced, 40);
}

TEST_F(LocalToLocalSyncTest, ReadOnlyDestinationDirectoryFailsLoudlyRatherThanUnsafely) {
    // An update is written to a temporary file beside the destination, so the
    // directory must be writable even when the file itself is. This refuses
    // rather than falling back to the in-place write that issue #58 is about -
    // a silent downgrade to the destructive path would defeat the fix.
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";

    fs::path src = source_dir_ / "f.txt";
    fs::path dst = dest_dir_ / "f.txt";
    { std::ofstream o(dst); o << "old"; }
    set_mtime(dst, 1000);
    { std::ofstream o(src); o << "new"; }
    set_mtime(src, 2000);

    ASSERT_EQ(::chmod(dest_dir_.c_str(), 0500), 0);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);

    ::chmod(dest_dir_.c_str(), 0700);   // so TearDown can clean up

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.files_failed, 1) << "the failure has to be reported, not swallowed";
    std::ifstream i(dst);
    std::string after((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, "old") << "the existing file must be left exactly as it was";
}

TEST_F(LocalToLocalSyncTest, ANoAccessDestinationIsNotRepublishedReadable) {
    // Mode 0000 is a real mode. Treating "no permission bits" as "there was no
    // file here" would create the replacement at 0644 and skip the fchmod,
    // handing out a file the owner had deliberately closed off.
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";

    fs::path src = source_dir_ / "closed.txt";
    fs::path dst = dest_dir_ / "closed.txt";
    { std::ofstream o(dst); o << "old"; }
    set_mtime(dst, 1000);
    ASSERT_EQ(::chmod(dst.c_str(), 0000), 0);
    { std::ofstream o(src); o << "new content"; }
    set_mtime(src, 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    run_sync(config, progress, nullptr);

    struct ::stat sb {};
    ASSERT_EQ(::stat(dst.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 07777, 0000u)
        << "a mode-0000 destination came back as " << std::oct << (sb.st_mode & 07777);
    ::chmod(dst.c_str(), 0600);   // so TearDown can clean up
}

TEST_F(LocalToLocalSyncTest, TheStickyBitSurvivesAReplace) {
    // The kernel does not strip the sticky bit on write, so masking it away
    // with setuid/setgid would lose something the old path kept.
    fs::path src = source_dir_ / "s.txt";
    fs::path dst = dest_dir_ / "s.txt";
    { std::ofstream o(dst); o << "old"; }
    set_mtime(dst, 1000);
    ASSERT_EQ(::chmod(dst.c_str(), 01644), 0);
    struct ::stat before {};
    ASSERT_EQ(::stat(dst.c_str(), &before), 0);
    if ((before.st_mode & S_ISVTX) == 0) GTEST_SKIP() << "filesystem drops the sticky bit";

    { std::ofstream o(src); o << "new content"; }
    set_mtime(src, 2000);

    SyncConfig config;
    config.direction = SyncDirection::LocalToLocal;
    config.local_path = source_dir_.string();
    config.dest_local_path = dest_dir_.string();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress, nullptr);
    ASSERT_EQ(result.files_failed, 0);

    struct ::stat sb {};
    ASSERT_EQ(::stat(dst.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 07777, 01644u);
}

// Records which upload route a sync took, and drops part bodies rather than
// keeping them. MockS3Client normally assembles the finished object in memory,
// which for a file over the single-PUT limit would cost the several gigabytes
// of RAM that the multipart path exists to avoid.
class RouteRecordingClient : public MockS3Client {
public:
    std::atomic<int> create_multipart_calls{0};
    std::atomic<int> put_from_file_calls{0};
    std::atomic<int> parts_uploaded{0};
    std::atomic<int64_t> part_bytes{0};
    std::atomic<int> completed_uploads{0};
    std::atomic<int> completed_parts{0};
    std::atomic<int> parts_missing_checksum{0};

    std::string CreateMultipartUpload(const std::string& bucket,
                                      const std::string& key) override {
        ++create_multipart_calls;
        return MockS3Client::CreateMultipartUpload(bucket, key);
    }

    bool PutObjectFromFile(const std::string& bucket, const std::string& key,
                           const std::string& file_path) override {
        ++put_from_file_calls;
        return MockS3Client::PutObjectFromFile(bucket, key, file_path);
    }

    S3PartResult UploadPart(const std::string& bucket, const std::string& key,
                            const std::string& upload_id, int part_number,
                            const std::vector<uint8_t>& data, uint32_t crc32) override {
        (void)bucket; (void)key; (void)upload_id; (void)crc32;
        ++parts_uploaded;
        part_bytes += static_cast<int64_t>(data.size());
        // Checksum computed before the body is dropped, the way a real endpoint
        // reports one, so the caller has something real to carry to completion.
        return S3PartResult{"\"mock-etag-part-" + std::to_string(part_number) + "\"",
                            MockS3Client::base64_crc32(data)};
    }

    bool CompleteMultipartUpload(
        const std::string& bucket, const std::string& key, const std::string& upload_id,
        const std::vector<std::pair<int, S3PartResult>>& parts) override {
        (void)bucket; (void)key; (void)upload_id;
        ++completed_uploads;
        completed_parts += static_cast<int>(parts.size());
        for (const auto& [num, result] : parts) {
            (void)num;
            if (result.checksum_crc32.empty()) ++parts_missing_checksum;
        }
        return true;   // the bodies were dropped above, so do not reassemble
    }
};

TEST_F(SyncTaskTest, NewLocalFileAboveTheSinglePutLimitUploadsAsParts) {
    // A first sync of a file over 5 GiB reached diff_upload_file, found nothing
    // in S3 to reuse, and fell back to a single PutObject - which cannot carry
    // a file that size, so the upload failed with no route that could work. The
    // bytes have to go up as parts instead (issues #91, #96).
    const int64_t kSize = static_cast<int64_t>(kMaxSinglePutBytes) + 1;
    fs::path test_file = temp_dir_ / "huge.bin";
    {
        std::ofstream out(test_file, std::ios::binary);
        ASSERT_TRUE(out.good());
    }
    // Sparse: the length is real, the bytes are holes, so this costs no disk.
    if (::truncate(test_file.c_str(), static_cast<off_t>(kSize)) != 0) {
        GTEST_SKIP() << "filesystem does not support sparse files this large";
    }

    SyncConfig config;
    config.local_path = temp_dir_.string();
    config.destination.type = SourceType::S3;
    config.destination.bucket = "test-bucket";
    config.destination.path = "prefix";
    config.destination.region = "us-east-1";
    config.dry_run = false;

    SyncProgress progress;
    auto mock_client = std::make_shared<RouteRecordingClient>();
    mock_client->CreateBucket("test-bucket");

    SyncResult result = run_sync(config, progress, mock_client);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files_diff_uploaded, 1);
    EXPECT_EQ(result.files_failed, 0);

    EXPECT_EQ(mock_client->put_from_file_calls, 0)
        << "a file this size cannot go up as a single PutObject";
    EXPECT_EQ(mock_client->create_multipart_calls, 1);
    EXPECT_EQ(mock_client->completed_uploads, 1);

    const int64_t expected_parts =
        (kSize + DEFAULT_CHUNK_SIZE - 1) / DEFAULT_CHUNK_SIZE;
    EXPECT_EQ(mock_client->parts_uploaded, expected_parts);
    EXPECT_EQ(mock_client->completed_parts, expected_parts);
    EXPECT_EQ(mock_client->part_bytes, kSize)
        << "the parts sent do not add up to the file";

    // The upload declares CRC32, so completion has to name each part's checksum
    // or S3 rejects the whole thing with InvalidPart after every byte is sent
    // (issue #98).
    EXPECT_EQ(mock_client->parts_missing_checksum, 0)
        << "parts reached CompleteMultipartUpload without their checksums";
}
