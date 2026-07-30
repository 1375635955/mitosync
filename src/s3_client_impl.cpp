#include "s3_interface.h"
#include "s3_client_testing.h"
#include "s3_utils.h"
#include "crc32_chunks.h"
#include "crc32_hw.h"
#include "app_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/Delete.h>
#include <aws/s3/model/ObjectIdentifier.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CompletedPart.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/ListMultipartUploadsRequest.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/client/DefaultRetryStrategy.h>

#include <spdlog/spdlog.h>
#include <sstream>
#include <future>
#include <thread>
#include <chrono>
#include <numeric>
#include <random>
#include <filesystem>
#include <fstream>

namespace {

// Generate a unique suffix for temp keys to avoid collisions
std::string GenerateUniqueSuffix() {
    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    std::ostringstream ss;
    ss << std::hex << nanos << "_" << dist(gen);
    return ss.str();
}

// Checks a byte range before it becomes an HTTP header or an allocation, and
// writes the number of bytes it covers to *length.
//
// Both range reads used to take start and end on trust. A reversed range built
// "bytes=100-50" and sent it; a negative start built "bytes=-5-10", which is
// not merely invalid - RFC 9110 gives "bytes=-5" the meaning "the last five
// bytes", so a gateway parsing loosely could answer with real data from the
// wrong part of the object. And GetObjectRangeInto sized its buffer from
// (end - start + 1) cast to size_t before checking anything, so a reversed
// range asked for roughly 2^64 bytes.
//
// Callers all compute exact in-bounds ranges from a known object size, so
// anything else is a bug in the caller, not a request worth making (issue #23).
bool ValidateRange(const std::string& bucket, const std::string& key,
                   int64_t start, int64_t end, int64_t* length) {
    if (start < 0) {
        spdlog::error("Invalid range for s3://{}/{}: start {} is negative", bucket, key, start);
        return false;
    }
    if (end < start) {
        spdlog::error("Invalid range for s3://{}/{}: end {} precedes start {}",
                      bucket, key, end, start);
        return false;
    }
    // end >= start >= 0, so the subtraction cannot overflow; only the +1 can,
    // and only for a range covering the whole of INT64_MAX.
    const int64_t span = end - start;
    if (span == std::numeric_limits<int64_t>::max()) {
        spdlog::error("Invalid range for s3://{}/{}: {}-{} does not fit in int64_t",
                      bucket, key, start, end);
        return false;
    }
    *length = span + 1;
    return true;
}

// Decides whether a successful GetObject response proves it honoured the range
// that was asked for.
//
// Body length alone does not prove it. #23 made a body longer or shorter than
// the requested span a failure, which catches a gateway that ignores Range and
// returns the whole object - unless the whole object happens to be exactly as
// long as the span asked for. Then "bytes=5-9" can come back as object[0..5),
// the right length at the wrong offset, and callers compare these bytes
// positionally, so it reads as a difference in the wrong place rather than as
// an error (issue #76).
//
// Content-Range is the response's own statement of which bytes these are. S3
// sends it with every 206, and MinIO and Ceph RGW were both measured doing the
// same. Some S3-compatible endpoint may not, so its absence is tolerated for a
// read that starts at 0.
//
// Why that is safe is not quite the obvious argument. It is NOT that a read
// from 0 is always the whole object - comparison_task asks for chunk 0 of a
// larger object, and S3ByteProvider for a prefix of one. It is that every way
// such a response can be wrong is caught elsewhere: if the endpoint ignored the
// range and sent a longer object the over-long check rejects it, if it sent a
// shorter one the short-read check does, and if the object is exactly the
// requested length then the whole object and the requested range are the same
// bytes. The exemption therefore leans on those two checks (issue #23) - a
// coupling worth knowing before either is relaxed.
//
// One case survives: an endpoint that neither honours nor ignores the range but
// mis-honours it, returning the right number of bytes from the wrong offset,
// with no header to contradict it. Nothing can detect that once the header is
// absent, and no such endpoint is known.
bool RangeResponseHonoursRequest(const Aws::String& content_range,
                                 const std::string& bucket, const std::string& key,
                                 int64_t start, int64_t end, bool allow_unverified) {
    if (content_range.empty()) {
        if (start == 0) {
            return true;
        }
        // --allow-unverified-ranges. Only the header's absence is excused: one
        // that names a different range is positive evidence the answer is not
        // what was asked for, and no compatibility argument covers that.
        if (allow_unverified) {
            spdlog::debug("s3://{}/{} answered the range {}-{} without a Content-Range; "
                          "accepting it because --allow-unverified-ranges is set",
                          bucket, key, start, end);
            return true;
        }
        spdlog::error("s3://{}/{} answered the range {}-{} without a Content-Range, so there "
                      "is nothing to show the bytes start at {}. If this endpoint is known to "
                      "omit the header, --allow-unverified-ranges accepts it.",
                      bucket, key, start, end, start);
        return false;
    }

    // "bytes <first>-<last>/<length>", where <length> may be "*".
    const std::string value(content_range.c_str());
    size_t pos = 0;
    while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) {
        ++pos;
    }

    // Match the range unit as a whole token, case-insensitively. RFC 9110
    // section 14.1 makes range unit names case-insensitive, so "Bytes 5-9/100"
    // is a conforming answer that a substring search for "bytes" would reject -
    // and the same search accepted anything merely containing it, so
    // "kilobytes 5-9/100" passed for a unit that is not bytes at all.
    const std::string unit = "bytes";
    const bool unit_matches =
        value.size() - pos >= unit.size() &&
        std::equal(unit.begin(), unit.end(), value.begin() + static_cast<long>(pos),
                   [](char expected, char actual) {
                       return expected == std::tolower(static_cast<unsigned char>(actual));
                   });
    if (!unit_matches) {
        spdlog::error("s3://{}/{} returned an unreadable Content-Range '{}' for {}-{}",
                      bucket, key, value, start, end);
        return false;
    }
    pos += unit.size();

    // The unit has to end here; without this "notbytes" still matches after the
    // leading-space skip on a header like " notbytes 5-9/100".
    if (pos >= value.size() || !std::isspace(static_cast<unsigned char>(value[pos]))) {
        spdlog::error("s3://{}/{} returned an unreadable Content-Range '{}' for {}-{}",
                      bucket, key, value, start, end);
        return false;
    }
    while (pos < value.size() && std::isspace(static_cast<unsigned char>(value[pos]))) {
        ++pos;
    }

