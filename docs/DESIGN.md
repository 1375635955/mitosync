# MitoSync - Technical Design Document

## 1. Overview

**MitoSync** is a C++ utility for validating CRC32 checksums between local files and Amazon S3 objects. It computes checksums in parallel on both the local filesystem and S3, enabling fast data integrity verification for large files.

### 1.1 Key Capabilities

- Parallel CRC32 computation on local files using bounded `pread` buffers
- S3 multipart copy with server-side CRC32 extraction
- Hardware-accelerated CRC32 from zlib-ng, selected per CPU at run time
- Cross-platform support (Linux x86_64, macOS x86_64/ARM64)
- Real-time progress reporting

### 1.2 Use Cases

- Verify data integrity after cloud uploads
- Compare local and remote copies of large files
- Spot-check specific chunks of multi-gigabyte files

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         main.cpp                                │
│                    (Orchestration Layer)                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────────┐         ┌─────────────────────────┐   │
│  │   std::async        │         │     std::async          │   │
│  │   (S3 Task)         │         │     (Local Task)        │   │
│  └──────────┬──────────┘         └───────────┬─────────────┘   │
│             │                                │                  │
│             ▼                                ▼                  │
│  ┌─────────────────────┐         ┌─────────────────────────┐   │
│  │  S3MultipartCopy    │         │ compute_crc32_chunks    │   │
│  │  (s3_utils.cpp)     │         │ (crc32_chunks.cpp)      │   │
│  └──────────┬──────────┘         └───────────┬─────────────┘   │
│             │                                │                  │
│             ▼                                ▼                  │
│  ┌─────────────────────┐         ┌─────────────────────────┐   │
│  │   AWS SDK S3        │         │  Boost.ASIO Thread Pool │   │
│  │   (UploadPartCopy)  │         │  + pread + crc32_hw     │   │
│  └─────────────────────┘         └─────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 Design Principles

1. **Parallel Execution**: Local and S3 operations run concurrently via `std::async`
2. **Chunked Processing**: Files divided into 8 MiB chunks for parallel computation
3. **Hardware Acceleration**: SIMD instructions when available, graceful fallback otherwise
4. **Resource Efficiency**: Single file descriptor, bounded read buffers, connection pooling

---

## 3. Components

### 3.1 Main Orchestrator (`main.cpp`)

**Responsibilities:**
- CLI argument parsing (local file path)
- AWS SDK initialization and configuration
- Launch parallel tasks for S3 and local CRC32 computation
- Display real-time progress
- Compare results and report mismatches

**Usage:**
```bash
./mito /path/to/local/file
```

**Key Configuration:**
```cpp
config.region = Aws::Region::EU_WEST_2;
config.maxConnections = 512;
setenv("AWS_EC2_METADATA_DISABLED", "true", 1);  // Faster init
```

### 3.2 S3 Operations (`s3_utils.cpp`)

**Free Function:**
| Function | Purpose |
|----------|---------|
| `GetS3ObjectSize()` | Get object size via HeadObject API, returns -1 on error |

**Class:** `S3MultipartCopy`

**Methods:**
| Method | Purpose |
|--------|---------|
| `CreateMultipartUpload()` | Initiate S3 multipart upload, get upload ID |
| `ParallelUploadPartCopyRequests()` | Copy chunks in parallel, extract CRC32 from responses |
| `GetHashes()` | Orchestrate flow, return CRC32 vector |
| `AbortMultipartUpload()` | Cleanup to prevent orphaned uploads |

**CRC32 Extraction:**
```cpp
// S3 returns CRC32 as Base64
Aws::String b64_crc32 = response.GetCopyPartResult().GetChecksumCRC32();
Aws::Utils::ByteBuffer decoded = Aws::Utils::HashingUtils::Base64Decode(b64_crc32);
uint32_t crc = /* convert bytes to uint32 */;
```

### 3.3 Local CRC32 Computation (`crc32_chunks.cpp`)

**Function:** `compute_crc32_chunks_boost_asio()`

**Algorithm:**
1. Open file once (single file descriptor)
2. Create thread pool with `hardware_concurrency()` threads
3. For each chunk:
   - Calculate offset: `chunk_id * CHUNK_SIZE`
   - Read the chunk in 256 KiB blocks with `pread()`
   - Feed each block into zlib-ng through `crc32_hw()`
   - Treat short reads or read errors as chunk failure
   - Update progress after successful bytes are processed

**Why pread?**
- A shrinking file reports an ordinary short read instead of killing the process
  with `SIGBUS`
- 256 KiB buffers stay cache-friendly while avoiding whole-chunk copies
- No page-alignment requirement, so unusual chunk sizes still work

### 3.4 CRC32 (`crc32_hw.cpp`)

