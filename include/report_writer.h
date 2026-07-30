#pragma once

// Result formatting and report output for the mito CLI.
//
// These are pure functions of their arguments (plus, for the writers, one
// output file or stream). They touch no global state and no network, so they
// live apart from main.cpp and can be tested directly.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "comparison_task.h"          // FileSource, SourceType
#include "directory_comparison.h"     // DirectoryComparisonResult, FileCompareStatus

// Human-readable byte count, e.g. 1536 -> "1.50 KiB". Exact multiples below
// 1 KiB are rendered without a decimal, e.g. 512 -> "512 B".
std::string format_bytes(uint64_t bytes);

// Whole-unit duration, e.g. 7 days -> "7d". Only exact multiples collapse to a
// larger unit; 90 minutes stays "5400s".
std::string format_duration(std::chrono::seconds dur);

// Elapsed time with two decimals, e.g. 75.5 -> "1m 15.50s", 9.25 -> "9.25s".
std::string format_duration(double seconds);

// Render a source for display: a local path as-is, an S3 source as s3://bucket/path.
std::string source_to_string(const FileSource& source);

// Per-file comparison verdict. A Mismatch where only one side has a size is
// reported as "ONLY <source>" (or ONLY_A / ONLY_B when no names are supplied).
std::string status_to_string(FileCompareStatus status, int64_t size_a, int64_t size_b,
                             const std::string& src_a = "", const std::string& src_b = "");

// Report writers. Each returns false if the file cannot be opened.
bool write_json_results(const std::string& filename, const DirectoryComparisonResult& result,
                        const std::string& source_a, const std::string& source_b);
bool write_csv_results(const std::string& filename, const DirectoryComparisonResult& result,
                       const std::string& source_a, const std::string& source_b);
bool write_txt_results(const std::string& filename, const DirectoryComparisonResult& result,
                       const std::string& source_a, const std::string& source_b);

// Dispatch on the filename extension: .json, .csv, anything else -> text.
bool write_results_to_file(const std::string& filename, const DirectoryComparisonResult& result,
                           const std::string& source_a, const std::string& source_b);

// One-line console summary. The stream is a parameter so it can be captured.
void print_directory_result(const DirectoryComparisonResult& result,
                            const std::string& src_a, const std::string& src_b,
                            std::ostream& out = std::cout);
