#pragma once

// Points CloudMetrics persistence at a scratch directory for the lifetime of
// the object, and removes it afterwards.
//
// Why every fixture that touches CloudMetrics needs one: it is a singleton, and
// its data directory defaults to the real one - ~/.local/share/mitosync on
// Linux, ~/Library/Application Support/MitoSync on macOS. Anything a test
// persists lands in the metrics of whoever ran the suite. That is how issue #41
// came about: clear() used to save(), so merely resetting the singleton in
// SetUp() emptied the user's real file.
//
// clear() no longer writes, which fixes the specific path. This exists so the
// next save() added to a test cannot reintroduce it - the redirect makes the
// real file unreachable rather than merely unused.
//
// The directory name includes the test name and the process id, so two suites
// running at once (ctest -j, issue #42) cannot collide.

#include "cloud_metrics.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define MITO_TEST_GETPID _getpid
#else
#include <unistd.h>
#define MITO_TEST_GETPID getpid
#endif

class ScopedCloudMetricsDir {
public:
    ScopedCloudMetricsDir() : previous_(CloudMetrics::testDataDirectory()) {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name()
                                : std::string("unnamed");
        // Type-parameterised and value-parameterised tests put "/" in the name.
        for (char& c : name) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }

        path_ = std::filesystem::temp_directory_path() /
                ("mito_metrics_" + std::to_string(static_cast<long>(MITO_TEST_GETPID())) + "_" + name);

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        CloudMetrics::setTestDataDirectory(path_.string());
    }

    ~ScopedCloudMetricsDir() {
        // Restore, rather than reset to "". Resetting would aim the singleton
        // at the user's real directory the moment an inner scope ended, even
        // though an outer scope still expected a scratch one.
        CloudMetrics::setTestDataDirectory(previous_);
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedCloudMetricsDir(const ScopedCloudMetricsDir&) = delete;
    ScopedCloudMetricsDir& operator=(const ScopedCloudMetricsDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path metrics_file() const { return path_ / "cloud_metrics.json"; }

private:
    std::string previous_;
    std::filesystem::path path_;
};
