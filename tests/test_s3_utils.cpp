#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <aws/s3/S3Errors.h>

#include "s3_utils.h"
#include "temp_test_path.h"

namespace {

Aws::S3::S3Error make_error(Aws::S3::S3Errors type, const std::string& message = "") {
    Aws::Client::AWSError<Aws::S3::S3Errors> err(type, "TestError", message.c_str(), false);
    return Aws::S3::S3Error(err);
}

// An error whose type is benign, so only the message can make it retryable.
Aws::S3::S3Error message_only_error(const std::string& message) {
    return make_error(Aws::S3::S3Errors::UNKNOWN, message);
}

class TempFile {
public:
    explicit TempFile(const std::string& contents) {
        // The address of `this` is not entropy: two runs of the same binary
        // hand out the same addresses, so it only separated live objects
        // within one process, never two processes (issue #42).
        static std::atomic<unsigned> counter{0};
        path_ = mito_test_temp_path("mito_s3utils").string() + "_" +
                std::to_string(counter.fetch_add(1));
        std::ofstream out(path_, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path_, ec); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

}  // namespace

// ============================================================================
// Shutdown flag
// ============================================================================

// This flag is process-global, so each test restores it.
class ShutdownFlag : public ::testing::Test {
protected:
    void TearDown() override { ResetShutdown(); }
};

TEST_F(ShutdownFlag, StartsClearAfterReset) {
    ResetShutdown();
    EXPECT_FALSE(IsShutdownRequested());
}

TEST_F(ShutdownFlag, RequestThenReset) {
    ResetShutdown();
    RequestShutdown();
    EXPECT_TRUE(IsShutdownRequested());
    ResetShutdown();
    EXPECT_FALSE(IsShutdownRequested());
}

TEST_F(ShutdownFlag, RequestIsIdempotent) {
    ResetShutdown();
    RequestShutdown();
    RequestShutdown();
    EXPECT_TRUE(IsShutdownRequested());
}

TEST_F(ShutdownFlag, IsVisibleAcrossThreads) {
    ResetShutdown();
    std::thread setter([] { RequestShutdown(); });
    setter.join();
    EXPECT_TRUE(IsShutdownRequested());
}

// ============================================================================
// GetJitter
// ============================================================================

TEST(GetJitterTest, StaysWithinQuarterOfBaseDelay) {
    // max_jitter = base/4 + 1, and the result is modulo that, so the value is
    // always in [0, base/4].
    for (int base : {0, 1, 4, 100, 1000, 5000}) {
        int max_exclusive = base / 4 + 1;
        for (int i = 0; i < 200; ++i) {
            int j = GetJitter(base);
            EXPECT_GE(j, 0) << "base=" << base;
            EXPECT_LT(j, max_exclusive) << "base=" << base;
        }
    }
}

TEST(GetJitterTest, ZeroAndTinyBaseAlwaysYieldZero) {
    // base/4 + 1 == 1 for base < 4, so the only possible value is 0.
    for (int base : {0, 1, 2, 3}) {
        for (int i = 0; i < 50; ++i) EXPECT_EQ(GetJitter(base), 0) << "base=" << base;
    }
}

TEST(GetJitterTest, ProducesVariationOverALargeRange) {
    std::set<int> seen;
    for (int i = 0; i < 500; ++i) seen.insert(GetJitter(4000));
    // With 1000 possible values, 500 draws should not collapse to one.
    EXPECT_GT(seen.size(), 1u);
}

// ============================================================================
// IsRetryableS3Error
// ============================================================================

TEST(IsRetryableS3ErrorTest, TransientErrorTypesRetry) {
    const Aws::S3::S3Errors retryable[] = {
        Aws::S3::S3Errors::NETWORK_CONNECTION,
        Aws::S3::S3Errors::REQUEST_TIMEOUT,
        Aws::S3::S3Errors::THROTTLING,
        Aws::S3::S3Errors::SLOW_DOWN,
        Aws::S3::S3Errors::INTERNAL_FAILURE,
        Aws::S3::S3Errors::SERVICE_UNAVAILABLE,
    };
    for (auto t : retryable) {
        EXPECT_TRUE(IsRetryableS3Error(make_error(t)))
            << "error type " << static_cast<int>(t) << " should retry";
    }
}

TEST(IsRetryableS3ErrorTest, PermanentErrorsDoNotRetry) {
    EXPECT_FALSE(IsRetryableS3Error(make_error(Aws::S3::S3Errors::NO_SUCH_BUCKET)));
    EXPECT_FALSE(IsRetryableS3Error(make_error(Aws::S3::S3Errors::NO_SUCH_KEY)));
    EXPECT_FALSE(IsRetryableS3Error(make_error(Aws::S3::S3Errors::ACCESS_DENIED)));
    EXPECT_FALSE(IsRetryableS3Error(make_error(Aws::S3::S3Errors::UNKNOWN, "malformed request")));
}

TEST(IsRetryableS3ErrorTest, DnsAndConnectionMessagesRetry) {
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("Could not resolve host: s3.amazonaws.com")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("getaddrinfo failed")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("Could not connect to server")));
}

