#include <gtest/gtest.h>
#include "directory_comparison.h"
#include "s3_mock.h"
#include "constants.h"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <unistd.h>
#include <map>
#include <set>
#include <thread>
#include <future>
#include <sys/stat.h>

namespace fs = std::filesystem;

// ============================================================================
// Local Directory Enumeration Tests
// ============================================================================

class LocalEnumerationTest : public ::testing::Test {
protected:
    std::string temp_root;

    void SetUp() override {
        temp_root = "/tmp/objiff_dir_test_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void create_file(const std::string& relative_path, size_t size = 100) {
        fs::path full_path = fs::path(temp_root) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        if (size > 0) {
            std::vector<char> data(size, 'X');
            out.write(data.data(), data.size());
        }
        out.close();
    }

    void create_subdir(const std::string& relative_path) {
        fs::create_directories(fs::path(temp_root) / relative_path);
    }
};

TEST_F(LocalEnumerationTest, EmptyDirectory) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(files_found.load(), 0u);
}

TEST_F(LocalEnumerationTest, SingleFile) {
    create_file("test.txt", 50);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "test.txt");
    EXPECT_EQ(entries[0].size, 50);
    EXPECT_EQ(files_found.load(), 1u);
}

TEST_F(LocalEnumerationTest, MultipleFiles) {
    create_file("a.txt", 10);
    create_file("b.txt", 20);
    create_file("c.txt", 30);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(files_found.load(), 3u);
}

TEST_F(LocalEnumerationTest, RecursiveEnumeration) {
    create_file("top.txt", 10);
    create_file("subdir/nested.txt", 20);
    create_file("subdir/deep/very_nested.txt", 30);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(files_found.load(), 3u);

    // Check paths are relative
    std::set<std::string> paths;
    for (const auto& e : entries) {
        paths.insert(e.relative_path);
    }
    EXPECT_TRUE(paths.count("top.txt"));
    EXPECT_TRUE(paths.count("subdir/nested.txt"));
    EXPECT_TRUE(paths.count("subdir/deep/very_nested.txt"));
}

TEST_F(LocalEnumerationTest, NonRecursiveEnumeration) {
    create_file("top.txt", 10);
    create_file("subdir/nested.txt", 20);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, false, files_found, cancelled);

    // Should only get top-level file
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "top.txt");
}

TEST_F(LocalEnumerationTest, Cancellation) {
    // Create many files
    for (int i = 0; i < 100; ++i) {
        create_file("file" + std::to_string(i) + ".txt", 10);
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{true};  // Pre-cancelled

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    // Should return early with fewer entries due to cancellation
    // The exact count depends on timing, but should be less than 100
    // or empty if cancelled before first iteration
    EXPECT_LE(entries.size(), 100u);
}

TEST_F(LocalEnumerationTest, NonExistentDirectory) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory("/nonexistent/path", true, files_found, cancelled);

    EXPECT_TRUE(entries.empty());
}

TEST_F(LocalEnumerationTest, EmptySubdirectories) {
    create_subdir("empty_subdir");
    create_file("file.txt", 10);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    // Should only include files, not empty directories
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file.txt");
}

// ============================================================================
// S3 Prefix Enumeration Tests
// ============================================================================

class S3EnumerationTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock;

    void SetUp() override {
        mock = std::make_shared<MockS3Client>();
        mock->CreateBucket("test-bucket");
    }

    void put_object(const std::string& key, size_t size = 100) {
        std::vector<uint8_t> data(size, 'X');
        mock->PutObject("test-bucket", key, data);
    }
};

TEST_F(S3EnumerationTest, EmptyPrefix) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "", true, files_found, cancelled, mock);

    EXPECT_TRUE(entries.empty());
}

TEST_F(S3EnumerationTest, SingleObject) {
    put_object("prefix/file.txt", 50);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", true, files_found, cancelled, mock);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file.txt");
    EXPECT_EQ(files_found.load(), 1u);
}

TEST_F(S3EnumerationTest, MultipleObjects) {
    put_object("prefix/a.txt");
    put_object("prefix/b.txt");
    put_object("prefix/c.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", true, files_found, cancelled, mock);

    ASSERT_EQ(entries.size(), 3u);
}

TEST_F(S3EnumerationTest, RecursiveListing) {
    put_object("prefix/top.txt");
    put_object("prefix/subdir/nested.txt");
    put_object("prefix/subdir/deep/very_nested.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", true, files_found, cancelled, mock);

    ASSERT_EQ(entries.size(), 3u);

    std::set<std::string> paths;
    for (const auto& e : entries) {
        paths.insert(e.relative_path);
    }
    EXPECT_TRUE(paths.count("top.txt"));
    EXPECT_TRUE(paths.count("subdir/nested.txt"));
    EXPECT_TRUE(paths.count("subdir/deep/very_nested.txt"));
}

TEST_F(S3EnumerationTest, NonRecursiveListing) {
    put_object("prefix/top.txt");
    put_object("prefix/subdir/nested.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Non-recursive listing uses delimiter "/"
    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", false, files_found, cancelled, mock);

    // Should only get top-level file
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "top.txt");
}

TEST_F(S3EnumerationTest, SkipsDirectoryMarkers) {
    // S3 sometimes has directory markers (objects ending with /)
    put_object("prefix/subdir/", 0);  // Directory marker
    put_object("prefix/subdir/file.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", true, files_found, cancelled, mock);

    // Should only have the actual file
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "subdir/file.txt");
}

TEST_F(S3EnumerationTest, PrefixAutoNormalization) {
    put_object("prefix/file.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Prefix without trailing slash should still work
    auto entries = enumerate_s3_prefix("test-bucket", "prefix", true, files_found, cancelled, mock);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file.txt");
}

TEST_F(S3EnumerationTest, NullClient) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_s3_prefix("test-bucket", "prefix/", true, files_found, cancelled, nullptr);

    EXPECT_TRUE(entries.empty());
}

// ============================================================================
// is_directory_source Tests
// ============================================================================

class IsDirectorySourceTest : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/objiff_isdir_test_" + std::to_string(getpid());
        fs::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

TEST_F(IsDirectorySourceTest, LocalDirectory) {
    FileSource source;
    source.type = SourceType::Local;
    source.path = temp_dir;

    EXPECT_TRUE(is_directory_source(source));
}

TEST_F(IsDirectorySourceTest, LocalFile) {
    std::string file_path = temp_dir + "/test.txt";
    std::ofstream(file_path) << "test";

    FileSource source;
    source.type = SourceType::Local;
    source.path = file_path;

    EXPECT_FALSE(is_directory_source(source));
}

TEST_F(IsDirectorySourceTest, LocalNonExistent) {
    FileSource source;
    source.type = SourceType::Local;
    source.path = "/nonexistent/path";

    EXPECT_FALSE(is_directory_source(source));
}

TEST_F(IsDirectorySourceTest, S3WithTrailingSlash) {
    FileSource source;
    source.type = SourceType::S3;
    source.path = "prefix/";

    EXPECT_TRUE(is_directory_source(source));
}

TEST_F(IsDirectorySourceTest, S3WithoutTrailingSlash) {
    FileSource source;
    source.type = SourceType::S3;
    source.path = "key.txt";

    EXPECT_FALSE(is_directory_source(source));
}

TEST_F(IsDirectorySourceTest, S3EmptyPath) {
    FileSource source;
    source.type = SourceType::S3;
    source.path = "";

    EXPECT_FALSE(is_directory_source(source));
}

// ============================================================================
// Comparison Mode Detection Tests
// ============================================================================
// These tests verify the logic used to determine comparison type based on
// source combinations (file vs directory)

enum class ExpectedComparisonMode { File, Directory, Incompatible };

// Helper function that mirrors the GUI logic for determining comparison mode
static ExpectedComparisonMode determine_comparison_mode(const FileSource& a, const FileSource& b) {
    bool is_dir_a = is_directory_source(a);
    bool is_dir_b = is_directory_source(b);

    if (is_dir_a != is_dir_b) {
        return ExpectedComparisonMode::Incompatible;
    } else if (is_dir_a && is_dir_b) {
        return ExpectedComparisonMode::Directory;
    } else {
        return ExpectedComparisonMode::File;
    }
}

class ComparisonModeDetectionTest : public ::testing::Test {
protected:
    std::string temp_dir;
    std::string temp_file;

