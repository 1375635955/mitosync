#include "report_writer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit_idx = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024.0 && unit_idx < 4) {
        value /= 1024.0;
        unit_idx++;
    }
    std::ostringstream oss;
    if (unit_idx == 0) {
        oss << bytes << " B";
    } else {
        oss << std::fixed << std::setprecision(2) << value << " " << units[unit_idx];
    }
    return oss.str();
}

// Format duration as human-readable string (e.g., "7d", "12h", "30m", "45s")
// Files that differ in content, as distinct from files present on only one side.
//
// DirectoryComparisonResult::mismatched_files counts every file that did not match, only-in-A
// and only-in-B included, because `mismatched_files == 0` is the "everything matched" test
// behind the exit status. Reports print the only-in counts beside it, so rendering the raw
// number counts those files twice: one match, one file only in A and one only in B came out as
// "1/3 match, 2 differ, 1 only in a/, 1 only in b/" - five files out of a total of three.
//
// The documentation already promises the derived figure: "summary.different counts genuine
// mismatches" (book/src/reference/exit-codes.md). This is what makes that true.
static size_t content_differences(const DirectoryComparisonResult& result) {
    const size_t only_one_side = result.only_in_a + result.only_in_b;
    return result.mismatched_files > only_one_side
               ? result.mismatched_files - only_one_side
               : 0;
}

std::string format_duration(std::chrono::seconds dur) {
    auto secs = dur.count();
    if (secs >= 86400 && secs % 86400 == 0) {
        return std::to_string(secs / 86400) + "d";
    } else if (secs >= 3600 && secs % 3600 == 0) {
        return std::to_string(secs / 3600) + "h";
    } else if (secs >= 60 && secs % 60 == 0) {
        return std::to_string(secs / 60) + "m";
    } else {
        return std::to_string(secs) + "s";
    }
}

std::string format_duration(double seconds) {
    if (seconds >= 60.0) {
        int mins = static_cast<int>(seconds) / 60;
        double secs = seconds - mins * 60;
        std::ostringstream oss;
        oss << mins << "m " << std::fixed << std::setprecision(2) << secs << "s";
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << seconds << "s";
        return oss.str();
    }
}

std::string source_to_string(const FileSource& source) {
    if (source.type == SourceType::Local) {
        return source.path;
    } else {
        return "s3://" + source.bucket + "/" + source.path;
    }
}

