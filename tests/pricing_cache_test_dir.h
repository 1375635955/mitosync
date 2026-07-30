#pragma once

// Points AwsPricingCache at a scratch directory for the lifetime of the object.
//
// The sibling of ScopedCloudMetricsDir, for the same reason: AwsPricingCache is
// a singleton that persists to ~/.mitosync/pricing_cache.json by default, and
// getPricing() writes through to disk on a cache miss. A fixture that exercises
// it without redirecting first leaves a real file in the home directory of
// whoever ran the suite.
//
// Unlike the metrics case (issue #41), nothing about the production behaviour
// is wrong here - a pricing cache is supposed to persist. Only the tests need
// to be pointed somewhere harmless.

#include "aws_pricing.h"

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

class ScopedPricingCacheDir {
public:
    ScopedPricingCacheDir() : previous_(AwsPricingCache::instance().getCacheDir()) {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name()
                                : std::string("unnamed");
        for (char& c : name) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }

        path_ = std::filesystem::temp_directory_path() /
                ("mito_pricing_" + std::to_string(static_cast<long>(MITO_TEST_GETPID())) + "_" + name);

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        AwsPricingCache::instance().setCacheDir(path_.string());
    }

    ~ScopedPricingCacheDir() {
        // Restore rather than reset, for the reason in ScopedCloudMetricsDir.
        AwsPricingCache::instance().setCacheDir(previous_);
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedPricingCacheDir(const ScopedPricingCacheDir&) = delete;
    ScopedPricingCacheDir& operator=(const ScopedPricingCacheDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::string previous_;
    std::filesystem::path path_;
};
