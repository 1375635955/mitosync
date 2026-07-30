#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

// Whether this translation unit is compiled under AddressSanitizer. GCC spells
// it one way and clang another, and clang's __has_feature cannot be probed with
// a plain && because GCC does not define it at all.
#if defined(__SANITIZE_ADDRESS__)
#define MITO_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MITO_UNDER_ASAN 1
#endif
#endif

#include <atomic>
#include <mutex>
#include <cstdlib>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3Errors.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

#include "crc32_hw.h"
#include "temp_test_path.h"
#include "s3_client_testing.h"
#include "app_settings.h"
#include "s3_utils.h"

// ============================================================================
// SDK lifetime
// ============================================================================

// The SDK must be initialised before any client is constructed, and shut down
// once at the end. No production test binary does this, so it is owned here.
class AwsSdkEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Without these, constructing a client configuration probes the EC2
        // instance metadata service for a region and blocks until it times out,
        // costing seconds per client on a machine that is not an EC2 instance.
        ::setenv("AWS_EC2_METADATA_DISABLED", "true", 1);
        ::setenv("AWS_REGION", "us-east-1", 1);
        ::setenv("AWS_DEFAULT_REGION", "us-east-1", 1);
        Aws::InitAPI(options_);
    }
    void TearDown() override { Aws::ShutdownAPI(options_); }

private:
    Aws::SDKOptions options_;
};

static ::testing::Environment* const kAwsEnv =
    ::testing::AddGlobalTestEnvironment(new AwsSdkEnvironment);

namespace {

// A stand-in for the SDK client. Every operation returns a canned outcome and
// records what it was asked for, so the adapter's request building, response
// mapping and retry loops can be driven without a network.
//
// The SDK declares these operations virtual, so overriding them intercepts the
// call before any HTTP work happens.
class FakeS3Client : public Aws::S3::S3Client {
public:
    FakeS3Client()
        : Aws::S3::S3Client(Aws::Auth::AWSCredentials("test-key", "test-secret"),
                            nullptr,
                            Aws::S3::S3ClientConfiguration()) {}

    // ---- recorded calls -----------------------------------------------
    mutable int head_calls = 0;
    mutable int get_calls = 0;
    mutable int list_calls = 0;
    mutable int put_calls = 0;
    mutable int delete_calls = 0;
    mutable int delete_objects_calls = 0;
    mutable int copy_calls = 0;
    mutable int create_mpu_calls = 0;

    mutable std::string last_bucket;
    mutable std::string last_key;
    mutable std::string last_range;
    mutable std::string last_prefix;
    mutable std::string last_delimiter;
    mutable std::string last_continuation_token;
    mutable int last_max_keys = 0;
    mutable std::string last_copy_source;
    mutable std::string last_checksum_crc32;
    mutable int64_t last_content_length = 0;
    mutable int64_t last_body_size = 0;
    mutable uint32_t last_body_crc32 = 0;

    // ---- canned behaviour ---------------------------------------------
    // Number of leading calls that fail before succeeding. Lets a test drive
    // the retry loop deterministically.
    int head_failures_before_success = 0;
    Aws::S3::S3Errors failure_type = Aws::S3::S3Errors::NETWORK_CONNECTION;
    std::string failure_message = "curlCode: 7, Failed to connect";
    bool head_should_succeed = true;

    int64_t content_length = 0;
    std::string body_payload;
    // Real S3 answers every ranged GET with Content-Range. The fake does too,
    // derived from the request, so a test that asks for a range gets a response
    // that says so. The knobs model the endpoints that misbehave: one that
    // omits the header, and one that claims a different range from the bytes it
    // sent (issue #76).
    bool omit_content_range = false;
    std::string content_range_override;

    bool list_should_succeed = true;
    std::vector<std::pair<std::string, int64_t>> list_contents;  // key, size
    std::vector<std::string> list_prefixes;
    bool list_truncated = false;
    std::string list_next_token;

    bool put_should_succeed = true;
    bool delete_should_succeed = true;
    bool copy_should_succeed = true;

    // ---- overrides -----------------------------------------------------
    Aws::S3::Model::HeadObjectOutcome HeadObject(
        const Aws::S3::Model::HeadObjectRequest& request) const override {
        ++head_calls;
        last_bucket = request.GetBucket();
        last_key = request.GetKey();

        if (head_calls <= head_failures_before_success) return error<Aws::S3::Model::HeadObjectResult>();
        if (!head_should_succeed) return error<Aws::S3::Model::HeadObjectResult>();

        Aws::S3::Model::HeadObjectResult result;
        result.SetContentLength(content_length);
        return Aws::S3::Model::HeadObjectOutcome(std::move(result));
    }

    Aws::S3::Model::GetObjectOutcome GetObject(
        const Aws::S3::Model::GetObjectRequest& request) const override {
        ++get_calls;
        last_bucket = request.GetBucket();
        last_key = request.GetKey();
        last_range = request.GetRange();

        if (!head_should_succeed) return error<Aws::S3::Model::GetObjectResult>();

        Aws::S3::Model::GetObjectResult result;
        auto stream = Aws::New<Aws::StringStream>("FakeS3Client");
        *stream << body_payload;
        result.ReplaceBody(stream);
        result.SetContentLength(static_cast<long long>(body_payload.size()));

        if (!content_range_override.empty()) {
            result.SetContentRange(content_range_override.c_str());
        } else if (!omit_content_range) {
            // Echo the requested range, the way a conforming endpoint would.
            const std::string requested(request.GetRange().c_str());
            const size_t eq = requested.find('=');
            if (eq != std::string::npos) {
                result.SetContentRange(("bytes " + requested.substr(eq + 1) + "/*").c_str());
            }
        }
        return Aws::S3::Model::GetObjectOutcome(std::move(result));
    }

    Aws::S3::Model::ListObjectsV2Outcome ListObjectsV2(
        const Aws::S3::Model::ListObjectsV2Request& request) const override {
        ++list_calls;
        last_bucket = request.GetBucket();
        last_prefix = request.GetPrefix();
        last_delimiter = request.GetDelimiter();
        last_continuation_token = request.GetContinuationToken();
        last_max_keys = request.GetMaxKeys();

        if (!list_should_succeed) return error<Aws::S3::Model::ListObjectsV2Result>();

        Aws::S3::Model::ListObjectsV2Result result;
        Aws::Vector<Aws::S3::Model::Object> contents;
        for (const auto& [key, size] : list_contents) {
            Aws::S3::Model::Object o;
            o.SetKey(key.c_str());
            o.SetSize(size);
            contents.push_back(o);
        }
        result.SetContents(contents);

        Aws::Vector<Aws::S3::Model::CommonPrefix> prefixes;
        for (const auto& p : list_prefixes) {
            Aws::S3::Model::CommonPrefix cp;
            cp.SetPrefix(p.c_str());
            prefixes.push_back(cp);
        }
        result.SetCommonPrefixes(prefixes);
        result.SetIsTruncated(list_truncated);
        result.SetNextContinuationToken(list_next_token.c_str());
        return Aws::S3::Model::ListObjectsV2Outcome(std::move(result));
    }

    Aws::S3::Model::PutObjectOutcome PutObject(
        const Aws::S3::Model::PutObjectRequest& request) const override {
        ++put_calls;
        last_bucket = request.GetBucket();
        last_key = request.GetKey();
        last_checksum_crc32 = request.GetChecksumCRC32();
        last_content_length = request.GetContentLength();

        // Drain the body the way the SDK would, so a test can check what would
        // actually have gone over the wire rather than only that a call
        // happened. Checksummed as it is read, so this stays O(1) in memory
        // even for a large upload.
        last_body_size = 0;
        last_body_crc32 = 0;
        if (auto body = request.GetBody()) {
            std::vector<char> buf(1u << 20);
            while (*body) {
                body->read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const std::streamsize got = body->gcount();
                if (got <= 0) break;
                last_body_crc32 = crc32_hw_update(
                    last_body_crc32, reinterpret_cast<const uint8_t*>(buf.data()),
                    static_cast<size_t>(got));
                last_body_size += static_cast<int64_t>(got);
            }
        }

        if (!put_should_succeed) return error<Aws::S3::Model::PutObjectResult>();
        return Aws::S3::Model::PutObjectOutcome(Aws::S3::Model::PutObjectResult{});
    }

    Aws::S3::Model::DeleteObjectOutcome DeleteObject(
        const Aws::S3::Model::DeleteObjectRequest& request) const override {
        ++delete_calls;
        last_bucket = request.GetBucket();
        last_key = request.GetKey();
        if (!delete_should_succeed) return error<Aws::S3::Model::DeleteObjectResult>();
        return Aws::S3::Model::DeleteObjectOutcome(Aws::S3::Model::DeleteObjectResult{});
    }

    // ---- multipart path, used by GetChunkCRC32s for files > chunk_size ----
    // supports_part_checksums = false models an S3-compatible gateway that
    // accepts UploadPartCopy but omits the x-amz-checksum-crc32 header.
    bool supports_part_checksums = true;
    // rejects_part_checksums = true models the other way an endpoint refuses:
    // MinIO answers a copy into a checksum-declaring upload with 400
    // InvalidArgument instead of answering without the header (issue #99).
    bool rejects_part_checksums = false;
    // The message the rejection carries. Defaults to MinIO's, which names a
    // checksum it never received; a mismatch reads differently and must not be
    // mistaken for it.
    std::string rejection_message =
        "Invalid arguments provided for bucket/key: (checksum missing, want \"CRC32\", got \"\")";
    // Request a process shutdown after this many UploadPartCopy calls, to model
    // Ctrl-C arriving partway through a comparison.
    int shutdown_after_n_parts = 0;
    mutable std::atomic<int> upload_part_copy_calls{0};
    mutable int abort_mpu_calls = 0;