TEST(IsRetryableS3ErrorTest, CurlErrorCodesRetry) {
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("curlCode: 6, Couldn't resolve host")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("curlCode: 7, Failed to connect")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("curlCode: 28, Timeout was reached")));
}

TEST(IsRetryableS3ErrorTest, ServerSideHintsRetryInBothCases) {
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("We encountered an internal error")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("Internal error, try later")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("Please try again shortly")));
}

TEST(IsRetryableS3ErrorTest, UnrelatedMessagesDoNotRetry) {
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("The specified key does not exist")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 35, SSL connect error")));
}

// Regression: curl codes are matched in full, not by prefix. A substring test
// for "curlCode: 6" also fires on 60..69, and "curlCode: 7" on 70..79 - ranges
// that contain permanent failures which can never succeed on retry.
TEST(IsRetryableS3ErrorTest, TwoDigitCodesAreNotMatchedByASingleDigitPrefix) {
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 60, SSL peer certificate was not OK")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 67, LOGIN DENIED")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 77, problem with the CA cert")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 63, filesize exceeded")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 78, remote file not found")));
}

TEST(IsRetryableS3ErrorTest, ThreeDigitCodesDoNotMatchTwoDigitPrefixes) {
    // Guards the "curlCode: 28" check against a hypothetical 280+.
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 280, not a real code")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 600, not a real code")));
}

TEST(IsRetryableS3ErrorTest, CurlCodeIsFoundWithoutATrailingDelimiter) {
    // The code may end the message, so the match must not depend on a comma.
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("curlCode: 6")));
    EXPECT_TRUE(IsRetryableS3Error(message_only_error("curlCode: 28")));
}

TEST(IsRetryableS3ErrorTest, MalformedCurlCodeIsIgnored) {
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: ")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: abc")));
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode:6")));  // marker needs the space
    // Absurdly long digit runs overflow stoi; must not throw or retry.
    EXPECT_FALSE(IsRetryableS3Error(message_only_error("curlCode: 99999999999999999999")));
}

// ============================================================================
// GetLocalFileSize
// ============================================================================

TEST(GetLocalFileSizeTest, ReturnsExactByteCount) {
    TempFile f("hello world");
    EXPECT_EQ(GetLocalFileSize(f.path()), 11);
}

TEST(GetLocalFileSizeTest, EmptyFileIsZeroNotAnError) {
    TempFile f("");
    EXPECT_EQ(GetLocalFileSize(f.path()), 0);
}

TEST(GetLocalFileSizeTest, HandlesBinaryContentIncludingNulls) {
    TempFile f(std::string("\0\1\2\0\3", 5));
    EXPECT_EQ(GetLocalFileSize(f.path()), 5);
}

TEST(GetLocalFileSizeTest, MissingFileReturnsMinusOne) {
    EXPECT_EQ(GetLocalFileSize("/nonexistent/path/to/nothing.bin"), -1);
}

TEST(GetLocalFileSizeTest, EmptyPathReturnsMinusOne) {
    EXPECT_EQ(GetLocalFileSize(""), -1);
}

TEST(GetLocalFileSizeTest, DirectoryReturnsItsStatSizeNotAnError) {
    // stat() succeeds on a directory, so this returns a non-negative size
    // rather than -1. Documents current behaviour.
    std::string tmp = std::filesystem::temp_directory_path().string();
    EXPECT_GE(GetLocalFileSize(tmp), 0);
}

// ============================================================================
// UploadRegistry
// ============================================================================

// The registry is a process-wide singleton; each test removes what it adds.
class Registry : public ::testing::Test {
protected:
    static ActiveUpload upload(const char* id) {
        ActiveUpload u;
        u.client = nullptr;      // never dereferenced: abort_all() is not called here
        u.bucket = "test-bucket";
        u.key = "some/key";
        u.upload_id = id;
        return u;
    }
};