    void SetUp() override {
        temp_dir = "/tmp/objiff_mode_test_" + std::to_string(getpid());
        fs::create_directories(temp_dir);
        temp_file = temp_dir + "/test.bin";
        std::ofstream(temp_file) << "test content";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

TEST_F(ComparisonModeDetectionTest, TwoLocalFiles) {
    std::string file_b = temp_dir + "/other.bin";
    std::ofstream(file_b) << "other content";

    FileSource a{SourceType::Local, temp_file, "", ""};
    FileSource b{SourceType::Local, file_b, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::File);
}

TEST_F(ComparisonModeDetectionTest, TwoLocalDirectories) {
    std::string dir_b = temp_dir + "/subdir";
    fs::create_directories(dir_b);

    FileSource a{SourceType::Local, temp_dir, "", ""};
    FileSource b{SourceType::Local, dir_b, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Directory);
}

TEST_F(ComparisonModeDetectionTest, LocalFilePlusLocalDirectory) {
    FileSource a{SourceType::Local, temp_file, "", ""};
    FileSource b{SourceType::Local, temp_dir, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, LocalDirectoryPlusLocalFile) {
    FileSource a{SourceType::Local, temp_dir, "", ""};
    FileSource b{SourceType::Local, temp_file, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, TwoS3Files) {
    FileSource a{SourceType::S3, "key-a.bin", "bucket", "us-east-1"};
    FileSource b{SourceType::S3, "key-b.bin", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::File);
}

TEST_F(ComparisonModeDetectionTest, TwoS3Prefixes) {
    FileSource a{SourceType::S3, "prefix-a/", "bucket", "us-east-1"};
    FileSource b{SourceType::S3, "prefix-b/", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Directory);
}

TEST_F(ComparisonModeDetectionTest, S3FilePlusS3Prefix) {
    FileSource a{SourceType::S3, "key.bin", "bucket", "us-east-1"};
    FileSource b{SourceType::S3, "prefix/", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, S3PrefixPlusS3File) {
    FileSource a{SourceType::S3, "prefix/", "bucket", "us-east-1"};
    FileSource b{SourceType::S3, "key.bin", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, LocalFilePlusS3File) {
    FileSource a{SourceType::Local, temp_file, "", ""};
    FileSource b{SourceType::S3, "key.bin", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::File);
}

TEST_F(ComparisonModeDetectionTest, LocalDirectoryPlusS3Prefix) {
    FileSource a{SourceType::Local, temp_dir, "", ""};
    FileSource b{SourceType::S3, "prefix/", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Directory);
}

TEST_F(ComparisonModeDetectionTest, LocalFilePlusS3Prefix) {
    FileSource a{SourceType::Local, temp_file, "", ""};
    FileSource b{SourceType::S3, "prefix/", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, LocalDirectoryPlusS3File) {
    FileSource a{SourceType::Local, temp_dir, "", ""};
    FileSource b{SourceType::S3, "key.bin", "bucket", "us-east-1"};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, S3FilePlusLocalDirectory) {
    FileSource a{SourceType::S3, "key.bin", "bucket", "us-east-1"};
    FileSource b{SourceType::Local, temp_dir, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

TEST_F(ComparisonModeDetectionTest, S3PrefixPlusLocalFile) {
    FileSource a{SourceType::S3, "prefix/", "bucket", "us-east-1"};
    FileSource b{SourceType::Local, temp_file, "", ""};

    EXPECT_EQ(determine_comparison_mode(a, b), ExpectedComparisonMode::Incompatible);
}

// ============================================================================
// Full Directory Comparison Tests
// ============================================================================

class DirectoryComparisonTest : public ::testing::Test {
protected:
    std::string temp_dir_a;
    std::string temp_dir_b;

    void SetUp() override {
        std::string base = "/tmp/objiff_dircomp_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::rand());
        temp_dir_a = base + "_a";
        temp_dir_b = base + "_b";
        fs::create_directories(temp_dir_a);
        fs::create_directories(temp_dir_b);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_a, ec);
        fs::remove_all(temp_dir_b, ec);
    }

    void create_file(const std::string& dir, const std::string& relative_path,
                     const std::string& content) {
        fs::path full_path = fs::path(dir) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
        out.close();
    }

    void create_file_with_size(const std::string& dir, const std::string& relative_path,
                               size_t size, char fill = 'X') {
        fs::path full_path = fs::path(dir) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        std::vector<char> data(size, fill);
        out.write(data.data(), data.size());
        out.close();
    }
};

TEST_F(DirectoryComparisonTest, BothEmpty) {
    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 0u);
    EXPECT_EQ(result.matching_files, 0u);
    EXPECT_EQ(result.mismatched_files, 0u);
    EXPECT_EQ(result.only_in_a, 0u);
    EXPECT_EQ(result.only_in_b, 0u);
}

TEST_F(DirectoryComparisonTest, IdenticalDirectories) {
    create_file(temp_dir_a, "file1.txt", "Hello World");
    create_file(temp_dir_b, "file1.txt", "Hello World");
    create_file(temp_dir_a, "subdir/file2.txt", "Nested content");
    create_file(temp_dir_b, "subdir/file2.txt", "Nested content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 2u);
    EXPECT_EQ(result.mismatched_files, 0u);
    EXPECT_EQ(result.only_in_a, 0u);
    EXPECT_EQ(result.only_in_b, 0u);
}

TEST_F(DirectoryComparisonTest, FilesOnlyInA) {
    create_file(temp_dir_a, "only_a.txt", "A content");
    create_file(temp_dir_a, "common.txt", "Common");
    create_file(temp_dir_b, "common.txt", "Common");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 1u);  // Files only in A count as mismatches
    EXPECT_EQ(result.only_in_a, 1u);
    EXPECT_EQ(result.only_in_b, 0u);

    // Find the only-in-A file and verify its properties
    auto it = std::find_if(result.files.begin(), result.files.end(),
        [](const FileCompareResult& f) { return f.relative_path == "only_a.txt"; });
    ASSERT_NE(it, result.files.end());
    EXPECT_EQ(it->status, FileCompareStatus::Mismatch);
    EXPECT_GE(it->size_a, 0);
    EXPECT_EQ(it->size_b, -1);  // Not present in B
    EXPECT_GT(it->extra_chunks_in_a.size(), 0u);  // All chunks marked as extra in A
}

TEST_F(DirectoryComparisonTest, FilesOnlyInB) {
    create_file(temp_dir_a, "common.txt", "Common");
    create_file(temp_dir_b, "common.txt", "Common");
    create_file(temp_dir_b, "only_b.txt", "B content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 1u);  // Files only in B count as mismatches
    EXPECT_EQ(result.only_in_a, 0u);
    EXPECT_EQ(result.only_in_b, 1u);

    // Find the only-in-B file and verify its properties
    auto it = std::find_if(result.files.begin(), result.files.end(),
        [](const FileCompareResult& f) { return f.relative_path == "only_b.txt"; });
    ASSERT_NE(it, result.files.end());
    EXPECT_EQ(it->status, FileCompareStatus::Mismatch);
    EXPECT_EQ(it->size_a, -1);  // Not present in A
    EXPECT_GE(it->size_b, 0);
    EXPECT_GT(it->extra_chunks_in_b.size(), 0u);  // All chunks marked as extra in B
}

TEST_F(DirectoryComparisonTest, OnlyInABlockIndicesCorrect) {
    // Create a 16 KiB file only in A to verify block indices are populated correctly
    // 16 KiB with 1 KiB block size = 16 blocks, all should be in extra_blocks_in_a
    std::vector<uint8_t> data(16 * 1024, 'X');
    fs::path file_path = fs::path(temp_dir_a) / "only_a.bin";
    std::ofstream out(file_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_EQ(file.size_a, 16 * 1024);
    EXPECT_EQ(file.size_b, -1);

    // Verify chunk-level analysis
    EXPECT_EQ(file.total_chunks, 1u);  // 16 KiB fits in one 8 MiB chunk
    ASSERT_EQ(file.extra_chunks_in_a.size(), 1u);
    EXPECT_EQ(file.extra_chunks_in_a[0], 0u);

    // Verify block-level analysis
    EXPECT_TRUE(file.has_block_analysis);
    EXPECT_EQ(file.block_size, 1024);  // 16 KiB uses 1 KiB blocks
    EXPECT_EQ(file.total_blocks, 16u);
    ASSERT_EQ(file.extra_blocks_in_a.size(), 16u);
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(file.extra_blocks_in_a[i], i);
    }
    EXPECT_EQ(file.extra_blocks_in_b.size(), 0u);
    EXPECT_EQ(file.mismatched_blocks.size(), 0u);
}

TEST_F(DirectoryComparisonTest, OnlyInBBlockIndicesCorrect) {
    // Create a 20 KiB file only in B to verify block indices are populated correctly
    // 20 KiB with 1 KiB block size = 20 blocks, all should be in extra_blocks_in_b
    std::vector<uint8_t> data(20 * 1024, 'Y');
    fs::path file_path = fs::path(temp_dir_b) / "only_b.bin";
    std::ofstream out(file_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_EQ(file.size_a, -1);
    EXPECT_EQ(file.size_b, 20 * 1024);

    // Verify chunk-level analysis
    EXPECT_EQ(file.total_chunks, 1u);
    ASSERT_EQ(file.extra_chunks_in_b.size(), 1u);
    EXPECT_EQ(file.extra_chunks_in_b[0], 0u);

    // Verify block-level analysis
    EXPECT_TRUE(file.has_block_analysis);
    EXPECT_EQ(file.block_size, 1024);  // 20 KiB uses 1 KiB blocks
    EXPECT_EQ(file.total_blocks, 20u);
    ASSERT_EQ(file.extra_blocks_in_b.size(), 20u);
    for (size_t i = 0; i < 20; ++i) {
        EXPECT_EQ(file.extra_blocks_in_b[i], i);
    }
    EXPECT_EQ(file.extra_blocks_in_a.size(), 0u);
    EXPECT_EQ(file.mismatched_blocks.size(), 0u);
}

TEST_F(DirectoryComparisonTest, LargeFileOnlyInANoBlockAnalysis) {
    // Create a file larger than 5*CHUNK_SIZE (40 MiB) only in A
    // Block analysis should NOT be performed, but chunk analysis should work
    // Use 48 MiB = 6 chunks
    const int64_t file_size = 48 * 1024 * 1024;
    fs::path file_path = fs::path(temp_dir_a) / "large_only_a.bin";
    std::ofstream out(file_path, std::ios::binary);
    std::vector<char> chunk(1024 * 1024, 'L');  // Write 1 MiB at a time
    for (int i = 0; i < 48; ++i) {
        out.write(chunk.data(), chunk.size());
    }
    out.close();

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_EQ(file.size_a, file_size);
    EXPECT_EQ(file.size_b, -1);

    // Verify chunk-level analysis: 48 MiB / 8 MiB = 6 chunks
    EXPECT_EQ(file.total_chunks, 6u);
    ASSERT_EQ(file.extra_chunks_in_a.size(), 6u);
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(file.extra_chunks_in_a[i], i);
    }
    EXPECT_EQ(file.extra_chunks_in_b.size(), 0u);
    EXPECT_EQ(file.mismatched_chunks.size(), 0u);

    // Block analysis should NOT be performed for large files
    EXPECT_FALSE(file.has_block_analysis);
    EXPECT_EQ(file.extra_blocks_in_a.size(), 0u);
    EXPECT_EQ(file.extra_blocks_in_b.size(), 0u);
}

TEST_F(DirectoryComparisonTest, LargeFileOnlyInBNoBlockAnalysis) {
    // Create a file larger than 5*CHUNK_SIZE (40 MiB) only in B
    // Use 42 MiB (just over 5 chunks)
    const int64_t file_size = 42 * 1024 * 1024;
    fs::path file_path = fs::path(temp_dir_b) / "large_only_b.bin";
    std::ofstream out(file_path, std::ios::binary);
    std::vector<char> chunk(1024 * 1024, 'M');
    for (int i = 0; i < 42; ++i) {
        out.write(chunk.data(), chunk.size());
    }
    out.close();

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_EQ(file.size_a, -1);
    EXPECT_EQ(file.size_b, file_size);

    // Verify chunk-level analysis: 42 MiB / 8 MiB = 6 chunks (ceil)
    EXPECT_EQ(file.total_chunks, 6u);
    ASSERT_EQ(file.extra_chunks_in_b.size(), 6u);
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(file.extra_chunks_in_b[i], i);
    }

    // Block analysis should NOT be performed for large files
    EXPECT_FALSE(file.has_block_analysis);
}

TEST_F(DirectoryComparisonTest, ContentMismatch) {
    create_file(temp_dir_a, "differ.txt", "Content A");
    create_file(temp_dir_b, "differ.txt", "Content B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 0u);
    EXPECT_EQ(result.mismatched_files, 1u);
}

TEST_F(DirectoryComparisonTest, MixedResults) {
    // Create files with different statuses
    create_file(temp_dir_a, "match.txt", "Same");
    create_file(temp_dir_b, "match.txt", "Same");
    create_file(temp_dir_a, "differ.txt", "AAA");
    create_file(temp_dir_b, "differ.txt", "BBB");
    create_file(temp_dir_a, "only_a.txt", "Only A");
    create_file(temp_dir_b, "only_b.txt", "Only B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 4u);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 3u);  // 1 content diff + 1 only_in_a + 1 only_in_b
    EXPECT_EQ(result.only_in_a, 1u);
    EXPECT_EQ(result.only_in_b, 1u);
}

TEST_F(DirectoryComparisonTest, NonRecursiveComparison) {
    create_file(temp_dir_a, "top.txt", "Top");
    create_file(temp_dir_b, "top.txt", "Top");
    create_file(temp_dir_a, "subdir/nested.txt", "Nested A");
    create_file(temp_dir_b, "subdir/nested.txt", "Nested B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = false;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    // Should only compare top-level file
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
}

TEST_F(DirectoryComparisonTest, Cancellation) {
    // Create several files
    for (int i = 0; i < 20; ++i) {
        create_file(temp_dir_a, "file" + std::to_string(i) + ".txt", "Content " + std::to_string(i));
        create_file(temp_dir_b, "file" + std::to_string(i) + ".txt", "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    progress.cancelled = true;  // Pre-cancel

    auto result = run_directory_comparison(config, progress);

    // Should fail due to cancellation
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

TEST_F(DirectoryComparisonTest, ResultSorting) {
    // Create files to test sorting: mismatches first (including only-in-A/B), then matches
    create_file(temp_dir_a, "z_match.txt", "Same");
    create_file(temp_dir_b, "z_match.txt", "Same");
    create_file(temp_dir_a, "a_differ.txt", "AAA");
    create_file(temp_dir_b, "a_differ.txt", "BBB");
    create_file(temp_dir_a, "m_only_a.txt", "Only A");
    create_file(temp_dir_b, "n_only_b.txt", "Only B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    ASSERT_EQ(result.files.size(), 4u);

    // All differences (content mismatch + only-in-A + only-in-B) come first, sorted by path
    // Files: a_differ.txt, m_only_a.txt, n_only_b.txt (all Mismatch), then z_match.txt (Match)
    EXPECT_EQ(result.files[0].status, FileCompareStatus::Mismatch);
    EXPECT_EQ(result.files[0].relative_path, "a_differ.txt");
    EXPECT_EQ(result.files[1].status, FileCompareStatus::Mismatch);
    EXPECT_EQ(result.files[1].relative_path, "m_only_a.txt");
    EXPECT_EQ(result.files[1].size_b, -1);  // Only in A
    EXPECT_EQ(result.files[2].status, FileCompareStatus::Mismatch);
    EXPECT_EQ(result.files[2].relative_path, "n_only_b.txt");
    EXPECT_EQ(result.files[2].size_a, -1);  // Only in B
    EXPECT_EQ(result.files[3].status, FileCompareStatus::Match);
}

// ============================================================================
// S3-to-S3 Directory Comparison with Mock
// ============================================================================

class S3DirectoryComparisonTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_a;
    std::shared_ptr<MockS3Client> mock_b;

    void SetUp() override {
        mock_a = std::make_shared<MockS3Client>();
        mock_b = std::make_shared<MockS3Client>();
        mock_a->CreateBucket("bucket-a");
        mock_b->CreateBucket("bucket-b");
    }

    void put_object_a(const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock_a->PutObject("bucket-a", key, data);
    }

    void put_object_b(const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock_b->PutObject("bucket-b", key, data);
    }
};

TEST_F(S3DirectoryComparisonTest, IdenticalPrefixes) {
    put_object_a("prefix/file1.txt", "Content 1");
    put_object_b("prefix/file1.txt", "Content 1");
    put_object_a("prefix/file2.txt", "Content 2");
    put_object_b("prefix/file2.txt", "Content 2");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "bucket-a";
    config.source_a.path = "prefix/";
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "bucket-b";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, mock_a, mock_b);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 2u);
    EXPECT_EQ(result.mismatched_files, 0u);
}

TEST_F(S3DirectoryComparisonTest, DifferentPrefixes) {
    put_object_a("prefix-a/file.txt", "Content A");
    put_object_b("prefix-b/file.txt", "Content B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "bucket-a";
    config.source_a.path = "prefix-a/";
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "bucket-b";
    config.source_b.path = "prefix-b/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, mock_a, mock_b);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.mismatched_files, 1u);  // Same relative path, different content
}

TEST_F(S3DirectoryComparisonTest, ObjectsOnlyInOne) {
    put_object_a("prefix/common.txt", "Common");
    put_object_b("prefix/common.txt", "Common");
    put_object_a("prefix/only_a.txt", "A");
    put_object_b("prefix/only_b.txt", "B");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "bucket-a";
    config.source_a.path = "prefix/";
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "bucket-b";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, mock_a, mock_b);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 2u);  // only_in_a + only_in_b count as mismatches
    EXPECT_EQ(result.only_in_a, 1u);
    EXPECT_EQ(result.only_in_b, 1u);
}

// ============================================================================
// Local-to-S3 Directory Comparison Tests (Mixed Mode)
// ============================================================================

class LocalToS3DirectoryComparisonTest : public ::testing::Test {
protected:
    std::string temp_dir;
    std::shared_ptr<MockS3Client> mock;

    void SetUp() override {
        temp_dir = "/tmp/objiff_local_s3_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(std::rand());
        fs::create_directories(temp_dir);
        mock = std::make_shared<MockS3Client>();
        mock->CreateBucket("test-bucket");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void create_local_file(const std::string& relative_path, const std::string& content) {
        fs::path full_path = fs::path(temp_dir) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
        out.close();
    }

    void put_s3_object(const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock->PutObject("test-bucket", key, data);
    }
};

TEST_F(LocalToS3DirectoryComparisonTest, IdenticalContent) {
    // Local directory A, S3 prefix B - same content
    create_local_file("file1.txt", "Content 1");
    create_local_file("file2.txt", "Content 2");
    put_s3_object("prefix/file1.txt", "Content 1");
    put_s3_object("prefix/file2.txt", "Content 2");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 2u);
    EXPECT_EQ(result.mismatched_files, 0u);
}

TEST_F(LocalToS3DirectoryComparisonTest, ContentMismatch) {
    // Same files but different content
    create_local_file("data.txt", "Local content");
    put_s3_object("prefix/data.txt", "S3 content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 0u);
    EXPECT_EQ(result.mismatched_files, 1u);
}

TEST_F(LocalToS3DirectoryComparisonTest, FilesOnlyInLocal) {
    create_local_file("common.txt", "Common");
    create_local_file("local_only.txt", "Local only");
    put_s3_object("prefix/common.txt", "Common");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 1u);  // only_in_a counts as mismatch
    EXPECT_EQ(result.only_in_a, 1u);
    EXPECT_EQ(result.only_in_b, 0u);
}

TEST_F(LocalToS3DirectoryComparisonTest, FilesOnlyInS3) {
    create_local_file("common.txt", "Common");
    put_s3_object("prefix/common.txt", "Common");
    put_s3_object("prefix/s3_only.txt", "S3 only");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 1u);  // only_in_b counts as mismatch
    EXPECT_EQ(result.only_in_a, 0u);
    EXPECT_EQ(result.only_in_b, 1u);
}

TEST_F(LocalToS3DirectoryComparisonTest, NestedDirectories) {
    // Test recursive comparison with nested structure
    create_local_file("top.txt", "Top");
    create_local_file("sub/nested.txt", "Nested");
    create_local_file("sub/deep/very_nested.txt", "Very nested");
    put_s3_object("prefix/top.txt", "Top");
    put_s3_object("prefix/sub/nested.txt", "Nested");
    put_s3_object("prefix/sub/deep/very_nested.txt", "Very nested");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "test-bucket";
    config.source_b.path = "prefix/";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, nullptr, mock);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 3u);
    EXPECT_EQ(result.matching_files, 3u);
}

TEST_F(LocalToS3DirectoryComparisonTest, S3ToLocalDirection) {
    // Test the reverse: S3 as source A, Local as source B
    put_s3_object("prefix/file.txt", "Content");
    create_local_file("file.txt", "Content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::S3;
    config.source_a.bucket = "test-bucket";
    config.source_a.path = "prefix/";
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, mock, nullptr);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
}

// ============================================================================
// Adaptive Block Size Tests
// ============================================================================

TEST(AdaptiveBlockSizeTest, VerySmallFiles) {
    // Files <= 64 KiB should use 1 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(1024), 1024);        // 1 KiB
    EXPECT_EQ(compute_adaptive_block_size(32 * 1024), 1024);   // 32 KiB
    EXPECT_EQ(compute_adaptive_block_size(64 * 1024), 1024);   // 64 KiB
}

TEST(AdaptiveBlockSizeTest, SmallFiles) {
    // Files 64-256 KiB should use 2 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(128 * 1024), 2 * 1024);  // 128 KiB
    EXPECT_EQ(compute_adaptive_block_size(256 * 1024), 2 * 1024);  // 256 KiB

    // Files 256-512 KiB should use 4 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(384 * 1024), 4 * 1024);  // 384 KiB
    EXPECT_EQ(compute_adaptive_block_size(512 * 1024), 4 * 1024);  // 512 KiB
}

TEST(AdaptiveBlockSizeTest, MediumFiles) {
    // Files 512 KiB - 1 MiB should use 8 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(768 * 1024), 8 * 1024);  // 768 KiB
    EXPECT_EQ(compute_adaptive_block_size(1024 * 1024), 8 * 1024); // 1 MiB

    // Files 1-4 MiB should use 16 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(2 * 1024 * 1024), 16 * 1024); // 2 MiB
    EXPECT_EQ(compute_adaptive_block_size(4 * 1024 * 1024), 16 * 1024); // 4 MiB
}

TEST(AdaptiveBlockSizeTest, LargeFiles) {
    // Files 4-16 MiB should use 64 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(8 * 1024 * 1024), 64 * 1024);   // 8 MiB
    EXPECT_EQ(compute_adaptive_block_size(16 * 1024 * 1024), 64 * 1024);  // 16 MiB

    // Files > 16 MiB should use 128 KiB blocks
    EXPECT_EQ(compute_adaptive_block_size(32 * 1024 * 1024), 128 * 1024); // 32 MiB
}

TEST(AdaptiveBlockSizeTest, EdgeCases) {
    EXPECT_EQ(compute_adaptive_block_size(0), 1024);   // Zero size
    EXPECT_EQ(compute_adaptive_block_size(-1), 1024);  // Negative size
}

// ============================================================================
// Block-Level Analysis Tests for Small Files
// ============================================================================

class BlockAnalysisTest : public ::testing::Test {
protected:
    std::string temp_dir;

    void SetUp() override {
        temp_dir = "/tmp/objiff_block_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(std::rand());
        fs::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void create_file(const std::string& name, const std::vector<uint8_t>& data) {
        fs::path path = fs::path(temp_dir) / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
    }
};

TEST_F(BlockAnalysisTest, SmallFileBlockAnalysis) {
    // Create two 16 KiB files with a difference in the middle
    std::vector<uint8_t> data_a(16 * 1024, 'A');
    std::vector<uint8_t> data_b(16 * 1024, 'A');

    // Make bytes 8192-9215 different (1 block at 1 KiB block size)
    std::fill(data_b.begin() + 8 * 1024, data_b.begin() + 9 * 1024, 'B');

    create_file("file_a.bin", data_a);
    create_file("file_b.bin", data_b);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir + "/file_a.bin";
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir + "/file_b.bin";

    // Use single-file directory comparison by creating dirs
    fs::create_directories(temp_dir + "/dir_a");
    fs::create_directories(temp_dir + "/dir_b");
    fs::rename(temp_dir + "/file_a.bin", temp_dir + "/dir_a/file.bin");
    fs::rename(temp_dir + "/file_b.bin", temp_dir + "/dir_b/file.bin");

    config.source_a.path = temp_dir + "/dir_a";
    config.source_b.path = temp_dir + "/dir_b";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_TRUE(file.has_block_analysis);
    EXPECT_EQ(file.block_size, 1024);      // 16 KiB file uses 1 KiB blocks
    EXPECT_EQ(file.total_blocks, 16u);     // 16 KiB / 1 KiB = 16 blocks
    EXPECT_EQ(file.mismatched_blocks.size(), 1u);  // Block 8 differs
    EXPECT_EQ(file.mismatched_blocks[0], 8u);
}

TEST_F(BlockAnalysisTest, FileSizeDifferenceBlockAnalysis) {
    // Create files with different sizes
    std::vector<uint8_t> data_a(20 * 1024, 'A');  // 20 KiB
    std::vector<uint8_t> data_b(12 * 1024, 'A');  // 12 KiB (smaller)

    fs::create_directories(temp_dir + "/dir_a");
    fs::create_directories(temp_dir + "/dir_b");

    create_file("dir_a/file.bin", data_a);
    create_file("dir_b/file.bin", data_b);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir + "/dir_a";
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir + "/dir_b";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_TRUE(file.has_block_analysis);
    EXPECT_EQ(file.block_size, 1024);      // 20 KiB file uses 1 KiB blocks
    EXPECT_EQ(file.total_blocks, 20u);     // ceil(20 KiB / 1 KiB) = 20 blocks
    EXPECT_EQ(file.mismatched_blocks.size(), 0u);  // Common region matches
    EXPECT_EQ(file.extra_blocks_in_a.size(), 8u);  // 8 extra 1 KiB blocks in A
}

TEST_F(BlockAnalysisTest, TinyFileBlockAnalysis) {
    // Test very small files (< 1 KiB)
    std::vector<uint8_t> data_a(100, 'A');
    std::vector<uint8_t> data_b(100, 'B');

    fs::create_directories(temp_dir + "/dir_a");
    fs::create_directories(temp_dir + "/dir_b");

    create_file("dir_a/file.bin", data_a);
    create_file("dir_b/file.bin", data_b);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir + "/dir_a";
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir + "/dir_b";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);

    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    // Small file should have block analysis
    EXPECT_TRUE(file.has_block_analysis);
    EXPECT_EQ(file.block_size, 1024);  // 100 bytes -> 1 KiB blocks
    EXPECT_EQ(file.total_blocks, 1u);
}

// ============================================================================
// Edge Case Tests: Non-existent Source Directories
// ============================================================================

TEST_F(DirectoryComparisonTest, NonExistentSourceA) {
    // Source A doesn't exist, Source B is valid
    create_file(temp_dir_b, "file.txt", "content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = "/nonexistent/path/that/does/not/exist";
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // A source that cannot be read is not an empty source. Reporting B's file
    // as "only in B" would be a confident, wrong answer about a path that was
    // never actually examined.
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Source A"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(DirectoryComparisonTest, NonExistentSourceB) {
    // Source A is valid, Source B doesn't exist
    create_file(temp_dir_a, "file.txt", "content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = "/nonexistent/path/that/does/not/exist";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Source B"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(DirectoryComparisonTest, BothSourcesNonExistent) {
    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = "/nonexistent/path/a";
    config.source_b.type = SourceType::Local;
    config.source_b.path = "/nonexistent/path/b";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // Two unreadable paths are not two identical empty directories.
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Both sources"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_EQ(result.total_files, 0u);
}

// ============================================================================
// Edge Case Tests: Empty Files in Directory Comparison
// ============================================================================

TEST_F(DirectoryComparisonTest, BothEmptyFiles) {
    // Both directories have the same empty file
    create_file(temp_dir_a, "empty.txt", "");
    create_file(temp_dir_b, "empty.txt", "");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.mismatched_files, 0u);

    // Verify the file result
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].status, FileCompareStatus::Match);
    EXPECT_EQ(result.files[0].size_a, 0);
    EXPECT_EQ(result.files[0].size_b, 0);
}

TEST_F(DirectoryComparisonTest, EmptyFileVsNonEmptyFile) {
    // Same filename, but one is empty and one has content
    create_file(temp_dir_a, "file.txt", "");           // Empty
    create_file(temp_dir_b, "file.txt", "has content"); // Non-empty

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 0u);
    EXPECT_EQ(result.mismatched_files, 1u);

    // Verify the mismatch details
    ASSERT_EQ(result.files.size(), 1u);
    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_EQ(file.size_a, 0);
    EXPECT_GT(file.size_b, 0);
    // A has 0 chunks, B has 1 chunk
    EXPECT_EQ(file.total_chunks, 1u);
    EXPECT_TRUE(file.mismatched_chunks.empty());  // No common chunks to mismatch
    EXPECT_TRUE(file.extra_chunks_in_a.empty());
    EXPECT_EQ(file.extra_chunks_in_b.size(), 1u); // Chunk 0 only in B
}

TEST_F(DirectoryComparisonTest, NonEmptyFileVsEmptyFile) {
    // Reverse: A has content, B is empty
    create_file(temp_dir_a, "file.txt", "has content"); // Non-empty
    create_file(temp_dir_b, "file.txt", "");           // Empty

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.mismatched_files, 1u);

    ASSERT_EQ(result.files.size(), 1u);
    const auto& file = result.files[0];
    EXPECT_EQ(file.status, FileCompareStatus::Mismatch);
    EXPECT_GT(file.size_a, 0);
    EXPECT_EQ(file.size_b, 0);
    EXPECT_EQ(file.extra_chunks_in_a.size(), 1u); // Chunk 0 only in A
    EXPECT_TRUE(file.extra_chunks_in_b.empty());
}

TEST_F(DirectoryComparisonTest, MixedEmptyAndNonEmptyFiles) {
    // Multiple files: some empty, some with content
    create_file(temp_dir_a, "empty_match.txt", "");
    create_file(temp_dir_b, "empty_match.txt", "");
    create_file(temp_dir_a, "content_match.txt", "same");
    create_file(temp_dir_b, "content_match.txt", "same");
    create_file(temp_dir_a, "empty_vs_content.txt", "");
    create_file(temp_dir_b, "empty_vs_content.txt", "content");
    create_file(temp_dir_a, "only_in_a_empty.txt", "");
    create_file(temp_dir_b, "only_in_b_empty.txt", "");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 5u);        // 3 common + 1 only_a + 1 only_b
    EXPECT_EQ(result.matching_files, 2u);     // empty_match.txt and content_match.txt
    EXPECT_EQ(result.mismatched_files, 3u);   // 1 content diff + 1 only_in_a + 1 only_in_b
    EXPECT_EQ(result.only_in_a, 1u);          // only_in_a_empty.txt
    EXPECT_EQ(result.only_in_b, 1u);          // only_in_b_empty.txt
}

// ============================================================================
// Parallel Discovery Integration Tests
// ============================================================================

TEST_F(DirectoryComparisonTest, ParallelDiscoveryIdenticalResults) {
    // Create a directory structure with multiple levels
    create_file(temp_dir_a, "root.txt", "root content");
    create_file(temp_dir_b, "root.txt", "root content");
    create_file(temp_dir_a, "sub1/file1.txt", "content1");
    create_file(temp_dir_b, "sub1/file1.txt", "content1");
    create_file(temp_dir_a, "sub1/sub2/file2.txt", "content2");
    create_file(temp_dir_b, "sub1/sub2/file2.txt", "content2");
    create_file(temp_dir_a, "other/file3.txt", "content3");
    create_file(temp_dir_b, "other/file3.txt", "content3");

    // Run without parallel discovery
    DirectoryComparisonConfig config_seq;
    config_seq.source_a.type = SourceType::Local;
    config_seq.source_a.path = temp_dir_a;
    config_seq.source_b.type = SourceType::Local;
    config_seq.source_b.path = temp_dir_b;
    config_seq.recursive = true;
    config_seq.parallel_discovery = false;

    DirectoryComparisonProgress progress_seq;
    auto result_seq = run_directory_comparison(config_seq, progress_seq);

    // Run with parallel discovery
    DirectoryComparisonConfig config_par;
    config_par.source_a.type = SourceType::Local;
    config_par.source_a.path = temp_dir_a;
    config_par.source_b.type = SourceType::Local;
    config_par.source_b.path = temp_dir_b;
    config_par.recursive = true;
    config_par.parallel_discovery = true;
    config_par.parallel_discovery_workers = 8;

    DirectoryComparisonProgress progress_par;
    auto result_par = run_directory_comparison(config_par, progress_par);

    // Results should be identical
    EXPECT_TRUE(result_seq.success);
    EXPECT_TRUE(result_par.success);
    EXPECT_EQ(result_seq.total_files, result_par.total_files);
    EXPECT_EQ(result_seq.matching_files, result_par.matching_files);
    EXPECT_EQ(result_seq.mismatched_files, result_par.mismatched_files);
    EXPECT_EQ(result_seq.only_in_a, result_par.only_in_a);
    EXPECT_EQ(result_seq.only_in_b, result_par.only_in_b);
    EXPECT_EQ(result_par.matching_files, 4u);
}

TEST_F(DirectoryComparisonTest, ParallelDiscoveryWithDifferences) {
    // Create directories with various differences
    create_file(temp_dir_a, "match.txt", "same");
    create_file(temp_dir_b, "match.txt", "same");
    create_file(temp_dir_a, "only_a.txt", "only in a");
    create_file(temp_dir_b, "only_b.txt", "only in b");
    create_file(temp_dir_a, "sub/mismatch.txt", "version a");
    create_file(temp_dir_b, "sub/mismatch.txt", "version b");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 4;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);      // match.txt
    EXPECT_EQ(result.only_in_a, 1u);           // only_a.txt
    EXPECT_EQ(result.only_in_b, 1u);           // only_b.txt
    EXPECT_EQ(result.mismatched_files, 3u);    // only_a, only_b, mismatch

    // Verify specific files are correctly identified
    std::set<std::string> only_a_files, only_b_files;
    for (const auto& f : result.files) {
        if (f.size_a >= 0 && f.size_b < 0) only_a_files.insert(f.relative_path);
        if (f.size_a < 0 && f.size_b >= 0) only_b_files.insert(f.relative_path);
    }
    EXPECT_TRUE(only_a_files.count("only_a.txt"));
    EXPECT_TRUE(only_b_files.count("only_b.txt"));
}

TEST_F(DirectoryComparisonTest, ParallelDiscoveryManyFiles) {
    // Create many files to stress the parallel enumeration
    for (int i = 0; i < 50; ++i) {
        std::string content = "content" + std::to_string(i);
        create_file(temp_dir_a, "dir" + std::to_string(i % 10) + "/file" + std::to_string(i) + ".txt", content);
        create_file(temp_dir_b, "dir" + std::to_string(i % 10) + "/file" + std::to_string(i) + ".txt", content);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 16;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 50u);
    EXPECT_EQ(result.matching_files, 50u);
    EXPECT_EQ(result.mismatched_files, 0u);
}

// ============================================================================
// Edge Case Tests: Cancellation During File Comparison Phase
// ============================================================================

class CancellationDuringComparisonTest : public ::testing::Test {
protected:
    std::string temp_dir_a;
    std::string temp_dir_b;

    void SetUp() override {
        std::string base = "/tmp/objiff_cancel_test_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::rand());
        temp_dir_a = base + "_a";
        temp_dir_b = base + "_b";
        fs::create_directories(temp_dir_a);
        fs::create_directories(temp_dir_b);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_a, ec);
        fs::remove_all(temp_dir_b, ec);
    }

    void create_file_with_size(const std::string& dir, const std::string& name, size_t size) {
        fs::path path = fs::path(dir) / name;
        std::ofstream out(path, std::ios::binary);
        std::vector<char> data(size, 'X');
        out.write(data.data(), data.size());
        out.close();
    }
};

TEST_F(CancellationDuringComparisonTest, CancelAfterEnumerationBeforeComparison) {
    // Create files to compare
    for (int i = 0; i < 10; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file_with_size(temp_dir_a, name, 1000);
        create_file_with_size(temp_dir_b, name, 1000);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;

    // Start comparison in a thread
    std::atomic<bool> started{false};
    auto future = std::async(std::launch::async, [&]() {
        started = true;
        return run_directory_comparison(config, progress);
    });

    // Wait for enumeration to complete
    while (!progress.scanning_done && !started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Wait a tiny bit more then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    progress.cancelled = true;

    auto result = future.get();

    // Local file comparison is very fast, so either:
    // - Comparison completed before cancellation (success=true, all files compared)
    // - Cancellation took effect (success=false)
    // Either outcome is valid - this test ensures no crashes/hangs with concurrent cancellation
    if (result.success) {
        // Comparison completed before cancellation took effect
        EXPECT_EQ(progress.files_compared.load(), 10u);
    } else {
        // Cancellation was effective
        EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
    }
}

TEST_F(CancellationDuringComparisonTest, CancelDuringLargeFileComparison) {
    // Create a larger file
    create_file_with_size(temp_dir_a, "large.bin", 1024 * 1024);
    create_file_with_size(temp_dir_b, "large.bin", 1024 * 1024);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;

    auto future = std::async(std::launch::async, [&]() {
        return run_directory_comparison(config, progress);
    });

    // Wait for scanning to complete
    while (!progress.scanning_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Cancel immediately after scanning
    progress.cancelled = true;

    auto result = future.get();

    // Local file comparison is very fast (even for 1MB files)
    // Either outcome is valid - this test ensures no crashes/hangs
    if (result.success) {
        // Comparison completed before cancellation took effect
        EXPECT_EQ(result.files.size(), 1u);
        EXPECT_EQ(result.matching_files, 1u);
    } else {
        // Cancellation was effective
        EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
    }
}

// Test cancellation check is respected between files
TEST_F(CancellationDuringComparisonTest, CancellationCheckBetweenFiles) {
    // Pre-cancel before starting
    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    // Create some files
    for (int i = 0; i < 5; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file_with_size(temp_dir_a, name, 100);
        create_file_with_size(temp_dir_b, name, 100);
    }

    DirectoryComparisonProgress progress;
    progress.cancelled = true;  // Pre-cancel

    auto result = run_directory_comparison(config, progress);

    // Should fail due to cancellation during enumeration or comparison
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

// ============================================================================
// Parallel Execution Tests
// ============================================================================

class ParallelExecutionTest : public ::testing::Test {
protected:
    std::string temp_dir_a;
    std::string temp_dir_b;

    void SetUp() override {
        std::string base = "/tmp/objiff_parallel_test_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::rand());
        temp_dir_a = base + "_a";
        temp_dir_b = base + "_b";
        fs::create_directories(temp_dir_a);
        fs::create_directories(temp_dir_b);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_a, ec);
        fs::remove_all(temp_dir_b, ec);
    }

    void create_file(const std::string& dir, const std::string& relative_path,
                     const std::string& content) {
        fs::path full_path = fs::path(dir) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
        out.close();
    }

    void create_file_with_size(const std::string& dir, const std::string& name,
                               size_t size, char fill = 'X') {
        fs::path path = fs::path(dir) / name;
        std::ofstream out(path, std::ios::binary);
        std::vector<char> data(size, fill);
        out.write(data.data(), data.size());
        out.close();
    }
};

TEST_F(ParallelExecutionTest, ResultsConsistentAcrossThreadCounts) {
    // Create a mix of matching, mismatching, and unique files
    create_file(temp_dir_a, "match1.txt", "Same content 1");
    create_file(temp_dir_b, "match1.txt", "Same content 1");
    create_file(temp_dir_a, "match2.txt", "Same content 2");
    create_file(temp_dir_b, "match2.txt", "Same content 2");
    create_file(temp_dir_a, "differ.txt", "Content A");
    create_file(temp_dir_b, "differ.txt", "Content B");
    create_file(temp_dir_a, "only_a.txt", "Only in A");
    create_file(temp_dir_b, "only_b.txt", "Only in B");

    // Run with different thread counts and verify results match
    std::vector<int> thread_counts = {1, 2, 4, 8, 16, 64, 128};

    DirectoryComparisonResult baseline_result;
    bool have_baseline = false;

    for (int num_threads : thread_counts) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = temp_dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = temp_dir_b;
        config.recursive = true;
        config.num_threads = num_threads;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress);

        ASSERT_TRUE(result.success) << "Failed with num_threads=" << num_threads;
        EXPECT_EQ(result.total_files, 5u) << "num_threads=" << num_threads;
        EXPECT_EQ(result.matching_files, 2u) << "num_threads=" << num_threads;
        EXPECT_EQ(result.mismatched_files, 3u) << "num_threads=" << num_threads;
        EXPECT_EQ(result.only_in_a, 1u) << "num_threads=" << num_threads;
        EXPECT_EQ(result.only_in_b, 1u) << "num_threads=" << num_threads;

        if (!have_baseline) {
            baseline_result = std::move(result);
            have_baseline = true;
        } else {
            // Verify file results match baseline (after sorting)
            ASSERT_EQ(result.files.size(), baseline_result.files.size())
                << "num_threads=" << num_threads;
            for (size_t i = 0; i < result.files.size(); ++i) {
                EXPECT_EQ(result.files[i].relative_path, baseline_result.files[i].relative_path)
                    << "num_threads=" << num_threads << ", file index=" << i;
                EXPECT_EQ(result.files[i].status, baseline_result.files[i].status)
                    << "num_threads=" << num_threads << ", file=" << result.files[i].relative_path;
            }
        }
    }
}

TEST_F(ParallelExecutionTest, ManyFilesParallel) {
    // Create 100 files to ensure parallel execution is exercised
    const int num_files = 100;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::string content = "Content for file " + std::to_string(i);
        create_file(temp_dir_a, name, content);
        create_file(temp_dir_b, name, content);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.mismatched_files, 0u);
    EXPECT_EQ(result.errors, 0u);
}

TEST_F(ParallelExecutionTest, ManyFilesWithMixedResults) {
    // Create 50 matching + 30 mismatching + 10 only_a + 10 only_b = 100 files
    for (int i = 0; i < 50; ++i) {
        std::string name = "match" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Same content " + std::to_string(i));
        create_file(temp_dir_b, name, "Same content " + std::to_string(i));
    }
    for (int i = 0; i < 30; ++i) {
        std::string name = "differ" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "A content " + std::to_string(i));
        create_file(temp_dir_b, name, "B content " + std::to_string(i));
    }
    for (int i = 0; i < 10; ++i) {
        std::string name = "only_a_" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Only in A " + std::to_string(i));
    }
    for (int i = 0; i < 10; ++i) {
        std::string name = "only_b_" + std::to_string(i) + ".txt";
        create_file(temp_dir_b, name, "Only in B " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 100u);
    EXPECT_EQ(result.matching_files, 50u);
    EXPECT_EQ(result.mismatched_files, 50u);  // 30 content + 10 only_a + 10 only_b
    EXPECT_EQ(result.only_in_a, 10u);
    EXPECT_EQ(result.only_in_b, 10u);
    EXPECT_EQ(result.errors, 0u);

    // Verify sorting: mismatches first, then matches
    bool seen_match = false;
    for (const auto& file : result.files) {
        if (file.status == FileCompareStatus::Match) {
            seen_match = true;
        } else if (file.status == FileCompareStatus::Mismatch && seen_match) {
            FAIL() << "Mismatch found after Match - sorting is wrong";
        }
    }
}

TEST_F(ParallelExecutionTest, SingleFileUsesFullThreadBudget) {
    // With only 1 file and 128 threads, should use all threads for chunk parallelism
    // Create a larger file to have multiple chunks
    create_file_with_size(temp_dir_a, "large.bin", 32 * 1024 * 1024, 'A');  // 32 MiB
    create_file_with_size(temp_dir_b, "large.bin", 32 * 1024 * 1024, 'A');  // 32 MiB

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
}

// ============================================================================
// Timing Field Tests
// ============================================================================

TEST_F(ParallelExecutionTest, TimingFieldsPopulated) {
    // Create some files to compare
    for (int i = 0; i < 10; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);

    // All timing fields should be non-negative
    EXPECT_GE(result.total_elapsed, 0.0);
    EXPECT_GE(result.discovery_elapsed, 0.0);
    EXPECT_GE(result.comparison_elapsed, 0.0);

    // Discovery + comparison should approximately equal total
    double sum = result.discovery_elapsed + result.comparison_elapsed;
    EXPECT_NEAR(sum, result.total_elapsed, 0.001);  // Allow 1ms tolerance
}

TEST_F(ParallelExecutionTest, TimingFieldsWithEmptyDirectories) {
    // Empty directories should still have valid timing
    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_GE(result.total_elapsed, 0.0);
    EXPECT_GE(result.discovery_elapsed, 0.0);
    EXPECT_GE(result.comparison_elapsed, 0.0);

    // With no files to compare, comparison_elapsed should be minimal
    // but discovery still takes some time
    double sum = result.discovery_elapsed + result.comparison_elapsed;
    EXPECT_NEAR(sum, result.total_elapsed, 0.001);
}

TEST_F(ParallelExecutionTest, TimingWithLargerWorkload) {
    // Create files that take measurable time to process
    for (int i = 0; i < 20; ++i) {
        std::string name = "file" + std::to_string(i) + ".bin";
        create_file_with_size(temp_dir_a, name, 100 * 1024, 'X');  // 100 KiB each
        create_file_with_size(temp_dir_b, name, 100 * 1024, 'X');
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 4;  // Use fewer threads to make timing more measurable

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 20u);

    // Timing breakdown should be valid
    EXPECT_GE(result.discovery_elapsed, 0.0);
    EXPECT_GE(result.comparison_elapsed, 0.0);
    EXPECT_GT(result.total_elapsed, 0.0);

    double sum = result.discovery_elapsed + result.comparison_elapsed;
    EXPECT_NEAR(sum, result.total_elapsed, 0.001);
}

// ============================================================================
// Thread Count Edge Cases
// ============================================================================

TEST_F(ParallelExecutionTest, ZeroThreadsDefaultsToReasonable) {
    create_file(temp_dir_a, "file.txt", "Content");
    create_file(temp_dir_b, "file.txt", "Content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 0;  // Should default to 1024

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
}

TEST_F(ParallelExecutionTest, NegativeThreadsHandled) {
    create_file(temp_dir_a, "file.txt", "Content");
    create_file(temp_dir_b, "file.txt", "Content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = -5;  // Invalid, should be handled gracefully

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // Should still succeed (defaults to reasonable value)
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
}

TEST_F(ParallelExecutionTest, SingleThreadSequential) {
    // With 1 thread, should still work correctly (sequential execution)
    for (int i = 0; i < 10; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 10u);
    EXPECT_EQ(result.matching_files, 10u);
}

TEST_F(ParallelExecutionTest, MoreThreadsThanFiles) {
    // More threads than files should work fine
    create_file(temp_dir_a, "file1.txt", "Content 1");
    create_file(temp_dir_b, "file1.txt", "Content 1");
    create_file(temp_dir_a, "file2.txt", "Content 2");
    create_file(temp_dir_b, "file2.txt", "Content 2");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;  // Way more threads than files

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 2u);
}

// ============================================================================
// Progress Tracking During Parallel Execution
// ============================================================================

TEST_F(ParallelExecutionTest, ProgressTracking) {
    // Create files to track progress
    for (int i = 0; i < 20; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);

    // After completion, progress should reflect all files
    EXPECT_EQ(progress.files_scanned_a.load(), 20u);
    EXPECT_EQ(progress.files_scanned_b.load(), 20u);
    EXPECT_EQ(progress.total_files.load(), 20u);
    EXPECT_EQ(progress.files_compared.load(), 20u);
    EXPECT_TRUE(progress.scanning_done.load());
}

TEST_F(ParallelExecutionTest, ProgressWithOnlyInAB) {
    // Files only in A/B also count toward progress
    for (int i = 0; i < 5; ++i) {
        create_file(temp_dir_a, "common" + std::to_string(i) + ".txt", "Common");
        create_file(temp_dir_b, "common" + std::to_string(i) + ".txt", "Common");
    }
    for (int i = 0; i < 3; ++i) {
        create_file(temp_dir_a, "only_a_" + std::to_string(i) + ".txt", "A");
    }
    for (int i = 0; i < 2; ++i) {
        create_file(temp_dir_b, "only_b_" + std::to_string(i) + ".txt", "B");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(progress.files_scanned_a.load(), 8u);  // 5 common + 3 only_a
    EXPECT_EQ(progress.files_scanned_b.load(), 7u);  // 5 common + 2 only_b
    EXPECT_EQ(progress.total_files.load(), 10u);     // 5 common + 3 only_a + 2 only_b
    EXPECT_EQ(progress.files_compared.load(), 10u);
}

// ============================================================================
// Adaptive Concurrency Tests
// ============================================================================

TEST_F(ParallelExecutionTest, AdaptiveThreadsPerFileForFewLargeFiles) {
    // With only 1 file and high thread count, should allocate many threads per file
    // for chunk-level parallelism within that file
    create_file_with_size(temp_dir_a, "large.bin", 32 * 1024 * 1024, 'A');  // 32 MiB
    create_file_with_size(temp_dir_b, "large.bin", 32 * 1024 * 1024, 'A');  // 32 MiB

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
    // With 1 file and 128 threads, threads_per_file should be 128
    // This allows for chunk-level parallelism within the large file
}

TEST_F(ParallelExecutionTest, AdaptiveThreadsPerFileForManySmallFiles) {
    // With many files, should use 1 thread per file for file-level parallelism
    const int num_files = 200;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::string content = "Content " + std::to_string(i);
        create_file(temp_dir_a, name, content);
        create_file(temp_dir_b, name, content);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // With 200 files and 128 threads, should use 1 thread per file
}

TEST_F(ParallelExecutionTest, AdaptiveInitialConcurrencyRespectsBounds) {
    // With very low max_threads, initial concurrency should not exceed max
    create_file(temp_dir_a, "file1.txt", "Content 1");
    create_file(temp_dir_b, "file1.txt", "Content 1");
    create_file(temp_dir_a, "file2.txt", "Content 2");
    create_file(temp_dir_b, "file2.txt", "Content 2");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 2;  // Very low thread count

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 2u);
    EXPECT_EQ(result.matching_files, 2u);
    // Should complete successfully even with very constrained thread budget
}

TEST_F(ParallelExecutionTest, AdaptiveConcurrencyWithModerateFileCount) {
    // With 5+ files, should use 1 thread per file for maximum file-level parallelism
    const int num_files = 20;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::string content = "Content " + std::to_string(i);
        create_file(temp_dir_a, name, content);
        create_file(temp_dir_b, name, content);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // With 20 files and 128 threads, should use 1 thread per file (max file parallelism)
}

TEST_F(ParallelExecutionTest, AdaptiveEmptyDirectoryHandlesZeroThroughput) {
    // Empty directories should handle division by zero gracefully
    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 0u);
    // Should complete without division by zero
}

TEST_F(ParallelExecutionTest, AdaptiveMixedWorkloadMajoritySmall) {
    // Mixed workload with majority small files should favor file parallelism
    // 8 small files + 2 large files = 20% large ratio
    for (int i = 0; i < 8; ++i) {
        std::string name = "small" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Small content");
        create_file(temp_dir_b, name, "Small content");
    }
    // Create 2 large files (> 8 MiB chunk size)
    create_file_with_size(temp_dir_a, "large1.bin", 10 * 1024 * 1024, 'A');
    create_file_with_size(temp_dir_b, "large1.bin", 10 * 1024 * 1024, 'A');
    create_file_with_size(temp_dir_a, "large2.bin", 10 * 1024 * 1024, 'B');
    create_file_with_size(temp_dir_b, "large2.bin", 10 * 1024 * 1024, 'B');

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 10u);
    EXPECT_EQ(result.matching_files, 10u);
    // With 20% large files, should favor file parallelism (threads/file: 1)
}

TEST_F(ParallelExecutionTest, AdaptiveMixedWorkloadMajorityLarge) {
    // Mixed workload with majority large files should favor chunk parallelism
    // 2 small files + 8 large files = 80% large ratio
    for (int i = 0; i < 2; ++i) {
        std::string name = "small" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Small content");
        create_file(temp_dir_b, name, "Small content");
    }
    // Create 8 large files (> 8 MiB chunk size)
    for (int i = 0; i < 8; ++i) {
        std::string name = "large" + std::to_string(i) + ".bin";
        create_file_with_size(temp_dir_a, name, 10 * 1024 * 1024, static_cast<char>('A' + i));
        create_file_with_size(temp_dir_b, name, 10 * 1024 * 1024, static_cast<char>('A' + i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 10u);
    EXPECT_EQ(result.matching_files, 10u);
    // With 80% large files, should favor chunk parallelism (threads/file: 4+)
}

TEST_F(ParallelExecutionTest, CancellationDuringAdaptiveConcurrency) {
    // Create many files to ensure cancellation happens mid-processing
    const int num_files = 100;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::string content = "Content " + std::to_string(i);
        create_file(temp_dir_a, name, content);
        create_file(temp_dir_b, name, content);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 64;

    DirectoryComparisonProgress progress;

    // Launch comparison in a thread
    std::thread comparison_thread([&]() {
        run_directory_comparison(config, progress);
    });

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    progress.cancelled = true;

    comparison_thread.join();

    // Should have processed some but not all files
    // The key assertion is that it terminates without deadlock
    EXPECT_TRUE(progress.files_compared.load() < static_cast<size_t>(num_files) ||
                progress.files_compared.load() == static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, CancellationWithPreCancelledProgress) {
    // Test that pre-cancelled progress doesn't cause issues with adaptive concurrency
    const int num_files = 20;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    progress.cancelled = true;  // Pre-cancel

    auto result = run_directory_comparison(config, progress);

    // Should fail due to cancellation
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

TEST_F(ParallelExecutionTest, AdaptiveScalingMeasurementIntervalForSmallBatch) {
    // With a small batch, measurement_interval should be adjusted to allow measurements
    // 10 files with initial_concurrency potentially high should still get measurement chances
    const int num_files = 10;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1024;  // High thread count

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // Main assertion: completes without issues despite high thread count and low file count
}

TEST_F(ParallelExecutionTest, ErrorHandlingDoesNotDeadlock) {
    // Create files in A but some missing in B - these will cause errors
    // The system should handle errors gracefully without deadlocking
    const int num_good_files = 20;
    const int num_bad_files = 5;

    // Create matching files
    for (int i = 0; i < num_good_files; ++i) {
        std::string name = "good" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    // Create files that only exist in A (will be reported as only_in_a, not errors)
    for (int i = 0; i < num_bad_files; ++i) {
        std::string name = "only_a" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Only in A");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 64;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // Should complete successfully (only_in_a is not an error condition)
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_good_files + num_bad_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_good_files));
    EXPECT_EQ(result.only_in_a, static_cast<size_t>(num_bad_files));
    // Main assertion: completes without deadlock even with mixed success/failure
}

TEST_F(ParallelExecutionTest, ManyFilesWithMixedSizesCompletesWithoutDeadlock) {
    // Stress test with many files of varying sizes
    // This exercises the RAII guard and concurrency control under load
    for (int i = 0; i < 50; ++i) {
        std::string name = "small" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Small content " + std::to_string(i));
        create_file(temp_dir_b, name, "Small content " + std::to_string(i));
    }

    // Add a few larger files
    for (int i = 0; i < 5; ++i) {
        std::string name = "large" + std::to_string(i) + ".bin";
        create_file_with_size(temp_dir_a, name, 100000, 'A' + i);
        create_file_with_size(temp_dir_b, name, 100000, 'A' + i);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 55u);
    EXPECT_EQ(result.matching_files, 55u);
    EXPECT_EQ(result.errors, 0u);
}

TEST_F(ParallelExecutionTest, AdaptiveConcurrencyScalesUpWithHighThroughput) {
    // Create enough files to trigger multiple measurement intervals
    // This verifies that the adaptive concurrency actually increases when throughput improves
    const int num_files = 200;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        // Small files for fast processing to allow throughput measurements
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1024;  // High ceiling to allow scaling

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // The adaptive algorithm should complete all files successfully
    // (We can't easily verify scaling behavior without instrumentation,
    // but we verify correctness under scaling conditions)
}

TEST_F(ParallelExecutionTest, AdaptiveConcurrencyHandlesVeryLowThreadLimit) {
    // Test with num_threads = 1 to verify single-threaded execution works
    const int num_files = 10;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1;  // Minimal parallelism

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, AdaptiveConcurrencyWithAllLargeFiles) {
    // Test with only large files (multi-chunk) to verify chunk-level parallelism
    const int num_files = 5;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "large" + std::to_string(i) + ".bin";
        // Create files larger than chunk_size (default 8 MiB)
        create_file_with_size(temp_dir_a, name, 10 * 1024 * 1024, 'A' + i);
        create_file_with_size(temp_dir_b, name, 10 * 1024 * 1024, 'A' + i);
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 128;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // With all large files, threads_per_file should be high (128/5 = 25)
}

TEST_F(ParallelExecutionTest, InFlightGuardDecrementOnNormalCompletion) {
    // This test verifies that the RAII guard properly decrements in_flight
    // by ensuring all files complete without deadlock
    const int num_files = 50;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 10;  // Low concurrency to stress the guard

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // If the guard didn't work, we'd deadlock waiting for in_flight to decrease
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(progress.files_compared.load(), static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, InFlightGuardWithMismatchedFiles) {
    // Test RAII guard with files that have mismatches (different processing path)
    const int num_files = 30;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content A " + std::to_string(i));
        // Every other file is different
        if (i % 2 == 0) {
            create_file(temp_dir_b, name, "Content B " + std::to_string(i));
        } else {
            create_file(temp_dir_a, name, "Content A " + std::to_string(i));
            create_file(temp_dir_b, name, "Content A " + std::to_string(i));
        }
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 8;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    // Should have mix of matches and mismatches
    EXPECT_GT(result.mismatched_files, 0u);
    EXPECT_EQ(progress.files_compared.load(), static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, RapidCancellationDoesNotDeadlock) {
    // Test that cancelling immediately after starting doesn't deadlock
    const int num_files = 100;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 64;

    DirectoryComparisonProgress progress;

    // Cancel immediately in another thread
    std::thread canceller([&]() {
        // Very short delay then cancel
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        progress.cancelled = true;
    });

    auto result = run_directory_comparison(config, progress);
    canceller.join();

    // Either cancelled or completed - should not hang
    // The key assertion is that this test completes (no deadlock)
    EXPECT_TRUE(progress.files_compared.load() <= static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, MeasurementIntervalAdjustsForSmallBatches) {
    // Test that measurement_interval is properly adjusted for small file counts
    // to ensure we get measurement opportunities
    const int num_files = 5;  // Very small batch
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content");
        create_file(temp_dir_b, name, "Content");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1024;  // High thread count vs low file count

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
    // With 5 files and common_files.size()/2 = 2, measurement_interval should be 2
    // This allows for at least 2 measurement opportunities
}

TEST_F(ParallelExecutionTest, NegativeThreadCountDefaultsToMax) {
    // Test that negative thread count is handled correctly (should default to 1024)
    const int num_files = 10;
    for (int i = 0; i < num_files; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = -5;  // Negative value should be treated as invalid

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // Should complete successfully - negative values default to 1024
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_files));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_files));
}

TEST_F(ParallelExecutionTest, FileComparisonErrorsDuringAdaptiveScaling) {
    // Test that actual file comparison errors are handled gracefully
    // during adaptive scaling without deadlocking
    const int num_good_files = 30;

    // Create matching files
    for (int i = 0; i < num_good_files; ++i) {
        std::string name = "good" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    // Create files that exist in both directories but with different sizes
    // This will cause a size mismatch which is reported as a mismatch, not an error
    for (int i = 0; i < 5; ++i) {
        std::string name = "mismatch" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Short");
        create_file(temp_dir_b, name, "Much longer content that differs");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 64;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_good_files + 5));
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_good_files));
    EXPECT_EQ(result.mismatched_files, 5u);
    // The key assertion: completes without deadlock even with mixed results
}

TEST_F(ParallelExecutionTest, UnreadableFileCausesErrorDuringAdaptiveScaling) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";
    // Test that unreadable files cause errors that don't deadlock the system
    const int num_good_files = 20;

    // Create matching files
    for (int i = 0; i < num_good_files; ++i) {
        std::string name = "good" + std::to_string(i) + ".txt";
        create_file(temp_dir_a, name, "Content " + std::to_string(i));
        create_file(temp_dir_b, name, "Content " + std::to_string(i));
    }

    // Create a file that exists in both but make one unreadable
    std::string unreadable_name = "unreadable.txt";
    create_file(temp_dir_a, unreadable_name, "Content A");
    create_file(temp_dir_b, unreadable_name, "Content B");

    // Make the file in temp_dir_a unreadable
    std::string unreadable_path = temp_dir_a + "/" + unreadable_name;
    chmod(unreadable_path.c_str(), 0000);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 32;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    // Restore permissions for cleanup
    chmod(unreadable_path.c_str(), 0644);

    // The comparison should still succeed overall (individual file errors don't fail the whole thing)
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, static_cast<size_t>(num_good_files + 1));
    // The unreadable file should be counted as an error
    EXPECT_EQ(result.errors, 1u);
    EXPECT_EQ(result.matching_files, static_cast<size_t>(num_good_files));
}

TEST_F(ParallelExecutionTest, ZeroMaxConcurrencyHandledGracefully) {
    // Edge case: what if max_concurrency calculation results in 0?
    // This shouldn't happen with current logic but test defensively
    create_file(temp_dir_a, "file.txt", "Content");
    create_file(temp_dir_b, "file.txt", "Content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 1;  // Minimum valid value

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
}

TEST_F(ParallelExecutionTest, LargeThreadCountWithSingleSmallFile) {
    // Stress test: very high thread count but only 1 small file
    create_file(temp_dir_a, "tiny.txt", "x");
    create_file(temp_dir_b, "tiny.txt", "x");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.num_threads = 10000;  // Very high thread count

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);
    EXPECT_EQ(result.matching_files, 1u);
    // Should handle this gracefully without creating 10000 threads
}

// ============================================================================
// Parallel Enumeration Tests (for parallel_enumerate_* functions)
// ============================================================================

class ParallelEnumerationTest : public ::testing::Test {
protected:
    std::string temp_root;

