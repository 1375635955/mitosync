#include <gtest/gtest.h>

#include <algorithm>  // std::count, used to balance braces in the JSON assertions
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "report_writer.h"
#include "temp_test_path.h"

namespace {

// Writes reports into a unique temp directory and reads them back.
class ReportFile : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = mito_test_temp_path("mito_report_test");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);   // a killed run under a since-reused pid
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string path(const std::string& name) const { return (dir_ / name).string(); }
    static std::string slurp(const std::string& p) {
        std::ifstream in(p);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    std::filesystem::path dir_;
};

// A small result: one match (always omitted from reports), one difference,
// one only-in-A, one only-in-B, one error.
DirectoryComparisonResult make_result() {
    DirectoryComparisonResult r;
    r.total_files = 5;
    r.matching_files = 1;
    r.mismatched_files = 1;
    r.only_in_a = 1;
    r.only_in_b = 1;
    r.errors = 1;
    r.total_elapsed = 12.5;

    FileCompareResult match;
    match.relative_path = "same.txt";
    match.status = FileCompareStatus::Match;
    match.size_a = 10; match.size_b = 10;

    FileCompareResult differ;
    differ.relative_path = "differs.bin";
    differ.status = FileCompareStatus::Mismatch;
    differ.size_a = 100; differ.size_b = 200;

    FileCompareResult only_a;
    only_a.relative_path = "only_a.txt";
    only_a.status = FileCompareStatus::Mismatch;
    only_a.size_a = 42; only_a.size_b = -1;

    FileCompareResult only_b;
    only_b.relative_path = "only_b.txt";
    only_b.status = FileCompareStatus::Mismatch;
    only_b.size_a = -1; only_b.size_b = 7;

    FileCompareResult err;
    err.relative_path = "boom.txt";
    err.status = FileCompareStatus::Error;
    err.size_a = -1; err.size_b = -1;
    err.error_message = "permission denied";

    r.files = {match, differ, only_a, only_b, err};
    return r;
}

}  // namespace

// ============================================================================
// format_bytes
// ============================================================================

TEST(FormatBytesTest, BelowOneKibHasNoDecimal) {
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(1), "1 B");
    EXPECT_EQ(format_bytes(1023), "1023 B");
}

TEST(FormatBytesTest, ScalesThroughEveryUnit) {
    EXPECT_EQ(format_bytes(1024), "1.00 KiB");
    EXPECT_EQ(format_bytes(1536), "1.50 KiB");
    EXPECT_EQ(format_bytes(1024ULL * 1024), "1.00 MiB");
    EXPECT_EQ(format_bytes(1024ULL * 1024 * 1024), "1.00 GiB");
    EXPECT_EQ(format_bytes(1024ULL * 1024 * 1024 * 1024), "1.00 TiB");
}

TEST(FormatBytesTest, StopsScalingAtTiB) {
    // 1024 TiB stays in TiB rather than moving to PiB.
    std::string s = format_bytes(1024ULL * 1024 * 1024 * 1024 * 1024);
    EXPECT_NE(s.find("TiB"), std::string::npos);
    EXPECT_EQ(s, "1024.00 TiB");
}

// ============================================================================
// format_duration
// ============================================================================

TEST(FormatDurationSecondsTest, CollapsesOnlyExactMultiples) {
    using std::chrono::seconds;
    EXPECT_EQ(format_duration(seconds(45)), "45s");
    EXPECT_EQ(format_duration(seconds(60)), "1m");
    EXPECT_EQ(format_duration(seconds(1800)), "30m");
    EXPECT_EQ(format_duration(seconds(3600)), "1h");
    EXPECT_EQ(format_duration(seconds(86400)), "1d");
    EXPECT_EQ(format_duration(seconds(604800)), "7d");
}

TEST(FormatDurationSecondsTest, NonMultiplesStayInSeconds) {
    using std::chrono::seconds;
    EXPECT_EQ(format_duration(seconds(90)), "90s");     // not a whole minute
    EXPECT_EQ(format_duration(seconds(3661)), "3661s"); // not a whole hour
    EXPECT_EQ(format_duration(seconds(0)), "0s");
}

