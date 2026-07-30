#include "../include/crc32_hw.h"
#include <zlib-ng.h>

// Every CRC32 in mito is computed by zlib-ng (issue #50).
//
// The history behind that choice: this file used to carry a hand-written x86
// PCLMUL implementation that produced wrong values for any 16-byte-aligned
// buffer of 64 bytes or more, and was dormant in the shipped build because
// -msse4.2 alone does not define __PCLMUL__ (issue #18). Writing a correct
// folding + Barrett-reduction CRC is worth real throughput, but it is delicate
// carry-less multiply maths that has to be re-verified per architecture. A
// maintained library does it once, with runtime CPU detection and its own CI.
//
// Why the prefixed zng_ API rather than zlib-ng's zlib-compat mode: aws-sdk-cpp
// already depends on zlib, and ZLIB_COMPAT is a vcpkg triplet-wide switch that
// would make zlib-ng stand in for zlib everywhere, for every dependency. The
// prefixed API lets the two libraries coexist in one binary and keeps each call
// site explicit about which one it is asking for.
//
// zlib-ng chooses its CRC32 implementation at runtime from the CPU it finds, so
// the build no longer needs -msse4.2 or -march=armv8-a+crc to get an
// accelerated CRC, and a binary built on one machine stays correct - and fast -
// on another.

uint32_t crc32_hw(const uint8_t* data, size_t length) {
    // zng_crc32_z takes a size_t length. The old zlib call cast it to uInt, so
    // any buffer of 4 GiB or more was checksummed over length mod 2^32 bytes -
    // which for PutObjectFromFile meant S3 validated an upload against a
    // checksum of the wrong bytes (issues #48, #21).
    return zng_crc32_z(0, data, length);
}

uint32_t crc32_hw_update(uint32_t crc, const uint8_t* data, size_t length) {
    // Same call, with the running value as the seed rather than 0. zlib-ng's
    // CRC is defined so that chaining chunks this way gives the whole-buffer
    // answer.
    return zng_crc32_z(crc, data, length);
}

// has_hw_crc32() and hw_crc32_name() are diagnostics: they describe which
// implementation zlib-ng will run on this CPU, and nothing consults them to
// decide how a CRC is computed. They detect at runtime rather than reporting
// what the compiler was told, because the previous version claimed "x86 PCLMUL"
// on builds whose flags left the PCLMUL path compiled out (issue #22).
//
// The conditions below mirror the dispatch in zlib-ng 2.2.x functable.c: x86
// takes crc32_pclmulqdq on PCLMULQDQ, and crc32_vpclmulqdq when VPCLMULQDQ
// joins AVX512F/DQ/BW/VL; ARM takes crc32_acle when the CPU reports CRC32.
// Anything else runs the portable braid implementation. If upstream changes
// those conditions this reporting can drift from reality - it is a debug line,
// not a correctness dependency.
//
// It also assumes the linked zlib-ng was built with those paths compiled in and
// with runtime dispatch enabled, which is true of the vcpkg port and of the
// FetchContent fallback in CMakeLists.txt. A zlib-ng configured with
// WITH_OPTIM=OFF would make this report claim an implementation that library
// cannot reach - the same shape of untruth as issue #22, one layer down.

#define MITO_ZLIB_NG_LABEL "zlib-ng " ZLIBNG_VERSION

// GCC and Clang only: this branch needs <cpuid.h> and __builtin_cpu_supports,
// which MSVC has neither of. An MSVC x86 build would fall through to the
// generic branch below and report that zlib-ng decides - true, if less
// specific. There is no Windows build to test that against today.
#if defined(__x86_64__) || defined(__i386__)

#include <cpuid.h>

namespace {

// __builtin_cpu_supports covers the OS-enablement check (XCR0) for the AVX512
// features, which a bare CPUID feature bit does not.
bool cpu_has_pclmulqdq() {
    return __builtin_cpu_supports("pclmul") != 0;
}

bool cpu_has_vpclmulqdq_crc32() {
    if (!cpu_has_pclmulqdq()) {
        return false;
    }
    // zlib-ng's has_avx512_common is these four plus BMI2 (x86_features.c), not
    // the four alone - no shipping part has AVX-512 without BMI2, but the
    // condition is copied here rather than approximated.
    if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512dq") ||
        !__builtin_cpu_supports("avx512bw") || !__builtin_cpu_supports("avx512vl") ||
        !__builtin_cpu_supports("bmi2")) {
        return false;
    }
    // VPCLMULQDQ is CPUID leaf 7, subleaf 0, ECX bit 10. Read directly rather
    // than through __builtin_cpu_supports("vpclmulqdq"), which older compilers
    // reject at compile time.
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (ecx & (1u << 10)) != 0;
}

}  // namespace

bool has_hw_crc32() {
    return cpu_has_pclmulqdq();
}

const char* hw_crc32_name() {
    if (cpu_has_vpclmulqdq_crc32()) {
        return MITO_ZLIB_NG_LABEL " (x86 VPCLMULQDQ)";
    }
    if (cpu_has_pclmulqdq()) {
        return MITO_ZLIB_NG_LABEL " (x86 PCLMULQDQ)";
    }
    return MITO_ZLIB_NG_LABEL " (portable C)";
}

#elif defined(__aarch64__) || defined(_M_ARM64)

// CRC32 is optional in ARMv8.0 and only mandatory from ARMv8.1, so it is
// detected rather than assumed - the previous version returned true
// unconditionally (issue #49). The hand-written ARM path that assumption went
// with is gone too, and with it its unguarded little-endian loads.
#if defined(__linux__)
#include <sys/auxv.h>
// glibc's aarch64 <sys/auxv.h> already defines HWCAP_CRC32, so <asm/hwcap.h> is
// only a backstop - and one that must not be required: musl and minimal
// sysroots do not ship the kernel UAPI headers at all, and an unconditional
// include there is a hard build failure rather than a fallback. zlib-ng's own
// probe makes the same distinction. The value is fixed aarch64 kernel ABI, so
// defining it when neither header does keeps this from silently reporting
// "portable C" on a CPU that has the instructions.
#if defined(__has_include)
#if __has_include(<asm/hwcap.h>)
#include <asm/hwcap.h>
#endif
#endif
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

bool has_hw_crc32() {
#if defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#elif defined(__APPLE__)
    int enabled = 0;
    size_t size = sizeof(enabled);
    if (sysctlbyname("hw.optional.armv8_crc32", &enabled, &size, nullptr, 0) != 0) {
        return false;
    }
    return enabled != 0;
#else
    // No detection mechanism known for this OS. zlib-ng runs its own check and
    // will still use the instructions if they are there; only this report is
    // conservative.
    return false;
#endif
}

const char* hw_crc32_name() {
    return has_hw_crc32() ? MITO_ZLIB_NG_LABEL " (ARMv8 CRC32)"
                          : MITO_ZLIB_NG_LABEL " (portable C)";
}

#else

// zlib-ng also accelerates CRC32 on Power8, IBM Z, LoongArch and RISC-V, but
// mito does not ship those targets and there is nothing here to check a claim
// against, so the report says only what is certain: zlib-ng decides.
bool has_hw_crc32() {
    return false;
}

const char* hw_crc32_name() {
    return MITO_ZLIB_NG_LABEL " (implementation chosen at runtime)";
}

#endif