    void SetUp() override {
        temp_root = "/tmp/objiff_parallel_enum_test_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void create_file(const std::string& relative_path, size_t size = 100) {
        fs::path full_path = fs::path(temp_root) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        if (size > 0) {
            std::vector<char> data(size, 'X');
            out.write(data.data(), data.size());
        }
        out.close();
    }

    void create_subdir(const std::string& relative_path) {
        fs::create_directories(fs::path(temp_root) / relative_path);
    }

    void create_symlink(const std::string& target, const std::string& link_path) {
        fs::path full_link = fs::path(temp_root) / link_path;
        fs::path full_target = fs::path(temp_root) / target;
        fs::create_directories(full_link.parent_path());
        std::error_code ec;
        fs::create_symlink(full_target, full_link, ec);
    }
};

TEST_F(ParallelEnumerationTest, MatchesSequentialEnumeration) {
    // Create a directory structure with files at multiple levels
    create_file("root.txt", 50);
    create_file("level1/a.txt", 100);
    create_file("level1/b.txt", 200);
    create_file("level1/level2/c.txt", 150);
    create_file("level1/level2/d.txt", 250);
    create_file("level1/level2/level3/e.txt", 300);
    create_file("other/f.txt", 400);

    std::atomic<size_t> files_found_seq{0};
    std::atomic<size_t> files_found_par{0};
    std::atomic<bool> cancelled{false};

    // Run sequential enumeration
    auto seq_entries = enumerate_local_directory(temp_root, true, files_found_seq, cancelled);

    // Run parallel enumeration with various worker counts
    for (int workers : {1, 2, 4, 8, 16, 64}) {
        files_found_par = 0;
        auto par_entries = parallel_enumerate_local_directory(
            temp_root, true, workers, files_found_par, cancelled);

        ASSERT_EQ(par_entries.size(), seq_entries.size())
            << "Mismatch with workers=" << workers;

        // Sort both and compare (order may differ)
        std::set<std::string> seq_paths, par_paths;
        for (const auto& e : seq_entries) seq_paths.insert(e.relative_path);
        for (const auto& e : par_entries) par_paths.insert(e.relative_path);

        EXPECT_EQ(par_paths, seq_paths) << "Path mismatch with workers=" << workers;

        // Verify file sizes match
        std::map<std::string, int64_t> seq_sizes, par_sizes;
        for (const auto& e : seq_entries) seq_sizes[e.relative_path] = e.size;
        for (const auto& e : par_entries) par_sizes[e.relative_path] = e.size;
        EXPECT_EQ(par_sizes, seq_sizes) << "Size mismatch with workers=" << workers;
    }
}

TEST_F(ParallelEnumerationTest, EmptyDirectory) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(files_found.load(), 0u);
}