    // The checksum this fake returns for a given copy-source range. Tests use it
    // to name the value they expect for a particular chunk, which is what makes
    // "chunk N's value came back in slot N" checkable at all.
    static uint32_t ChecksumForRange(const std::string& range) {
        uint32_t hash = 2166136261u;  // FNV-1a, so the value is stable across runs
        for (unsigned char c : range) {
            hash = (hash ^ c) * 16777619u;
        }
        return hash | 1u;  // never zero
    }

    // The range mito asks for when it wants one chunk of an object.
    static std::string RangeForChunk(int64_t chunk_id, int64_t chunk_size, int64_t file_size) {
        const int64_t start = chunk_id * chunk_size;
        const int64_t end = std::min(start + chunk_size, file_size) - 1;
        return "bytes=" + std::to_string(start) + "-" + std::to_string(end);
    }

    static uint32_t ChecksumForChunk(int64_t chunk_id, int64_t chunk_size, int64_t file_size) {
        return ChecksumForRange(RangeForChunk(chunk_id, chunk_size, file_size));
    }

    Aws::S3::Model::CreateMultipartUploadOutcome CreateMultipartUpload(
        const Aws::S3::Model::CreateMultipartUploadRequest& request) const override {
        ++create_mpu_calls;
        last_bucket = request.GetBucket();
        Aws::S3::Model::CreateMultipartUploadResult result;
        result.SetUploadId("fake-upload-id");
        return Aws::S3::Model::CreateMultipartUploadOutcome(std::move(result));
    }

    Aws::S3::Model::UploadPartCopyOutcome UploadPartCopy(
        const Aws::S3::Model::UploadPartCopyRequest& request) const override {
        ++upload_part_copy_calls;
        (void)request;
        if (shutdown_after_n_parts > 0 && upload_part_copy_calls >= shutdown_after_n_parts) {
            RequestShutdown();
        }
        if (rejects_part_checksums) {
            Aws::Client::AWSError<Aws::S3::S3Errors> err(
                Aws::S3::S3Errors::INVALID_PARAMETER_VALUE, "InvalidArgument",
                rejection_message.c_str(), false);
            err.SetResponseCode(Aws::Http::HttpResponseCode::BAD_REQUEST);
            return Aws::S3::Model::UploadPartCopyOutcome(Aws::S3::S3Error(err));
        }
        Aws::S3::Model::CopyPartResult part;
        if (supports_part_checksums) {
            // A distinct non-zero CRC per part, derived from the byte range the
            // caller asked for rather than from a call counter. Deriving it from
            // the range is what lets a test check that chunk N's value came
            // back for chunk N: with a counter, every value depended only on
            // arrival order, so a result vector could be in any order - or hold
            // the wrong chunk entirely - and still look right (issue #26).
            // A constant of zero would also make a fabricated zero
            // indistinguishable from a computed one, which is the confusion
            // several of these tests exist to catch.
            const uint32_t hash = ChecksumForRange(std::string(request.GetCopySourceRange().c_str()));
            unsigned char raw[4] = {static_cast<unsigned char>((hash >> 24) & 0xFF),
                                    static_cast<unsigned char>((hash >> 16) & 0xFF),
                                    static_cast<unsigned char>((hash >> 8) & 0xFF),
                                    static_cast<unsigned char>(hash & 0xFF)};
            Aws::Utils::ByteBuffer bb(raw, sizeof(raw));
            part.SetChecksumCRC32(Aws::Utils::HashingUtils::Base64Encode(bb));
        }
        Aws::S3::Model::UploadPartCopyResult result;
        result.SetCopyPartResult(part);
        return Aws::S3::Model::UploadPartCopyOutcome(std::move(result));
    }

    Aws::S3::Model::AbortMultipartUploadOutcome AbortMultipartUpload(
        const Aws::S3::Model::AbortMultipartUploadRequest& request) const override {
        ++abort_mpu_calls;
        (void)request;
        return Aws::S3::Model::AbortMultipartUploadOutcome(
            Aws::S3::Model::AbortMultipartUploadResult{});
    }

    Aws::S3::Model::CopyObjectOutcome CopyObject(
        const Aws::S3::Model::CopyObjectRequest& request) const override {
        ++copy_calls;
        last_bucket = request.GetBucket();
        last_key = request.GetKey();
        last_copy_source = request.GetCopySource();
        if (!copy_should_succeed) return error<Aws::S3::Model::CopyObjectResult>();
        return Aws::S3::Model::CopyObjectOutcome(Aws::S3::Model::CopyObjectResult{});
    }

private:
    template <typename ResultT>
    Aws::Utils::Outcome<ResultT, Aws::S3::S3Error> error() const {
        Aws::Client::AWSError<Aws::S3::S3Errors> err(
            failure_type, "FakeError", failure_message.c_str(), false);
        return Aws::Utils::Outcome<ResultT, Aws::S3::S3Error>(Aws::S3::S3Error(err));
    }
};

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old) {
            had_old_ = true;
            old_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool had_old_ = false;
    std::string old_;
};

class OneShotHttpServer {
public:
    OneShotHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error(std::strerror(errno));

        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        if (::listen(listen_fd_, 1) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~OneShotHttpServer() {
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::string request() {
        if (worker_.joinable()) worker_.join();
        return request_;
    }

private:
    void serve() {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd_, &readfds);
        timeval timeout {5, 0};
        if (::select(listen_fd_ + 1, &readfds, nullptr, nullptr, &timeout) <= 0) {
            return;
        }

        int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) return;

        char buf[4096];
        for (;;) {
            const ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request_.append(buf, static_cast<size_t>(n));
            if (request_.find("\r\n\r\n") != std::string::npos) break;
        }

        const std::string body =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
            "<Name>bucket</Name><Prefix></Prefix><KeyCount>0</KeyCount>"
            "<MaxKeys>1000</MaxKeys><IsTruncated>false</IsTruncated>"
            "</ListBucketResult>";
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "Content-Type: application/xml\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < response.size()) {
            const ssize_t n = ::send(client_fd, response.data() + sent, response.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        ::close(client_fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread worker_;
    std::string request_;
};

// Build the real adapter over a fake SDK client.
struct Harness {
    std::shared_ptr<FakeS3Client> fake = std::make_shared<FakeS3Client>();
    std::shared_ptr<IS3Client> client;

    explicit Harness(int max_retries = 0) {
        client = CreateS3ClientForTesting(fake, "us-east-1", false, max_retries);
    }
};

}  // namespace

// ============================================================================
// Factory / SDK construction
// ============================================================================

TEST(S3ClientImplFactory, EndpointOverrideStillHonorsNamedProfileCredentials) {
    const std::string creds_path =
        mito_test_temp_path("mito_endpoint_profile_credentials").string();
    const std::string config_path =
        mito_test_temp_path("mito_endpoint_profile_config").string();
    {
        std::ofstream creds(creds_path);
        ASSERT_TRUE(creds.good());
        creds << "[default]\n"
              << "aws_access_key_id = DEFAULTKEY000000000\n"
              << "aws_secret_access_key = defaultsecretdefaultsecret\n\n"
              << "[endpoint-profile]\n"
              << "aws_access_key_id = PROFILEKEY000000000\n"
              << "aws_secret_access_key = profilesecretprofilesecret\n";
    }
    {
        std::ofstream config(config_path);
        ASSERT_TRUE(config.good());
    }

    ScopedEnv shared_creds("AWS_SHARED_CREDENTIALS_FILE", creds_path.c_str());
    ScopedEnv shared_config("AWS_CONFIG_FILE", config_path.c_str());
    ScopedEnv profile("AWS_PROFILE", "default");
    ScopedEnv env_access_key("AWS_ACCESS_KEY_ID", nullptr);
    ScopedEnv env_secret_key("AWS_SECRET_ACCESS_KEY", nullptr);
    ScopedEnv env_session("AWS_SESSION_TOKEN", nullptr);

    OneShotHttpServer server;
    auto client = CreateS3Client("us-east-1", server.endpoint(), 1, "endpoint-profile");

    S3ListResult result = client->ListObjects("bucket", "", "", "", 1);
    std::remove(creds_path.c_str());
    std::remove(config_path.c_str());

    EXPECT_TRUE(result.success) << result.error_message;
    const std::string request = server.request();
    ASSERT_TRUE(request.find("Authorization:") != std::string::npos ||
                request.find("authorization:") != std::string::npos) << request;
    EXPECT_NE(request.find("Credential=PROFILEKEY000000000/"), std::string::npos)
        << "endpoint override must not make the constructor ignore the named profile";
    EXPECT_EQ(request.find("Credential=DEFAULTKEY000000000/"), std::string::npos)
        << "the request was signed with the default profile instead";
    EXPECT_NE(request.find("GET /bucket?"), std::string::npos)
        << "the endpoint override should still force path-style addressing";
}

// ============================================================================
// GetObjectSize
// ============================================================================

TEST(S3ClientImplGetObjectSize, ReturnsContentLengthOnSuccess) {
    Harness h;
    h.fake->content_length = 4096;
    EXPECT_EQ(h.client->GetObjectSize("my-bucket", "path/to/obj"), 4096);
    EXPECT_EQ(h.fake->head_calls, 1);
}

TEST(S3ClientImplGetObjectSize, PassesBucketAndKeyThrough) {
    Harness h;
    h.fake->content_length = 1;
    h.client->GetObjectSize("bucket-x", "a/b/c.bin");
    EXPECT_EQ(h.fake->last_bucket, "bucket-x");
    EXPECT_EQ(h.fake->last_key, "a/b/c.bin");
}

TEST(S3ClientImplGetObjectSize, ZeroLengthObjectIsNotAnError) {
    Harness h;
    h.fake->content_length = 0;
    EXPECT_EQ(h.client->GetObjectSize("b", "empty"), 0);
}

TEST(S3ClientImplGetObjectSize, ReturnsMinusOneOnFailure) {
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    h.fake->failure_message = "The specified key does not exist";
    EXPECT_EQ(h.client->GetObjectSize("b", "missing"), -1);
}

TEST(S3ClientImplGetObjectSize, PermanentErrorIsNotRetried) {
    Harness h(/*max_retries=*/3);
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    h.fake->failure_message = "Access Denied";
    EXPECT_EQ(h.client->GetObjectSize("b", "k"), -1);
    EXPECT_EQ(h.fake->head_calls, 1) << "a permanent error must not consume the retry budget";
}

TEST(S3ClientImplGetObjectSize, TransientErrorIsRetriedThenSucceeds) {
    Harness h(/*max_retries=*/3);
    h.fake->head_failures_before_success = 2;   // fail twice, then succeed
    h.fake->content_length = 77;
    EXPECT_EQ(h.client->GetObjectSize("b", "k"), 77);
    EXPECT_EQ(h.fake->head_calls, 3);
}

TEST(S3ClientImplGetObjectSize, RetriesAreBoundedByTheBudget) {
    Harness h(/*max_retries=*/2);
    h.fake->head_should_succeed = false;        // always fails, transiently
    EXPECT_EQ(h.client->GetObjectSize("b", "k"), -1);
    // initial attempt + 2 retries
    EXPECT_EQ(h.fake->head_calls, 3);
}

TEST(S3ClientImplGetObjectSize, ZeroRetryBudgetMeansOneAttempt) {
    Harness h(/*max_retries=*/0);
    h.fake->head_should_succeed = false;
    EXPECT_EQ(h.client->GetObjectSize("b", "k"), -1);
    EXPECT_EQ(h.fake->head_calls, 1);
}

// ============================================================================
// GetObjectRange
// ============================================================================

TEST(S3ClientImplGetObjectRange, ReturnsBodyBytes) {
    Harness h;
    h.fake->body_payload = "hello world";
    auto data = h.client->GetObjectRange("b", "k", 0, 10);
    ASSERT_EQ(data.size(), 11u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "hello world");
}

TEST(S3ClientImplGetObjectRange, BuildsAnInclusiveHttpRangeHeader) {
    Harness h;
    h.fake->body_payload = "xyz";
    h.client->GetObjectRange("b", "k", 100, 199);
    EXPECT_EQ(h.fake->last_range, "bytes=100-199");
}

TEST(S3ClientImplGetObjectRange, HandlesBinaryPayloadWithNulls) {
    Harness h;
    h.fake->body_payload = std::string("\x01\x00\x02\x00\x03", 5);
    auto data = h.client->GetObjectRange("b", "k", 0, 4);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[4], 3);
}

TEST(S3ClientImplGetObjectRange, ReturnsEmptyOnFailure) {
    Harness h;
    h.fake->head_should_succeed = false;   // drives GetObject failure too
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    auto data = h.client->GetObjectRange("b", "k", 0, 10);
    EXPECT_TRUE(data.empty());
}

TEST(S3ClientImplGetObjectRange, IntoBufferFillsExactlyTheRequestedRange) {
    Harness h;
    h.fake->body_payload = "abcd";
    std::vector<uint8_t> buffer(1024, 0xFF);   // deliberately oversized
    ASSERT_TRUE(h.client->GetObjectRangeInto("b", "k", 0, 3, buffer));
    ASSERT_EQ(buffer.size(), 4u);
    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "abcd");
}

