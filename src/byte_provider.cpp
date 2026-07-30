#include "byte_provider.h"
#include "s3_interface.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

class LocalByteProvider : public IByteProvider {
public:
    explicit LocalByteProvider(const std::string& path)
        : path_(path), size_(-1) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            size_ = std::filesystem::file_size(path, ec);
            if (ec) size_ = -1;
        }
    }

    int64_t size() const override { return size_; }

    bool read(int64_t offset, int64_t length, std::vector<uint8_t>& buffer) override {
        // Invalid offset
        if (size_ < 0 || offset < 0 || offset > size_) {
            buffer.clear();
            return false;
        }
        // Valid: reading 0 bytes or at EOF
        if (length <= 0 || offset == size_) {
            buffer.clear();
            return true;
        }

        std::ifstream file(path_, std::ios::binary);
        if (!file) {
            // Every other failure path here clears the buffer. These two did
            // not, so a failed local read left whatever the caller passed in
            // still sitting there, while the S3 provider returned it empty.
            buffer.clear();
            return false;
        }

        file.seekg(offset);
        if (!file) {
            buffer.clear();
            return false;
        }

        // Clamp length to available bytes
        int64_t available = size_ - offset;
        int64_t to_read = std::min(length, available);

        buffer.resize(static_cast<size_t>(to_read));
        file.read(reinterpret_cast<char*>(buffer.data()), to_read);

        if (!file && !file.eof()) {
            buffer.clear();
            return false;
        }

        buffer.resize(static_cast<size_t>(file.gcount()));
        return true;
    }

    bool is_local() const override { return true; }

private:
    std::string path_;
    int64_t size_;
};

std::unique_ptr<IByteProvider> CreateLocalByteProvider(const std::string& path) {
    auto provider = std::make_unique<LocalByteProvider>(path);
    if (provider->size() < 0) return nullptr;
    return provider;
}

class S3ByteProvider : public IByteProvider {
public:
    S3ByteProvider(const std::string& bucket, const std::string& key,
                   std::shared_ptr<IS3Client> client)
        : bucket_(bucket), key_(key), client_(client), size_(-1) {
        if (client_) {
            size_ = client_->GetObjectSize(bucket_, key_);
        }
    }

    int64_t size() const override { return size_; }

    bool read(int64_t offset, int64_t length, std::vector<uint8_t>& buffer) override {
        // Invalid state or offset
        if (!client_ || size_ < 0 || offset < 0 || offset > size_) {
            buffer.clear();
            return false;
        }
        // Valid: reading 0 bytes or at EOF
        if (length <= 0 || offset == size_) {
            buffer.clear();
            return true;
        }

        // Clamp before adding, not after. "offset + length - 1" overflows
        // int64_t for a large length, which is undefined behaviour and in
        // practice wraps negative - the clamp then keeps the negative value and
        // asks S3 for a backwards range. LocalByteProvider already computes it
        // this way; this is the same arithmetic.
        const int64_t available = size_ - offset;
        const int64_t to_read = std::min(length, available);
        const int64_t end = offset + to_read - 1;
        buffer = client_->GetObjectRange(bucket_, key_, offset, end);
        return !buffer.empty();
    }

    bool is_local() const override { return false; }

private:
    std::string bucket_;
    std::string key_;
    std::shared_ptr<IS3Client> client_;
    int64_t size_;
};

std::unique_ptr<IByteProvider> CreateS3ByteProvider(
    const std::string& bucket,
    const std::string& key,
    std::shared_ptr<IS3Client> client) {
    auto provider = std::make_unique<S3ByteProvider>(bucket, key, client);
    if (provider->size() < 0) return nullptr;
    return provider;
}