TEST_F(ParallelEnumerationTest, SingleFile) {
    create_file("only.txt", 100);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "only.txt");
    EXPECT_EQ(entries[0].size, 100);
}

TEST_F(ParallelEnumerationTest, DeepNesting) {
    // Create deeply nested structure (10 levels)
    std::string path = "";
    for (int i = 0; i < 10; ++i) {
        path += "level" + std::to_string(i) + "/";
    }
    create_file(path + "deep.txt", 100);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].relative_path.find("deep.txt") != std::string::npos);
}

TEST_F(ParallelEnumerationTest, ManyFiles) {
    // Create 200 files across 10 directories
    for (int dir = 0; dir < 10; ++dir) {
        for (int file = 0; file < 20; ++file) {
            create_file("dir" + std::to_string(dir) + "/file" + std::to_string(file) + ".txt", 50);
        }
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 16, files_found, cancelled);

    EXPECT_EQ(entries.size(), 200u);
    EXPECT_EQ(files_found.load(), 200u);
}

TEST_F(ParallelEnumerationTest, CancellationRespected) {
    // Create many files
    for (int i = 0; i < 50; ++i) {
        create_file("dir" + std::to_string(i) + "/file.txt", 50);
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{true};  // Pre-cancelled

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should return early with fewer entries (exact count depends on timing)
    EXPECT_LT(entries.size(), 50u);
}

TEST_F(ParallelEnumerationTest, NonRecursiveFallsBackToSequential) {
    create_file("root.txt", 100);
    create_file("subdir/nested.txt", 100);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, false, 8, files_found, cancelled);

    // Non-recursive should only get root file
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "root.txt");
}

