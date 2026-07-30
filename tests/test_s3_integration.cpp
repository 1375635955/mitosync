// Integration tests against a live S3-compatible endpoint.
//
// Everything else in this suite runs against MockS3Client, which is fast,
// deterministic, and cannot tell you what a real service does. Two bugs found
// in one afternoon were green against the mock and rejected on the first
// request by a real endpoint:
//
//   #98  CompleteMultipartUpload omitted each part's checksum. The mock happily
//        assembled the object; MinIO answered InvalidPart after all 641 parts
//        of a 5 GiB upload had already been sent.
//   #99  UploadPartCopy into a checksum-declaring upload was rejected outright,
//        so remote chunk checksums could not be derived at all.
//
// A mock cannot catch either, because a mock is written from the same
// understanding of the protocol as the code it is testing. These tests exist to
// hold that understanding against a service that disagrees.
//
// They are skipped unless MITO_TEST_S3_ENDPOINT names a reachable endpoint, so
// a plain `ctest` on a machine without Docker stays green:
//
//   scripts/s3-emulator.sh start
//   eval "$(scripts/s3-emulator.sh env)"
//   ctest --test-dir build -L s3-integration
//   scripts/s3-emulator.sh stop

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aws/core/Aws.h>

#include "crc32_chunks.h"
#include "crc32_hw.h"
#include "leftovers_task.h"
#include "rm_task.h"
#include "s3_interface.h"
#include "sync_task.h"
#include "temp_test_path.h"

namespace fs = std::filesystem;