TEST_F(Registry, RegisterThenUnregisterRestoresCount) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    reg.register_upload(upload("upload-1"));
    EXPECT_EQ(reg.count(), before + 1);

    reg.unregister_upload("upload-1");
    EXPECT_EQ(reg.count(), before);
}

TEST_F(Registry, TracksSeveralUploadsIndependently) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    reg.register_upload(upload("a"));
    reg.register_upload(upload("b"));
    reg.register_upload(upload("c"));
    EXPECT_EQ(reg.count(), before + 3);

    reg.unregister_upload("b");
    EXPECT_EQ(reg.count(), before + 2);

    reg.unregister_upload("a");
    reg.unregister_upload("c");
    EXPECT_EQ(reg.count(), before);
}

TEST_F(Registry, RegisteringTheSameIdTwiceDoesNotDoubleCount) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    reg.register_upload(upload("dup"));
    reg.register_upload(upload("dup"));
    EXPECT_EQ(reg.count(), before + 1);

    reg.unregister_upload("dup");
    EXPECT_EQ(reg.count(), before);
}

TEST_F(Registry, UnregisteringAnUnknownIdIsHarmless) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();
    reg.unregister_upload("never-registered");
    EXPECT_EQ(reg.count(), before);
}

TEST_F(Registry, InstanceIsASingleton) {
    EXPECT_EQ(&UploadRegistry::instance(), &UploadRegistry::instance());
}

