#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

// S3 refuses a single PutObject larger than this; anything bigger has to go up
// as a multipart upload. Shared rather than written out at each call site so
// the mock and the real client cannot drift apart about what is a legal
// request - a test written against a more permissive mock proves nothing about
// what the real client would do (issue #92).
constexpr uint64_t kMaxSinglePutBytes = 5ull * 1024 * 1024 * 1024;

// S3 accepts at most this many parts in one multipart upload. A path that maps
// a fixed-size chunk to a part therefore has a file size ceiling, and has to
// say so before uploading rather than at CompleteMultipartUpload.
constexpr size_t kMaxMultipartParts = 10000;

// What one uploaded or copied part of a multipart upload came back as.
//
// The checksum is not decoration. When CreateMultipartUpload declares a
// checksum algorithm, CompleteMultipartUpload has to name each part's checksum
// as well as its ETag; leave them out and the upload is rejected with
// InvalidPart - an error that talks about ETags and says nothing about
// checksums - after every byte has already been sent. Carrying the value from
// the part response to the completion request is the only way to satisfy that,
// and for a copied part it is the only place the value can come from at all:
// the bytes never pass through this process (issue #98).
struct S3PartResult {
    std::string etag;            // empty means the part failed
    std::string checksum_crc32;  // base64, as S3 returns it; empty if none

    bool ok() const { return !etag.empty(); }
};

// Object info returned from S3 listing
struct S3ObjectInfo {
    std::string key;
    int64_t size = 0;
    // LastModified, Unix epoch seconds. 0 means the listing did not report one.
    int64_t last_modified = 0;
};

// Result of listing S3 objects
struct S3ListResult {
    std::vector<S3ObjectInfo> objects;        // Object keys and sizes
    std::vector<std::string> common_prefixes; // Common prefixes (folders)
    std::string next_continuation_token;      // For pagination
    bool is_truncated = false;                // More results available
    bool success = false;
    std::string error_message;
};

// Info about an active multipart upload
struct S3MultipartUploadInfo {
    std::string key;
    std::string upload_id;
    std::chrono::system_clock::time_point initiated;
};

// Result of listing multipart uploads
struct S3ListMultipartUploadsResult {
    std::vector<S3MultipartUploadInfo> uploads;
    std::string next_key_marker;
    std::string next_upload_id_marker;
    bool is_truncated = false;
    bool success = false;
    std::string error_message;
};

// Outcome of asking whether an object exists.
//
// A plain size lookup cannot answer this: it returns the same sentinel for
// "not there" and for "the request failed", so a permission error, a throttle
// or a network fault reads as absence. Callers that treat absence as proof of
// deletion need the third state.
enum class S3ObjectPresence {
    Exists,    // HeadObject succeeded
    NotFound,  // the service said the key does not exist
    Unknown    // the request failed for some other reason
};

// Abstract interface for S3 operations
// Allows dependency injection for testing
class IS3Client {
public:
    virtual ~IS3Client() = default;

    // Get the size of an S3 object. Returns -1 on error.
    virtual int64_t GetObjectSize(const std::string& bucket, const std::string& key) = 0;

    // Ask whether an object exists, distinguishing a definite "not found" from
    // a failed lookup. Never infer absence from GetObjectSize returning -1.
    virtual S3ObjectPresence CheckObjectPresence(const std::string& bucket,
                                                 const std::string& key) = 0;

    // Get a byte range from an S3 object. Both ends are inclusive.
    //
    // The range must satisfy 0 <= start <= end and must lie entirely within the
    // object. A range that is reversed or negative is rejected outright, before
    // any request is sent. A range that reaches past the last byte also fails,
    // but not for free: an implementation talking to S3 cannot know the object
    // size without asking, so it discovers the overrun from the short answer
    // and only then fails. Either way the range is never clamped.
    //
    // Success means exactly (end - start + 1) bytes, and the response must
    // show they are the bytes that were asked for. A shorter answer - the
    // object shrank, or the transfer was cut off - is a failure, because
    // callers compare these bytes positionally and a prefix would read as a
    // difference in the wrong place rather than as an error (issue #23).
    //
    // Length alone is not proof of position: an endpoint that ignores Range and
    // returns a whole object of exactly the requested length would otherwise
    // pass off object[0..n) as object[start..start+n). An implementation that
    // talks HTTP must check Content-Range; it may accept its absence only for a
    // read starting at 0, where the whole object and the requested range are
    // the same bytes (issue #76).
    //
    // Returns empty vector on error
    virtual std::vector<uint8_t> GetObjectRange(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end
    ) = 0;

    // Get a byte range from an S3 object into an existing buffer (for memory
    // reuse). Same contract as GetObjectRange above: the range must be valid
    // and fully in bounds, and success means the buffer holds exactly
    // (end - start + 1) bytes.
    //
    // On failure the buffer is left empty rather than holding a partial read.
    // It used to be resized down to whatever arrived and the call still
    // reported success, so a caller that checked only the return value
    // compared fewer bytes than it asked for (issue #23).
    virtual bool GetObjectRangeInto(
        const std::string& bucket,
        const std::string& key,
        int64_t start,
        int64_t end,
        std::vector<uint8_t>& buffer
    ) = 0;