// ----------------------------------------------------------------------------
// Range validation and short reads (issue #23)
//
// start and end used to be taken on trust: a reversed range built
// "bytes=100-50" and sent it, a negative start built "bytes=-5-10" - which RFC
// 9110 reads as "the last five bytes", so a loosely parsing gateway could
// answer with real data from the wrong part of the object - and
// GetObjectRangeInto sized its buffer from (end - start + 1) cast to size_t
// before checking anything, asking for roughly 2^64 bytes on a reversed range.
// ----------------------------------------------------------------------------

TEST(S3ClientImplGetObjectRange, NegativeStartIsRejectedWithoutARequest) {
    Harness h;
    h.fake->body_payload = "abcdefghij";
    auto data = h.client->GetObjectRange("b", "k", -1, 4);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 0) << "a bad range must not reach the network";
}

TEST(S3ClientImplGetObjectRange, ReversedRangeIsRejectedWithoutARequest) {
    Harness h;
    h.fake->body_payload = "abcdefghij";
    auto data = h.client->GetObjectRange("b", "k", 100, 50);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 0);
}

TEST(S3ClientImplGetObjectRange, ARangeTooLongForInt64IsRejected) {
    Harness h;
    auto data = h.client->GetObjectRange("b", "k", 0, std::numeric_limits<int64_t>::max());
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 0);
}

TEST(S3ClientImplGetObjectRange, AShortBodyIsAFailureNotAPrefix) {
    // The object claims 100 bytes but 10 arrive. Returning the prefix would be
    // compared positionally against the other side and read as a difference in
    // the wrong place.
    Harness h(/*max_retries=*/0);
    h.fake->body_payload = "abcdefghij";  // 10 bytes
    auto data = h.client->GetObjectRange("b", "k", 0, 99);
    EXPECT_TRUE(data.empty()) << "a truncated body must not be returned as data";
    EXPECT_EQ(h.fake->get_calls, 1);
}

TEST(S3ClientImplGetObjectRange, AShortBodyIsRetriedOnceNotFiveTimes) {
    // A cut-off transfer is worth a second attempt. It is not worth six: the
    // usual cause is an object that shrank, which answers identically every
    // time, and the sync callers wrap this in a retry loop of their own - at
    // the default max_retries of 5 that compounded to ~24 requests and ~13s of
    // backoff to reach a conclusion available on the first attempt.
    Harness h(/*max_retries=*/5);
    h.fake->body_payload = "abcdefghij";
    auto data = h.client->GetObjectRange("b", "k", 0, 99);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 2) << "the initial attempt plus exactly one retry";
}

TEST(S3ClientImplGetObjectRange, ARangeIgnoredByTheServerIsNotTreatedAsData) {
    // Some gateways ignore Range and answer 200 with the whole object. Those
    // bytes start at the object's start, not at `start`, so for any start > 0
    // they are simply the wrong data - and there is no point retrying, since it
    // will do the same thing again.
    Harness h(/*max_retries=*/3);
    h.fake->body_payload = "abcdefghij";  // 10 bytes for a 5 byte request
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    EXPECT_TRUE(data.empty()) << "a body longer than the range must not be returned";
    EXPECT_EQ(h.fake->get_calls, 1) << "and must not be retried";
}

TEST(S3ClientImplGetObjectRange, IntoBufferRejectsARangeIgnoredByTheServer) {
    // The dangerous shape of the same thing: Into read exactly as many bytes as
    // it asked for and reported success, so the caller got object[0..5) for a
    // request that asked for object[5..10).
    Harness h;
    h.fake->body_payload = "abcdefghij";
    std::vector<uint8_t> buffer;
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 5, 9, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST(S3ClientImplGetObjectRange, IntoBufferIsEmptiedWhenTheRequestFails) {
    // The buffer is reused across reads, so stale bytes are the previous file's
    // bytes. IS3Client documents an empty buffer on failure; these are the
    // paths that did not honour it.
    Harness h(/*max_retries=*/0);
    h.fake->head_should_succeed = false;  // the fake's knob for "the GET fails too"
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    std::vector<uint8_t> buffer = {1, 2, 3, 4};
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 0, 3, buffer));
    EXPECT_TRUE(buffer.empty()) << "an S3 error must not leave the previous read's bytes";
}

TEST(S3ClientImplGetObjectRange, IntoBufferDoesNotRetryAnImpossibleAllocation) {
    // (1, INT64_MAX) is arithmetically valid but cannot be buffered. It used to
    // throw inside the loop, be swallowed, and be retried with backoff.
#ifdef MITO_UNDER_ASAN
    // ASan's allocator aborts the process on a request this size rather than
    // letting operator new throw, so the throw-and-recover path cannot be
    // exercised under it. The plain build still covers this.
    GTEST_SKIP() << "allocation failure is not recoverable under AddressSanitizer";
#else
    Harness h(/*max_retries=*/5);
    std::vector<uint8_t> buffer = {9, 9, 9};
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 1,
                                              std::numeric_limits<int64_t>::max(), buffer));
    EXPECT_TRUE(buffer.empty());
    EXPECT_LE(h.fake->get_calls, 1) << "an allocation that cannot succeed must not be retried";
#endif
}

TEST(S3ClientImplGetObjectRange, AnExactBodyStillSucceeds) {
    Harness h;
    h.fake->body_payload = "abcdefghij";
    auto data = h.client->GetObjectRange("b", "k", 0, 9);
    ASSERT_EQ(data.size(), 10u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "abcdefghij");
}