std::string status_to_string(FileCompareStatus status, int64_t size_a, int64_t size_b,
                             const std::string& src_a, const std::string& src_b) {
    if (status == FileCompareStatus::Mismatch) {
        if (size_a >= 0 && size_b < 0) {
            return src_a.empty() ? "ONLY_A" : "ONLY " + src_a;
        }
        if (size_b >= 0 && size_a < 0) {
            return src_b.empty() ? "ONLY_B" : "ONLY " + src_b;
        }
        return "DIFFERENT";
    }
    switch (status) {
        case FileCompareStatus::Match: return "MATCH";
        case FileCompareStatus::Error: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Write results to JSON file
bool write_json_results(const std::string& filename, const DirectoryComparisonResult& result,
                        const std::string& source_a, const std::string& source_b) {
    std::ofstream out(filename);
    if (!out) return false;

    out << "{\n";
    out << "  \"source_a\": \"" << source_a << "\",\n";
    out << "  \"source_b\": \"" << source_b << "\",\n";
    out << "  \"summary\": {\n";
    out << "    \"total_files\": " << result.total_files << ",\n";
    out << "    \"matching\": " << result.matching_files << ",\n";
    out << "    \"different\": " << content_differences(result) << ",\n";
    out << "    \"only_in_" << source_a << "\": " << result.only_in_a << ",\n";
    out << "    \"only_in_" << source_b << "\": " << result.only_in_b << ",\n";
    out << "    \"errors\": " << result.errors << ",\n";
    out << "    \"elapsed_seconds\": " << std::fixed << std::setprecision(2) << result.total_elapsed << "\n";
    out << "  },\n";
    out << "  \"files\": [\n";

    bool first = true;
    for (const auto& file : result.files) {
        if (file.status == FileCompareStatus::Match) continue;
        if (!first) out << ",\n";
        first = false;

        out << "    {\n";
        out << "      \"path\": \"" << file.relative_path << "\",\n";
        out << "      \"status\": \"" << status_to_string(file.status, file.size_a, file.size_b, source_a, source_b) << "\",\n";
        out << "      \"size_a\": " << file.size_a << ",\n";
        out << "      \"size_b\": " << file.size_b;
        if (!file.error_message.empty()) {
            out << ",\n      \"error\": \"" << file.error_message << "\"";
        }
        out << "\n    }";
    }
    out << "\n  ]\n}\n";
    return true;
}

// Write results to CSV file
bool write_csv_results(const std::string& filename, const DirectoryComparisonResult& result,
                       const std::string& source_a, const std::string& source_b) {
    std::ofstream out(filename);
    if (!out) return false;

    out << "status,path,size_a,size_b,error\n";
    for (const auto& file : result.files) {
        if (file.status == FileCompareStatus::Match) continue;
        out << status_to_string(file.status, file.size_a, file.size_b, source_a, source_b) << ",";
        out << "\"" << file.relative_path << "\",";
        out << file.size_a << "," << file.size_b << ",";
        out << "\"" << file.error_message << "\"\n";
    }
    return true;
}

// Write results to text file
bool write_txt_results(const std::string& filename, const DirectoryComparisonResult& result,
                       const std::string& source_a, const std::string& source_b) {
    std::ofstream out(filename);
    if (!out) return false;

    out << "Directory Comparison Results\n";
    out << "============================\n\n";
    out << "Source A: " << source_a << "\n";
    out << "Source B: " << source_b << "\n\n";
    out << "Summary:\n";
    out << "  Total files:      " << result.total_files << "\n";
    out << "  Matching:         " << result.matching_files << "\n";
    out << "  Different:        " << content_differences(result) << "\n";
    out << "  Only in " << source_a << ": " << result.only_in_a << "\n";
    out << "  Only in " << source_b << ": " << result.only_in_b << "\n";
    out << "  Errors:           " << result.errors << "\n";
    out << "  Time:             " << format_duration(result.total_elapsed) << "\n\n";

    if (result.mismatched_files > 0 || result.only_in_a > 0 || result.only_in_b > 0 || result.errors > 0) {
        out << "Differences:\n";
        for (const auto& file : result.files) {
            if (file.status == FileCompareStatus::Match) continue;
            out << "  [" << status_to_string(file.status, file.size_a, file.size_b, source_a, source_b) << "] " << file.relative_path;
            if (!file.error_message.empty()) {
                out << " - " << file.error_message;
            }
            out << "\n";
        }
    }
    return true;
}

// Write results to file based on extension
bool write_results_to_file(const std::string& filename, const DirectoryComparisonResult& result,
                           const std::string& source_a, const std::string& source_b) {
    std::string ext = filename.substr(filename.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == "json") {
        return write_json_results(filename, result, source_a, source_b);
    } else if (ext == "csv") {
        return write_csv_results(filename, result, source_a, source_b);
    } else {
        return write_txt_results(filename, result, source_a, source_b);
    }
}

// Print directory comparison results to console
void print_directory_result(const DirectoryComparisonResult& result,
                            const std::string& src_a, const std::string& src_b,
                            std::ostream& out) {
    bool all_match = (result.mismatched_files == 0 && result.only_in_a == 0 &&
                      result.only_in_b == 0 && result.errors == 0);

    if (all_match) {
        out << "✓ " << result.matching_files << " files match ("
            << format_duration(result.total_elapsed) << ")\n";
    } else {
        out << "✗ " << result.matching_files << "/" << result.total_files << " match";
        const size_t differing = content_differences(result);
        if (differing > 0) out << ", " << differing << " differ";
        if (result.only_in_a > 0) out << ", " << result.only_in_a << " only in " << src_a;
        if (result.only_in_b > 0) out << ", " << result.only_in_b << " only in " << src_b;
        if (result.errors > 0) out << ", " << result.errors << " errors";
        out << " (" << format_duration(result.total_elapsed) << ")\n";
    }
}