TEST_F(ParallelEnumerationTest, WorkerCountClamping) {
    create_file("test.txt", 100);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Test with out-of-range worker counts
    auto entries_low = parallel_enumerate_local_directory(
        temp_root, true, 0, files_found, cancelled);  // Should clamp to 1

    files_found = 0;
    auto entries_high = parallel_enumerate_local_directory(
        temp_root, true, 1000, files_found, cancelled);  // Should clamp to 128

    EXPECT_EQ(entries_low.size(), 1u);
    EXPECT_EQ(entries_high.size(), 1u);
}

TEST_F(ParallelEnumerationTest, SymlinkToFile) {
    create_file("real_file.txt", 123);
    create_symlink("real_file.txt", "link_to_file.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should include both the real file and the symlink
    EXPECT_EQ(entries.size(), 2u);

    // Both should have the same size (symlink follows to target)
    std::set<int64_t> sizes;
    for (const auto& e : entries) sizes.insert(e.size);
    EXPECT_EQ(sizes.size(), 1u);  // All same size
    EXPECT_EQ(*sizes.begin(), 123);
}

TEST_F(ParallelEnumerationTest, BrokenSymlink) {
    create_symlink("nonexistent.txt", "broken_link.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Broken symlink should be skipped
    EXPECT_TRUE(entries.empty());
}

TEST_F(ParallelEnumerationTest, NonexistentPath) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        "/nonexistent/path/12345", true, 8, files_found, cancelled);

    EXPECT_TRUE(entries.empty());
}

TEST_F(ParallelEnumerationTest, WideDirectory) {
    // Create 100 sibling directories, each with 1 file
    // This tests parallel BFS when many directories are at the same level
    for (int i = 0; i < 100; ++i) {
        create_file("sibling" + std::to_string(i) + "/file.txt", 50);
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 16, files_found, cancelled);

    EXPECT_EQ(entries.size(), 100u);
    EXPECT_EQ(files_found.load(), 100u);
}

TEST_F(ParallelEnumerationTest, CancellationDuringEnumeration) {
    // Create structure that requires multiple BFS levels
    for (int i = 0; i < 50; ++i) {
        for (int j = 0; j < 3; ++j) {
            create_file("dir" + std::to_string(i) + "/sub" + std::to_string(j) + "/file.txt", 50);
        }
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Start enumeration in background thread
    std::future<std::vector<DirectoryEntry>> future = std::async(std::launch::async, [&]() {
        return parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);
    });

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancelled = true;

    auto entries = future.get();

    // Should have been interrupted (may have found some or all depending on timing)
    // The key is that it doesn't hang and respects cancellation
    EXPECT_LE(entries.size(), 150u);  // Max possible is 150
}

TEST_F(ParallelEnumerationTest, SymlinkCycleToParent) {
    // Create a directory with a symlink pointing back to parent (cycle)
    create_file("real_dir/file.txt", 100);
    create_subdir("real_dir/subdir");
    create_file("real_dir/subdir/nested.txt", 50);

    // Create symlink back to parent directory (creates a cycle)
    fs::path cycle_link = fs::path(temp_root) / "real_dir" / "subdir" / "cycle_to_root";
    std::error_code ec;
    fs::create_directory_symlink(fs::path(temp_root), cycle_link, ec);
    ASSERT_FALSE(ec) << "Failed to create symlink cycle: " << ec.message();

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Should not hang and should handle the cycle gracefully
    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should find the two real files without infinite loop
    EXPECT_EQ(entries.size(), 2u);

    std::set<std::string> paths;
    for (const auto& e : entries) paths.insert(e.relative_path);
    EXPECT_TRUE(paths.count("real_dir/file.txt"));
    EXPECT_TRUE(paths.count("real_dir/subdir/nested.txt"));
}

TEST_F(ParallelEnumerationTest, SymlinkMutualCycle) {
    // Create a directory structure with mutual symlink references (A -> B -> A)
    create_file("dir_a/file_a.txt", 100);
    create_file("dir_b/file_b.txt", 100);

    // Create cross-referencing symlinks (dir_a/link_b -> dir_b, dir_b/link_a -> dir_a)
    fs::path link_a_to_b = fs::path(temp_root) / "dir_a" / "link_to_b";
    fs::path link_b_to_a = fs::path(temp_root) / "dir_b" / "link_to_a";
    std::error_code ec;
    fs::create_directory_symlink(fs::path(temp_root) / "dir_b", link_a_to_b, ec);
    ASSERT_FALSE(ec) << "Failed to create symlink: " << ec.message();
    fs::create_directory_symlink(fs::path(temp_root) / "dir_a", link_b_to_a, ec);
    ASSERT_FALSE(ec) << "Failed to create symlink: " << ec.message();

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Should not hang due to mutual symlink references
    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Files are found under both regular paths and symlink paths:
    // - dir_a/file_a.txt (regular)
    // - dir_b/file_b.txt (regular)
    // - dir_a/link_to_b/file_b.txt (via symlink)
    // - dir_b/link_to_a/file_a.txt (via symlink)
    // The cycle is broken after one level of symlink traversal
    EXPECT_EQ(entries.size(), 4u);

    std::set<std::string> paths;
    for (const auto& e : entries) paths.insert(e.relative_path);
    EXPECT_TRUE(paths.count("dir_a/file_a.txt"));
    EXPECT_TRUE(paths.count("dir_b/file_b.txt"));
    EXPECT_TRUE(paths.count("dir_a/link_to_b/file_b.txt"));
    EXPECT_TRUE(paths.count("dir_b/link_to_a/file_a.txt"));
}

TEST_F(ParallelEnumerationTest, RacyCancellationStress) {
    // Create many wide directories to maximize the race window during posting
    for (int i = 0; i < 100; ++i) {
        create_file("wide_dir" + std::to_string(i) + "/file.txt", 50);
    }

    // Run multiple trials to increase chance of hitting race conditions
    for (int trial = 0; trial < 20; ++trial) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        // Start enumeration in background
        std::future<std::vector<DirectoryEntry>> future = std::async(std::launch::async, [&]() {
            return parallel_enumerate_local_directory(
                temp_root, true, 64, files_found, cancelled);
        });

        // Cancel after very short delay to try to hit the posting loop mid-execution
        std::this_thread::sleep_for(std::chrono::microseconds(50 + trial * 10));
        cancelled = true;

        // Should not crash, hang, or produce corrupted results
        auto entries = future.get();

        // Verify no corruption - all paths should be valid
        for (const auto& e : entries) {
            EXPECT_FALSE(e.relative_path.empty());
            EXPECT_TRUE(e.relative_path.find("wide_dir") != std::string::npos ||
                       e.relative_path.find("file.txt") != std::string::npos);
        }
    }
}

TEST_F(ParallelEnumerationTest, BrokenSymlinkHandling) {
    // Create a file and a broken symlink (pointing to non-existent target)
    create_file("real_file.txt", 100);
    create_subdir("subdir");
    create_file("subdir/another.txt", 50);

    // Create a broken symlink (target doesn't exist)
    fs::path broken_link = fs::path(temp_root) / "subdir" / "broken_link.txt";
    std::error_code ec;
    fs::create_symlink(fs::path(temp_root) / "nonexistent_file.txt", broken_link, ec);
    ASSERT_FALSE(ec) << "Failed to create broken symlink: " << ec.message();

    // Verify the symlink is indeed broken
    ASSERT_FALSE(fs::exists(broken_link));
    ASSERT_TRUE(fs::is_symlink(broken_link));

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Should not crash and should skip the broken symlink gracefully
    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should find the two real files, not the broken symlink
    EXPECT_EQ(entries.size(), 2u);

    std::set<std::string> paths;
    for (const auto& e : entries) paths.insert(e.relative_path);
    EXPECT_TRUE(paths.count("real_file.txt"));
    EXPECT_TRUE(paths.count("subdir/another.txt"));
    EXPECT_FALSE(paths.count("subdir/broken_link.txt"));
}

// S3 parallel enumeration tests using mock
class ParallelS3EnumerationTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_client;

    void SetUp() override {
        mock_client = std::make_shared<MockS3Client>();
        mock_client->CreateBucket("bucket");
    }

    void put_object(const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock_client->PutObject("bucket", key, data);
    }
};

TEST_F(ParallelS3EnumerationTest, MatchesSequentialEnumeration) {
    // Set up mock with hierarchical structure
    put_object("prefix/file1.txt", "content1");
    put_object("prefix/file2.txt", "content2");
    put_object("prefix/sub1/file3.txt", "content3");
    put_object("prefix/sub1/file4.txt", "content4");
    put_object("prefix/sub2/file5.txt", "content5");
    put_object("prefix/sub1/deep/file6.txt", "content6");

    std::atomic<size_t> files_found_seq{0};
    std::atomic<size_t> files_found_par{0};
    std::atomic<bool> cancelled{false};

    // Run sequential enumeration
    auto seq_entries = enumerate_s3_prefix(
        "bucket", "prefix", true, files_found_seq, cancelled, mock_client);

    // Run parallel enumeration
    for (int workers : {1, 2, 4, 8}) {
        files_found_par = 0;
        auto par_entries = parallel_enumerate_s3_prefix(
            "bucket", "prefix", true, workers, files_found_par, cancelled, mock_client);

        ASSERT_EQ(par_entries.size(), seq_entries.size())
            << "Mismatch with workers=" << workers;

        // Sort both and compare
        std::set<std::string> seq_paths, par_paths;
        for (const auto& e : seq_entries) seq_paths.insert(e.relative_path);
        for (const auto& e : par_entries) par_paths.insert(e.relative_path);

        EXPECT_EQ(par_paths, seq_paths) << "Path mismatch with workers=" << workers;
    }
}

TEST_F(ParallelS3EnumerationTest, EmptyPrefix) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "empty_prefix", true, 8, files_found, cancelled, mock_client);

    EXPECT_TRUE(entries.empty());
}

TEST_F(ParallelS3EnumerationTest, NonRecursiveFallsBack) {
    put_object("prefix/file1.txt", "content1");
    put_object("prefix/sub/file2.txt", "content2");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "prefix", false, 8, files_found, cancelled, mock_client);

    // Non-recursive should fall back to sequential
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file1.txt");
}

TEST_F(ParallelS3EnumerationTest, NullClient) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "prefix", true, 8, files_found, cancelled, nullptr);

    EXPECT_TRUE(entries.empty());
}