TEST(FormatDurationDoubleTest, UnderAMinuteIsSecondsWithTwoDecimals) {
    EXPECT_EQ(format_duration(0.0), "0.00s");
    EXPECT_EQ(format_duration(9.25), "9.25s");
    EXPECT_EQ(format_duration(59.999), "60.00s");  // rounds in display, stays sub-minute
}

TEST(FormatDurationDoubleTest, AtOrAboveAMinuteSplitsIntoMinutesAndSeconds) {
    EXPECT_EQ(format_duration(60.0), "1m 0.00s");
    EXPECT_EQ(format_duration(75.5), "1m 15.50s");
    EXPECT_EQ(format_duration(3600.0), "60m 0.00s");
}

// ============================================================================
// source_to_string
// ============================================================================

TEST(SourceToStringTest, LocalReturnsPathVerbatim) {
    FileSource s;
    s.type = SourceType::Local;
    s.path = "/data/files";
    EXPECT_EQ(source_to_string(s), "/data/files");
}

TEST(SourceToStringTest, S3IsRenderedAsUrl) {
    FileSource s;
    s.type = SourceType::S3;
    s.bucket = "my-bucket";
    s.path = "prefix/key";
    EXPECT_EQ(source_to_string(s), "s3://my-bucket/prefix/key");
}

// ============================================================================
// status_to_string
// ============================================================================

TEST(StatusToStringTest, MatchAndError) {
    EXPECT_EQ(status_to_string(FileCompareStatus::Match, 1, 1), "MATCH");
    EXPECT_EQ(status_to_string(FileCompareStatus::Error, -1, -1), "ERROR");
}

TEST(StatusToStringTest, BothSidesPresentIsDifferent) {
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, 100, 200), "DIFFERENT");
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, 0, 0), "DIFFERENT");
}

TEST(StatusToStringTest, OneSidedMismatchWithoutNamesUsesAOrB) {
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, 42, -1), "ONLY_A");
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, -1, 42), "ONLY_B");
}

TEST(StatusToStringTest, OneSidedMismatchWithNamesUsesThem) {
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, 42, -1, "dirA", "dirB"), "ONLY dirA");
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, -1, 42, "dirA", "dirB"), "ONLY dirB");
}

TEST(StatusToStringTest, ZeroLengthFileCountsAsPresent) {
    // size 0 is a real file; only a negative size means absent.
    EXPECT_EQ(status_to_string(FileCompareStatus::Mismatch, 0, -1), "ONLY_A");
}

// ============================================================================
// write_json_results
// ============================================================================

TEST_F(ReportFile, JsonContainsSummaryAndOmitsMatches) {
    auto r = make_result();
    std::string p = path("out.json");
    ASSERT_TRUE(write_json_results(p, r, "dirA", "dirB"));
    std::string j = slurp(p);

    EXPECT_NE(j.find("\"source_a\": \"dirA\""), std::string::npos);
    EXPECT_NE(j.find("\"source_b\": \"dirB\""), std::string::npos);
    EXPECT_NE(j.find("\"total_files\": 5"), std::string::npos);
    EXPECT_NE(j.find("\"matching\": 1"), std::string::npos);
    EXPECT_NE(j.find("\"errors\": 1"), std::string::npos);
    EXPECT_NE(j.find("\"elapsed_seconds\": 12.50"), std::string::npos);

    // Differing entries appear; the matching one does not.
    EXPECT_NE(j.find("differs.bin"), std::string::npos);
    EXPECT_NE(j.find("only_a.txt"), std::string::npos);
    EXPECT_EQ(j.find("same.txt"), std::string::npos);

    // Error message is carried through.
    EXPECT_NE(j.find("permission denied"), std::string::npos);
}

