#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "s3_interface.h"

// Abstract interface for reading bytes from a file
class IByteProvider {
public:
    virtual ~IByteProvider() = default;

    // Get total size of the file in bytes
    virtual int64_t size() const = 0;

    // Read bytes from offset into buffer
    // Returns true on success (buffer resized to actual bytes read)
    //
    // Note the two implementations differ when the data shrinks under the size
    // this provider was constructed with: the local one returns what it could
    // read and reports success, while the S3 one fails, because IS3Client
    // treats a short range read as an error (issue #23). Nothing consumes this
    // interface in production yet; whichever meaning is wanted should be
    // settled before something does.
    // Returns false on error
    virtual bool read(int64_t offset, int64_t length, std::vector<uint8_t>& buffer) = 0;

    // Returns true if this is a local file (false for S3)
    virtual bool is_local() const = 0;
};

// Factory function for local file provider
// Returns nullptr if file doesn't exist or can't be opened
std::unique_ptr<IByteProvider> CreateLocalByteProvider(const std::string& path);

// Factory function for S3 byte provider
// Returns nullptr if object doesn't exist
std::unique_ptr<IByteProvider> CreateS3ByteProvider(
    const std::string& bucket,
    const std::string& key,
    std::shared_ptr<IS3Client> client
);
