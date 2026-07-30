#pragma once

// A scratch path under the system temp directory that belongs to one running
// test and nobody else.
//
// Fixtures used to name these after the fixture alone - mito_sync_test,
// test_byte_provider.bin - which is fine until a second copy of the suite runs
// at the same time. Then two processes share the directory, and each one's
// SetUp removes the files the other is midway through using: tests fail with
// a file that vanished under them, in whichever process lost the race, and the
// failures move around from run to run (issue #42).
//
// The process id keeps concurrent runs apart. The test name keeps the pieces
// of one run apart, and makes a stray directory left by a killed run easy to
// place. Neither survives a pid the OS has recycled, so callers that create a
// directory should still clear it before use rather than trusting it to be
// absent.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#ifndef MITO_TEST_GETPID
#ifdef _WIN32
#include <process.h>
#define MITO_TEST_GETPID _getpid
#else
#include <unistd.h>
#define MITO_TEST_GETPID getpid
#endif
#endif

// `name` identifies the fixture; the rest of the path is what makes it unique.
// Safe to call outside a running test, where the test-name part becomes
// "unnamed".
inline std::filesystem::path mito_test_temp_path(const std::string& name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string test = info ? std::string(info->test_suite_name()) + "_" + info->name()
                            : std::string("unnamed");
    // Parameterised tests put separators in the name.
    for (char& c : test) {
        if (c == '/' || c == '\\') {
            c = '_';
        }
    }

    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<long>(MITO_TEST_GETPID())) + "_" + test);
}