    int64_t first = 0;
    int64_t last = 0;
    char sep = 0;
    std::istringstream parser(value.substr(pos));
    if (!(parser >> first) || !(parser >> sep) || sep != '-' || !(parser >> last)) {
        spdlog::error("s3://{}/{} returned an unreadable Content-Range '{}' for {}-{}",
                      bucket, key, value, start, end);
        return false;
    }

    if (first != start || last != end) {
        spdlog::error("s3://{}/{} answered the range {}-{} with bytes {}-{}",
                      bucket, key, start, end, first, last);
        return false;
    }
    return true;
}

} // namespace

// Real S3 client implementation using AWS SDK
// Reads at most `limit` bytes from a file. Two reasons it is not a plain
// ifstream: the request declares a Content-Length taken from a stat, and a file
// that grows during the upload would otherwise stream past it; and bounding it
// means a file being appended to uploads a consistent prefix rather than
// failing, which is what buffering it in memory used to give for free.
class BoundedFileBuf : public std::streambuf {
public:
    BoundedFileBuf(const std::string& path, std::streamsize limit)
        : file_(path, std::ios::binary), limit_(limit), remaining_(limit) {}

    bool is_open() const { return file_.is_open(); }

protected:
    int_type underflow() override {
        if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (remaining_ <= 0) return traits_type::eof();
        const std::streamsize want =
            std::min(static_cast<std::streamsize>(buffer_.size()), remaining_);
        file_.read(buffer_.data(), want);
        const std::streamsize got = file_.gcount();
        if (got <= 0) return traits_type::eof();
        remaining_ -= got;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + got);
        return traits_type::to_int_type(*gptr());
    }

    // The SDK rewinds the body between retry attempts, so seeking back to the
    // start has to work or a retried upload would send nothing.
    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(static_cast<off_type>(pos), std::ios_base::beg, which);
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override {
        if (!(which & std::ios_base::in)) return pos_type(off_type(-1));

        // The get area is null before the first underflow() and after every
        // rewind below. Pointer subtraction is defined only inside one array, so
        // count unread buffered bytes only while the buffer pointers are valid.
        const off_type consumed = static_cast<off_type>(limit_ - remaining_);
        const off_type unread =
            (gptr() && egptr()) ? static_cast<off_type>(egptr() - gptr()) : 0;
        const off_type current = consumed - unread;
        off_type target = off;
        if (dir == std::ios_base::cur) target = current + off;
        else if (dir == std::ios_base::end) target = static_cast<off_type>(limit_) + off;

        if (target < 0 || target > static_cast<off_type>(limit_)) return pos_type(off_type(-1));

        file_.clear();
        file_.seekg(target, std::ios_base::beg);
        if (!file_) return pos_type(off_type(-1));
        remaining_ = limit_ - target;
        setg(nullptr, nullptr, nullptr);
        return pos_type(target);
    }

private:
    std::ifstream file_;
    std::streamsize limit_;
    std::streamsize remaining_;
    std::array<char, 1 << 16> buffer_{};
};

// An Aws::IOStream over the bounded buffer, owning it.
class BoundedFileStream : public Aws::IOStream {
public:
    BoundedFileStream(const std::string& path, std::streamsize limit)
        : Aws::IOStream(&buf_), buf_(path, limit) {}
    bool is_open() const { return buf_.is_open(); }

private:
    BoundedFileBuf buf_;
};

class S3ClientImpl : public IS3Client {
public:
    S3ClientImpl(const std::string& region, const std::string& endpoint_override = "", int max_connections = 128, const std::string& profile = "")
        : m_region(region)
        , m_use_path_style(!endpoint_override.empty())
        // Snapshot settings at construction time for thread-safety
        , m_max_retries(g_app_settings.max_retries)
        , m_allow_unverified_ranges(g_app_settings.allow_unverified_ranges)
    {
        Aws::Client::ClientConfiguration config;
        config.region = region.c_str();
        config.maxConnections = max_connections;
        // Snapshot timeout settings at construction time for thread-safety
        config.connectTimeoutMs = g_app_settings.connect_timeout_ms();
        config.httpRequestTimeoutMs = g_app_settings.request_timeout_ms();
        config.requestTimeoutMs = g_app_settings.request_timeout_ms();
        config.enableTcpKeepAlive = true;
        // No SDK-level retries - app-level retry logic handles transient failures
        config.retryStrategy = std::make_shared<Aws::Client::DefaultRetryStrategy>(0, 0);

        if (!endpoint_override.empty()) {
            config.endpointOverride = endpoint_override.c_str();
            // m_use_path_style already set in initializer list for LocalStack/MinIO
        }

        std::shared_ptr<Aws::Auth::AWSCredentialsProvider> profile_provider;
        if (!profile.empty()) {
            profile_provider =
                std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(
                    profile.c_str());
        }

        if (m_use_path_style) {
            if (profile_provider) {
                m_client = std::make_shared<Aws::S3::S3Client>(
                    profile_provider,
                    config,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                    false,  // useVirtualAddressing = false for path-style
                    Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET
                );
            } else {
                m_client = std::make_shared<Aws::S3::S3Client>(
                    config,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                    false,  // useVirtualAddressing = false for path-style
                    Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET
                );
            }
        } else if (profile_provider) {
            m_client = std::make_shared<Aws::S3::S3Client>(profile_provider, nullptr, config);
        } else {
            m_client = std::make_shared<Aws::S3::S3Client>(config);
        }
    }

    // Test seam: adopt a caller-supplied SDK client instead of constructing one.
    // Lets tests drive the request-building, response-mapping and retry logic
    // with canned outcomes, without reaching the network. See
    // CreateS3ClientForTesting in s3_client_testing.h.
    S3ClientImpl(std::shared_ptr<Aws::S3::S3Client> client,
                 const std::string& region,
                 bool use_path_style,
                 int max_retries)
        : m_client(std::move(client))
        , m_region(region)
        , m_use_path_style(use_path_style)
        , m_max_retries(max_retries)
        , m_allow_unverified_ranges(g_app_settings.allow_unverified_ranges)
    {}