TEST(S3ClientImplGetObjectRange, IntoBufferRejectsAReversedRangeWithoutAllocating) {
    // (end - start + 1) as size_t is about 2^64 here. Before validation this
    // reached buffer.resize() and threw, which the retry loop then swallowed
    // and repeated.
    Harness h;
    std::vector<uint8_t> buffer(8, 0xFF);
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 100, 50, buffer));
    EXPECT_TRUE(buffer.empty()) << "a failed read must not leave stale bytes behind";
    EXPECT_EQ(h.fake->get_calls, 0);
}

TEST(S3ClientImplGetObjectRange, IntoBufferReportsAShortReadAsFailure) {
    // This is the one that could corrupt a comparison quietly: the buffer used
    // to be shrunk to whatever arrived and the call still returned true.
    Harness h(/*max_retries=*/0);
    h.fake->body_payload = "abcd";  // 4 bytes
    std::vector<uint8_t> buffer;
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 0, 99, buffer));
    EXPECT_TRUE(buffer.empty()) << "no partial data may survive a failed read";
}

// ----------------------------------------------------------------------------
// The response has to prove which bytes it carries (issue #76)
//
// #23 made a body longer or shorter than the requested span a failure, which
// catches a gateway that ignores Range and returns the whole object - unless
// the whole object happens to be exactly as long as the span asked for. Then
// "bytes=5-9" comes back as object[0..5): right length, wrong offset, and
// callers compare positionally.
// ----------------------------------------------------------------------------

TEST(S3ClientImplGetObjectRange, AnOffsetReadWithoutContentRangeIsRejected) {
    Harness h(/*max_retries=*/3);
    h.fake->body_payload = "fghij";        // five bytes, the requested span
    h.fake->omit_content_range = true;     // ...but nothing says where they start
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    EXPECT_TRUE(data.empty()) << "nothing proves these are the bytes at offset 5";
    EXPECT_EQ(h.fake->get_calls, 1) << "and it must not be retried";
}

TEST(S3ClientImplGetObjectRange, AnOffsetReadWithMatchingContentRangeSucceeds) {
    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->content_range_override = "bytes 5-9/100";
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "fghij");
}

TEST(S3ClientImplGetObjectRange, ContentRangeNamingDifferentBytesIsRejected) {
    // The precise shape #76 describes: correct length, wrong offset, and the
    // response says so.
    Harness h;
    h.fake->body_payload = "abcde";
    h.fake->content_range_override = "bytes 0-4/5";
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    EXPECT_TRUE(data.empty()) << "a response naming other bytes is not this range";
}

TEST(S3ClientImplGetObjectRange, AnUnreadableContentRangeIsRejected) {
    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->content_range_override = "pages 5-9/100";
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    EXPECT_TRUE(data.empty());
}

TEST(S3ClientImplGetObjectRange, AReadFromZeroWithoutContentRangeIsStillAccepted) {
    // The one case where a missing header proves nothing either way: a 200
    // carrying the whole object is indistinguishable from the 206 that was
    // asked for, and the bytes are the same. Endpoints that omit Content-Range
    // keep working for whole-object reads, which is what the small-file paths
    // do.
    Harness h;
    h.fake->body_payload = "abcde";
    h.fake->omit_content_range = true;
    auto data = h.client->GetObjectRange("b", "k", 0, 4);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "abcde");
}

TEST(S3ClientImplGetObjectRange, IntoBufferRejectsAResponseThatCannotProveItsOffset) {
    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->omit_content_range = true;
    std::vector<uint8_t> buffer = {1, 2, 3, 4};
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 5, 9, buffer));
    EXPECT_TRUE(buffer.empty()) << "and no stale bytes may survive it";
}

TEST(S3ClientImplGetObjectRange, IntoBufferRejectsContentRangeNamingDifferentBytes) {
    Harness h;
    h.fake->body_payload = "abcde";
    h.fake->content_range_override = "bytes 0-4/5";
    std::vector<uint8_t> buffer = {1, 2, 3, 4};
    EXPECT_FALSE(h.client->GetObjectRangeInto("b", "k", 5, 9, buffer));
    EXPECT_TRUE(buffer.empty());
}

TEST(S3ClientImplGetObjectRange, ARangeUnitIsMatchedCaseInsensitively) {
    // RFC 9110 section 14.1 makes range unit names case-insensitive, so this is
    // a conforming answer. A substring search for "bytes" rejected it.
    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->content_range_override = "Bytes 5-9/100";
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "fghij");
}

TEST(S3ClientImplGetObjectRange, AUnitThatMerelyContainsBytesIsRejected) {
    // The same substring search accepted units that are not bytes at all, so
    // the numbers were read against the wrong scale.
    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->content_range_override = "kilobytes 5-9/100";
    EXPECT_TRUE(h.client->GetObjectRange("b", "k", 5, 9).empty());

    h.fake->content_range_override = "notbytes 5-9/100";
    EXPECT_TRUE(h.client->GetObjectRange("b", "k", 5, 9).empty());
}

TEST(S3ClientImplGetObjectRange, AZeroStartSubRangeIsStillProtectedWithoutAHeader) {
    // The start-at-0 exemption is not "a read from 0 is the whole object" -
    // chunk 0 of a larger object starts at 0 too. It holds because a response
    // that ignored the range is caught by the length checks instead. This pins
    // that coupling, so relaxing the over-long check cannot quietly reopen #76.
    Harness h;
    h.fake->body_payload = "abcdefghij";  // the whole object, 10 bytes
    h.fake->omit_content_range = true;
    auto data = h.client->GetObjectRange("b", "k", 0, 4);  // asked for 5
    EXPECT_TRUE(data.empty())
        << "a whole-object answer to a sub-range request must not be accepted";
}

TEST(S3ClientImplGetObjectRange, AClampedRangeFailsImmediatelyWithoutRetrying) {
    // What a real server sends when the object shrank: a 206 whose
    // Content-Range names fewer bytes than were asked for. Measured on MinIO as
    // "bytes 5-25/26" for a request for 5-1000. Deterministic, so the short-read
    // retry must not fire.
    Harness h(/*max_retries=*/5);
    h.fake->body_payload = "abcdefghij";
    h.fake->content_range_override = "bytes 0-9/10";
    auto data = h.client->GetObjectRange("b", "k", 0, 99);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 1) << "a clamped range is an answer, not a hiccup";
}

TEST(S3ClientImplGetObjectRange, ATruncatedTransferStillRetriesOnce) {
    // The other half: the header names the full span but the body is cut off
    // mid-transfer. That is the transient case the single retry is for.
    Harness h(/*max_retries=*/5);
    h.fake->body_payload = "abc";                        // 3 of the 5 promised
    h.fake->content_range_override = "bytes 0-4/100";
    auto data = h.client->GetObjectRange("b", "k", 0, 4);
    EXPECT_TRUE(data.empty());
    EXPECT_EQ(h.fake->get_calls, 2) << "worth exactly one second attempt";
}

TEST(S3ClientImplGetObjectRange, AllowUnverifiedRangesAcceptsAMissingHeader) {
    // The escape hatch for an endpoint that omits Content-Range. Off by
    // default; this is what it buys when a user turns it on.
    struct SettingGuard {
        bool saved = g_app_settings.allow_unverified_ranges;
        ~SettingGuard() { g_app_settings.allow_unverified_ranges = saved; }
    } guard;
    g_app_settings.allow_unverified_ranges = true;

    Harness h;  // constructed after the setting, which the client snapshots
    h.fake->body_payload = "fghij";
    h.fake->omit_content_range = true;
    auto data = h.client->GetObjectRange("b", "k", 5, 9);
    ASSERT_EQ(data.size(), 5u);
    EXPECT_EQ(std::string(data.begin(), data.end()), "fghij");
}

TEST(S3ClientImplGetObjectRange, AllowUnverifiedRangesStillRejectsAContradictingHeader) {
    // The flag excuses a missing header, not a wrong one. A header naming other
    // bytes is positive evidence the answer is not what was asked for, and no
    // compatibility argument covers that.
    struct SettingGuard {
        bool saved = g_app_settings.allow_unverified_ranges;
        ~SettingGuard() { g_app_settings.allow_unverified_ranges = saved; }
    } guard;
    g_app_settings.allow_unverified_ranges = true;

    Harness h;
    h.fake->body_payload = "abcde";
    h.fake->content_range_override = "bytes 0-4/5";
    EXPECT_TRUE(h.client->GetObjectRange("b", "k", 5, 9).empty());

    h.fake->content_range_override = "kilobytes 5-9/100";
    EXPECT_TRUE(h.client->GetObjectRange("b", "k", 5, 9).empty());
}

TEST(S3ClientImplGetObjectRange, TheDefaultIsToRefuseAnUnplaceableRead) {
    // Guards the default itself: a test above flips the global, so this pins
    // that it is off unless asked for.
    EXPECT_FALSE(g_app_settings.allow_unverified_ranges);

    Harness h;
    h.fake->body_payload = "fghij";
    h.fake->omit_content_range = true;
    EXPECT_TRUE(h.client->GetObjectRange("b", "k", 5, 9).empty());
}

// ============================================================================
// ListObjects
// ============================================================================

