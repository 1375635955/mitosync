# CRC32 Test Suite

This document describes the unit tests for the CRC32 implementation used by MitoSync.

## Running Tests

```bash
# Run all tests
./build/mito_tests

# Run via CTest
cd build && ctest

# Run specific test
./build/mito_tests --gtest_filter=CRC32Test.CanonicalTestVector

# List all tests
./build/mito_tests --gtest_list_tests

# Measure CRC32 throughput against zlib (not a test, not run by ctest)
./build/mito_bench_crc32
```

---

## Test Summary

### Known Test Vectors

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `EmptyInput` | CRC32 of empty data returns 0 | Edge case handling |
| `CanonicalTestVector` | "123456789" → 0xCBF43926 | Industry standard test vector |
| `SingleCharacter` | "a" → 0xE8B7BE43 | Minimum input case |
| `ThreeCharacters` | "abc" → 0x352441C2 | Short string handling |
| `AdditionalKnownVectors` | 7 standard test vectors | Validates IEEE polynomial |

### Size & Boundary Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `AllZerosSmall` | 16 bytes of 0x00 | Simple pattern, small buffer |
| `AllZeros64Bytes` | 64 bytes of 0x00 | Where a folding CRC switches strategy |
| `AllZerosLarge` | 1KB of 0x00 | Large buffer handling |
| `AllOnesSmall` | 16 bytes of 0xFF | Inverse pattern, small |
| `AllOnes64Bytes` | 64 bytes of 0xFF | Same boundary, saturated input |
| `VariousSizes` | Sizes 1-128 bytes | Catches off-by-one errors |
| `PCLMULBoundary` | Sizes 60-70 bytes | Straddles zlib-ng's vector/scalar boundary |
| `PCLMULBlockSizes` | Multiples of 16 (64-512) | Whole-block folding, no tail |
| `PowerOfTwoSizes` | 1B to 1MB (powers of 2) | Common buffer sizes |
| `PrimeNumberSizes` | Prime sizes up to 8191 | Alignment edge cases |
| `ARM64ProcessingSizes` | Sizes 1-56 bytes | 8/4/2/1-byte tail handling |

### Alignment Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `UnalignedBuffer` | 128 bytes at offset 1 | Non-aligned memory access |
| `AllAlignments` | All 16 alignment offsets | Complete alignment coverage |
| `UnalignedPCLMULBoundary` | Unaligned + boundary sizes | Combined edge cases |
| `Aligned64BitData` | 64-byte aligned buffer | Alignment a vector path may assume |

### Pattern Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `SequentialBytes` | Bytes 0-255 | Non-repeating pattern |
| `AlternatingBits` | 0xAA and 0x55 patterns | Bit pattern sensitivity |
| `RepeatingPatterns` | 2-byte and 4-byte repeats | Pattern detection |
| `SingleBitSet` | One bit set per position | 1024 edge cases |
| `AllSameByteValues` | All 256 byte values | Complete byte coverage |
| `IncreasingDecreasingPattern` | Symmetric pattern | Pattern symmetry |
| `HighEntropyData` | PRNG-generated data | Compressed/encrypted data |

### Large Buffer Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `LargeBuffer` | 1MB sequential data | Production-scale buffer |
| `ChunkSizeBoundary` | 8KB chunk | Chunk boundary handling |
| `ChunkSize8MB` | 8MB buffer | **Actual mito chunk size** |

### Random Data Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `RandomDataMultipleSeeds` | 100 seeds × 4KB | Statistical coverage |
| `RandomSizesRandomData` | 50 random sizes (1-10KB) | Randomized testing |

### Text/ASCII Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `ASCIIStrings` | Common strings, punctuation | Real-world text data |
| `JSONLikeData` | JSON structure | Config file patterns |

### Mathematical Property Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `DetectsSingleBitFlip` | Flip each of 2048 bits | **CRC32 must detect all single-bit errors** |
| `DetectsBurstErrors` | 2-32 bit burst errors | Burst error detection guarantee |
| `NotCRC32C` | Verify NOT 0xE3069283 | **Catches wrong polynomial (CRC32-C)** |
| `PolynomialVerification` | 3 vectors where CRC32 ≠ CRC32-C | Confirms IEEE polynomial |