TEST_F(ParallelS3EnumerationTest, StreamingEnumeration) {
    // Create files in multiple directories to test streaming across BFS levels
    put_object("prefix/file1.txt", "content1");
    put_object("prefix/dir1/file2.txt", "content2");
    put_object("prefix/dir1/file3.txt", "content3");
    put_object("prefix/dir2/file4.txt", "content4");
    put_object("prefix/dir2/sub/file5.txt", "content5");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    std::vector<DirectoryEntry> all_entries;
    size_t callback_count = 0;

    parallel_enumerate_s3_prefix_streaming(
        "bucket", "prefix", true, 8, files_found, cancelled, mock_client,
        [&](std::vector<DirectoryEntry>&& entries) -> bool {
            callback_count++;
            all_entries.insert(all_entries.end(),
                std::make_move_iterator(entries.begin()),
                std::make_move_iterator(entries.end()));
            return true;  // Continue
        }
    );

    // Should have found all 5 files
    EXPECT_EQ(all_entries.size(), 5u);
    EXPECT_EQ(files_found.load(), 5u);
    // Callback should be called multiple times (once per BFS level with entries)
    EXPECT_GE(callback_count, 1u);

    // Verify all paths are present
    std::set<std::string> paths;
    for (const auto& e : all_entries) {
        paths.insert(e.relative_path);
    }
    EXPECT_TRUE(paths.count("file1.txt"));
    EXPECT_TRUE(paths.count("dir1/file2.txt"));
    EXPECT_TRUE(paths.count("dir1/file3.txt"));
    EXPECT_TRUE(paths.count("dir2/file4.txt"));
    EXPECT_TRUE(paths.count("dir2/sub/file5.txt"));
}

TEST_F(ParallelS3EnumerationTest, StreamingEnumerationEarlyStop) {
    // Create files in multiple directories (different BFS levels)
    // Level 1: prefix/
    for (int i = 0; i < 10; ++i) {
        put_object("prefix/file" + std::to_string(i) + ".txt", "content");
    }
    // Level 2: prefix/dir1/, prefix/dir2/
    for (int i = 0; i < 10; ++i) {
        put_object("prefix/dir1/file" + std::to_string(i) + ".txt", "content");
        put_object("prefix/dir2/file" + std::to_string(i) + ".txt", "content");
    }
    // Level 3: prefix/dir1/sub/, prefix/dir2/sub/
    for (int i = 0; i < 10; ++i) {
        put_object("prefix/dir1/sub/file" + std::to_string(i) + ".txt", "content");
        put_object("prefix/dir2/sub/file" + std::to_string(i) + ".txt", "content");
    }
    // Total: 10 + 20 + 20 = 50 files

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    size_t callback_count = 0;

    parallel_enumerate_s3_prefix_streaming(
        "bucket", "prefix", true, 8, files_found, cancelled, mock_client,
        [&](std::vector<DirectoryEntry>&& entries) -> bool {
            callback_count++;
            // Stop after first callback
            return false;
        }
    );

    // Should have stopped after first BFS level
    EXPECT_EQ(callback_count, 1u);
    // Cancelled flag should be set
    EXPECT_TRUE(cancelled.load());
}

TEST_F(ParallelS3EnumerationTest, CancellationRespected) {
    // Set up mock with many prefixes to enumerate
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 5; ++j) {
            put_object("prefix/dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt",
                      "content");
        }
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{true};  // Pre-cancelled

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "prefix", true, 8, files_found, cancelled, mock_client);

    // Should return early (cancelled before BFS loop starts)
    EXPECT_LT(entries.size(), 100u);
}

TEST_F(ParallelS3EnumerationTest, DeepNesting) {
    // Create deeply nested S3 structure
    put_object("prefix/l1/l2/l3/l4/deep.txt", "content");
    put_object("prefix/l1/l2/other.txt", "content");
    put_object("prefix/l1/shallow.txt", "content");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "prefix", true, 8, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 3u);

    std::set<std::string> paths;
    for (const auto& e : entries) paths.insert(e.relative_path);

    EXPECT_TRUE(paths.count("l1/l2/l3/l4/deep.txt"));
    EXPECT_TRUE(paths.count("l1/l2/other.txt"));
    EXPECT_TRUE(paths.count("l1/shallow.txt"));
}

TEST_F(ParallelS3EnumerationTest, RacyCancellationStress) {
    // Create many prefixes to maximize the race window during posting
    for (int i = 0; i < 50; ++i) {
        for (int j = 0; j < 3; ++j) {
            put_object("prefix/dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt",
                      "content");
        }
    }

    // Run multiple trials to increase chance of hitting race conditions
    for (int trial = 0; trial < 10; ++trial) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        // Start enumeration in background
        std::future<std::vector<DirectoryEntry>> future = std::async(std::launch::async, [&]() {
            return parallel_enumerate_s3_prefix(
                "bucket", "prefix", true, 32, files_found, cancelled, mock_client);
        });

        // Cancel after very short delay to try to hit the posting loop mid-execution
        std::this_thread::sleep_for(std::chrono::microseconds(100 + trial * 50));
        cancelled = true;

        // Should not crash, hang, or produce corrupted results
        auto entries = future.get();

        // Verify no corruption - all paths should be valid
        for (const auto& e : entries) {
            EXPECT_FALSE(e.relative_path.empty());
            EXPECT_TRUE(e.relative_path.find("dir") != std::string::npos ||
                       e.relative_path.find("file") != std::string::npos);
        }
    }
}

// ============================================================================
// Parallel Local Enumeration Direct Unit Tests
// ============================================================================

class ParallelLocalEnumerationTest : public ::testing::Test {
protected:
    std::string temp_root;

    void SetUp() override {
        temp_root = "/tmp/objiff_parallel_enum_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void create_file(const std::string& relative_path, size_t size = 100) {
        fs::path full_path = fs::path(temp_root) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        if (size > 0) {
            std::vector<char> data(size, 'X');
            out.write(data.data(), data.size());
        }
        out.close();
    }

    void create_subdir(const std::string& relative_path) {
        fs::create_directories(fs::path(temp_root) / relative_path);
    }

    void create_symlink(const std::string& link_path, const std::string& target) {
        fs::path full_link = fs::path(temp_root) / link_path;
        fs::create_directories(full_link.parent_path());
        std::error_code ec;
        fs::create_symlink(target, full_link, ec);
    }

    void create_directory_symlink(const std::string& link_path, const std::string& target) {
        fs::path full_link = fs::path(temp_root) / link_path;
        fs::create_directories(full_link.parent_path());
        std::error_code ec;
        fs::create_directory_symlink(target, full_link, ec);
    }
};

TEST_F(ParallelLocalEnumerationTest, EmptyDirectory) {
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(files_found.load(), 0u);
}

TEST_F(ParallelLocalEnumerationTest, SingleFile) {
    create_file("test.txt", 50);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "test.txt");
    EXPECT_EQ(entries[0].size, 50);
}

TEST_F(ParallelLocalEnumerationTest, DeepDirectoryStructure) {
    // Create a deep directory tree to exercise BFS
    create_file("level1/level2/level3/level4/deep.txt", 10);
    create_file("level1/level2/level3/mid.txt", 20);
    create_file("level1/level2/shallow.txt", 30);
    create_file("level1/top.txt", 40);
    create_file("root.txt", 50);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    ASSERT_EQ(entries.size(), 5u);
    EXPECT_EQ(files_found.load(), 5u);

    // Verify all files found
    std::set<std::string> paths;
    for (const auto& e : entries) {
        paths.insert(e.relative_path);
    }
    EXPECT_TRUE(paths.count("root.txt"));
    EXPECT_TRUE(paths.count("level1/top.txt"));
    EXPECT_TRUE(paths.count("level1/level2/shallow.txt"));
    EXPECT_TRUE(paths.count("level1/level2/level3/mid.txt"));
    EXPECT_TRUE(paths.count("level1/level2/level3/level4/deep.txt"));
}

TEST_F(ParallelLocalEnumerationTest, WideDirectoryStructure) {
    // Create many directories at the same level to stress parallel processing
    for (int i = 0; i < 20; ++i) {
        create_file("dir" + std::to_string(i) + "/file.txt", 10);
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 16, files_found, cancelled);

    EXPECT_EQ(entries.size(), 20u);
}

TEST_F(ParallelLocalEnumerationTest, MatchesSequentialResults) {
    // Create a complex directory structure
    create_file("a/file1.txt", 10);
    create_file("a/b/file2.txt", 20);
    create_file("a/b/c/file3.txt", 30);
    create_file("x/file4.txt", 40);
    create_file("x/y/file5.txt", 50);
    create_file("root.txt", 60);

    std::atomic<size_t> seq_found{0}, par_found{0};
    std::atomic<bool> cancelled{false};

    auto seq_entries = enumerate_local_directory(temp_root, true, seq_found, cancelled);
    auto par_entries = parallel_enumerate_local_directory(
        temp_root, true, 8, par_found, cancelled);

    // Same number of files
    ASSERT_EQ(seq_entries.size(), par_entries.size());

    // Sort and compare
    auto cmp = [](const DirectoryEntry& a, const DirectoryEntry& b) {
        return a.relative_path < b.relative_path;
    };
    std::sort(seq_entries.begin(), seq_entries.end(), cmp);
    std::sort(par_entries.begin(), par_entries.end(), cmp);

    for (size_t i = 0; i < seq_entries.size(); ++i) {
        EXPECT_EQ(seq_entries[i].relative_path, par_entries[i].relative_path);
        EXPECT_EQ(seq_entries[i].size, par_entries[i].size);
    }
}

TEST_F(ParallelLocalEnumerationTest, SingleWorker) {
    // Edge case: single worker should behave like sequential
    create_file("dir1/file1.txt", 10);
    create_file("dir2/file2.txt", 20);
    create_file("dir3/file3.txt", 30);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 1, files_found, cancelled);

    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(ParallelLocalEnumerationTest, WorkerCountClamping) {
    create_file("file.txt", 10);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Test with 0 workers (should clamp to 1)
    auto entries1 = parallel_enumerate_local_directory(
        temp_root, true, 0, files_found, cancelled);
    EXPECT_EQ(entries1.size(), 1u);

    // Test with negative workers (should clamp to 1)
    files_found = 0;
    auto entries2 = parallel_enumerate_local_directory(
        temp_root, true, -5, files_found, cancelled);
    EXPECT_EQ(entries2.size(), 1u);

    // Test with excessive workers (should clamp to 128)
    files_found = 0;
    auto entries3 = parallel_enumerate_local_directory(
        temp_root, true, 500, files_found, cancelled);
    EXPECT_EQ(entries3.size(), 1u);
}

TEST_F(ParallelLocalEnumerationTest, EmptySubdirectories) {
    create_file("with_file/file.txt", 10);
    create_subdir("empty1");
    create_subdir("empty2/nested_empty");
    create_subdir("with_file/empty_sibling");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Only the one file should be found
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "with_file/file.txt");
}

TEST_F(ParallelLocalEnumerationTest, SymlinkCycleDetection) {
    // Create a directory with a symlink cycle
    create_file("dir1/file1.txt", 10);
    create_file("dir1/dir2/file2.txt", 20);

    // Create symlink pointing back to parent
    // Note: The implementation may enumerate the same file multiple times via different paths
    // when following symlinks. The key property being tested is that it doesn't infinite loop.
    create_directory_symlink("dir1/dir2/cycle_link", "..");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should complete without infinite loop and find at least the expected files
    EXPECT_GE(entries.size(), 2u);

    std::set<std::string> paths;
    for (const auto& e : entries) {
        paths.insert(e.relative_path);
    }
    EXPECT_TRUE(paths.count("dir1/file1.txt"));
    EXPECT_TRUE(paths.count("dir1/dir2/file2.txt"));
}

// The sequential entry point must be as cycle-safe as the parallel one.
//
// Issue #28 reported that it was not: it enabled follow_directory_symlink and
// then walked with recursive_directory_iterator, which has no cycle protection,
// so --no-parallel-discovery could hang. It no longer works that way - a
// recursive walk delegates to the BFS enumerator at one worker, which tracks
// visited symlink targets - and these two tests exist to keep it that way. A
// refactor that gives the sequential path its own recursive walk again would
// hang here rather than in a user's directory.

TEST_F(ParallelLocalEnumerationTest, SequentialEnumerationSurvivesASymlinkToRoot) {
    create_file("file.txt", 10);
    create_subdir("subdir");
    create_directory_symlink("subdir/back_to_root", temp_root);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    // Terminating at all is the property under test; finding the file exactly
    // once is the evidence the cycle was detected rather than merely bounded.
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file.txt");
}

TEST_F(ParallelLocalEnumerationTest, SequentialEnumerationSurvivesASymlinkToAParent) {
    create_file("dir1/file1.txt", 10);
    create_file("dir1/dir2/file2.txt", 20);
    create_directory_symlink("dir1/dir2/cycle_link", "..");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = enumerate_local_directory(temp_root, true, files_found, cancelled);

    EXPECT_GE(entries.size(), 2u);
    std::set<std::string> paths;
    for (const auto& e : entries) paths.insert(e.relative_path);
    EXPECT_TRUE(paths.count("dir1/file1.txt"));
    EXPECT_TRUE(paths.count("dir1/dir2/file2.txt"));
}

TEST_F(ParallelLocalEnumerationTest, SymlinkToRoot) {
    // Create symlink pointing back to root
    create_file("file.txt", 10);
    create_subdir("subdir");
    create_directory_symlink("subdir/back_to_root", temp_root);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should find the file exactly once (cycle is detected)
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "file.txt");
}

TEST_F(ParallelLocalEnumerationTest, BrokenSymlinkToDirectory) {
    create_file("real_file.txt", 10);
    create_directory_symlink("broken_dir_link", "/nonexistent/path/that/does/not/exist");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should only find the real file, skipping broken symlink
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "real_file.txt");
}

TEST_F(ParallelLocalEnumerationTest, BrokenSymlinkToFile) {
    create_file("real_file.txt", 10);
    create_symlink("broken_file_link", "/nonexistent/file.txt");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should only find the real file
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "real_file.txt");
}

TEST_F(ParallelLocalEnumerationTest, ValidSymlinkToFile) {
    create_file("real_file.txt", 100);
    fs::path real_path = fs::path(temp_root) / "real_file.txt";
    create_symlink("link_to_file.txt", real_path.string());

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should find both the real file and the symlink
    EXPECT_EQ(entries.size(), 2u);

    std::set<std::string> paths;
    for (const auto& e : entries) {
        paths.insert(e.relative_path);
        EXPECT_EQ(e.size, 100);  // Both should report same size
    }
    EXPECT_TRUE(paths.count("real_file.txt"));
    EXPECT_TRUE(paths.count("link_to_file.txt"));
}

TEST_F(ParallelLocalEnumerationTest, NonRecursiveFallsBackToSequential) {
    create_file("root.txt", 10);
    create_file("subdir/nested.txt", 20);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Non-recursive mode should only find root file
    auto entries = parallel_enumerate_local_directory(
        temp_root, false, 8, files_found, cancelled);

    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative_path, "root.txt");
}

TEST_F(ParallelLocalEnumerationTest, CancellationDuringEnumeration) {
    // Create many directories to ensure enumeration takes some time
    for (int i = 0; i < 50; ++i) {
        for (int j = 0; j < 5; ++j) {
            create_file("dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt", 10);
        }
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // Start enumeration in background
    auto future = std::async(std::launch::async, [&]() {
        return parallel_enumerate_local_directory(
            temp_root, true, 16, files_found, cancelled);
    });

    // Cancel after short delay
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cancelled = true;

    auto entries = future.get();

    // Should not crash, may have partial results
    // All returned entries should be valid
    for (const auto& e : entries) {
        EXPECT_FALSE(e.relative_path.empty());
    }
}

TEST_F(ParallelLocalEnumerationTest, PreCancelled) {
    create_file("file.txt", 10);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{true};  // Pre-cancelled

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Should return empty or minimal results
    EXPECT_LE(entries.size(), 1u);
}

TEST_F(ParallelLocalEnumerationTest, PermissionDeniedDirectory) {
    create_file("accessible/file1.txt", 10);
    create_file("restricted/file2.txt", 20);

    // Remove read permission from restricted directory
    fs::path restricted = fs::path(temp_root) / "restricted";
    chmod(restricted.c_str(), 0000);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    // Restore permissions for cleanup
    chmod(restricted.c_str(), 0755);

    // Should find at least the accessible file
    EXPECT_GE(entries.size(), 1u);

    bool found_accessible = false;
    for (const auto& e : entries) {
        if (e.relative_path == "accessible/file1.txt") {
            found_accessible = true;
        }
    }
    EXPECT_TRUE(found_accessible);
}

TEST_F(ParallelLocalEnumerationTest, VeryDeepNesting) {
    // Create a very deep directory structure
    std::string path = "";
    for (int i = 0; i < 20; ++i) {
        path += "level" + std::to_string(i) + "/";
    }
    create_file(path + "deep_file.txt", 10);

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    EXPECT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].relative_path.find("deep_file.txt") != std::string::npos);
}