    int64_t GetObjectSize(const std::string& bucket, const std::string& key) override {
        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::HeadObjectRequest request;
                request.SetBucket(bucket.c_str());
                request.SetKey(key.c_str());

                auto outcome = m_client->HeadObject(request);
                if (outcome.IsSuccess()) {
                    return outcome.GetResult().GetContentLength();
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    // Add jitter (0-25%) to prevent thundering herd
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectSize s3://{}/{}: {}",
                                  attempt + 1, bucket, key, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::debug("Failed to get object size: {}", error.GetMessage());
                return -1;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectSize s3://{}/{}: {}",
                                  attempt + 1, bucket, key, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::debug("Exception in GetObjectSize: {}", e.what());
                return -1;
            }
        }
        return -1;  // Unreachable
    }

    S3ObjectPresence CheckObjectPresence(const std::string& bucket,
                                         const std::string& key) override {
        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::HeadObjectRequest request;
                request.SetBucket(bucket.c_str());
                request.SetKey(key.c_str());

                auto outcome = m_client->HeadObject(request);
                if (outcome.IsSuccess()) {
                    return S3ObjectPresence::Exists;
                }

                const auto& error = outcome.GetError();
                // A missing BUCKET is also a bare 404 here, and that must not
                // read as "the object was deleted" - it would credit an entire
                // failed batch against a bucket that is not there. NoSuchBucket
                // cannot be distinguished from NoSuchKey by error type, because
                // HeadObject has no response body for the SDK to parse, so the
                // bucket is confirmed separately before trusting a 404.
                if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_BUCKET) {
                    return S3ObjectPresence::Unknown;
                }
                if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY ||
                    error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND ||
                    error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
                    // Confirm the bucket exists; a 404 from a missing bucket
                    // says nothing about the key.
                    // Scope the probe to the key. A bucket-root listing needs
                    // s3:ListBucket with no prefix condition, which a policy
                    // or Storj grant scoped to one prefix does not give - the
                    // probe would be denied and every genuine miss would come
                    // back Unknown. Prefixing it still separates NoSuchBucket
                    // (an error) from a live bucket (success, empty), and it
                    // is cheaper.
                    Aws::S3::Model::ListObjectsV2Request probe;
                    probe.SetBucket(bucket.c_str());
                    probe.SetPrefix(key.c_str());
                    probe.SetMaxKeys(1);
                    auto probe_outcome = m_client->ListObjectsV2(probe);
                    if (!probe_outcome.IsSuccess()) {
                        spdlog::debug("404 on s3://{}/{} but the bucket could not be "
                                      "confirmed: {}", bucket, key,
                                      probe_outcome.GetError().GetMessage());
                        return S3ObjectPresence::Unknown;
                    }
                    // The probe answered, so read what it says about the key
                    // rather than only that it succeeded. A listing that names
                    // the key is positive evidence the object is there, and it
                    // outranks a 404 from HeadObject - the two encode the key
                    // differently (path vs query parameter) and some gateways
                    // disagree about the result. Reporting NotFound here would
                    // credit a delete that never happened.
                    for (const auto& obj : probe_outcome.GetResult().GetContents()) {
                        if (obj.GetKey() == key.c_str()) {
                            spdlog::debug("HeadObject 404 for s3://{}/{} but a listing "
                                          "names it; treating it as present", bucket, key);
                            return S3ObjectPresence::Exists;
                        }
                    }
                    return S3ObjectPresence::NotFound;
                }

                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::debug("Presence check for s3://{}/{} is inconclusive: {}",
                              bucket, key, error.GetMessage());
                return S3ObjectPresence::Unknown;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::debug("Exception in CheckObjectPresence: {}", e.what());
                return S3ObjectPresence::Unknown;
            }
        }
        return S3ObjectPresence::Unknown;
    }

    std::vector<uint8_t> GetObjectRange(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end
    ) override {
        int64_t expected_length = 0;
        if (!ValidateRange(bucket, key, start, end, &expected_length)) {
            return {};
        }

        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::GetObjectRequest request;
                request.SetBucket(bucket.c_str());
                request.SetKey(key.c_str());

                std::ostringstream range_str;
                range_str << "bytes=" << start << "-" << end;
                request.SetRange(range_str.str().c_str());

                auto outcome = m_client->GetObject(request);
                if (outcome.IsSuccess()) {
                    // Ask the response which bytes these are before trusting
                    // them. Deterministic, so no retry (issue #76).
                    if (!RangeResponseHonoursRequest(outcome.GetResult().GetContentRange(),
                                                     bucket, key, start, end,
                                                     m_allow_unverified_ranges)) {
                        return {};
                    }
                    auto& stream = outcome.GetResult().GetBody();
                    std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)),
                                               std::istreambuf_iterator<char>());
                    // A body shorter than the range is not a successful read.
                    // It means the object shrank under us or the transfer was
                    // cut short, and callers compare these bytes positionally -
                    // returning the prefix would corrupt a comparison rather
                    // than fail it. Retry first: a truncated transfer is often
                    // transient.
                    if (static_cast<int64_t>(data.size()) > expected_length) {
                        // The server ignored the Range header and sent more
                        // than was asked for - some gateways answer 200 rather
                        // than 206. Those bytes start at the object's start,
                        // not at `start`. Deterministic, so no retry.
                        spdlog::error("s3://{}/{} ignored the range {}-{}: got {} bytes for a "
                                      "{} byte range", bucket, key, start, end,
                                      data.size(), expected_length);
                        return {};
                    }
                    if (static_cast<int64_t>(data.size()) != expected_length) {
                        // One retry, not max_retries: see GetObjectRangeInto.
                        // A shrunk object now fails at the header check instead.
                        if (attempt < 1 && attempt < max_retries) {
                            int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                            spdlog::debug("Short read for s3://{}/{}: {} of {} bytes, retrying once",
                                          bucket, key, data.size(), expected_length);
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                            retry_delay_ms *= 2;
                            continue;
                        }
                        spdlog::error("Short read for s3://{}/{}: got {} of {} bytes",
                                      bucket, key, data.size(), expected_length);
                        return {};
                    }
                    return data;
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    // Add jitter (0-25%) to prevent thundering herd
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectRange s3://{}/{}: {}",
                                  attempt + 1, bucket, key, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::error("Failed to get object range: {}", error.GetMessage());
                return {};
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectRange s3://{}/{}: {}",
                                  attempt + 1, bucket, key, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::error("Exception in GetObjectRange: {}", e.what());
                return {};
            }
        }
        return {};  // Unreachable
    }

    bool GetObjectRangeInto(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end,
        std::vector<uint8_t>& buffer
    ) override {
        int64_t expected_length = 0;
        if (!ValidateRange(bucket, key, start, end, &expected_length)) {
            buffer.clear();
            return false;
        }

        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;
        const size_t expected_size = static_cast<size_t>(expected_length);

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::GetObjectRequest request;
                request.SetBucket(bucket.c_str());
                request.SetKey(key.c_str());

                std::ostringstream range_str;
                range_str << "bytes=" << start << "-" << end;
                request.SetRange(range_str.str().c_str());

                auto outcome = m_client->GetObject(request);
                if (outcome.IsSuccess()) {
                    if (!RangeResponseHonoursRequest(outcome.GetResult().GetContentRange(),
                                                     bucket, key, start, end,
                                                     m_allow_unverified_ranges)) {
                        buffer.clear();
                        return false;
                    }
                    auto& stream = outcome.GetResult().GetBody();
                    // Sizing the buffer is where an absurd - but arithmetically
                    // valid - range turns into a throw. Deterministic, so it is
                    // answered rather than retried five times.
                    try {
                        buffer.resize(expected_size);
                    } catch (const std::exception& e) {
                        buffer.clear();
                        spdlog::error("Cannot buffer {} bytes for s3://{}/{}: {}",
                                      expected_size, bucket, key, e.what());
                        return false;
                    }
                    stream.read(reinterpret_cast<char*>(buffer.data()), expected_size);
                    size_t bytes_read = static_cast<size_t>(stream.gcount());

                    if (bytes_read == expected_size && stream.peek() != std::char_traits<char>::eof()) {
                        // More bytes than the range asked for means the server
                        // ignored the Range header and sent the whole object -
                        // some gateways answer 200 instead of 206. The prefix
                        // sitting in the buffer is object[0..len), not
                        // object[start..start+len), so for any start > 0 it is
                        // the wrong data. Deterministic, so no retry.
                        buffer.clear();
                        spdlog::error("s3://{}/{} ignored the range {}-{} and returned more "
                                      "than was asked for", bucket, key, start, end);
                        return false;
                    }

                    if (bytes_read != expected_size) {
                        // This used to shrink the buffer and report success, so
                        // a caller that trusted the return value compared fewer
                        // bytes than it asked for and called that a match.
                        buffer.clear();
                        // One retry, not max_retries, and since the
                        // Content-Range check landed this reaches only the case
                        // that deserves it. An object that shrank is answered
                        // with a clamped Content-Range - measured on MinIO,
                        // "bytes 5-25/26" for a request for 5-1000 - which now
                        // fails at the header check without retrying. What is
                        // left here is a transfer cut off mid-body, where the
                        // header still names the full span, and that is worth a
                        // second attempt. The sync callers wrap this in a retry
                        // loop of their own, so the cost still multiplies.
                        if (attempt < 1 && attempt < max_retries) {
                            int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                            spdlog::debug("Short read for s3://{}/{}: {} of {} bytes, retrying once",
                                          bucket, key, bytes_read, expected_size);
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                            retry_delay_ms *= 2;
                            continue;
                        }
                        spdlog::error("Short read for s3://{}/{}: got {} of {} bytes",
                                      bucket, key, bytes_read, expected_size);
                        return false;
                    }
                    return true;
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectRangeInto s3://{}/{}: {}",
                                  attempt + 1, bucket, key, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::error("Failed to get object range: {}", error.GetMessage());
                buffer.clear();
                return false;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for GetObjectRangeInto s3://{}/{}: {}",
                                  attempt + 1, bucket, key, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::error("Exception in GetObjectRangeInto: {}", e.what());
                buffer.clear();
                return false;
            }
        }
        buffer.clear();
        return false;
    }

    S3ListResult ListObjects(
        const std::string& bucket,
        const std::string& prefix,
        const std::string& delimiter,
        const std::string& continuation_token,
        int max_keys
    ) override {
        S3ListResult result;
        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::ListObjectsV2Request request;
                request.SetBucket(bucket.c_str());
                request.SetMaxKeys(max_keys);

                if (!prefix.empty()) {
                    request.SetPrefix(prefix.c_str());
                }
                if (!delimiter.empty()) {
                    request.SetDelimiter(delimiter.c_str());
                }
                if (!continuation_token.empty()) {
                    request.SetContinuationToken(continuation_token.c_str());
                }

                auto outcome = m_client->ListObjectsV2(request);
                if (outcome.IsSuccess()) {
                    const auto& list_result = outcome.GetResult();
                    for (const auto& object : list_result.GetContents()) {
                        S3ObjectInfo info;
                        info.key = object.GetKey();
                        info.size = object.GetSize();
                        info.last_modified = object.GetLastModified().Seconds();
                        result.objects.push_back(std::move(info));
                    }
                    for (const auto& cp : list_result.GetCommonPrefixes()) {
                        result.common_prefixes.push_back(cp.GetPrefix());
                    }

                    result.is_truncated = list_result.GetIsTruncated();
                    if (result.is_truncated) {
                        result.next_continuation_token = list_result.GetNextContinuationToken();
                    }
                    result.success = true;
                    return result;
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    // Add jitter (0-25%) to prevent thundering herd
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for ListObjects s3://{}/{}: {}",
                                  attempt + 1, bucket, prefix, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                result.error_message = error.GetMessage();
                spdlog::error("Failed to list objects: {}", result.error_message);
                return result;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for ListObjects s3://{}/{}: {}",
                                  attempt + 1, bucket, prefix, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                result.error_message = std::string("Exception: ") + e.what();
                spdlog::error("Exception in ListObjects: {}", e.what());
                return result;
            }
        }
        return result;  // Unreachable
    }

    bool PutObject(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data
    ) override {
        try {
            Aws::S3::Model::PutObjectRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());

            auto stream = Aws::MakeShared<Aws::StringStream>("PutObject");
            stream->write(reinterpret_cast<const char*>(data.data()), data.size());
            request.SetBody(stream);
            request.SetContentLength(data.size());

            auto outcome = m_client->PutObject(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("PutObject rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to put object: {}", msg);
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in PutObject: {}", e.what());
            return false;
        }
    }

    // AWS wants the 4-byte big-endian CRC32, base64-encoded.
    static Aws::String Crc32ToBase64(uint32_t crc32) {
        uint8_t crc_bytes[4];
        crc_bytes[0] = (crc32 >> 24) & 0xFF;
        crc_bytes[1] = (crc32 >> 16) & 0xFF;
        crc_bytes[2] = (crc32 >> 8) & 0xFF;
        crc_bytes[3] = crc32 & 0xFF;
        Aws::Utils::ByteBuffer crc_buffer(crc_bytes, 4);
        return Aws::Utils::HashingUtils::Base64Encode(crc_buffer);
    }

    // Uploads the file directly, without a copy in memory.
    //
    // No checksum is supplied. With the algorithm set but no value, the SDK
    // computes the CRC32 over the body as it streams it, so the checksum
    // describes exactly the bytes that went out. Precomputing it here meant
    // checksumming one read of the file and uploading a second, independent
    // one - a file rewritten in between would have declared a checksum for
    // bytes that were never sent.
    bool PutObjectStreamed(
        const std::string& bucket,
        const std::string& key,
        const std::string& file_path,
        int64_t content_length
    ) {
        try {
            Aws::S3::Model::PutObjectRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());

            auto body = Aws::MakeShared<BoundedFileStream>(
                "PutObjectStreamed", file_path, static_cast<std::streamsize>(content_length));
            if (!body || !body->is_open()) {
                spdlog::error("Failed to open {} for upload", file_path);
                return false;
            }
            request.SetBody(body);
            request.SetContentLength(content_length);
            request.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::CRC32);

            auto outcome = m_client->PutObject(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("PutObject rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to put object with CRC32: {}", msg);
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in PutObjectStreamed: {}", e.what());
            return false;
        }
    }

    bool PutObjectWithCRC32(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data,
        uint32_t crc32
    ) override {
        try {
            Aws::S3::Model::PutObjectRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());

            auto stream = Aws::MakeShared<Aws::StringStream>("PutObjectWithCRC32");
            stream->write(reinterpret_cast<const char*>(data.data()), data.size());
            request.SetBody(stream);
            request.SetContentLength(data.size());

            // Set CRC32 checksum algorithm and value
            request.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::CRC32);

            request.SetChecksumCRC32(Crc32ToBase64(crc32));

            auto outcome = m_client->PutObject(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("PutObject rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to put object with CRC32: {}", msg);
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in PutObjectWithCRC32: {}", e.what());
            return false;
        }
    }

    bool PutObjectFromFile(
        const std::string& bucket,
        const std::string& key,
        const std::string& file_path
    ) override {
        try {
            // Neither the file nor a copy of it is held in memory. This used to
            // read the whole thing into a vector and then copy that vector into
            // a StringStream, so peak memory was twice the file size - measured
            // at 1039 MiB for a 512 MiB upload. The caller is named "small
            // file", but diff_upload_file falls back here for any file that is
            // not in the destination yet, which is every first upload of a
            // large one (issue #56).
            std::error_code ec;
            const auto file_size = std::filesystem::file_size(file_path, ec);
            if (ec) {
                spdlog::error("Cannot determine the size of {}: {}", file_path, ec.message());
                return false;
            }

            // PutObject tops out at 5 GiB. Reading a larger file only to be
            // told EntityTooLarge wastes the whole transfer, so say so first.
            // Uploading beyond this needs multipart, which this path does not
            // do - it failed before too, just slowly and after allocating
            // twice the file size.
            if (file_size > kMaxSinglePutBytes) {
                spdlog::error("{} is {} bytes, over the 5 GiB limit for a single PutObject. "
                              "Uploading a file this large needs multipart, which this path "
                              "does not do.", file_path, file_size);
                return false;
            }

            return PutObjectStreamed(bucket, key, file_path,
                                     static_cast<int64_t>(file_size));
        } catch (const std::exception& e) {
            spdlog::error("Exception in PutObjectFromFile: {}", e.what());
            return false;
        }
    }

    bool DeleteObject(const std::string& bucket, const std::string& key) override {
        try {
            Aws::S3::Model::DeleteObjectRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());

            auto outcome = m_client->DeleteObject(request);
            if (!outcome.IsSuccess()) {
                spdlog::error("Failed to delete object: {}", outcome.GetError().GetMessage());
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in DeleteObject: {}", e.what());
            return false;
        }
    }

    bool CopyObject(
        const std::string& source_bucket,
        const std::string& source_key,
        const std::string& dest_bucket,
        const std::string& dest_key,
        int64_t known_size = -1,
        const std::atomic<bool>* cancelled = nullptr
    ) override {
        // S3 CopyObject API has a 5GB limit. For larger files, use multipart copy.
        constexpr int64_t FIVE_GB = 5LL * 1024 * 1024 * 1024;
        constexpr int64_t MULTIPART_PART_SIZE = 500 * 1024 * 1024;  // 500MB parts for large files

        // Check cancellation before starting
        if (cancelled && cancelled->load()) {
            return false;
        }

        // Use provided size if available, otherwise fetch it
        int64_t source_size = known_size;
        if (source_size < 0) {
            source_size = GetObjectSize(source_bucket, source_key);
            if (source_size < 0) {
                spdlog::error("Failed to get size for copy source: {}/{}", source_bucket, source_key);
                return false;
            }
        }

        // For files <= 5GB, use simple CopyObject
        if (source_size <= FIVE_GB) {
            return CopyObjectSimple(source_bucket, source_key, dest_bucket, dest_key);
        }

        // For files > 5GB, use multipart copy
        spdlog::debug("Using multipart copy for large file ({} bytes): {}/{}",
                      source_size, source_bucket, source_key);
        return CopyObjectMultipart(source_bucket, source_key, dest_bucket, dest_key,
                                   source_size, MULTIPART_PART_SIZE, cancelled);
    }