**MitoSync writes no CRC arithmetic of its own.** Every checksum comes from
zlib-ng, which selects an implementation at run time from the CPU it finds
(issue #50):

```cpp
uint32_t crc32_hw(const uint8_t* data, size_t length) {
    return zng_crc32_z(0, data, length);
}
```

| CPU | zlib-ng dispatches to |
|-----|-----------------------|
| x86 with AVX-512 and VPCLMULQDQ | `crc32_vpclmulqdq` |
| x86 with PCLMULQDQ | `crc32_pclmulqdq` |
| ARM64 reporting the optional CRC32 extension | `crc32_acle` |
| anything else | `crc32_braid`, portable C |

This replaced two hand-written implementations. The x86 one was a PCLMUL block
removed in issue #18 for producing wrong CRC32 values on any 16-byte-aligned
buffer of 64 bytes or more; it was compiled only when `__PCLMUL__` was defined,
which `-msse4.2` does not do but `-march=native` does, so release builds were
unaffected while local native builds silently computed wrong checksums for every
chunk. The ARM one was correct on the shipped targets but assumed little-endian
loads without saying so (issue #49). Folding plus Barrett reduction is worth the
throughput — about 5x over zlib on one core — but it has to be re-verified per
architecture, which is the job a maintained library does better than this repo.

Note that SSE4.2's `_mm_crc32_*` instructions cannot substitute for any of this:
they implement CRC-32C (Castagnoli, `0x1EDC6F41`), a different polynomial from
the IEEE CRC-32 that zlib and S3 use.

The length is a `size_t` all the way through. The earlier `crc32(0, data,
static_cast<uInt>(length))` truncated at 4 GiB, so a single-part upload of a
larger file sent S3 a checksum over `length mod 2^32` bytes (issues #48, #21).

**Reporting (also in `crc32_hw.cpp`):** `has_hw_crc32()` and `hw_crc32_name()`
exist only for the `--debug` log. They detect the CPU at run time — CPUID on
x86, `HWCAP_CRC32`/`sysctlbyname` on ARM64 — rather than reporting what the
compiler was told, which is how the old version came to claim `"x86 PCLMUL"` on
builds containing no PCLMUL code (issue #22). Nothing consults them to decide
how a CRC is computed.

### 3.5 Constants (`constants.h`)

```cpp
constexpr size_t CHUNK_SIZE = 8 * 1024 * 1024;  // 8 MiB
```

---

## 4. Data Flow

```
Input:
  Local File: /path/to/file
  S3 Object:  s3://bucket/key

                    ┌──────────────────┐
                    │   Main Thread    │
                    └────────┬─────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
              ▼                             ▼
    ┌─────────────────┐           ┌─────────────────┐
    │  S3 Task        │           │  Local Task     │
    │  (async)        │           │  (async)        │
    └────────┬────────┘           └────────┬────────┘
             │                             │
             ▼                             ▼
    ┌─────────────────┐           ┌─────────────────┐
    │ For each chunk: │           │ For each chunk: │
    │ UploadPartCopy  │           │ pread blocks    │
    │ Extract CRC32   │           │ crc32_hw        │
    └────────┬────────┘           └────────┬────────┘
             │                             │
             ▼                             ▼
    ┌─────────────────┐           ┌─────────────────┐
    │ Vector<uint32>  │           │ Vector<uint32>  │
    │ (S3 CRCs)       │           │ (Local CRCs)    │
    └────────┬────────┘           └────────┬────────┘
             │                             │
             └──────────────┬──────────────┘
                            ▼
                   ┌─────────────────┐
                   │    Compare      │
                   │  Report Diffs   │
                   └─────────────────┘
```

---

## 5. Performance Optimizations

### 5.1 Compilation

| Flag | Purpose |
|------|---------|
| `-O3` | Maximum optimization |
| `-flto` | Link-time optimization |
| `-fstack-protector-strong` | Security hardening |

### 5.2 Runtime

| Optimization | Benefit |
|--------------|---------|
| Parallel S3 + Local | Total time = max(S3, local), not sum |
| Thread pool sizing | Matches CPU cores, avoids oversubscription |
| `pread` block I/O | Survives file truncation and keeps buffers cache-sized |
| Connection pooling | 512 concurrent S3 connections |
| Single file descriptor | Avoids ulimit exhaustion |
| EC2 metadata disabled | Faster AWS SDK init |

### 5.3 Selective Processing

```cpp
std::vector<int64_t> chunk_ids = {0, 1, 2, ...};  // Specific chunks
// Empty vector = process all chunks
```

---

## 6. Error Handling

### 6.1 Strategy

- **Fail-fast on errors**: If any chunk fails, return empty vector to signal failure
- **Atomic error tracking**: Thread-safe error counter for parallel operations
- **Structured logging**: All errors logged via spdlog with context
- **Resource cleanup**: `AbortMultipartUpload()` always called
- **Exception safety**: Try-catch blocks around S3 operations

### 6.2 Error Points

| Component | Error | Handling |
|-----------|-------|----------|
| File open | `open()` fails | Return empty vector |
| Chunk read | short read or `pread()` error | Return empty vector |
| S3 CreateMultipart | API error | Log, return empty string |
| S3 UploadPartCopy | API error | Increment error counter, return empty vector if any errors |
| S3 GetObjectSize | API error | Log, return -1 |
| Chunk count mismatch | S3 != local | Log error, set `has_diff=true` |

---

## 7. Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| aws-sdk-cpp | 1.11.x | S3 API, multipart operations |
| Boost.ASIO | 1.88.x | Thread pool, async scheduling |
| OpenSSL | 3.x | TLS for AWS SDK |
| spdlog | 1.15.x | Structured logging |
| zlib | 1.3.x | Software CRC32 fallback |

**Build System:**
- CMake 3.15+
- vcpkg (manifest mode)
- Ninja (optional, via presets)

---

## 8. Build Configuration

### 8.1 CMakeLists.txt Structure

```cmake
# No platform-specific ISA flags: zlib-ng brings its own SIMD CRC32 paths and
# chooses between them at run time, so the build sets no architecture baseline.

# LTO (if supported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Security hardening (non-Debug)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fstack-protector-strong)
endif()
```

### 8.2 Presets

| Preset | Platform | Generator |
|--------|----------|-----------|
| `linux-release` | Linux | Ninja |
| `macos-release` | macOS | Ninja |

### 8.3 Build Commands

```bash
# Bootstrap (one-time)
./bootstrap.sh

# Configure + Build
cmake --preset macos-release  # or linux-release
cmake --build build

# Install
cmake --install build --prefix /usr/local
```

---

## 9. Platform Support

| Platform | Architecture | HW Acceleration | Status |
|----------|--------------|-----------------|--------|
| Linux | x86_64 | PCLMULQDQ / VPCLMULQDQ | Full support |
| Linux | ARM64 | ARMv8 CRC32 | Full support |
| macOS | x86_64 | PCLMULQDQ / VPCLMULQDQ | Full support |
| macOS | ARM64 (Apple Silicon) | ARMv8 CRC32 | Full support |

All four are chosen by zlib-ng at run time, not by the build.

---

## 10. Future Improvements

### 10.1 Potential Enhancements

1. **Configuration File**: YAML/JSON config for bucket, region, chunk size
2. **Resumable Operations**: Save progress for interrupted large file checks
3. **Multiple File Support**: Batch verification of directory trees
4. **Output Formats**: JSON/CSV reports for automation
5. **S3 Path as CLI Argument**: Currently bucket/key are hardcoded

### 10.2 Testing

Currently no test suite. Recommended additions:
- Unit tests for CRC32 functions
- Integration tests with LocalStack (S3 mock)
- Benchmark suite for performance regression

### 10.3 CI/CD

No pipeline currently. Recommended:
- GitHub Actions for Linux/macOS builds
- Automated testing on push
- Release artifact generation

---

## 11. Security Considerations

| Measure | Implementation |
|---------|----------------|
| Stack protection | `-fstack-protector-strong` |
| RELRO | `-Wl,-z,relro,-z,now` (Linux) |
| No EC2 metadata | `AWS_EC2_METADATA_DISABLED=true` |
| TLS | Via OpenSSL (AWS SDK) |
| No secrets in code | AWS credentials via environment/IAM |

---

## 12. File Structure

```
mitosync/
├── CMakeLists.txt          # Build configuration
├── CMakePresets.json       # Build presets (linux/macos)
├── vcpkg.json              # Dependency manifest
├── bootstrap.sh            # vcpkg setup script
├── include/
│   ├── constants.h         # CHUNK_SIZE definition
│   ├── crc32_hw.h          # Hardware CRC32 interface + detection
│   ├── crc32_chunks.h      # Chunk computation interface
│   └── s3_utils.h          # S3MultipartCopy class + GetS3ObjectSize
└── src/
    ├── main.cpp            # Entry point, CLI, orchestration
    ├── crc32_hw.cpp        # HW-accelerated CRC32 + platform detection
    ├── crc32_chunks.cpp    # Parallel chunk processing
    └── s3_utils.cpp        # S3 operations (GetObjectSize, multipart)
```

---

## 13. References

- [AWS S3 Multipart Upload](https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html)
- [Intel PCLMULQDQ Instruction](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#text=pclmul)
- [Boost.ASIO Thread Pool](https://www.boost.org/doc/libs/release/doc/html/boost_asio/reference/thread_pool.html)
- [pread(2) Manual](https://man7.org/linux/man-pages/man2/pread.2.html)