    // List objects in a bucket with optional prefix
    virtual S3ListResult ListObjects(
        const std::string& bucket,
        const std::string& prefix = "",
        const std::string& delimiter = "/",
        const std::string& continuation_token = "",
        int max_keys = 1000
    ) = 0;

    // Upload an object (primarily for tests)
    virtual bool PutObject(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data
    ) = 0;

    // Upload an object with CRC32 checksum verification
    // Returns true on success, false on failure
    virtual bool PutObjectWithCRC32(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data,
        uint32_t crc32
    ) = 0;

    // Upload an object from a file path with CRC32 checksum
    virtual bool PutObjectFromFile(
        const std::string& bucket,
        const std::string& key,
        const std::string& file_path
    ) = 0;

    // Delete an object (primarily for test cleanup)
    virtual bool DeleteObject(const std::string& bucket, const std::string& key) = 0;

    // Copy an object server-side (S3 to S3, no data through client)
    // For objects > 5GB, implementations should use multipart copy internally
    // source_size: if >= 0, use this value instead of calling HeadObject (optimization)
    // cancelled: optional atomic bool to check for cancellation during multipart copies
    virtual bool CopyObject(
        const std::string& source_bucket,
        const std::string& source_key,
        const std::string& dest_bucket,
        const std::string& dest_key,
        int64_t source_size = -1,
        const std::atomic<bool>* cancelled = nullptr
    ) = 0;

    // Delete up to 1000 objects in one batch request
    // Returns vector of keys that failed to delete (empty on full success)
    virtual std::vector<std::string> DeleteObjects(
        const std::string& bucket,
        const std::vector<std::string>& keys
    ) = 0;

    // Create a bucket (primarily for tests)
    virtual bool CreateBucket(const std::string& bucket) = 0;

    // Compute CRC32 checksums for chunks of an S3 object
    // Uses the multipart copy trick to get S3-computed checksums
    //
    // The contract, which every implementation must honour identically because
    // callers compare two implementations' results position by position:
    //
    //   - chunk_ids empty: one entry per chunk of the object, in order.
    //   - chunk_ids given: one entry per requested id, in the order requested,
    //     duplicates included. {0, 0} yields two entries.
    //   - any id outside [0, num_chunks) fails the whole request. It is not
    //     skipped and not answered with a neighbouring chunk's checksum: a
    //     wrong value cannot be told apart from a right one by the caller.
    //   - failure of any kind returns an empty vector. Never a partial result
    //     and never a zero standing in for a chunk that was not read - zero is
    //     a checksum a real chunk can produce.
    //
    // The same contract is implemented by compute_crc32_chunks_boost_asio() in
    // include/crc32_chunks.h for local files, and by MockS3Client.
    //
    // num_threads: number of threads in pool (0 = unbounded/std::async, default 64)
    // ramp_up: gradually increase concurrency to avoid DNS issues
    // chunk_size: size of each chunk in bytes (default 8 MiB)
    virtual std::vector<uint32_t> GetChunkCRC32s(
        const std::string& bucket,
        const std::string& key,
        int64_t file_size,
        const std::vector<int64_t>& chunk_ids = {},
        std::function<void(double)> progress_cb = nullptr,
        bool debug = false,
        int num_threads = 64,
        bool ramp_up = false,
        int64_t chunk_size = 8 * 1024 * 1024
    ) = 0;

    // Create a multipart upload, returns upload ID or empty string on failure
    virtual std::string CreateMultipartUpload(
        const std::string& bucket,
        const std::string& key
    ) = 0;

    // Upload a part from data with CRC32 verification
    // Returns the part's ETag and checksum; empty ETag means failure
    virtual S3PartResult UploadPart(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::vector<uint8_t>& data,
        uint32_t crc32
    ) = 0;

    // Copy a part from an existing object
    // Returns the part's ETag and checksum; empty ETag means failure
    virtual S3PartResult UploadPartCopy(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        int part_number,
        const std::string& source_bucket,
        const std::string& source_key,
        int64_t start_byte,
        int64_t end_byte
    ) = 0;

    // Complete a multipart upload from the results of its parts
    // Returns true on success
    virtual bool CompleteMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id,
        const std::vector<std::pair<int, S3PartResult>>& parts  // (part_num, result)
    ) = 0;

    // Abort a multipart upload
    virtual bool AbortMultipartUpload(
        const std::string& bucket,
        const std::string& key,
        const std::string& upload_id
    ) = 0;

    // List active multipart uploads in a bucket
    virtual S3ListMultipartUploadsResult ListMultipartUploads(
        const std::string& bucket,
        const std::string& prefix = "",
        const std::string& key_marker = "",
        const std::string& upload_id_marker = "",
        int max_uploads = 1000
    ) = 0;
};

// Factory function to create real S3 client
// max_connections: size of HTTP connection pool (default 128, scales with expected concurrency)
// profile: AWS credentials profile name (empty = default credentials chain)
std::shared_ptr<IS3Client> CreateS3Client(
    const std::string& region,
    const std::string& endpoint_override = "",
    int max_connections = 128,
    const std::string& profile = ""
);