TEST_F(ReportFile, JsonSummaryKeysUseTheSourceNames) {
    auto r = make_result();
    std::string p = path("named.json");
    ASSERT_TRUE(write_json_results(p, r, "left", "right"));
    std::string j = slurp(p);
    EXPECT_NE(j.find("\"only_in_left\": 1"), std::string::npos);
    EXPECT_NE(j.find("\"only_in_right\": 1"), std::string::npos);
}

TEST_F(ReportFile, JsonIsWellFormedBracketing) {
    auto r = make_result();
    std::string p = path("shape.json");
    ASSERT_TRUE(write_json_results(p, r, "a", "b"));
    std::string j = slurp(p);
    EXPECT_EQ(std::count(j.begin(), j.end(), '{'), std::count(j.begin(), j.end(), '}'));
    EXPECT_EQ(std::count(j.begin(), j.end(), '['), std::count(j.begin(), j.end(), ']'));
    EXPECT_EQ(j.front(), '{');
    // No trailing comma before the closing array bracket.
    EXPECT_EQ(j.find(",\n  ]"), std::string::npos);
}

TEST_F(ReportFile, JsonWithNoDifferencesStillHasEmptyFileArray) {
    DirectoryComparisonResult r;
    r.total_files = 1;
    r.matching_files = 1;
    r.total_elapsed = 0.5;
    FileCompareResult m;
    m.relative_path = "x";
    m.status = FileCompareStatus::Match;
    r.files = {m};

    std::string p = path("empty.json");
    ASSERT_TRUE(write_json_results(p, r, "a", "b"));
    std::string j = slurp(p);
    EXPECT_NE(j.find("\"files\": ["), std::string::npos);
    EXPECT_EQ(std::count(j.begin(), j.end(), '['), std::count(j.begin(), j.end(), ']'));
}

// ============================================================================
// write_csv_results
// ============================================================================

TEST_F(ReportFile, CsvHasHeaderAndOneRowPerDifference) {
    auto r = make_result();
    std::string p = path("out.csv");
    ASSERT_TRUE(write_csv_results(p, r, "dirA", "dirB"));

    std::ifstream in(p);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);

    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines[0], "status,path,size_a,size_b,error");
    // 4 non-matching entries out of 5.
    EXPECT_EQ(lines.size(), 5u);
    for (const auto& l : lines) EXPECT_EQ(l.find("same.txt"), std::string::npos);
}

TEST_F(ReportFile, CsvQuotesPathAndError) {
    auto r = make_result();
    std::string p = path("q.csv");
    ASSERT_TRUE(write_csv_results(p, r, "a", "b"));
    std::string c = slurp(p);
    EXPECT_NE(c.find("\"differs.bin\""), std::string::npos);
    EXPECT_NE(c.find("\"permission denied\""), std::string::npos);
}

// ============================================================================
// write_txt_results
// ============================================================================

TEST_F(ReportFile, TxtHasHeadingSummaryAndDifferences) {
    auto r = make_result();
    std::string p = path("out.txt");
    ASSERT_TRUE(write_txt_results(p, r, "dirA", "dirB"));
    std::string t = slurp(p);

    EXPECT_NE(t.find("Directory Comparison Results"), std::string::npos);
    EXPECT_NE(t.find("Source A: dirA"), std::string::npos);
    EXPECT_NE(t.find("Source B: dirB"), std::string::npos);
    EXPECT_NE(t.find("Total files:      5"), std::string::npos);
    EXPECT_NE(t.find("Differences:"), std::string::npos);
    EXPECT_NE(t.find("[DIFFERENT] differs.bin"), std::string::npos);
    EXPECT_NE(t.find("permission denied"), std::string::npos);
    EXPECT_NE(t.find("12.50s"), std::string::npos);
}

TEST_F(ReportFile, TxtOmitsDifferencesSectionWhenEverythingMatches) {
    DirectoryComparisonResult r;
    r.total_files = 2;
    r.matching_files = 2;
    r.total_elapsed = 1.0;
    std::string p = path("clean.txt");
    ASSERT_TRUE(write_txt_results(p, r, "a", "b"));
    std::string t = slurp(p);
    EXPECT_EQ(t.find("Differences:"), std::string::npos);
}