TEST(S3ClientImplListObjects, MapsContentsToObjectInfo) {
    Harness h;
    h.fake->list_contents = {{"a.txt", 10}, {"b/c.bin", 2048}};
    auto r = h.client->ListObjects("bucket");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.objects.size(), 2u);
    EXPECT_EQ(r.objects[0].key, "a.txt");
    EXPECT_EQ(r.objects[0].size, 10);
    EXPECT_EQ(r.objects[1].key, "b/c.bin");
    EXPECT_EQ(r.objects[1].size, 2048);
}

TEST(S3ClientImplListObjects, MapsCommonPrefixes) {
    Harness h;
    h.fake->list_prefixes = {"dir1/", "dir2/"};
    auto r = h.client->ListObjects("bucket");
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.common_prefixes.size(), 2u);
    EXPECT_EQ(r.common_prefixes[0], "dir1/");
}

TEST(S3ClientImplListObjects, ForwardsPrefixDelimiterTokenAndMaxKeys) {
    Harness h;
    h.client->ListObjects("bucket", "some/prefix/", "/", "tok-123", 250);
    EXPECT_EQ(h.fake->last_bucket, "bucket");
    EXPECT_EQ(h.fake->last_prefix, "some/prefix/");
    EXPECT_EQ(h.fake->last_delimiter, "/");
    EXPECT_EQ(h.fake->last_continuation_token, "tok-123");
    EXPECT_EQ(h.fake->last_max_keys, 250);
}

TEST(S3ClientImplListObjects, EmptyPrefixAndTokenAreNotSentAsBlanks) {
    Harness h;
    h.client->ListObjects("bucket", "", "", "", 1000);
    EXPECT_TRUE(h.fake->last_prefix.empty());
    EXPECT_TRUE(h.fake->last_continuation_token.empty());
}

TEST(S3ClientImplListObjects, TruncatedResultCarriesTheNextToken) {
    Harness h;
    h.fake->list_contents = {{"a", 1}};
    h.fake->list_truncated = true;
    h.fake->list_next_token = "next-page-token";
    auto r = h.client->ListObjects("bucket");
    ASSERT_TRUE(r.success);
    EXPECT_TRUE(r.is_truncated);
    EXPECT_EQ(r.next_continuation_token, "next-page-token");
}

TEST(S3ClientImplListObjects, UntruncatedResultHasNoToken) {
    Harness h;
    h.fake->list_contents = {{"a", 1}};
    h.fake->list_truncated = false;
    h.fake->list_next_token = "should-be-ignored";
    auto r = h.client->ListObjects("bucket");
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.is_truncated);
    EXPECT_TRUE(r.next_continuation_token.empty());
}

TEST(S3ClientImplListObjects, EmptyBucketSucceedsWithNoObjects) {
    Harness h;
    auto r = h.client->ListObjects("bucket");
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.objects.empty());
    EXPECT_TRUE(r.common_prefixes.empty());
}

TEST(S3ClientImplListObjects, FailureReportsTheErrorMessage) {
    Harness h;
    h.fake->list_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_BUCKET;
    h.fake->failure_message = "The specified bucket does not exist";
    auto r = h.client->ListObjects("nope");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error_message.find("does not exist"), std::string::npos);
}

TEST(S3ClientImplListObjects, TransientFailureIsRetried) {
    Harness h(/*max_retries=*/2);
    h.fake->list_should_succeed = false;   // transient by default
    auto r = h.client->ListObjects("bucket");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(h.fake->list_calls, 3);
}

// ============================================================================
// PutObject / DeleteObject / CopyObject
// ============================================================================

TEST(S3ClientImplPutObject, SuccessAndFailureAreReported) {
    Harness h;
    std::vector<uint8_t> data{1, 2, 3};
    EXPECT_TRUE(h.client->PutObject("b", "k", data));
    EXPECT_EQ(h.fake->last_bucket, "b");
    EXPECT_EQ(h.fake->last_key, "k");

    Harness bad;
    bad.fake->put_should_succeed = false;
    bad.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    EXPECT_FALSE(bad.client->PutObject("b", "k", data));
}

TEST(S3ClientImplPutObject, EmptyPayloadIsAccepted) {
    Harness h;
    std::vector<uint8_t> empty;
    EXPECT_TRUE(h.client->PutObject("b", "empty-key", empty));
    EXPECT_EQ(h.fake->put_calls, 1);
}

TEST(S3ClientImplDeleteObject, SuccessAndFailureAreReported) {
    Harness h;
    EXPECT_TRUE(h.client->DeleteObject("b", "k"));
    EXPECT_EQ(h.fake->delete_calls, 1);
    EXPECT_EQ(h.fake->last_key, "k");

    Harness bad;
    bad.fake->delete_should_succeed = false;
    bad.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    EXPECT_FALSE(bad.client->DeleteObject("b", "k"));
}

TEST(S3ClientImplCopyObject, EncodesTheCopySourceAsBucketSlashKey) {
    Harness h;
    // A known source size avoids a HeadObject round trip and the >5GB
    // multipart path.
    EXPECT_TRUE(h.client->CopyObject("src-bucket", "src/key.bin",
                                     "dst-bucket", "dst/key.bin",
                                     /*source_size=*/1024));
    EXPECT_EQ(h.fake->copy_calls, 1);
    EXPECT_EQ(h.fake->last_bucket, "dst-bucket");
    EXPECT_EQ(h.fake->last_key, "dst/key.bin");
    EXPECT_NE(h.fake->last_copy_source.find("src-bucket"), std::string::npos);
    EXPECT_NE(h.fake->last_copy_source.find("src/key.bin"), std::string::npos);
}

TEST(S3ClientImplCopyObject, FailureIsReported) {
    Harness h;
    h.fake->copy_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    h.fake->failure_message = "Access Denied";
    EXPECT_FALSE(h.client->CopyObject("sb", "sk", "db", "dk", 512));
}

// ============================================================================
// GetChunkCRC32s: server-side checksum support
// ============================================================================
//
// Remote chunk checksums come from the x-amz-checksum-crc32 header on the
// UploadPartCopy response (AWS "additional checksums"). S3-compatible gateways
// that do not implement it accept the copy and simply omit the header. These
// tests pin what mito does in both cases.

TEST(S3ClientImplChunkCRC32s, SmallFileIsChecksummedLocallyWithoutMultipart) {
    // At or below the chunk size the object is downloaded and CRC'd locally,
    // so no multipart machinery - and no server-side checksum - is involved.
    Harness h;
    h.fake->body_payload = "hello world";
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/11, {}, nullptr, false,
                                         /*num_threads=*/1, /*ramp_up=*/false,
                                         /*chunk_size=*/8 * 1024 * 1024);
    ASSERT_EQ(crcs.size(), 1u);
    EXPECT_NE(crcs[0], 0u);
    EXPECT_EQ(h.fake->get_calls, 1);
    EXPECT_EQ(h.fake->create_mpu_calls, 0) << "small files must not open a multipart upload";
}

TEST(S3ClientImplChunkCRC32s, EmptyObjectYieldsNoChunks) {
    Harness h;
    auto crcs = h.client->GetChunkCRC32s("b", "k", 0, {}, nullptr, false, 1, false,
                                         8 * 1024 * 1024);
    EXPECT_TRUE(crcs.empty());
    EXPECT_EQ(h.fake->get_calls, 0);
    EXPECT_EQ(h.fake->create_mpu_calls, 0);
}

TEST(S3ClientImplChunkCRC32s, LargeFileUsesMultipartCopyWhenChecksumsAreSupported) {
    Harness h;
    h.fake->supports_part_checksums = true;
    // Two chunks at a 1 KiB chunk size.
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/2048, {}, nullptr, false,
                                         1, false, /*chunk_size=*/1024);
    EXPECT_EQ(crcs.size(), 2u);
    EXPECT_GT(h.fake->create_mpu_calls, 0);
    EXPECT_EQ(h.fake->upload_part_copy_calls, 2);
}

TEST(S3ClientImplChunkCRC32s, GatewayWithoutPartChecksumsFailsRatherThanReturningGarbage) {
    // The Storj / older-MinIO case: the copy succeeds but no checksum header
    // comes back. mito must report failure, not fabricate a checksum, since a
    // wrong CRC would silently mark identical files as different.
    Harness h;
    h.fake->supports_part_checksums = false;
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/2048, {}, nullptr, false,
                                         1, false, /*chunk_size=*/1024);
    EXPECT_TRUE(crcs.empty()) << "a gateway without part checksums must not yield checksums";
    EXPECT_GT(h.fake->upload_part_copy_calls, 0) << "it should have tried";
}

TEST(S3ClientImplChunkCRC32s, GatewayThatRejectsChecksummedCopiesYieldsNoChecksums) {
    // The MinIO case: rather than answering without the checksum header, the
    // endpoint rejects the copy outright. Same capability gap, announced as a
    // rejection instead of a silence, so it must reach the same outcome -
    // failure rather than a fabricated checksum (issue #99).
    //
    // Whether the rejection is classified as the capability gap is not visible
    // from here: it changes only which explanation is logged, and a 400 is not
    // retried either way. IsMissingChecksumSupportErrorTest in test_s3_utils
    // covers that classification directly.
    Harness h(/*max_retries=*/3);
    h.fake->rejects_part_checksums = true;
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/2048, {}, nullptr, false,
                                         1, false, /*chunk_size=*/1024);
    EXPECT_TRUE(crcs.empty()) << "a rejected copy must not yield checksums";
    EXPECT_EQ(h.fake->upload_part_copy_calls, 2) << "one attempt per chunk";
}