TEST_F(Registry, ConcurrentRegistrationIsThreadSafe) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&reg, t] {
            for (int i = 0; i < 25; ++i) {
                ActiveUpload u;
                u.client = nullptr;
                u.bucket = "b";
                u.key = "k";
                u.upload_id = ("t" + std::to_string(t) + "-" + std::to_string(i)).c_str();
                reg.register_upload(u);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(reg.count(), before + 200);

    for (int t = 0; t < 8; ++t)
        for (int i = 0; i < 25; ++i)
            reg.unregister_upload(("t" + std::to_string(t) + "-" + std::to_string(i)).c_str());
    EXPECT_EQ(reg.count(), before);
}

// ============================================================================
// UploadGuard
// ============================================================================

TEST_F(Registry, GuardReleaseUnregistersTheUpload) {
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    reg.register_upload(upload("guarded"));
    {
        UploadGuard guard("guarded");
        EXPECT_EQ(reg.count(), before + 1);
        guard.release();
    }
    EXPECT_EQ(reg.count(), before);
}

TEST_F(Registry, GuardWithoutReleaseLeavesUploadRegisteredForLaterCleanup) {
    // On an exceptional path the guard deliberately does NOT unregister, so
    // abort_all() can still find and clean up the upload at exit.
    auto& reg = UploadRegistry::instance();
    size_t before = reg.count();

    reg.register_upload(upload("leaked"));
    {
        UploadGuard guard("leaked");
        (void)guard;
    }
    EXPECT_EQ(reg.count(), before + 1);

    reg.unregister_upload("leaked");  // clean up after ourselves
    EXPECT_EQ(reg.count(), before);
}


// ============================================================================
// S3MultipartCopy chunk id validation (issues #26, #52)
// ============================================================================
//
// S3ClientImpl validates before it ever constructs an S3MultipartCopy, so these
// guards are unreachable through the adapter - which is exactly why they need
// their own tests. S3MultipartCopy is public in s3_utils.h, and an out-of-range
// id here is not a failed request but a heap write past the end of the results
// vector, from a worker thread.
//
// No SDK client is needed: the guard runs before any request is made, so a null
// client is never dereferenced on these paths.

TEST(S3MultipartCopyChunkIds, RejectsAnIdPastTheEndWithoutOpeningAnUpload) {
    S3MultipartCopy copier(nullptr, "bucket", "src", "dst",
                           /*filesize=*/4096, /*debug=*/false, /*chunk_size=*/1024);
    EXPECT_TRUE(copier.GetHashes({4}).empty()) << "four chunks exist, so id 4 is past the end";
    EXPECT_TRUE(copier.GetHashes({0, 9}).empty()) << "one bad id fails the whole request";
}

TEST(S3MultipartCopyChunkIds, RejectsANegativeId) {
    S3MultipartCopy copier(nullptr, "bucket", "src", "dst", 4096, false, 1024);
    EXPECT_TRUE(copier.GetHashes({-1}).empty());
}

TEST(S3MultipartCopyChunkIds, RejectsAnIdThatWouldOverflowAByteOffset) {
    S3MultipartCopy copier(nullptr, "bucket", "src", "dst", 4096, false, 1024);
    EXPECT_TRUE(copier.GetHashes({std::numeric_limits<int64_t>::max()}).empty());
}

TEST(S3MultipartCopyChunkIds, HugeObjectSizeDoesNotOverflowTheChunkCountGuard) {
    S3MultipartCopy copier(nullptr, "bucket", "src", "dst",
                           std::numeric_limits<int64_t>::max(), false, 1024);
    const std::vector<int64_t> too_large_id = {std::numeric_limits<int64_t>::max()};

    EXPECT_NO_THROW({
        EXPECT_TRUE(copier.GetHashes(too_large_id).empty());
        EXPECT_TRUE(copier.ParallelUploadPartCopyRequests("upload-id", too_large_id).empty());
        EXPECT_TRUE(copier.ParallelUploadPartCopyRequestsThreadPool(
                              "upload-id", 1, false, too_large_id)
                        .empty());
    });
}

TEST(S3MultipartCopyChunkIds, RejectsANegativeObjectSize) {
    // A negative size makes a negative chunk count, which used to reach
    // std::vector's constructor and throw length_error out of GetHashes.
    S3MultipartCopy copier(nullptr, "bucket", "src", "dst", /*filesize=*/-1, false, 1024);
    EXPECT_NO_THROW({ EXPECT_TRUE(copier.GetHashes({}).empty()); });
}

// ---------------------------------------------------------------------------
// Telling "I want a checksum you did not send" apart from "yours was wrong"
// (issue #99)
// ---------------------------------------------------------------------------
//
// Both arrive as a 400 mentioning a checksum, and they need opposite handling:
// the first is a capability gap to explain once and stop retrying, the second
// is a real integrity failure that must not be explained away as one.

namespace {

Aws::S3::S3Error bad_request(const std::string& message) {
    Aws::Client::AWSError<Aws::S3::S3Errors> err(
        Aws::S3::S3Errors::INVALID_PARAMETER_VALUE, "InvalidArgument", message.c_str(), false);
    err.SetResponseCode(Aws::Http::HttpResponseCode::BAD_REQUEST);
    return Aws::S3::S3Error(err);
}

}  // namespace

TEST(IsMissingChecksumSupportErrorTest, MinioRejectionIsRecognised) {
    // The message MinIO actually returns, verbatim.
    EXPECT_TRUE(IsMissingChecksumSupportError(bad_request(
        "Invalid arguments provided for bucket/key: (checksum missing, want \"CRC32\", got \"\")")));
}

TEST(IsMissingChecksumSupportErrorTest, OtherWordingsForAnAbsentChecksumAreRecognised) {
    EXPECT_TRUE(IsMissingChecksumSupportError(bad_request("Checksum is required for this request")));
    EXPECT_TRUE(IsMissingChecksumSupportError(bad_request("checksum algorithm unsupported")));
    EXPECT_TRUE(IsMissingChecksumSupportError(bad_request("CRC32 checksums are not supported here")));
}

TEST(IsMissingChecksumSupportErrorTest, AMismatchIsNotACapabilityGap) {
    // The distinction that matters: a value that did not match is a genuine
    // integrity failure. Reporting it as "this endpoint cannot do checksums"
    // would be wrong, and would go quiet about corruption.
    EXPECT_FALSE(IsMissingChecksumSupportError(bad_request(
        "The CRC32 checksum you specified did not match what we received")));
    EXPECT_FALSE(IsMissingChecksumSupportError(bad_request("BadDigest: checksum mismatch")));
    EXPECT_FALSE(IsMissingChecksumSupportError(bad_request("Invalid checksum value")));
}

TEST(IsMissingChecksumSupportErrorTest, UnrelatedBadRequestsAreNotMatched) {
    EXPECT_FALSE(IsMissingChecksumSupportError(bad_request("Invalid part number")));
    EXPECT_FALSE(IsMissingChecksumSupportError(bad_request("")));
}

TEST(IsMissingChecksumSupportErrorTest, OnlyABadRequestQualifies) {
    // The same wording over a 500 is a server fault, which is retryable and
    // must not be mistaken for a permanent statement about the endpoint.
    Aws::Client::AWSError<Aws::S3::S3Errors> err(
        Aws::S3::S3Errors::INTERNAL_FAILURE, "InternalError", "checksum missing", false);
    err.SetResponseCode(Aws::Http::HttpResponseCode::INTERNAL_SERVER_ERROR);
    EXPECT_FALSE(IsMissingChecksumSupportError(Aws::S3::S3Error(err)));
}
