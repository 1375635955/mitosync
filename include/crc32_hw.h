#pragma once
#include <cstdint>
#include <cstddef>

// Diagnostics only. has_hw_crc32() says whether this CPU has an instruction set
// zlib-ng accelerates CRC32 with, and hw_crc32_name() names the implementation
// it will pick; both are detected at runtime. Nothing consults them to decide
// how a CRC is computed - crc32_hw() is always the same call.
bool has_hw_crc32();
const char* hw_crc32_name();

// IEEE CRC-32 (polynomial 0x04C11DB7, the flavour zlib and S3 use) of the whole
// buffer, computed by zlib-ng, which selects an accelerated implementation for
// the running CPU. length is honoured in full: there is no 32-bit truncation.
//
// This header pulls in neither <zlib.h> nor <zlib-ng.h>, which is deliberate:
// both libraries are linked into mito, and including both in one translation
// unit does not compile (they define conflicting types and macros). Everything
// that needs a CRC includes this instead.
uint32_t crc32_hw(const uint8_t* data, size_t length);

// The same CRC, computed a piece at a time. Seed the first call with 0 and feed
// each subsequent chunk the value the previous call returned; the result equals
// crc32_hw() over the concatenation. Lets a stream be checksummed as it is read
// instead of being held in memory whole.
//
// Nothing in src/ calls this today: uploads let the SDK checksum the body it
// sends. It exists for callers that cannot reach zlib-ng themselves - see the
// note above about why this header hides it - and its only user is the test
// that checks what a streamed upload actually put on the wire.
uint32_t crc32_hw_update(uint32_t crc, const uint8_t* data, size_t length);