TEST(S3ClientImplChunkCRC32s, SmallFilePathStillWorksOnAGatewayWithoutPartChecksums) {
    // The practical consequence: objects at or below the chunk size remain
    // comparable against such an endpoint, because that path never asks the
    // server for a checksum.
    Harness h;
    h.fake->supports_part_checksums = false;
    h.fake->body_payload = "small object";
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/12, {}, nullptr, false,
                                         1, false, /*chunk_size=*/1024);
    ASSERT_EQ(crcs.size(), 1u);
    EXPECT_NE(crcs[0], 0u);
}

TEST(S3ClientImplChunkCRC32s, NegativeChunkIdIsRejected) {
    Harness h;
    auto crcs = h.client->GetChunkCRC32s("b", "k", 2048, {0, -1}, nullptr, false,
                                         1, false, 1024);
    EXPECT_TRUE(crcs.empty());
}

// ----------------------------------------------------------------------------
// chunk_ids semantics (issues #26, #52)
//
// Three implementations answer this interface: this adapter, MockS3Client, and
// the local path in crc32_chunks.cpp. The caller compares their results
// position by position, so they have to agree on two things - what a bad id
// means, and what shape the answer has. This adapter used to disagree on both
// whenever the object fitted in a single chunk.
// ----------------------------------------------------------------------------





TEST(S3ClientImplChunkCRC32s, LargeFileReturnsRequestedChunksInOrderWithDuplicates) {
    Harness h;
    h.fake->supports_part_checksums = true;
    auto all = h.client->GetChunkCRC32s("b", "k", /*file_size=*/3072, {}, nullptr, false,
                                        1, false, /*chunk_size=*/1024);
    ASSERT_EQ(all.size(), 3u);

    auto picked = h.client->GetChunkCRC32s("b", "k", 3072, {2, 0, 2}, nullptr, false,
                                           1, false, 1024);
    ASSERT_EQ(picked.size(), 3u) << "one entry per requested id, duplicates included";

    // Named directly rather than by position in `all`, so the assertion does
    // not inherit whatever `all` happened to contain.
    EXPECT_EQ(picked[0], FakeS3Client::ChecksumForChunk(2, 1024, 3072));
    EXPECT_EQ(picked[1], FakeS3Client::ChecksumForChunk(0, 1024, 3072));
    EXPECT_EQ(picked[2], FakeS3Client::ChecksumForChunk(2, 1024, 3072));

    // And the same values the unfiltered request gives, in the requested order.
    EXPECT_EQ(picked[0], all[2]);
    EXPECT_EQ(picked[1], all[0]);
    EXPECT_EQ(picked[2], all[2]);
}


TEST(S3ClientImplChunkCRC32s, NegativeFileSizeIsNotTreatedAsASmallObject) {
    Harness h;
    h.fake->body_payload = "irrelevant";
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/-5, {}, nullptr, false,
                                         1, false, 1024);
    EXPECT_TRUE(crcs.empty());
    EXPECT_EQ(h.fake->get_calls, 0) << "a negative size must not become a byte range";
}

TEST(S3ClientImplChunkCRC32s, AHugeChunkIdDoesNotOverflowTheBoundsCheck) {
    Harness h;
    h.fake->supports_part_checksums = true;
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/2048,
                                         {std::numeric_limits<int64_t>::max()}, nullptr,
                                         false, 1, false, 1024);
    EXPECT_TRUE(crcs.empty());
    EXPECT_EQ(h.fake->create_mpu_calls, 0);
}

TEST(S3ClientImplChunkCRC32s, DuplicateIdsCostOneRequestEach) {
    // Duplicates belong in the answer, not in the work. Two tasks for one chunk
    // wrote the same result slot from two threads - a data race - and sent two
    // UploadPartCopy calls with the same PartNumber against one upload id.
    Harness h;
    h.fake->supports_part_checksums = true;
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/3072, {1, 1, 1, 0}, nullptr,
                                         false, /*num_threads=*/4, false, /*chunk_size=*/1024);
    ASSERT_EQ(crcs.size(), 4u) << "the answer still repeats what was asked for";
    EXPECT_EQ(crcs[0], FakeS3Client::ChecksumForChunk(1, 1024, 3072));
    EXPECT_EQ(crcs[1], crcs[0]);
    EXPECT_EQ(crcs[2], crcs[0]);
    EXPECT_EQ(crcs[3], FakeS3Client::ChecksumForChunk(0, 1024, 3072));
    EXPECT_EQ(h.fake->upload_part_copy_calls, 2) << "but only distinct chunks are fetched";
}

TEST(S3ClientImplChunkCRC32s, EmptyObjectRejectsAnyRequestedChunk) {
    // Nothing has a chunk 0 when there are no chunks at all.
    Harness h;
    auto crcs = h.client->GetChunkCRC32s("b", "k", 0, {0}, nullptr, false, 1, false, 1024);
    EXPECT_TRUE(crcs.empty());
    EXPECT_EQ(h.fake->get_calls, 0);
}

// ============================================================================
// CheckObjectPresence
// ============================================================================
//
// This is the only place NotFound is produced in production, and it decides
// whether rm credits a deletion. A size lookup cannot answer the question: it
// returns the same sentinel for "absent" and for "the lookup failed".

TEST(S3ClientImplPresence, SuccessfulHeadMeansExists) {
    Harness h;
    h.fake->content_length = 10;
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Exists);
}

TEST(S3ClientImplPresence, NoSuchKeyMeansNotFound) {
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    // The bucket probe must succeed for a 404 to be trusted.
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::NotFound);
}

TEST(S3ClientImplPresence, AccessDeniedIsInconclusiveNotAbsent) {
    // S3 returns 403 rather than 404 for a missing key when the caller lacks
    // s3:ListBucket, so this must never be read as "deleted".
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Unknown);
}

TEST(S3ClientImplPresence, ThrottlingIsInconclusive) {
    Harness h(/*max_retries=*/0);
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::SLOW_DOWN;
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Unknown);
}

TEST(S3ClientImplPresence, MissingBucketIsInconclusiveNotAbsent) {
    // A missing bucket is also a bare 404 on HeadObject. Treating it as
    // absence would credit an entire failed batch against a bucket that is
    // not there.
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_BUCKET;
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Unknown);
}

TEST(S3ClientImplPresence, A404IsNotTrustedWhenTheBucketCannotBeConfirmed) {
    // Bare 404 from HeadObject, but the bucket probe also fails: nothing is
    // proven, so the answer must be Unknown rather than NotFound.
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::RESOURCE_NOT_FOUND;
    h.fake->list_should_succeed = false;
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Unknown);
}

TEST(S3ClientImplPresence, TheBucketProbeIsScopedToTheKey) {
    // The probe used to list the bucket root, which needs s3:ListBucket with
    // no prefix condition. A policy or Storj grant scoped to a prefix denies
    // that, so the probe failed and every genuine miss read as Unknown.
    //
    // Scoping it to the key does not rescue every policy shape - a grant
    // written as s3:prefix "data/*" still denies a probe for the bare name
    // "data" - but it works wherever the grant covers the key itself, and it
    // is cheaper besides.
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;

    EXPECT_EQ(h.client->CheckObjectPresence("b", "team/reports"),
              S3ObjectPresence::NotFound);
    EXPECT_EQ(h.fake->last_prefix, "team/reports")
        << "a bucket-root listing is denied under prefix-scoped credentials";
}

TEST(S3ClientImplPresence, AListingThatNamesTheKeyOutranksA404) {
    // HeadObject and ListObjectsV2 encode the key differently - path segment
    // versus query parameter - and gateways have been known to disagree. A
    // listing that names the key is positive evidence; a 404 is not evidence
    // of absence when it contradicts one. Reporting NotFound here would let
    // rm credit a delete that never happened.
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    h.fake->list_contents = {{"data", 100}};

    EXPECT_EQ(h.client->CheckObjectPresence("b", "data"), S3ObjectPresence::Exists);
}

TEST(S3ClientImplPresence, ASiblingSharingThePrefixIsNotMistakenForTheKey) {
    // The probe is a prefix listing, so "database" comes back when asking
    // about "data". Only an exact match counts.
    Harness h;
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::NO_SUCH_KEY;
    h.fake->list_contents = {{"database", 100}, {"data/child.txt", 5}};

    EXPECT_EQ(h.client->CheckObjectPresence("b", "data"), S3ObjectPresence::NotFound);
}

TEST(S3ClientImplPresence, PermanentErrorIsNotRetried) {
    Harness h(/*max_retries=*/3);
    h.fake->head_should_succeed = false;
    h.fake->failure_type = Aws::S3::S3Errors::ACCESS_DENIED;
    // The fake's default message is a retryable curl error; a permanent failure
    // needs a permanent message too, or the retry is correct behaviour.
    h.fake->failure_message = "Access Denied";
    EXPECT_EQ(h.client->CheckObjectPresence("b", "k"), S3ObjectPresence::Unknown);
    EXPECT_EQ(h.fake->head_calls, 1) << "a permanent error must not consume the retry budget";
}

// ============================================================================
// An interrupted comparison must not report unread chunks as zero (issue #51)
// ============================================================================

class S3ClientImplShutdown : public ::testing::Test {
protected:
    void TearDown() override { ResetShutdown(); }   // process-global
};