private:
    // Simple single-request copy for files <= 5GB
    bool CopyObjectSimple(
        const std::string& source_bucket,
        const std::string& source_key,
        const std::string& dest_bucket,
        const std::string& dest_key
    ) {
        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::CopyObjectRequest request;
                request.SetBucket(dest_bucket.c_str());
                request.SetKey(dest_key.c_str());
                // CopySource format: "bucket/key"
                // Note: AWS SDK for C++ handles URL encoding internally
                std::string copy_source = source_bucket + "/" + source_key;
                request.SetCopySource(copy_source.c_str());

                auto outcome = m_client->CopyObject(request);
                if (outcome.IsSuccess()) {
                    return true;
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for CopyObject {}/{} -> {}/{}: {}",
                                  attempt + 1, source_bucket, source_key,
                                  dest_bucket, dest_key, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }

                // Non-retryable error or max retries exceeded
                // Rate limiting is expected with adaptive concurrency
                if (error.GetMessage().find("SlowDown") != std::string::npos ||
                    error.GetMessage().find("reduce your request rate") != std::string::npos) {
                    spdlog::debug("CopyObject rate limited: {}", error.GetMessage());
                } else {
                    spdlog::error("Failed to copy object {}/{} -> {}/{}: {}",
                                  source_bucket, source_key, dest_bucket, dest_key,
                                  error.GetMessage());
                }
                return false;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for CopyObject {}/{} -> {}/{}: {}",
                                  attempt + 1, source_bucket, source_key,
                                  dest_bucket, dest_key, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                spdlog::error("Exception in CopyObject: {}", e.what());
                return false;
            }
        }
        return false;  // Unreachable
    }

    // Multipart copy for files > 5GB
    bool CopyObjectMultipart(
        const std::string& source_bucket,
        const std::string& source_key,
        const std::string& dest_bucket,
        const std::string& dest_key,
        int64_t source_size,
        int64_t part_size,
        const std::atomic<bool>* cancelled
    ) {
        // Create multipart upload
        std::string upload_id = CreateMultipartUpload(dest_bucket, dest_key);
        if (upload_id.empty()) {
            spdlog::error("Failed to create multipart upload for copy: {}/{}", dest_bucket, dest_key);
            return false;
        }

        // Register upload for cleanup on crash/exit
        // Note: m_client is available since we're inside S3ClientImpl
        UploadRegistry::instance().register_upload({
            m_client,
            Aws::String(dest_bucket.c_str()),
            Aws::String(dest_key.c_str()),
            Aws::String(upload_id.c_str())
        });
        UploadGuard guard(Aws::String(upload_id.c_str()));

        std::vector<std::pair<int, S3PartResult>> completed_parts;
        bool success = true;

        // Calculate number of parts
        int64_t num_parts = (source_size + part_size - 1) / part_size;

        for (int64_t i = 0; i < num_parts && success; ++i) {
            // Check for cancellation before each part
            if (cancelled && cancelled->load()) {
                spdlog::debug("Multipart copy cancelled at part {}/{}", i + 1, num_parts);
                if (AbortMultipartUpload(dest_bucket, dest_key, upload_id)) {
                    guard.release();  // Only unregister if abort succeeded
                }
                return false;
            }

            int part_number = static_cast<int>(i + 1);  // S3 parts are 1-indexed
            int64_t start_byte = i * part_size;
            int64_t end_byte = std::min(start_byte + part_size - 1, source_size - 1);

            // Retry loop for each part
            S3PartResult part_result;
            int retry_delay_ms = 100;
            for (int attempt = 0; attempt <= m_max_retries; ++attempt) {
                // Check cancellation before retry attempts too
                if (cancelled && cancelled->load()) {
                    break;
                }

                if (attempt > 0) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for UploadPartCopy part {}", attempt, part_number);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                }

                part_result = UploadPartCopy(dest_bucket, dest_key, upload_id, part_number,
                                             source_bucket, source_key, start_byte, end_byte);
                if (part_result.ok()) {
                    break;
                }
            }

            if (!part_result.ok()) {
                spdlog::error("Failed to copy part {} after retries: {}/{}", part_number, source_bucket, source_key);
                success = false;
            } else {
                completed_parts.emplace_back(part_number, part_result);
            }
        }

        if (!success) {
            if (AbortMultipartUpload(dest_bucket, dest_key, upload_id)) {
                guard.release();  // Only unregister if abort succeeded
            }
            return false;
        }

        // Complete the multipart upload
        if (!CompleteMultipartUpload(dest_bucket, dest_key, upload_id, completed_parts)) {
            spdlog::error("Failed to complete multipart copy: {}/{}", dest_bucket, dest_key);
            if (AbortMultipartUpload(dest_bucket, dest_key, upload_id)) {
                guard.release();  // Only unregister if abort succeeded
            }
            return false;
        }

        guard.release();  // Unregister from cleanup registry - upload completed successfully
        spdlog::debug("Multipart copy completed: {}/{} -> {}/{} ({} parts)",
                      source_bucket, source_key, dest_bucket, dest_key, num_parts);
        return true;
    }