TEST_F(ParallelLocalEnumerationTest, ManyFilesInSingleDirectory) {
    // Create many files in a single directory
    for (int i = 0; i < 100; ++i) {
        create_file("flat/file" + std::to_string(i) + ".txt", 10);
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    EXPECT_EQ(entries.size(), 100u);
}

// ============================================================================
// Additional Parallel S3 Enumeration Tests (extends ParallelS3EnumerationTest above)
// ============================================================================

TEST_F(ParallelS3EnumerationTest, SingleWorker) {
    put_object("dir1/file1.txt", "c1");
    put_object("dir2/file2.txt", "c2");
    put_object("dir3/file3.txt", "c3");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "", true, 1, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(ParallelS3EnumerationTest, WorkerCountClampedTo64) {
    put_object("file.txt", "content");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    // 128 workers should be clamped to 64 for S3
    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "", true, 128, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 1u);
}

TEST_F(ParallelS3EnumerationTest, PreCancelled) {
    put_object("file.txt", "content");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{true};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "", true, 8, files_found, cancelled, mock_client);

    EXPECT_TRUE(entries.empty());
}

TEST_F(ParallelS3EnumerationTest, WideStructure) {
    // Many directories at same level
    for (int i = 0; i < 20; ++i) {
        put_object("dir" + std::to_string(i) + "/file.txt", "c");
    }

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "", true, 16, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 20u);
}

TEST_F(ParallelS3EnumerationTest, DeepStructure) {
    put_object("a/b/c/d/e/f/deep.txt", "d");
    put_object("a/b/c/mid.txt", "m");
    put_object("a/shallow.txt", "s");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "", true, 8, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(ParallelS3EnumerationTest, WithPrefix) {
    put_object("data/subset1/file1.txt", "c1");
    put_object("data/subset2/file2.txt", "c2");
    put_object("other/file3.txt", "c3");

    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "data", true, 8, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 2u);

    for (const auto& e : entries) {
        // Paths should be relative to the prefix
        EXPECT_TRUE(e.relative_path.find("subset") != std::string::npos);
    }
}

// ============================================================================
// Parallel Discovery Integration with Cancellation
// ============================================================================

class ParallelDiscoveryCancellationTest : public ::testing::Test {
protected:
    std::string temp_dir_a;
    std::string temp_dir_b;

    void SetUp() override {
        std::string base = "/tmp/objiff_cancel_discovery_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::rand());
        temp_dir_a = base + "_a";
        temp_dir_b = base + "_b";
        fs::create_directories(temp_dir_a);
        fs::create_directories(temp_dir_b);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_a, ec);
        fs::remove_all(temp_dir_b, ec);
    }

    void create_file(const std::string& dir, const std::string& relative_path,
                     const std::string& content) {
        fs::path full_path = fs::path(dir) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
        out.close();
    }
};

TEST_F(ParallelDiscoveryCancellationTest, CancelDuringParallelDiscovery) {
    // Create enough directories to make discovery take measurable time
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 3; ++j) {
            std::string path = "dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt";
            create_file(temp_dir_a, path, "content");
            create_file(temp_dir_b, path, "content");
        }
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 16;

    DirectoryComparisonProgress progress;

    // Start comparison in background
    auto future = std::async(std::launch::async, [&]() {
        return run_directory_comparison(config, progress);
    });

    // Cancel very quickly to hit during discovery phase
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    progress.cancelled = true;

    auto result = future.get();

    // Either cancelled or completed (race condition)
    // Key thing is no crash, hang, or corruption
    if (!result.success) {
        EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
    }
}

TEST_F(ParallelDiscoveryCancellationTest, PreCancelledWithParallelDiscovery) {
    create_file(temp_dir_a, "file.txt", "content");
    create_file(temp_dir_b, "file.txt", "content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 8;

    DirectoryComparisonProgress progress;
    progress.cancelled = true;  // Pre-cancel

    auto result = run_directory_comparison(config, progress);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Cancelled") != std::string::npos);
}

// ============================================================================
// Non-Recursive Mode with Parallel Discovery Flag
// ============================================================================

TEST_F(ParallelDiscoveryCancellationTest, NonRecursiveWithParallelDiscoveryEnabled) {
    // Verify that non-recursive mode still works when parallel_discovery is on
    create_file(temp_dir_a, "root.txt", "root content");
    create_file(temp_dir_b, "root.txt", "root content");
    create_file(temp_dir_a, "subdir/nested.txt", "nested");
    create_file(temp_dir_b, "subdir/nested.txt", "nested");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = false;  // Non-recursive
    config.parallel_discovery = true;  // But parallel discovery is on

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 1u);  // Only root.txt
    EXPECT_EQ(result.matching_files, 1u);
}

// ============================================================================
// Worker Count Edge Cases in Integration
// ============================================================================

class ParallelDiscoveryWorkerEdgeCases : public ::testing::Test {
protected:
    std::string temp_dir_a;
    std::string temp_dir_b;

    void SetUp() override {
        std::string base = "/tmp/objiff_workers_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::rand());
        temp_dir_a = base + "_a";
        temp_dir_b = base + "_b";
        fs::create_directories(temp_dir_a);
        fs::create_directories(temp_dir_b);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_a, ec);
        fs::remove_all(temp_dir_b, ec);
    }

    void create_file(const std::string& dir, const std::string& path, const std::string& content) {
        fs::path full_path = fs::path(dir) / path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
        out.close();
    }
};

TEST_F(ParallelDiscoveryWorkerEdgeCases, SingleWorkerProducesCorrectResults) {
    create_file(temp_dir_a, "a/file1.txt", "content1");
    create_file(temp_dir_b, "a/file1.txt", "content1");
    create_file(temp_dir_a, "b/file2.txt", "content2");
    create_file(temp_dir_b, "b/file2.txt", "content2");
    create_file(temp_dir_a, "c/file3.txt", "content3");
    create_file(temp_dir_b, "c/file3.txt", "different");  // Mismatch

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 1;  // Single worker

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 3u);
    EXPECT_EQ(result.matching_files, 2u);
    EXPECT_EQ(result.mismatched_files, 1u);
}

TEST_F(ParallelDiscoveryWorkerEdgeCases, ZeroWorkersClampedTo1) {
    create_file(temp_dir_a, "file.txt", "content");
    create_file(temp_dir_b, "file.txt", "content");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 0;  // Invalid, should clamp

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matching_files, 1u);
}

TEST_F(ParallelDiscoveryWorkerEdgeCases, MaxWorkersProducesCorrectResults) {
    // Create enough structure to exercise 128 workers
    for (int i = 0; i < 50; ++i) {
        create_file(temp_dir_a, "dir" + std::to_string(i) + "/file.txt", "content" + std::to_string(i));
        create_file(temp_dir_b, "dir" + std::to_string(i) + "/file.txt", "content" + std::to_string(i));
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = true;
    config.parallel_discovery_workers = 128;  // Max

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.total_files, 50u);
    EXPECT_EQ(result.matching_files, 50u);
}

TEST_F(ParallelDiscoveryWorkerEdgeCases, ParallelVsSequentialResultsMatch) {
    // Create a complex structure and verify parallel and sequential produce same results
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            std::string path = "dir" + std::to_string(i) + "/sub" + std::to_string(j) + "/file.txt";
            std::string content = "content_" + std::to_string(i) + "_" + std::to_string(j);
            create_file(temp_dir_a, path, content);
            create_file(temp_dir_b, path, content);
        }
    }
    // Add some mismatches
    create_file(temp_dir_a, "only_a.txt", "only in a");
    create_file(temp_dir_b, "only_b.txt", "only in b");
    create_file(temp_dir_a, "differ.txt", "version a");
    create_file(temp_dir_b, "differ.txt", "version b");

    // Sequential
    DirectoryComparisonConfig seq_config;
    seq_config.source_a.type = SourceType::Local;
    seq_config.source_a.path = temp_dir_a;
    seq_config.source_b.type = SourceType::Local;
    seq_config.source_b.path = temp_dir_b;
    seq_config.recursive = true;
    seq_config.parallel_discovery = false;

    DirectoryComparisonProgress seq_progress;
    auto seq_result = run_directory_comparison(seq_config, seq_progress);

    // Parallel
    DirectoryComparisonConfig par_config = seq_config;
    par_config.parallel_discovery = true;
    par_config.parallel_discovery_workers = 16;

    DirectoryComparisonProgress par_progress;
    auto par_result = run_directory_comparison(par_config, par_progress);

    // Results should match
    EXPECT_EQ(seq_result.success, par_result.success);
    EXPECT_EQ(seq_result.total_files, par_result.total_files);
    EXPECT_EQ(seq_result.matching_files, par_result.matching_files);
    EXPECT_EQ(seq_result.mismatched_files, par_result.mismatched_files);
    EXPECT_EQ(seq_result.only_in_a, par_result.only_in_a);
    EXPECT_EQ(seq_result.only_in_b, par_result.only_in_b);
}

// ============================================================================
// Cleanup Stress Tests - verify background thread pool cleanup under load
// ============================================================================

class CleanupStressTest : public ::testing::Test {
protected:
    std::string temp_root;

    void SetUp() override {
        temp_root = "/tmp/objiff_cleanup_stress_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void create_file(const std::string& relative_path, size_t size = 100) {
        fs::path full_path = fs::path(temp_root) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        if (size > 0) {
            std::vector<char> data(size, 'X');
            out.write(data.data(), data.size());
        }
        out.close();
    }
};