TEST_F(S3ClientImplShutdown, ChunksSkippedByShutdownYieldNoResultRatherThanZeros) {
    ResetShutdown();
    Harness h;
    h.fake->supports_part_checksums = true;
    h.fake->shutdown_after_n_parts = 1;   // interrupt after the first part

    // Eight chunks at a 1 KiB chunk size; only the first can complete.
    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/8192, {}, nullptr,
                                         false, 1, false, /*chunk_size=*/1024);

    EXPECT_TRUE(crcs.empty())
        << "chunks never checksummed must not be returned as zeros; got "
        << crcs.size() << " values";
}

TEST_F(S3ClientImplShutdown, AnUninterruptedRunStillReturnsEveryChunk) {
    // Guard against over-firing: the success counter must reach the total on a
    // clean run, or every comparison would fail.
    ResetShutdown();
    Harness h;
    h.fake->supports_part_checksums = true;
    h.fake->shutdown_after_n_parts = 0;

    auto crcs = h.client->GetChunkCRC32s("b", "k", 8192, {}, nullptr,
                                         false, 1, false, 1024);

    EXPECT_EQ(crcs.size(), 8u) << "a clean run must return every chunk";
}

TEST_F(S3ClientImplShutdown, AsyncPathAlsoDiscardsAPartialResult) {
    // num_threads = 0 selects the std::async path rather than the thread pool.
    // It carries an identical guard, and the two tests above only exercise the
    // pool.
    ResetShutdown();
    Harness h;
    h.fake->supports_part_checksums = true;
    h.fake->shutdown_after_n_parts = 1;

    auto crcs = h.client->GetChunkCRC32s("b", "k", 8192, {}, nullptr,
                                         false, /*num_threads=*/0, false, 1024);

    EXPECT_TRUE(crcs.empty())
        << "the async path must discard a partial result too; got " << crcs.size();
}

TEST_F(S3ClientImplShutdown, CleanRunReturnsTheComputedValuesNotZeros) {
    // Assert content, not just size: every entry must be a real per-part CRC.
    ResetShutdown();
    Harness h;
    h.fake->supports_part_checksums = true;

    auto crcs = h.client->GetChunkCRC32s("b", "k", 4096, {}, nullptr,
                                         false, 1, false, 1024);

    ASSERT_EQ(crcs.size(), 4u);
    for (size_t i = 0; i < crcs.size(); ++i) {
        EXPECT_NE(crcs[i], 0u) << "chunk " << i << " came back as a zero";
        // Not merely "a per-part value": the value for *this* chunk, in this
        // slot. The fake derives its checksum from the requested byte range, so
        // a result vector that was ordered wrongly would fail here.
        EXPECT_EQ(crcs[i], FakeS3Client::ChecksumForChunk(static_cast<int64_t>(i), 1024, 4096))
            << "slot " << i << " does not hold chunk " << i << "'s checksum";
    }
}

// ============================================================================
// Chunk id semantics on the real adapter (issues #26 and #52)
// ============================================================================
//
// The adapter validated only negative ids, and its small-file shortcut ignored
// chunk_ids entirely and always returned one CRC. Both the local path and the
// mock range-check and preserve the requested shape; only this one did not.
// The results are compared positionally against the other side, so a result of
// the wrong length silently misaligns every entry after it.

TEST(S3ClientImplChunkIds, SmallFileHonoursASingleRequestedChunk) {
    // A control: {0} on a one-chunk object behaved correctly before the fix
    // and must keep doing so. It pins the CRC *value* against an independent
    // computation, so reshaping the result cannot quietly change what the
    // entries contain.
    Harness h;
    h.fake->body_payload = "small object";   // one chunk at any sane chunk size

    auto all = h.client->GetChunkCRC32s("b", "k", 12, {}, nullptr, false, 1, false, 1024);
    auto one = h.client->GetChunkCRC32s("b", "k", 12, {0}, nullptr, false, 1, false, 1024);

    const uint32_t expected =
        crc32_hw(reinterpret_cast<const uint8_t*>(h.fake->body_payload.data()),
                 h.fake->body_payload.size());
    ASSERT_EQ(all.size(), 1u);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(all[0], expected);
    EXPECT_EQ(one[0], expected) << "requesting chunk 0 must give chunk 0's CRC";
}

TEST(S3ClientImplChunkIds, SmallFileRepeatsAChunkAsManyTimesAsRequested) {
    // {0, 0} must yield two entries. Returning one collapsed the result and
    // shifted every subsequent comparison.
    Harness h;
    h.fake->body_payload = "small object";

    auto crcs = h.client->GetChunkCRC32s("b", "k", 12, {0, 0}, nullptr, false, 1, false, 1024);

    ASSERT_EQ(crcs.size(), 2u) << "the result must match the requested shape";
    EXPECT_EQ(crcs[0], crcs[1]);
    EXPECT_NE(crcs[0], 0u);
}

TEST(S3ClientImplChunkIds, OutOfRangeChunkIdIsRejectedOnASmallFile) {
    // A one-chunk object has no chunk 1.
    Harness h;
    h.fake->body_payload = "small object";

    auto crcs = h.client->GetChunkCRC32s("b", "k", 12, {1}, nullptr, false, 1, false, 1024);

    EXPECT_TRUE(crcs.empty()) << "an id past the end must fail, not be silently clamped";
    EXPECT_EQ(h.fake->get_calls, 0) << "and must fail before spending an API call";
}

TEST(S3ClientImplChunkIds, OutOfRangeChunkIdIsRejectedOnAMultipartFile) {
    // The same id used to be passed straight through to an index into a vector
    // sized by chunk count - a heap write and read past the end, reproduced
    // under AddressSanitizer during review of #51.
    Harness h;
    h.fake->supports_part_checksums = true;

    // 8 KiB at 1 KiB chunks = 8 chunks, so 99 is far out of range.
    auto crcs = h.client->GetChunkCRC32s("b", "k", 8192, {99}, nullptr, false, 1, false, 1024);

    EXPECT_TRUE(crcs.empty());
    EXPECT_EQ(h.fake->create_mpu_calls, 0)
        << "an invalid request must be rejected before any S3 call is made - "
           "starting an upload we then abandon leaves a billable orphan";
    EXPECT_EQ(h.fake->upload_part_copy_calls, 0);
}

TEST(S3ClientImplChunkIds, NegativeChunkIdIsStillRejected) {
    Harness h;
    h.fake->body_payload = "small object";
    EXPECT_TRUE(h.client->GetChunkCRC32s("b", "k", 12, {-1}, nullptr, false, 1, false, 1024)
                    .empty());
}

TEST(S3ClientImplChunkIds, ShortReadOnASmallFileFailsRatherThanChecksummingAPrefix) {
    // The object is 100 bytes but the body returns 12. Checksumming what
    // arrived would produce a confident wrong CRC.
    Harness h;
    h.fake->body_payload = "small object";   // 12 bytes

    auto crcs = h.client->GetChunkCRC32s("b", "k", /*file_size=*/100, {}, nullptr,
                                         false, 1, false, 1024);

    EXPECT_TRUE(crcs.empty()) << "a short read must not be checksummed as if complete";

    auto requested = h.client->GetChunkCRC32s("b", "k", 100, {0}, nullptr, false,
                                              1, false, 1024);
    EXPECT_TRUE(requested.empty()) << "and the same for an explicitly requested chunk";
}

// ============================================================================
// Upload memory (issue #56)
// ============================================================================

#include <sys/resource.h>

// Peak resident set size in KiB. getrusage rather than /proc/self/status so
// this reports a real number on macOS too - reading VmHWM there returned -1 for
// both samples, so the difference was zero and the test passed having measured
// nothing.
static long peak_rss_kb() {
    struct rusage ru {};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) return -1;
#ifdef __APPLE__
    return static_cast<long>(ru.ru_maxrss / 1024);   // bytes on macOS
#else
    return static_cast<long>(ru.ru_maxrss);          // KiB on Linux
#endif
}

// Writes `size` bytes of varied, non-repeating content and returns its CRC32.
static uint32_t write_test_file(const std::string& path, size_t size) {
    std::vector<uint8_t> chunk(1u << 20);
    for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    std::ofstream o(path, std::ios::binary);
    uint32_t crc = 0;
    size_t written = 0;
    while (written < size) {
        const size_t n = std::min(chunk.size(), size - written);
        o.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n));
        crc = crc32_hw_update(crc, chunk.data(), n);
        written += n;
    }
    return crc;
}

TEST(S3ClientImplUpload, PutObjectFromFileDoesNotHoldTheFileInMemory) {
    // The file was read into a vector and that vector copied into a
    // StringStream, so peak memory was twice the file size - 1039 MiB for a
    // 512 MiB upload, on a path whose caller is named "small file" but which
    // diff_upload_file falls back to for every first upload of a large one
    // (issue #56).
    Harness h;
    const size_t kSize = 512u * 1024 * 1024;
    const std::string path = mito_test_temp_path("mito_put_stream").string();
    write_test_file(path, kSize);

    const long before = peak_rss_kb();
    ASSERT_GT(before, 0) << "peak RSS is unavailable, so this test proves nothing";
    const bool ok = h.client->PutObjectFromFile("b", "k", path);
    const long after = peak_rss_kb();
    std::remove(path.c_str());   // before any assertion, so a failure leaks nothing
    ASSERT_TRUE(ok);

    const long growth_mib = (after - before) >> 10;
    EXPECT_LT(growth_mib, 64)
        << "uploading a " << (kSize >> 20) << " MiB file grew peak RSS by "
        << growth_mib << " MiB";
}