### S3 Compatibility Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `S3Base64Format` | Big-endian byte extraction | **Matches S3's encoding format** |
| `S3MultipartChunks` | 3 × 8MB chunks | **Simulates S3 multipart upload** |
| `S3LastChunkPartial` | 2 full + 1 partial chunk | Real-world file sizes |

### Incremental CRC Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `ZlibIncrementalReference` | 4 × 256 byte increments | Validates zlib compatibility |
| `CRC32CombineProperty` | `crc32_combine()` verification | Parallel computation support |

### Endianness Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `EndiannessPatterns` | Forward vs reversed bytes | Byte order correctness |

### Collision & Avalanche Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `CollisionResistance` | 10,000 unique CRCs | No implementation bugs |
| `SimilarDataDifferentCRCs` | Single byte change → 8+ bits differ | Avalanche effect |

### Robustness Tests

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `Consistency` | 100 identical calls | Deterministic output |
| `NoStateLeakage` | Interleaved computations | No global state corruption |
| `ThreadSafety` | 8 threads × 1000 iterations | **Concurrent computation safety** |

### Hardware Detection

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `DetectionWorks` | `has_hw_crc32()` doesn't crash | Runtime detection works |
| `ReportedNameMatchesWhetherAPathExists` | Name and detection agree | Reporting cannot claim a path the CPU lacks |

### Length Handling

| Test | Description | Why It Matters |
|------|-------------|----------------|
| `DoesNotTruncateLengthAtFourGiB` | 4 GiB + 100 bytes vs zlib's `crc32_z` | **A truncated length would checksum the wrong bytes and send them to S3** |

---

## Critical Tests

These tests are most important for S3 compatibility:

1. **`CanonicalTestVector`** - If this fails, everything is broken
2. **`NotCRC32C`** - Detects wrong polynomial
3. **`S3MultipartChunks`** - Real-world S3 scenario
4. **`ChunkSize8MB`** - Actual chunk size used by mito
5. **`ThreadSafety`** - mito uses parallel CRC computation

## Test Count

- **Total: 56 tests**
- Execution time: ~2s, nearly all of it in `DoesNotTruncateLengthAtFourGiB`
- Memory: ~50MB peak (during 24MB S3 simulation). The 4 GiB mapping is read-only
  anonymous memory, so it resolves to the shared zero page and costs almost nothing.

---

## Tests and persistent state

Two singletons in this codebase write to the user's home directory: `CloudMetrics`
(`cloud_metrics.json` in the app data directory) and `AwsPricingCache`
(`~/.mitosync/pricing_cache.json`). A fixture that touches either without redirecting it
first writes into the real files of whoever runs the suite — which is what issue #41 was.

Any fixture that exercises them must hold the matching guard:

```cpp
#include "cloud_metrics_test_dir.h"
#include "pricing_cache_test_dir.h"

class MyTest : public ::testing::Test {
protected:
    ScopedCloudMetricsDir metrics_dir_;   // redirects CloudMetrics
    ScopedPricingCacheDir pricing_dir_;   // redirects AwsPricingCache
};
```

Each points its singleton at a scratch directory named for the test and the process, removes
it afterwards, and restores whatever redirect was in force before. Being fixture *members*
rather than `SetUp()` code, they survive a fatal assertion in the test body.

Two tests guard the guards: `CloudMetricsRealDirectorySafety.ResettingWritesNothingToTheApp
DataDirectory` deliberately runs with no redirect, pointing `HOME` and `XDG_DATA_HOME` at a
scratch directory instead, and fails if anything is written beneath it;
`AwsPricingCacheTest.WritesItsCacheInsideTheTestDirectory` asserts the pricing cache lands in
the scratch directory, since a cache is meant to persist and cannot assert nothing is written.

If you add a singleton that persists, add a guard beside these and a line here.