// ============================================================================
// write_results_to_file dispatch
// ============================================================================

TEST_F(ReportFile, DispatchesOnExtension) {
    auto r = make_result();

    std::string j = path("r.json");
    ASSERT_TRUE(write_results_to_file(j, r, "a", "b"));
    EXPECT_EQ(slurp(j).front(), '{');

    std::string c = path("r.csv");
    ASSERT_TRUE(write_results_to_file(c, r, "a", "b"));
    EXPECT_EQ(slurp(c).substr(0, 6), "status");

    std::string t = path("r.txt");
    ASSERT_TRUE(write_results_to_file(t, r, "a", "b"));
    EXPECT_NE(slurp(t).find("Directory Comparison Results"), std::string::npos);
}

TEST_F(ReportFile, ExtensionMatchIsCaseInsensitive) {
    auto r = make_result();
    std::string p = path("upper.JSON");
    ASSERT_TRUE(write_results_to_file(p, r, "a", "b"));
    EXPECT_EQ(slurp(p).front(), '{');
}

TEST_F(ReportFile, UnknownAndMissingExtensionFallBackToText) {
    auto r = make_result();
    std::string odd = path("r.xyz");
    ASSERT_TRUE(write_results_to_file(odd, r, "a", "b"));
    EXPECT_NE(slurp(odd).find("Directory Comparison Results"), std::string::npos);

    std::string none = path("noext");
    ASSERT_TRUE(write_results_to_file(none, r, "a", "b"));
    EXPECT_NE(slurp(none).find("Directory Comparison Results"), std::string::npos);
}

TEST_F(ReportFile, UnwritablePathReturnsFalse) {
    auto r = make_result();
    std::string bad = (dir_ / "no_such_subdir" / "x.json").string();
    EXPECT_FALSE(write_json_results(bad, r, "a", "b"));
    EXPECT_FALSE(write_csv_results(bad, r, "a", "b"));
    EXPECT_FALSE(write_txt_results(bad, r, "a", "b"));
    EXPECT_FALSE(write_results_to_file(bad, r, "a", "b"));
}

// ============================================================================
// print_directory_result
// ============================================================================

TEST(PrintDirectoryResultTest, AllMatchingUsesTickAndFileCount) {
    DirectoryComparisonResult r;
    r.total_files = 3;
    r.matching_files = 3;
    r.total_elapsed = 2.0;

    std::ostringstream out;
    print_directory_result(r, "a", "b", out);
    std::string s = out.str();
    EXPECT_NE(s.find("3 files match"), std::string::npos);
    EXPECT_NE(s.find("2.00s"), std::string::npos);
    EXPECT_EQ(s.find("differ"), std::string::npos);
}

TEST(PrintDirectoryResultTest, MismatchListsEveryNonZeroCategory) {
    auto r = make_result();
    std::ostringstream out;
    print_directory_result(r, "dirA", "dirB", out);
    std::string s = out.str();
    EXPECT_NE(s.find("1/5 match"), std::string::npos);
    EXPECT_NE(s.find("1 differ"), std::string::npos);
    EXPECT_NE(s.find("1 only in dirA"), std::string::npos);
    EXPECT_NE(s.find("1 only in dirB"), std::string::npos);
    EXPECT_NE(s.find("1 errors"), std::string::npos);
}

TEST(PrintDirectoryResultTest, ZeroCategoriesAreOmitted) {
    DirectoryComparisonResult r;
    r.total_files = 2;
    r.matching_files = 1;
    r.mismatched_files = 1;
    r.total_elapsed = 1.0;

    std::ostringstream out;
    print_directory_result(r, "a", "b", out);
    std::string s = out.str();
    EXPECT_NE(s.find("1 differ"), std::string::npos);
    EXPECT_EQ(s.find("only in"), std::string::npos);
    EXPECT_EQ(s.find("errors"), std::string::npos);
}