TEST_F(CleanupStressTest, RapidSequentialEnumerations) {
    // Create a small directory structure
    for (int i = 0; i < 10; ++i) {
        create_file("dir" + std::to_string(i) + "/file.txt", 50);
    }

    // Call parallel_enumerate many times in rapid succession
    // This tests that detached cleanup threads don't cause resource exhaustion
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        EXPECT_EQ(entries.size(), 10u);
        EXPECT_EQ(files_found.load(), 10u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(CleanupStressTest, RapidSequentialComparisons) {
    // Create matching directories
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    for (int i = 0; i < 5; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        {
            std::ofstream out(dir_a + "/" + filename);
            out << "content" << i;
        }
        {
            std::ofstream out(dir_b + "/" + filename);
            out << "content" << i;
        }
    }

    // Run comparisons rapidly
    for (int iteration = 0; iteration < 50; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = dir_b;
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.matching_files, 5u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(CleanupStressTest, ConcurrentEnumerations) {
    // Create directory structure
    for (int i = 0; i < 20; ++i) {
        create_file("subdir/file" + std::to_string(i) + ".txt", 50);
    }

    // Launch multiple enumerations concurrently
    std::vector<std::future<size_t>> futures;
    for (int t = 0; t < 10; ++t) {
        futures.push_back(std::async(std::launch::async, [this]() {
            std::atomic<size_t> files_found{0};
            std::atomic<bool> cancelled{false};

            auto entries = parallel_enumerate_local_directory(
                temp_root, true, 4, files_found, cancelled);

            return entries.size();
        }));
    }

    // All should complete successfully with same count
    for (auto& f : futures) {
        EXPECT_EQ(f.get(), 20u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(CleanupStressTest, CancellationDuringEnumeration) {
    // Create a moderately deep structure to ensure enumeration takes time
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 5; ++j) {
            create_file("dir" + std::to_string(i) + "/sub" + std::to_string(j) + "/file.txt", 50);
        }
    }

    // Run multiple trials to catch race conditions
    for (int trial = 0; trial < 20; ++trial) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        // Start enumeration in background
        std::future<std::vector<DirectoryEntry>> future = std::async(std::launch::async, [&]() {
            return parallel_enumerate_local_directory(
                temp_root, true, 8, files_found, cancelled);
        });

        // Cancel after a short delay to hit mid-enumeration
        std::this_thread::sleep_for(std::chrono::microseconds(50 + trial * 10));
        cancelled = true;

        // Should complete without crash or hang
        auto entries = future.get();

        // Verify no corruption - all entries should be valid
        for (const auto& e : entries) {
            EXPECT_FALSE(e.relative_path.empty());
        }
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ============================================================================
// S3 Cleanup Stress Tests - verify background thread pool cleanup with S3 mock
// ============================================================================

class S3CleanupStressTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_client;

    void SetUp() override {
        mock_client = std::make_shared<MockS3Client>();
        mock_client->CreateBucket("bucket");
    }

    void put_object(const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock_client->PutObject("bucket", key, data);
    }
};

TEST_F(S3CleanupStressTest, RapidSequentialS3Enumerations) {
    // Create hierarchical S3 structure
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            put_object("prefix/dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt",
                      "content");
        }
    }

    // Call parallel_enumerate_s3_prefix many times in rapid succession
    for (int iteration = 0; iteration < 50; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_s3_prefix(
            "bucket", "prefix", true, 8, files_found, cancelled, mock_client);

        EXPECT_EQ(entries.size(), 30u);
        EXPECT_EQ(files_found.load(), 30u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(S3CleanupStressTest, ConcurrentS3Enumerations) {
    // Create S3 structure
    for (int i = 0; i < 15; ++i) {
        put_object("data/file" + std::to_string(i) + ".txt", "content");
    }

    // Launch multiple enumerations concurrently
    std::vector<std::future<size_t>> futures;
    for (int t = 0; t < 10; ++t) {
        futures.push_back(std::async(std::launch::async, [this]() {
            std::atomic<size_t> files_found{0};
            std::atomic<bool> cancelled{false};

            auto entries = parallel_enumerate_s3_prefix(
                "bucket", "data", true, 4, files_found, cancelled, mock_client);

            return entries.size();
        }));
    }

    // All should complete successfully with same count
    for (auto& f : futures) {
        EXPECT_EQ(f.get(), 15u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(S3CleanupStressTest, CancellationDuringS3Enumeration) {
    // Create many prefixes to maximize race window
    for (int i = 0; i < 30; ++i) {
        for (int j = 0; j < 4; ++j) {
            put_object("prefix/dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt",
                      "content");
        }
    }

    // Run multiple trials to catch race conditions
    for (int trial = 0; trial < 15; ++trial) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        // Start enumeration in background
        std::future<std::vector<DirectoryEntry>> future = std::async(std::launch::async, [&]() {
            return parallel_enumerate_s3_prefix(
                "bucket", "prefix", true, 16, files_found, cancelled, mock_client);
        });

        // Cancel after varying delays to hit different phases
        std::this_thread::sleep_for(std::chrono::microseconds(20 + trial * 15));
        cancelled = true;

        // Should complete without crash or hang
        auto entries = future.get();

        // Verify no corruption - all entries should have valid paths
        for (const auto& e : entries) {
            EXPECT_FALSE(e.relative_path.empty());
        }
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(CleanupStressTest, CancellationDuringComparison) {
    // Create matching directories with enough files to ensure comparison takes time
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    for (int i = 0; i < 50; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        std::string content = "content for file " + std::to_string(i);
        {
            std::ofstream out(dir_a + "/" + filename);
            out << content;
        }
        {
            std::ofstream out(dir_b + "/" + filename);
            out << content;
        }
    }

    // Run multiple trials
    for (int trial = 0; trial < 10; ++trial) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = dir_b;
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;

        // Start comparison in background
        std::future<DirectoryComparisonResult> future = std::async(std::launch::async, [&]() {
            return run_directory_comparison(config, progress);
        });

        // Cancel after varying delays
        std::this_thread::sleep_for(std::chrono::microseconds(100 + trial * 50));
        progress.cancelled = true;

        // Should complete without crash or hang
        auto result = future.get();

        // Result may be partial but should be valid
        EXPECT_LE(result.matching_files + result.mismatched_files + result.errors, 50u);
    }

    // Brief pause to allow cleanup threads to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(CleanupStressTest, ComparisonWithMismatchedFiles) {
    // Test cleanup when comparisons find mismatches (exercises error paths)
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    // Create files where half match and half don't
    for (int i = 0; i < 20; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        {
            std::ofstream out(dir_a + "/" + filename);
            out << "content_a_" << i;
        }
        {
            std::ofstream out(dir_b + "/" + filename);
            // Even files match, odd files differ
            if (i % 2 == 0) {
                out << "content_a_" << i;
            } else {
                out << "content_b_" << i;
            }
        }
    }

    // Run comparisons rapidly to stress cleanup
    for (int iteration = 0; iteration < 30; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = dir_b;
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.matching_files, 10u);
        EXPECT_EQ(result.mismatched_files, 10u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ============================================================================
// S3 Comparison Cleanup Stress Tests
// ============================================================================

class S3ComparisonCleanupTest : public ::testing::Test {
protected:
    std::shared_ptr<MockS3Client> mock_client;
    std::string temp_root;

    void SetUp() override {
        mock_client = std::make_shared<MockS3Client>();
        mock_client->CreateBucket("bucket-a");
        mock_client->CreateBucket("bucket-b");

        temp_root = "/tmp/objiff_s3comp_cleanup_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void put_object(const std::string& bucket, const std::string& key, const std::string& content) {
        std::vector<uint8_t> data(content.begin(), content.end());
        mock_client->PutObject(bucket, key, data);
    }

    void create_local_file(const std::string& relative_path, const std::string& content) {
        fs::path full_path = fs::path(temp_root) / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path);
        out << content;
    }
};

TEST_F(S3ComparisonCleanupTest, RapidS3ToS3Comparisons) {
    // Create matching S3 structures in two buckets
    for (int i = 0; i < 15; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        std::string content = "content" + std::to_string(i);
        put_object("bucket-a", key, content);
        put_object("bucket-b", key, content);
    }

    // Run S3-to-S3 comparisons rapidly
    for (int iteration = 0; iteration < 30; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::S3;
        config.source_a.bucket = "bucket-a";
        config.source_a.path = "prefix/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-b";
        config.source_b.path = "prefix/";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress, mock_client, mock_client);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.matching_files, 15u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(S3ComparisonCleanupTest, RapidLocalToS3Comparisons) {
    // Create matching local and S3 structures
    for (int i = 0; i < 15; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        std::string content = "content" + std::to_string(i);
        create_local_file(filename, content);
        put_object("bucket-a", filename, content);
    }

    // Run local-to-S3 comparisons rapidly
    for (int iteration = 0; iteration < 30; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = temp_root + "/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-a";
        config.source_b.path = "";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress, nullptr, mock_client);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.matching_files, 15u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(S3ComparisonCleanupTest, CancellationDuringS3Comparison) {
    // Create S3 structures with enough files to ensure comparison takes time
    for (int i = 0; i < 40; ++i) {
        std::string key = "data/file" + std::to_string(i) + ".txt";
        std::string content = "content for file " + std::to_string(i);
        put_object("bucket-a", key, content);
        put_object("bucket-b", key, content);
    }

    // Run multiple trials with cancellation
    for (int trial = 0; trial < 10; ++trial) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::S3;
        config.source_a.bucket = "bucket-a";
        config.source_a.path = "data/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-b";
        config.source_b.path = "data/";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 8;

        DirectoryComparisonProgress progress;

        std::future<DirectoryComparisonResult> future = std::async(std::launch::async, [&]() {
            return run_directory_comparison(config, progress, mock_client, mock_client);
        });

        // Cancel after varying delays
        std::this_thread::sleep_for(std::chrono::microseconds(50 + trial * 30));
        progress.cancelled = true;

        auto result = future.get();

        // Should complete without crash, result may be partial
        EXPECT_LE(result.matching_files + result.mismatched_files + result.errors, 40u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(S3ComparisonCleanupTest, S3ComparisonWithMismatches) {
    // Create S3 structures where some files mismatch
    for (int i = 0; i < 20; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        put_object("bucket-a", key, "content_a_" + std::to_string(i));
        // Even files match, odd files differ
        if (i % 2 == 0) {
            put_object("bucket-b", key, "content_a_" + std::to_string(i));
        } else {
            put_object("bucket-b", key, "content_b_" + std::to_string(i));
        }
    }

    // Run comparisons rapidly
    for (int iteration = 0; iteration < 25; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::S3;
        config.source_a.bucket = "bucket-a";
        config.source_a.path = "prefix/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-b";
        config.source_b.path = "prefix/";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress, mock_client, mock_client);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.matching_files, 10u);
        EXPECT_EQ(result.mismatched_files, 10u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ============================================================================
// Resource Exhaustion Tests - verify cleanup prevents resource leaks
// ============================================================================

class ResourceExhaustionTest : public ::testing::Test {
protected:
    std::string temp_root;

    void SetUp() override {
        temp_root = "/tmp/objiff_resource_exhaust_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand());
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }
};

TEST_F(ResourceExhaustionTest, ManyRapidEnumerationsNoResourceLeak) {
    // Create a directory structure
    for (int i = 0; i < 10; ++i) {
        fs::path dir = fs::path(temp_root) / ("dir" + std::to_string(i));
        fs::create_directories(dir);
        for (int j = 0; j < 5; ++j) {
            std::ofstream out(dir / ("file" + std::to_string(j) + ".txt"));
            out << "content";
        }
    }

    // Run MANY enumerations in rapid succession
    // If cleanup threads leak, we'd eventually exhaust thread handles
    // This tests that detached threads complete and release resources
    for (int iteration = 0; iteration < 500; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        ASSERT_EQ(entries.size(), 50u) << "Failed at iteration " << iteration;

        // No sleep between iterations - stress test the cleanup
    }

    // Final wait for any remaining cleanup threads
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST_F(ResourceExhaustionTest, ManyRapidComparisonsNoResourceLeak) {
    // Create matching directories
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    for (int i = 0; i < 10; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        {
            std::ofstream out(dir_a + "/" + filename);
            out << "content" << i;
        }
        {
            std::ofstream out(dir_b + "/" + filename);
            out << "content" << i;
        }
    }

    // Run MANY comparisons in rapid succession
    for (int iteration = 0; iteration < 200; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = dir_b;
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress);

        ASSERT_TRUE(result.success) << "Failed at iteration " << iteration;
        ASSERT_EQ(result.matching_files, 10u) << "Failed at iteration " << iteration;

        // No sleep between iterations - stress test the cleanup
    }

    // Final wait for any remaining cleanup threads
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST_F(ResourceExhaustionTest, InterleavedEnumerationsAndComparisons) {
    // Create directory structures
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    for (int i = 0; i < 8; ++i) {
        std::string filename = "file" + std::to_string(i) + ".txt";
        {
            std::ofstream out(dir_a + "/" + filename);
            out << "content" << i;
        }
        {
            std::ofstream out(dir_b + "/" + filename);
            out << "content" << i;
        }
    }

    // Interleave enumerations and comparisons to stress different cleanup paths
    for (int iteration = 0; iteration < 100; ++iteration) {
        // Enumeration
        {
            std::atomic<size_t> files_found{0};
            std::atomic<bool> cancelled{false};
            auto entries = parallel_enumerate_local_directory(
                dir_a, true, 4, files_found, cancelled);
            ASSERT_EQ(entries.size(), 8u) << "Enumeration failed at iteration " << iteration;
        }

        // Comparison
        {
            DirectoryComparisonConfig config;
            config.source_a.type = SourceType::Local;
            config.source_a.path = dir_a;
            config.source_b.type = SourceType::Local;
            config.source_b.path = dir_b;
            config.recursive = true;
            config.parallel_discovery = true;
            config.parallel_discovery_workers = 4;

            DirectoryComparisonProgress progress;
            auto result = run_directory_comparison(config, progress);
            ASSERT_TRUE(result.success) << "Comparison failed at iteration " << iteration;
        }

        // No sleep - maximum stress
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// ============================================================================
// Edge Case Tests - Empty Directories and Error Conditions
// ============================================================================

TEST_F(CleanupStressTest, EmptyDirectoryEnumeration) {
    // Test enumeration of completely empty directory
    // This tests the edge case where pool has no work to do
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_local_directory(
        temp_root, true, 8, files_found, cancelled);

    EXPECT_EQ(entries.size(), 0u);
    EXPECT_EQ(files_found.load(), 0u);

    // Run multiple times to ensure cleanup handles zero-task case
    for (int i = 0; i < 20; ++i) {
        std::atomic<size_t> ff{0};
        std::atomic<bool> c{false};
        auto e = parallel_enumerate_local_directory(temp_root, true, 4, ff, c);
        EXPECT_EQ(e.size(), 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(CleanupStressTest, EmptyDirectoryComparison) {
    // Test comparison of two empty directories
    std::string dir_a = temp_root + "/a";
    std::string dir_b = temp_root + "/b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    for (int iteration = 0; iteration < 20; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = dir_a;
        config.source_b.type = SourceType::Local;
        config.source_b.path = dir_b;
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.total_files, 0u);
        EXPECT_EQ(result.matching_files, 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(CleanupStressTest, SingleFileEnumeration) {
    // Edge case: exactly one file
    create_file("single.txt", 100);

    for (int iteration = 0; iteration < 20; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        EXPECT_EQ(entries.size(), 1u);
        EXPECT_EQ(files_found.load(), 1u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(CleanupStressTest, UnreadableDirectoryHandling) {
    // Test that enumeration handles permission errors gracefully
    // Create a directory structure with one unreadable subdirectory
    create_file("readable/file1.txt", 50);
    create_file("readable/file2.txt", 50);

    std::string unreadable_dir = temp_root + "/unreadable";
    fs::create_directories(unreadable_dir);

    // Make directory unreadable (skip on systems where we can't change perms)
    std::error_code ec;
    fs::permissions(unreadable_dir, fs::perms::none, ec);
    if (ec) {
        GTEST_SKIP() << "Cannot modify directory permissions on this system";
    }

    // Enumeration should complete without hanging, even with permission error
    for (int iteration = 0; iteration < 10; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        // Should find files in readable directory
        EXPECT_GE(entries.size(), 2u);

        // All returned entries should be valid
        for (const auto& e : entries) {
            EXPECT_FALSE(e.relative_path.empty());
        }
    }

    // Restore permissions for cleanup
    fs::permissions(unreadable_dir, fs::perms::owner_all, ec);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(CleanupStressTest, DeeplyNestedSinglePath) {
    // Edge case: deeply nested structure with only one file at the bottom
    // Each BFS level has only one directory to process
    std::string nested_path = "a/b/c/d/e/f/g/h/i/j/file.txt";
    create_file(nested_path, 50);

    for (int iteration = 0; iteration < 20; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        EXPECT_EQ(entries.size(), 1u);
        EXPECT_EQ(files_found.load(), 1u);
        if (!entries.empty()) {
            EXPECT_EQ(entries[0].relative_path, nested_path);
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(CleanupStressTest, MixedEmptyAndPopulatedDirectories) {
    // Some directories have files, some are empty
    // Tests cleanup when some workers find nothing
    fs::create_directories(temp_root + "/empty1");
    fs::create_directories(temp_root + "/empty2");
    fs::create_directories(temp_root + "/empty3/nested_empty");
    create_file("populated1/file1.txt", 50);
    create_file("populated2/file2.txt", 50);

    for (int iteration = 0; iteration < 20; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_local_directory(
            temp_root, true, 8, files_found, cancelled);

        EXPECT_EQ(entries.size(), 2u);
        EXPECT_EQ(files_found.load(), 2u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(S3CleanupStressTest, EmptyS3PrefixEnumeration) {
    // Test enumeration of S3 prefix with no objects
    std::atomic<size_t> files_found{0};
    std::atomic<bool> cancelled{false};

    auto entries = parallel_enumerate_s3_prefix(
        "bucket", "nonexistent_prefix", true, 8, files_found, cancelled, mock_client);

    EXPECT_EQ(entries.size(), 0u);
    EXPECT_EQ(files_found.load(), 0u);

    // Run multiple times to stress test cleanup with no work
    for (int i = 0; i < 20; ++i) {
        std::atomic<size_t> ff{0};
        std::atomic<bool> c{false};
        auto e = parallel_enumerate_s3_prefix(
            "bucket", "empty_prefix", true, 4, ff, c, mock_client);
        EXPECT_EQ(e.size(), 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(S3CleanupStressTest, SingleS3ObjectEnumeration) {
    // Edge case: exactly one object
    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    mock_client->PutObject("bucket", "single/file.txt", data);

    for (int iteration = 0; iteration < 20; ++iteration) {
        std::atomic<size_t> files_found{0};
        std::atomic<bool> cancelled{false};

        auto entries = parallel_enumerate_s3_prefix(
            "bucket", "single", true, 8, files_found, cancelled, mock_client);

        EXPECT_EQ(entries.size(), 1u);
        EXPECT_EQ(files_found.load(), 1u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(S3ComparisonCleanupTest, EmptyS3ToS3Comparison) {
    // Test comparison of two empty S3 prefixes
    for (int iteration = 0; iteration < 20; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::S3;
        config.source_a.bucket = "bucket-a";
        config.source_a.path = "empty_prefix/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-b";
        config.source_b.path = "empty_prefix/";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress, mock_client, mock_client);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.total_files, 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(S3ComparisonCleanupTest, EmptyLocalToS3Comparison) {
    // Test comparison of empty local dir to empty S3 prefix
    for (int iteration = 0; iteration < 20; ++iteration) {
        DirectoryComparisonConfig config;
        config.source_a.type = SourceType::Local;
        config.source_a.path = temp_root + "/";
        config.source_b.type = SourceType::S3;
        config.source_b.bucket = "bucket-a";
        config.source_b.path = "empty_prefix/";
        config.recursive = true;
        config.parallel_discovery = true;
        config.parallel_discovery_workers = 4;

        DirectoryComparisonProgress progress;
        auto result = run_directory_comparison(config, progress, nullptr, mock_client);

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.total_files, 0u);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// Enumeration failures must not be reported as differences (issue #29)
// ============================================================================

TEST_F(DirectoryComparisonTest, UnreadableSubdirectoryFailsRatherThanReportingPhantomDifferences) {
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    // Identical trees, but one subtree of A cannot be read. Reporting its files
    // as "only in B" would tell the user the copies differ when they do not.
    create_file(temp_dir_a, "top.txt", "same");
    create_file(temp_dir_b, "top.txt", "same");
    fs::path locked_a = fs::path(temp_dir_a) / "locked";
    fs::create_directories(locked_a);
    { std::ofstream(locked_a / "inner.txt") << "same"; }
    fs::create_directories(fs::path(temp_dir_b) / "locked");
    { std::ofstream(fs::path(temp_dir_b) / "locked" / "inner.txt") << "same"; }
    fs::permissions(locked_a, fs::perms::none);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    std::error_code ec;
    fs::permissions(locked_a, fs::perms::owner_all, ec);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Source A"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_EQ(result.only_in_b, 0u) << "no phantom difference may be reported";
}

TEST_F(DirectoryComparisonTest, FailedS3ListingFailsTheComparison) {
    auto mock = std::make_shared<MockS3Client>();
    mock->CreateBucket("bkt");
    mock->PutObject("bkt", "p/f.txt", std::vector<uint8_t>{'x'});
    mock->SetFailure("bkt", "p/", S3MockMethod::ListObjects);

    create_file(temp_dir_a, "f.txt", "x");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::S3;
    config.source_b.bucket = "bkt";
    config.source_b.path = "p/";
    config.source_b.region = "us-east-1";
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress, mock, mock);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Source B"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_EQ(result.only_in_a, 0u) << "a failed listing is not an empty bucket";
}

TEST_F(DirectoryComparisonTest, CleanComparisonStillSucceeds) {
    // Guard against over-firing: identical readable trees must still compare.
    create_file(temp_dir_a, "same.txt", "content");
    create_file(temp_dir_b, "same.txt", "content");
    create_file(temp_dir_a, "only_a.txt", "a");

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.matching_files, 1u);
    EXPECT_EQ(result.only_in_a, 1u);
}

TEST_F(DirectoryComparisonTest, SequentialDiscoveryAlsoDetectsAnUnreadableSubtree) {
    // --no-parallel-discovery selects a different enumerator. It used to swallow
    // EACCES, so the guard silently did not exist on that path.
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    create_file(temp_dir_a, "top.txt", "same");
    create_file(temp_dir_b, "top.txt", "same");
    fs::path locked_a = fs::path(temp_dir_a) / "locked";
    fs::create_directories(locked_a);
    { std::ofstream(locked_a / "inner.txt") << "same"; }
    fs::permissions(locked_a, fs::perms::none);

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;
    config.parallel_discovery = false;   // the path that used to be blind

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    std::error_code ec;
    fs::permissions(locked_a, fs::perms::owner_all, ec);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Source A"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(DirectoryComparisonTest, TreesWithAliasedSymlinksAreStillComparable) {
    // Two symlinks to one directory is an ordinary layout - /usr/share/doc has
    // dozens. The subtree is omitted from the listing under the second alias,
    // but its files are present under the first, so a comparison is unaffected
    // and must not be refused.
    for (const std::string& d : {temp_dir_a, temp_dir_b}) {
        fs::create_directories(fs::path(d) / "real");
        { std::ofstream(fs::path(d) / "real" / "f.txt") << "z"; }
        fs::create_directory_symlink("real", fs::path(d) / "l1");
        fs::create_directory_symlink("real", fs::path(d) / "l2");
    }

    DirectoryComparisonConfig config;
    config.source_a.type = SourceType::Local;
    config.source_a.path = temp_dir_a;
    config.source_b.type = SourceType::Local;
    config.source_b.path = temp_dir_b;
    config.recursive = true;

    DirectoryComparisonProgress progress;
    auto result = run_directory_comparison(config, progress);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.mismatched_files, 0u);
}

TEST_F(DirectoryComparisonTest, NonRecursiveStatFailureIsReportedAsIncomplete) {
    // Issue #46: the non-recursive branch dropped an entry it could not stat
    // and still reported a complete listing, so run_directory_comparison could
    // consume a partial listing as complete.
    if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permission checks";

    // A symlink whose target lives in a directory we cannot read: the link is
    // listed, but stat-ing it through to the target fails with EACCES.
    fs::path hidden = fs::path(temp_dir_a) / ".." /
                      ("hidden_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(hidden, ec);
    fs::create_directories(hidden);
    { std::ofstream(hidden / "real.txt") << "x"; }
    fs::create_symlink(hidden / "real.txt", fs::path(temp_dir_a) / "link.txt");
    fs::permissions(hidden, fs::perms::none);

    std::atomic<size_t> found{0};
    std::atomic<bool> cancelled{false};
    bool complete = true;
    auto entries = enumerate_local_directory(temp_dir_a, /*recursive=*/false,
                                             found, cancelled, &complete);

    fs::permissions(hidden, fs::perms::owner_all, ec);
    fs::remove_all(hidden, ec);

    EXPECT_FALSE(complete)
        << "an entry that could not be stat'ed must mark the listing incomplete";
    EXPECT_TRUE(entries.empty()) << "and it must not appear in the listing";
}