TEST(S3ClientImplUpload, PutObjectFromFileSendsEveryByteAndTheMatchingChecksum) {
    // Streaming the body must not change what is uploaded. A size that is not
    // a multiple of the read chunk catches an off-by-one in the final chunk.
    Harness h;
    const size_t kSize = (3u << 20) + 12345;
    const std::string path = mito_test_temp_path("mito_put_bytes").string();
    const uint32_t expected_crc = write_test_file(path, kSize);

    ASSERT_TRUE(h.client->PutObjectFromFile("b", "k", path));
    std::remove(path.c_str());

    EXPECT_EQ(h.fake->last_body_size, static_cast<int64_t>(kSize))
        << "the body sent was not the whole file";
    EXPECT_EQ(h.fake->last_body_crc32, expected_crc)
        << "the bytes sent do not match the file";
    (void)expected_crc;
    EXPECT_EQ(h.fake->last_content_length, static_cast<int64_t>(kSize));

    // No checksum value is set on the request: the SDK computes CRC32 over the
    // body as it streams it, below this layer. That is the point - the checksum
    // then describes exactly the bytes sent, which a value computed from a
    // separate read of the file cannot promise.
    EXPECT_TRUE(h.fake->last_checksum_crc32.empty())
        << "a precomputed checksum would describe a different read of the file";
}

TEST(S3ClientImplUpload, PutObjectFromFileHandlesAnEmptyFile) {
    Harness h;
    const std::string path = mito_test_temp_path("mito_put_empty").string();
    { std::ofstream o(path, std::ios::binary); }

    ASSERT_TRUE(h.client->PutObjectFromFile("b", "k", path));
    std::remove(path.c_str());

    EXPECT_EQ(h.fake->last_body_size, 0);
    EXPECT_EQ(h.fake->last_content_length, 0);
    EXPECT_EQ(h.fake->last_body_crc32, 0u);
}

TEST(S3ClientImplUpload, PutObjectFromFileRefusesAMissingFile) {
    Harness h;
    EXPECT_FALSE(h.client->PutObjectFromFile("b", "k", "/tmp/mito-no-such-file-12345"));
    EXPECT_EQ(h.fake->put_calls, 0) << "nothing should be sent for a file that is not there";
}

TEST(S3ClientImplUpload, ChunkedCrcMatchesTheWholeBufferCrc) {
    // The streaming upload chains crc32_hw_update across reads. If that did not
    // agree with crc32_hw over the whole buffer, every upload would declare a
    // checksum S3 would reject.
    std::vector<uint8_t> data(1000000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);

    const uint32_t whole = crc32_hw(data.data(), data.size());
    for (size_t chunk : {1u, 2u, 7u, 4096u, 65536u, 999983u}) {
        uint32_t chained = 0;
        for (size_t off = 0; off < data.size(); off += chunk) {
            chained = crc32_hw_update(chained, data.data() + off,
                                      std::min(chunk, data.size() - off));
        }
        EXPECT_EQ(chained, whole) << "chunk size " << chunk;
    }
}

TEST(S3ClientImplUpload, PutObjectFromFileRefusesAFileTooLargeForASinglePut) {
    // PutObject tops out at 5 GiB. The old path read and checksummed the whole
    // file first - allocating twice its size - and only then got EntityTooLarge
    // back from S3. A sparse file makes this cost nothing on disk.
    Harness h;
    const std::string path = mito_test_temp_path("mito_put_huge").string();
    {
        std::ofstream o(path, std::ios::binary);
        ASSERT_TRUE(o.good());
    }
    if (::truncate(path.c_str(), 6ll * 1024 * 1024 * 1024) != 0) {
        std::remove(path.c_str());
        GTEST_SKIP() << "filesystem does not support sparse files this large";
    }

    EXPECT_FALSE(h.client->PutObjectFromFile("b", "k", path));
    EXPECT_EQ(h.fake->put_calls, 0)
        << "an upload that cannot succeed must not be attempted";
    std::remove(path.c_str());
}

// A fake that rewrites the file from inside PutObject, before the SDK has read
// the body. That is the real window: the body stream is consumed lazily.
class MutateDuringPut : public FakeS3Client {
public:
    std::string path;
    enum class How { Grow, Shrink, Rewrite } how = How::Grow;

    Aws::S3::Model::PutObjectOutcome PutObject(
        const Aws::S3::Model::PutObjectRequest& request) const override {
        if (how == How::Grow) {
            std::ofstream o(path, std::ios::binary | std::ios::app);
            std::vector<char> extra(1u << 20, 'G');
            o.write(extra.data(), static_cast<std::streamsize>(extra.size()));
        } else if (how == How::Shrink) {
            ::truncate(path.c_str(), 1024);
        } else {
            std::fstream o(path, std::ios::binary | std::ios::in | std::ios::out);
            std::vector<char> over(4096, 'R');
            o.write(over.data(), static_cast<std::streamsize>(over.size()));
        }
        return FakeS3Client::PutObject(request);
    }
};

TEST(S3ClientImplUpload, AFileThatGrowsDuringUploadStillSendsAConsistentPrefix) {
    // The body is bounded by the size the request declared. Without that a
    // growing file streams past Content-Length, and an upload that used to
    // work - a log being appended to while the directory is synced - fails.
    auto fake = Aws::MakeShared<MutateDuringPut>("t");
    fake->how = MutateDuringPut::How::Grow;
    auto client = CreateS3ClientForTesting(fake, "us-east-1", false, /*max_retries=*/0);

    const std::string path = mito_test_temp_path("mito_put_grow").string();
    fake->path = path;
    const size_t kSize = 2u << 20;
    const uint32_t crc_before = write_test_file(path, kSize);

    EXPECT_TRUE(client->PutObjectFromFile("b", "k", path));
    std::remove(path.c_str());

    EXPECT_EQ(fake->last_body_size, static_cast<int64_t>(kSize))
        << "the upload streamed past the length it declared";
    EXPECT_EQ(fake->last_content_length, static_cast<int64_t>(kSize));
    EXPECT_EQ(fake->last_body_crc32, crc_before)
        << "the prefix that was sent is not the prefix that was there";
}

TEST(S3ClientImplUpload, AFileThatShrinksDuringUploadDoesNotSendPadding) {
    // Nothing can rescue this one - the bytes are gone. What matters is that
    // the body does not get padded out to the declared length with junk.
    auto fake = Aws::MakeShared<MutateDuringPut>("t");
    fake->how = MutateDuringPut::How::Shrink;
    auto client = CreateS3ClientForTesting(fake, "us-east-1", false, /*max_retries=*/0);

    const std::string path = mito_test_temp_path("mito_put_shrink").string();
    fake->path = path;
    write_test_file(path, 2u << 20);

    client->PutObjectFromFile("b", "k", path);
    std::remove(path.c_str());

    EXPECT_LE(fake->last_body_size, fake->last_content_length)
        << "sent more than it declared";
}

// A fake that treats the body the way the SDK does when it retries: consume
// part of it, rewind to the start, then read it again from scratch. It also
// asks for the position both mid-stream and just after the rewind, which is
// where the bounded buffer has to account for bytes it has read ahead but not
// handed out yet.
class RewindDuringPut : public FakeS3Client {
public:
    static constexpr std::streamsize kPrefix = 4096;

    mutable std::streamsize prefix_read = 0;
    mutable std::streamoff tell_after_prefix = -1;
    mutable std::streamoff tell_after_rewind = -1;

    Aws::S3::Model::PutObjectOutcome PutObject(
        const Aws::S3::Model::PutObjectRequest& request) const override {
        if (auto body = request.GetBody()) {
            std::vector<char> buf(static_cast<size_t>(kPrefix));
            body->read(buf.data(), kPrefix);
            prefix_read = body->gcount();
            tell_after_prefix = body->tellg();

            body->clear();
            body->seekg(0, std::ios_base::beg);
            tell_after_rewind = body->tellg();
        }
        // Drains from wherever the rewind left it, and records what a retried
        // attempt would actually have put on the wire.
        return FakeS3Client::PutObject(request);
    }
};

TEST(S3ClientImplUpload, ARewoundBodySendsTheWholeFileAgain) {
    // The SDK rewinds the request body between retry attempts. If the bounded
    // buffer got its position accounting wrong, a retried upload would send a
    // short or offset body and silently store a corrupt object (issue #97).
    auto fake = Aws::MakeShared<RewindDuringPut>("t");
    auto client = CreateS3ClientForTesting(fake, "us-east-1", false, /*max_retries=*/0);

    const std::string path = mito_test_temp_path("mito_put_rewind").string();
    const size_t kSize = (1u << 20) + 517;
    const uint32_t expected_crc = write_test_file(path, kSize);

    EXPECT_TRUE(client->PutObjectFromFile("b", "k", path));
    std::remove(path.c_str());

    ASSERT_EQ(fake->prefix_read, RewindDuringPut::kPrefix)
        << "the body was already short before any rewind";

    // The buffer reads ahead in 64 KiB blocks, so at this point it has pulled
    // far more from the file than the stream has handed out. The position must
    // describe what was handed out, not what was buffered.
    EXPECT_EQ(fake->tell_after_prefix, RewindDuringPut::kPrefix)
        << "mid-stream position counted bytes that were only read ahead";
    EXPECT_EQ(fake->tell_after_rewind, 0)
        << "the body did not report the start after being rewound";

    EXPECT_EQ(fake->last_body_size, static_cast<int64_t>(kSize))
        << "a retried upload would have stored a truncated object";
    EXPECT_EQ(fake->last_body_crc32, expected_crc)
        << "the bytes sent after the rewind are not the file";
}