namespace {

// The endpoint under test, or empty when none was configured.
std::string endpoint() {
    const char* value = std::getenv("MITO_TEST_S3_ENDPOINT");
    return value ? value : "";
}

std::string region() {
    const char* value = std::getenv("MITO_TEST_S3_REGION");
    return value ? value : "us-east-1";
}

// A bucket name unique to this process, so concurrent runs and leftovers from a
// killed run cannot collide. S3 bucket names are lowercase and >= 3 characters.
std::string unique_bucket() {
    return "mito-it-" + std::to_string(::getpid());
}

class S3Integration : public ::testing::Test {
protected:
    void SetUp() override {
        if (endpoint().empty()) {
            GTEST_SKIP() << "MITO_TEST_S3_ENDPOINT is not set; start one with "
                            "scripts/s3-emulator.sh start";
        }
        client_ = CreateS3Client(region(), endpoint());
        ASSERT_TRUE(client_) << "could not build a client for " << endpoint();
        bucket_ = unique_bucket();
        // Idempotent: a bucket left by an earlier run of the same pid is fine.
        client_->CreateBucket(bucket_);

        dir_ = mito_test_temp_path("mito_it");
        fs::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // A file of `size` bytes with content that varies along its length, so a
    // chunk landing at the wrong offset does not still checksum correctly.
    uint32_t write_file(const fs::path& path, size_t size, uint8_t seed = 0) {
        std::vector<uint8_t> block(1u << 20);
        std::ofstream out(path, std::ios::binary);
        size_t written = 0;
        uint32_t crc = 0;
        while (written < size) {
            const size_t n = std::min(block.size(), size - written);
            for (size_t i = 0; i < n; ++i) {
                block[i] = static_cast<uint8_t>(((written + i) * 31 + seed) & 0xFF);
            }
            out.write(reinterpret_cast<const char*>(block.data()),
                      static_cast<std::streamsize>(n));
            crc = crc32_hw_update(crc, block.data(), n);
            written += n;
        }
        return crc;
    }

    // A prefix per test, so objects left by one cannot be seen by the next.
    // Sharing one prefix made every run report a growing S3 object count and
    // would eventually have produced a confusing cross-test failure.
    std::string prefix() const {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        return std::string("p/") + (info ? info->name() : "unknown");
    }

    SyncConfig sync_config() {
        SyncConfig config;
        config.local_path = dir_.string();
        config.destination.type = SourceType::S3;
        config.destination.bucket = bucket_;
        config.destination.path = prefix();
        config.destination.region = region();
        config.destination.endpoint = endpoint();
        return config;
    }

    // Move a file's mtime into the past.
    //
    // A sync only skips when the destination stamp is strictly newer than the
    // source, because both are whole seconds and an in-place edit in the same
    // second as the upload would otherwise be skipped forever (see
    // destination_is_current in sync_task.cpp). A test that writes and syncs
    // within one second therefore sees a legitimate redundant upload. Backdate
    // the file so the comparison means what the test intends.
    void backdate(const fs::path& path, int seconds) {
        struct ::timespec times[2];
        const auto now = ::time(nullptr);
        times[0].tv_sec = now - seconds; times[0].tv_nsec = 0;
        times[1].tv_sec = now - seconds; times[1].tv_nsec = 0;
        ASSERT_EQ(::utimensat(AT_FDCWD, path.c_str(), times, 0), 0)
            << "utimensat failed for " << path;
    }

    std::shared_ptr<IS3Client> client_;
    std::string bucket_;
    fs::path dir_;
};

// ---------------------------------------------------------------------------
// S3 protocol details the mock cannot prove
// ---------------------------------------------------------------------------

TEST_F(S3Integration, ServerSideCopyHandlesKeysThatNeedCopySourceEncoding) {
    // The copy source is passed to the SDK as "bucket/key". Spaces, plus, '#',
    // and '%' all need correct request encoding; a raw map-backed mock cannot
    // say whether the HTTP request names the original key or a mangled one.
    const std::string source_key =
        prefix() + "/folder with space/My Document + #%25.txt";
    const std::string dest_key =
        prefix() + "/copied result + #%25.txt";
    const std::vector<uint8_t> bytes = {'m', 'i', 't', 'o', 0, '#', '%', '+'};

    ASSERT_TRUE(client_->PutObject(bucket_, source_key, bytes));
    ASSERT_TRUE(client_->CopyObject(bucket_, source_key, bucket_, dest_key,
                                    static_cast<int64_t>(bytes.size())));

    ASSERT_EQ(client_->GetObjectSize(bucket_, dest_key),
              static_cast<int64_t>(bytes.size()));
    const std::vector<uint8_t> copied =
        client_->GetObjectRange(bucket_, dest_key, 0,
                                static_cast<int64_t>(bytes.size()) - 1);
    EXPECT_EQ(copied, bytes);
}

TEST_F(S3Integration, ListingPaginationCarriesObjectsAndCommonPrefixes) {
    // A real endpoint owns continuation tokens, delimiter handling, and result
    // ordering. Force pagination with max_keys=2 so the test does not need a
    // large bucket.
    const std::string root = prefix() + "/paged/";
    ASSERT_TRUE(client_->PutObject(bucket_, root + "a.txt", {'a'}));
    ASSERT_TRUE(client_->PutObject(bucket_, root + "b.txt", {'b'}));
    ASSERT_TRUE(client_->PutObject(bucket_, root + "nested space/file.txt", {'c'}));
    ASSERT_TRUE(client_->PutObject(bucket_, root + "z-dir/file.txt", {'d'}));

    std::set<std::string> objects;
    std::set<std::string> prefixes;
    std::string token;
    int pages = 0;
    do {
        S3ListResult page = client_->ListObjects(bucket_, root, "/", token, 2);
        ASSERT_TRUE(page.success) << page.error_message;
        ++pages;
        for (const auto& obj : page.objects) {
            objects.insert(obj.key);
        }
        prefixes.insert(page.common_prefixes.begin(), page.common_prefixes.end());
        token = page.next_continuation_token;
        if (!page.is_truncated) {
            EXPECT_TRUE(token.empty()) << "final page should not carry a next token";
            break;
        }
        ASSERT_FALSE(token.empty()) << "truncated listing needs a continuation token";
        ASSERT_LT(pages, 10) << "pagination did not converge";
    } while (true);

    EXPECT_GT(pages, 1) << "max_keys=2 should force a paginated listing";
    EXPECT_TRUE(objects.count(root + "a.txt"));
    EXPECT_TRUE(objects.count(root + "b.txt"));
    EXPECT_TRUE(prefixes.count(root + "nested space/"));
    EXPECT_TRUE(prefixes.count(root + "z-dir/"));
}

TEST_F(S3Integration, RmHandlesObjectPrefixAmbiguityAndDirectoryMarkers) {
    const std::string shadow = prefix() + "/shadow";
    ASSERT_TRUE(client_->PutObject(bucket_, shadow, {'o'}));
    ASSERT_TRUE(client_->PutObject(bucket_, shadow + "/child.txt", {'c'}));

    RmConfig object_rm;
    object_rm.bucket = bucket_;
    object_rm.prefix = shadow;
    object_rm.region = region();
    object_rm.endpoint = endpoint();
    object_rm.force = true;
    object_rm.recursive = true;
    object_rm.max_threads = 4;

    RmProgress object_progress;
    RmResult object_result = run_rm(object_rm, object_progress, client_);
    ASSERT_TRUE(object_result.success) << object_result.error_message;
    EXPECT_EQ(object_result.objects_deleted, 1u);
    EXPECT_EQ(client_->CheckObjectPresence(bucket_, shadow), S3ObjectPresence::NotFound);
    EXPECT_EQ(client_->CheckObjectPresence(bucket_, shadow + "/child.txt"),
              S3ObjectPresence::Exists)
        << "naming the object must not delete the same-named prefix";

    const std::string marker_prefix = prefix() + "/marker/";
    ASSERT_TRUE(client_->PutObject(bucket_, marker_prefix, {}));
    ASSERT_TRUE(client_->PutObject(bucket_, marker_prefix + "child.txt", {'x'}));

    RmConfig prefix_rm;
    prefix_rm.bucket = bucket_;
    prefix_rm.prefix = marker_prefix;
    prefix_rm.region = region();
    prefix_rm.endpoint = endpoint();
    prefix_rm.force = true;
    prefix_rm.recursive = true;
    prefix_rm.batch = true;
    prefix_rm.max_threads = 4;

    RmProgress prefix_progress;
    RmResult prefix_result = run_rm(prefix_rm, prefix_progress, client_);
    ASSERT_TRUE(prefix_result.success) << prefix_result.error_message;
    EXPECT_EQ(client_->CheckObjectPresence(bucket_, marker_prefix),
              S3ObjectPresence::NotFound);
    EXPECT_EQ(client_->CheckObjectPresence(bucket_, marker_prefix + "child.txt"),
              S3ObjectPresence::NotFound);
}

TEST_F(S3Integration, LeftoversListsAndAbortsARealMultipartUpload) {
    const std::string key = prefix() + "/unfinished.bin";
    const std::string upload_id = client_->CreateMultipartUpload(bucket_, key);
    ASSERT_FALSE(upload_id.empty());

    std::vector<uint8_t> part(5 * 1024 * 1024, 'p');
    const uint32_t crc = crc32_hw(part.data(), part.size());
    const S3PartResult uploaded =
        client_->UploadPart(bucket_, key, upload_id, 1, part, crc);
    ASSERT_TRUE(uploaded.ok());

    S3ListMultipartUploadsResult listed;
    for (int attempt = 0; attempt < 10; ++attempt) {
        listed = client_->ListMultipartUploads(bucket_, "", "", "", 1);
        if (listed.success && !listed.uploads.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(listed.success) << listed.error_message;
    ASSERT_EQ(listed.uploads.size(), 1u);
    EXPECT_EQ(listed.uploads[0].key, key);
    EXPECT_EQ(listed.uploads[0].upload_id, upload_id);

    LeftoversConfig config;
    config.bucket = bucket_;
    config.region = region();
    config.endpoint = endpoint();
    config.abort_uploads = true;

    LeftoversProgress progress;
    LeftoversResult result = run_leftovers(config, progress, client_);
    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.uploads_listed, 1u);
    EXPECT_EQ(result.uploads_aborted, 1u);
    EXPECT_EQ(result.abort_failures, 0u);

    S3ListMultipartUploadsResult after =
        client_->ListMultipartUploads(bucket_, "", "", "", 1);
    ASSERT_TRUE(after.success) << after.error_message;
    EXPECT_TRUE(after.uploads.empty());
}

// ---------------------------------------------------------------------------
// Multipart upload completion (issue #98)
// ---------------------------------------------------------------------------

TEST_F(S3Integration, AMultiChunkFileRoundTripsAndARetailChangeIsRepaired) {
    // Note what this does NOT cover: the multipart upload path, and therefore
    // #98. Against an endpoint that refuses chunk-checksum discovery (#99),
    // diff_upload_file finds nothing reusable, and a file under 5 GiB then
    // takes the single-PUT fallback every time - so multipart is unreachable
    // here at any size below that. AMultipartUploadOverTheSinglePutLimit below
    // is the test that reaches it; this one covers the ordinary path such a
    // file actually takes.
    const size_t size = static_cast<size_t>(DEFAULT_CHUNK_SIZE) * 2 + 4096;
    const fs::path file = dir_ / "multipart.bin";
    write_file(file, size);

    SyncConfig config = sync_config();
    SyncProgress progress;
    SyncResult first = run_sync(config, progress);
    ASSERT_TRUE(first.success) << "first upload failed";

    // Change the tail only, so the re-sync reuses the leading chunks through
    // UploadPartCopy and uploads the rest - the mixed path, where copied parts
    // and uploaded parts must both carry a checksum into completion.
    {
        std::ofstream out(file, std::ios::binary | std::ios::in | std::ios::ate);
        const std::vector<char> tail(4096, 'Z');
        out.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    }
    const int64_t changed_size = static_cast<int64_t>(fs::file_size(file));

    SyncProgress progress2;
    SyncResult second = run_sync(config, progress2);
    EXPECT_TRUE(second.success) << "differential re-sync failed";
    EXPECT_EQ(second.files_failed, 0);

    EXPECT_EQ(client_->GetObjectSize(bucket_, prefix() + "/multipart.bin"), changed_size)
        << "the stored object is not the size of the file that was synced";
}

TEST_F(S3Integration, AMultipartUploadOverTheSinglePutLimitCompletes) {
    // The regression for #98, and the only test here that reaches the multipart
    // upload path at all - see the note above. The upload declares CRC32, so
    // completion has to name each part's checksum; when it did not, the service
    // answered InvalidPart after all 641 parts had already been sent. It is
    // also the regression for #96/#91: before that fix this file had no route
    // at all, because the single-PUT fallback was taken unconditionally.
    //
    // Opt-in: it moves 5 GiB through the endpoint and wants that much room
    // wherever the emulator keeps its data. Roughly 25s against a local MinIO.
    if (!std::getenv("MITO_TEST_S3_LARGE")) {
        GTEST_SKIP() << "set MITO_TEST_S3_LARGE=1 to run the >5 GiB multipart upload";
    }

    const fs::path file = dir_ / "huge.bin";
    {
        std::ofstream out(file, std::ios::binary);
        ASSERT_TRUE(out.good());
    }
    // Sparse: the length is real, the bytes are holes, so this costs no local
    // disk. The endpoint still stores all of it.
    const int64_t size = static_cast<int64_t>(kMaxSinglePutBytes) + 1;
    if (::truncate(file.c_str(), static_cast<off_t>(size)) != 0) {
        GTEST_SKIP() << "filesystem does not support sparse files this large";
    }

    SyncConfig config = sync_config();
    SyncProgress progress;
    SyncResult result = run_sync(config, progress);

    EXPECT_TRUE(result.success) << "a file above the single-PUT limit did not upload";
    EXPECT_EQ(result.files_failed, 0);
    EXPECT_EQ(client_->GetObjectSize(bucket_, prefix() + "/huge.bin"), size)
        << "the stored object is not the size of the file that was synced";
}

// ---------------------------------------------------------------------------
// Remote chunk checksums (issue #99)
// ---------------------------------------------------------------------------

TEST_F(S3Integration, ChunkChecksumsEitherMatchTheFileOrAreRefusedOutright) {
    // GetChunkCRC32s derives remote checksums via UploadPartCopy. Endpoints
    // differ in three ways here: AWS answers with the checksum, some gateways
    // accept the copy and omit it, and MinIO rejects the copy outright (#99).
    //
    // Only one outcome is unacceptable: a checksum that is neither the file's
    // nor an honest refusal. A fabricated value would silently mark identical
    // files as different, which is the failure this whole tool exists to avoid.
    const size_t size = static_cast<size_t>(DEFAULT_CHUNK_SIZE) + 1024;
    const fs::path file = dir_ / "chunked.bin";
    write_file(file, size);

    SyncConfig config = sync_config();
    SyncProgress progress;
    ASSERT_TRUE(run_sync(config, progress).success);

    const std::vector<uint32_t> local =
        compute_crc32_chunks_boost_asio(file.string(), {}, nullptr, DEFAULT_CHUNK_SIZE);
    ASSERT_EQ(local.size(), 2u);

    const std::vector<uint32_t> remote = client_->GetChunkCRC32s(
        bucket_, prefix() + "/chunked.bin", static_cast<int64_t>(size), {}, nullptr, false,
        4, false, DEFAULT_CHUNK_SIZE);

    if (remote.empty()) {
        SUCCEED() << "endpoint does not support server-side chunk checksums; "
                     "refusing is the correct answer and callers fall back";
        return;
    }
    ASSERT_EQ(remote.size(), local.size());
    EXPECT_EQ(remote, local) << "remote chunk checksums disagree with the bytes "
                                "that were uploaded";
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST_F(S3Integration, SyncUploadsSkipsUnchangedAndRepairsAChange) {
    const fs::path file = dir_ / "small.bin";
    write_file(file, 4096);
    backdate(file, 10);

    SyncConfig config = sync_config();
    SyncProgress p1;
    SyncResult first = run_sync(config, p1);
    ASSERT_TRUE(first.success);
    EXPECT_EQ(first.files_uploaded, 1);

    // Unchanged: the second run must not re-upload.
    SyncProgress p2;
    SyncResult second = run_sync(config, p2);
    ASSERT_TRUE(second.success);
    EXPECT_EQ(second.files_uploaded, 0) << "an unchanged file was uploaded again";
    EXPECT_EQ(second.files_skipped, 1);

    // Changed: the third run must repair it.
    write_file(file, 8192, /*seed=*/7);
    SyncProgress p3;
    SyncResult third = run_sync(config, p3);
    ASSERT_TRUE(third.success);
    EXPECT_EQ(client_->GetObjectSize(bucket_, prefix() + "/small.bin"), 8192);
}

TEST_F(S3Integration, AnEmptyFileRoundTrips) {
    // Zero-length objects have their own handling in several places and are a
    // recurring source of off-by-one behaviour.
    const fs::path file = dir_ / "empty.bin";
    { std::ofstream out(file, std::ios::binary); }

    SyncConfig config = sync_config();
    SyncProgress progress;
    ASSERT_TRUE(run_sync(config, progress).success);
    EXPECT_EQ(client_->GetObjectSize(bucket_, prefix() + "/empty.bin"), 0);
}

TEST_F(S3Integration, RangedReadsReturnTheRequestedBytes) {
    // GetObjectRange must return exactly the bytes asked for, and must verify
    // the response describes that range rather than trusting its length (#76).
    const fs::path file = dir_ / "ranged.bin";
    write_file(file, 64 * 1024);

    SyncConfig config = sync_config();
    SyncProgress progress;
    ASSERT_TRUE(run_sync(config, progress).success);

    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> expected(4096);
    in.seekg(1000);
    in.read(reinterpret_cast<char*>(expected.data()), 4096);

    const std::vector<uint8_t> got =
        client_->GetObjectRange(bucket_, prefix() + "/ranged.bin", 1000, 1000 + 4096 - 1);
    ASSERT_EQ(got.size(), expected.size());
    EXPECT_EQ(got, expected) << "a ranged read returned the wrong bytes";

    const std::vector<uint8_t> first =
        client_->GetObjectRange(bucket_, prefix() + "/ranged.bin", 0, 0);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0], static_cast<uint8_t>(0));

    const std::vector<uint8_t> last =
        client_->GetObjectRange(bucket_, prefix() + "/ranged.bin", 64 * 1024 - 1,
                                64 * 1024 - 1);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[0], static_cast<uint8_t>(((64 * 1024 - 1) * 31) & 0xFF));

    EXPECT_TRUE(client_->GetObjectRange(bucket_, prefix() + "/ranged.bin",
                                        64 * 1024, 64 * 1024)
                    .empty())
        << "a range past EOF must fail rather than clamp to the last byte";
}

}  // namespace

// The SDK has to be running for any of this. Registered as a global environment
// so it is initialised once even when only a subset of tests is selected.
namespace {
class AwsEnvironment : public ::testing::Environment {
public:
    void SetUp() override { Aws::InitAPI(options_); }
    void TearDown() override { Aws::ShutdownAPI(options_); }

private:
    Aws::SDKOptions options_;
};
::testing::Environment* const kAwsEnv =
    ::testing::AddGlobalTestEnvironment(new AwsEnvironment);
}  // namespace