public:

    std::vector<std::string> DeleteObjects(
        const std::string& bucket,
        const std::vector<std::string>& keys
    ) override {
        std::vector<std::string> failed;
        if (keys.empty()) {
            return failed;
        }

        try {
            Aws::S3::Model::DeleteObjectsRequest request;
            request.SetBucket(bucket.c_str());

            Aws::S3::Model::Delete delete_obj;
            for (const auto& key : keys) {
                Aws::S3::Model::ObjectIdentifier obj_id;
                obj_id.SetKey(key.c_str());
                delete_obj.AddObjects(obj_id);
            }
            delete_obj.SetQuiet(true);  // Only return errors, not successes
            request.SetDelete(delete_obj);

            auto outcome = m_client->DeleteObjects(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("DeleteObjects rate limited: {}", msg);
                } else {
                    spdlog::error("DeleteObjects request failed: {}", msg);
                }
                // On error, S3 might have partially processed the request
                // Return all keys as failed - caller will retry
                return keys;
            }

            // Collect keys that failed to delete
            for (const auto& error : outcome.GetResult().GetErrors()) {
                failed.push_back(error.GetKey());
                spdlog::debug("Failed to delete {}: {}", error.GetKey(), error.GetMessage());
            }
            return failed;
        } catch (const std::exception& e) {
            spdlog::error("Exception in DeleteObjects: {}", e.what());
            // On exception, all keys failed
            return keys;
        }
    }

    bool CreateBucket(const std::string& bucket) override {
        try {
            Aws::S3::Model::CreateBucketRequest request;
            request.SetBucket(bucket.c_str());

            // For regions other than us-east-1, we need to specify location constraint
            if (m_region != "us-east-1") {
                Aws::S3::Model::CreateBucketConfiguration config;
                config.SetLocationConstraint(
                    Aws::S3::Model::BucketLocationConstraintMapper::GetBucketLocationConstraintForName(m_region.c_str())
                );
                request.SetCreateBucketConfiguration(config);
            }

            auto outcome = m_client->CreateBucket(request);
            if (!outcome.IsSuccess()) {
                // Bucket already exists is not an error for our purposes
                auto error_type = outcome.GetError().GetErrorType();
                if (error_type == Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS ||
                    error_type == Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU) {
                    return true;
                }
                spdlog::error("Failed to create bucket: {}", outcome.GetError().GetMessage());
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in CreateBucket: {}", e.what());
            return false;
        }
    }

    std::vector<uint32_t> GetChunkCRC32s(
        const std::string& bucket,
        const std::string& key,
        int64_t file_size,
        const std::vector<int64_t>& chunk_ids,
        std::function<void(double)> progress_cb,
        bool debug,
        int num_threads,
        bool ramp_up,
        int64_t chunk_size
    ) override {
        // Use default if not specified
        if (chunk_size <= 0) {
            chunk_size = DEFAULT_CHUNK_SIZE;
        }

        // Validate chunk_ids against the actual chunk count, not just the sign.
        //
        // An id past the end used to be accepted here and then used to index the
        // results vector in s3_utils.cpp, which is an out-of-bounds read rather
        // than an error (issue #52). The mock and the local path both reject the
        // whole request for such an id, so this now does too - three
        // implementations of one interface should not disagree about what a bad
        // id means (issue #26).
        // Written as a division plus a remainder rather than the usual
        // (file_size + chunk_size - 1) / chunk_size, which overflows for a
        // file_size near INT64_MAX - undefined behaviour inside the very check
        // that is supposed to be trustworthy.
        const int64_t num_chunks =
            file_size > 0 ? file_size / chunk_size + (file_size % chunk_size != 0 ? 1 : 0) : 0;
        for (int64_t id : chunk_ids) {
            if (id < 0 || id >= num_chunks) {
                spdlog::error("S3ClientImpl::GetChunkCRC32s: chunk_id {} is out of range for "
                              "s3://{}/{} ({} bytes, {} chunks of {})",
                              id, bucket, key, file_size, num_chunks, chunk_size);
                return {};
            }
        }

        if (debug) {
            spdlog::debug("GetChunkCRC32s: bucket='{}', key='{}', size={}, chunk_size={}",
                         bucket, key, file_size, chunk_size);
        }

        // Optimization for small files: download and compute CRC locally
        // This is 1 API call vs 3 for multipart copy (Create + Copy + Abort)
        if (file_size <= chunk_size) {
            if (file_size <= 0) {
                // Empty file - no chunks to compute (matches local file
                // behavior). A negative size lands here too: it is not a size,
                // and the alternative was a GET for "bytes=0--2".
                if (progress_cb) progress_cb(100.0);
                return {};
            }
            // Download the entire file
            auto data = GetObjectRange(bucket, key, 0, file_size - 1);
            if (data.empty()) {
                spdlog::error("Failed to download small file s3://{}/{}", bucket, key);
                return {};
            }
            // A short body is not this object. Checksumming the prefix would
            // hand back a confident wrong answer of exactly the kind the rest
            // of this function now refuses to produce.
            if (static_cast<int64_t>(data.size()) != file_size) {
                spdlog::error("Short read for s3://{}/{}: got {} of {} bytes",
                              bucket, key, data.size(), file_size);
                return {};
            }
            // Compute CRC32
            uint32_t crc = crc32_hw(data.data(), data.size());
            if (progress_cb) progress_cb(100.0);

            // Answer in the shape that was asked for: one entry per requested
            // id, in request order, duplicates included. This path used to
            // return a single CRC whatever the request, so {0, 0} came back
            // with one entry where the local path and the mock give two - and
            // the caller compares these vectors position by position (#26).
            // Every valid id is 0 here, since the object is one chunk.
            if (chunk_ids.empty()) {
                return {crc};
            }
            return std::vector<uint32_t>(chunk_ids.size(), crc);
        }

        // Use unique suffix to avoid collisions when multiple threads compare the same object
        std::string dst_key = key + "_crc32_tmp_" + GenerateUniqueSuffix();
        S3MultipartCopy copier(
            m_client,
            bucket.c_str(),
            key.c_str(),
            dst_key.c_str(),
            file_size,
            debug,
            chunk_size,
            m_max_retries
        );
        return copier.GetHashes(chunk_ids, progress_cb, num_threads, ramp_up);
    }

    std::string CreateMultipartUpload(
        const std::string& bucket,
        const std::string& key
    ) override {
        try {
            Aws::S3::Model::CreateMultipartUploadRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());
            request.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::CRC32);

            auto outcome = m_client->CreateMultipartUpload(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("CreateMultipartUpload rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to create multipart upload: {}", msg);
                }
                return "";
            }
            return outcome.GetResult().GetUploadId();
        } catch (const std::exception& e) {
            spdlog::error("Exception in CreateMultipartUpload: {}", e.what());
            return "";
        }
    }

    S3PartResult UploadPart(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::vector<uint8_t>& data,
        uint32_t crc32
    ) override {
        try {
            Aws::S3::Model::UploadPartRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());
            request.SetUploadId(upload_id.c_str());
            request.SetPartNumber(part_number);

            auto stream = Aws::MakeShared<Aws::StringStream>("UploadPart");
            stream->write(reinterpret_cast<const char*>(data.data()), data.size());
            request.SetBody(stream);
            request.SetContentLength(data.size());

            // Set CRC32 checksum
            request.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::CRC32);
            uint8_t crc_bytes[4];
            crc_bytes[0] = (crc32 >> 24) & 0xFF;
            crc_bytes[1] = (crc32 >> 16) & 0xFF;
            crc_bytes[2] = (crc32 >> 8) & 0xFF;
            crc_bytes[3] = crc32 & 0xFF;
            Aws::Utils::ByteBuffer crc_buffer(crc_bytes, 4);
            Aws::String crc_base64 = Aws::Utils::HashingUtils::Base64Encode(crc_buffer);
            request.SetChecksumCRC32(crc_base64);

            auto outcome = m_client->UploadPart(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("UploadPart rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to upload part {}: {}", part_number, msg);
                }
                return {};
            }
            // Echo back what S3 recorded rather than the value sent above: the
            // completion request has to name the checksum S3 stored, and if the
            // two ever disagreed, sending ours would hide that.
            return S3PartResult{outcome.GetResult().GetETag(),
                                outcome.GetResult().GetChecksumCRC32()};
        } catch (const std::exception& e) {
            spdlog::error("Exception in UploadPart: {}", e.what());
            return {};
        }
    }

    S3PartResult UploadPartCopy(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::string& source_bucket,
        const std::string& source_key,
        int64_t start_byte,
        int64_t end_byte
    ) override {
        try {
            Aws::S3::Model::UploadPartCopyRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());
            request.SetUploadId(upload_id.c_str());
            request.SetPartNumber(part_number);

            // Set copy source - SDK handles URL encoding
            std::string copy_source = source_bucket + "/" + source_key;
            request.SetCopySource(copy_source.c_str());

            // Set byte range (inclusive)
            std::ostringstream range_str;
            range_str << "bytes=" << start_byte << "-" << end_byte;
            request.SetCopySourceRange(range_str.str().c_str());

            auto outcome = m_client->UploadPartCopy(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("UploadPartCopy rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to copy part {}: {}", part_number, msg);
                }
                return {};
            }
            // The copied bytes never pass through this process, so the response
            // is the only place this part's checksum can come from.
            const auto& copied = outcome.GetResult().GetCopyPartResult();
            return S3PartResult{copied.GetETag(), copied.GetChecksumCRC32()};
        } catch (const std::exception& e) {
            spdlog::error("Exception in UploadPartCopy: {}", e.what());
            return {};
        }
    }

    bool CompleteMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        const std::vector<std::pair<int, S3PartResult>>& parts
    ) override {
        try {
            Aws::S3::Model::CompleteMultipartUploadRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());
            request.SetUploadId(upload_id.c_str());

            Aws::S3::Model::CompletedMultipartUpload completed_upload;
            for (const auto& [part_num, result] : parts) {
                Aws::S3::Model::CompletedPart part;
                part.SetPartNumber(part_num);
                part.SetETag(result.etag.c_str());
                // CreateMultipartUpload declares CRC32, and an upload that
                // declares a checksum algorithm must name each part's checksum
                // here too or the whole upload is rejected with InvalidPart -
                // after every byte has been sent. Set only when the part
                // actually reported one, so an endpoint that does not do
                // server-side checksums still completes (issue #98).
                if (!result.checksum_crc32.empty()) {
                    part.SetChecksumCRC32(result.checksum_crc32.c_str());
                }
                completed_upload.AddParts(part);
            }
            request.SetMultipartUpload(completed_upload);

            auto outcome = m_client->CompleteMultipartUpload(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("CompleteMultipartUpload rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to complete multipart upload: {}", msg);
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in CompleteMultipartUpload: {}", e.what());
            return false;
        }
    }

    bool AbortMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id
    ) override {
        try {
            Aws::S3::Model::AbortMultipartUploadRequest request;
            request.SetBucket(bucket.c_str());
            request.SetKey(key.c_str());
            request.SetUploadId(upload_id.c_str());

            auto outcome = m_client->AbortMultipartUpload(request);
            if (!outcome.IsSuccess()) {
                const auto& msg = outcome.GetError().GetMessage();
                // Rate limiting is expected with adaptive concurrency - log at debug level
                if (msg.find("reduce your request rate") != std::string::npos ||
                    msg.find("SlowDown") != std::string::npos) {
                    spdlog::debug("AbortMultipartUpload rate limited: {}", msg);
                } else {
                    spdlog::error("Failed to abort multipart upload: {}", msg);
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Exception in AbortMultipartUpload: {}", e.what());
            return false;
        }
    }

    S3ListMultipartUploadsResult ListMultipartUploads(
        const std::string& bucket,
        const std::string& prefix,
        const std::string& key_marker,
        const std::string& upload_id_marker,
        int max_uploads
    ) override {
        S3ListMultipartUploadsResult result;
        const int max_retries = m_max_retries;
        int retry_delay_ms = 100;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                Aws::S3::Model::ListMultipartUploadsRequest request;
                request.SetBucket(bucket.c_str());
                if (!prefix.empty()) {
                    request.SetPrefix(prefix.c_str());
                }
                if (!key_marker.empty()) {
                    request.SetKeyMarker(key_marker.c_str());
                }
                if (!upload_id_marker.empty()) {
                    request.SetUploadIdMarker(upload_id_marker.c_str());
                }
                request.SetMaxUploads(max_uploads);

                auto outcome = m_client->ListMultipartUploads(request);

                if (outcome.IsSuccess()) {
                    const auto& response = outcome.GetResult();
                    result.is_truncated = response.GetIsTruncated();
                    if (result.is_truncated) {
                        result.next_key_marker = response.GetNextKeyMarker();
                        result.next_upload_id_marker = response.GetNextUploadIdMarker();
                    }

                    for (const auto& upload : response.GetUploads()) {
                        S3MultipartUploadInfo info;
                        info.key = upload.GetKey();
                        info.upload_id = upload.GetUploadId();
                        // Convert AWS DateTime to system_clock
                        auto aws_time = upload.GetInitiated();
                        info.initiated = std::chrono::system_clock::from_time_t(
                            static_cast<time_t>(aws_time.SecondsWithMSPrecision())
                        );
                        result.uploads.push_back(std::move(info));
                    }

                    result.success = true;
                    return result;
                }

                const auto& error = outcome.GetError();
                if (attempt < max_retries && IsRetryableS3Error(error)) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for ListMultipartUploads s3://{}: {}",
                                  attempt + 1, bucket, error.GetMessage());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                result.error_message = error.GetMessage();
                spdlog::error("ListMultipartUploads failed: {}", result.error_message);
                return result;
            } catch (const std::exception& e) {
                if (attempt < max_retries) {
                    int delay_with_jitter = retry_delay_ms + GetJitter(retry_delay_ms);
                    spdlog::debug("Retry {} for ListMultipartUploads s3://{}: {}",
                                  attempt + 1, bucket, e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_with_jitter));
                    retry_delay_ms *= 2;
                    continue;
                }
                result.error_message = e.what();
                spdlog::error("Exception in ListMultipartUploads: {}", e.what());
                return result;
            }
        }

        return result;
    }

private:
    std::shared_ptr<Aws::S3::S3Client> m_client;
    std::string m_region;
    bool m_use_path_style;  // Initialized in constructor based on endpoint_override
    int m_max_retries;
    bool m_allow_unverified_ranges;      // Snapshot of settings at construction time
};

// Factory function implementation
std::shared_ptr<IS3Client> CreateS3Client(
    const std::string& region,
    const std::string& endpoint_override,
    int max_connections,
    const std::string& profile
) {
    return std::make_shared<S3ClientImpl>(region, endpoint_override, max_connections, profile);
}

// Test-only factory: see include/s3_client_testing.h
std::shared_ptr<IS3Client> CreateS3ClientForTesting(
    std::shared_ptr<Aws::S3::S3Client> client,
    const std::string& region,
    bool use_path_style,
    int max_retries
) {
    return std::make_shared<S3ClientImpl>(std::move(client), region, use_path_style, max_retries);
}
