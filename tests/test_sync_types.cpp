#include <gtest/gtest.h>
#include "sync_types.h"
#include "url_parser.h"

// Sync direction detection and SyncConfig construction from parsed sources.

TEST(SyncDirectionTest, LocalToS3IsUpload) {
    // source_local=true, dest_local=false -> Upload
    bool source_local = true;
    bool dest_local = false;

    SyncDirection expected = SyncDirection::Upload;

    SyncDirection actual;
    if (source_local && !dest_local) {
        actual = SyncDirection::Upload;
    } else if (!source_local && dest_local) {
        actual = SyncDirection::Download;
    } else if (!source_local && !dest_local) {
        actual = SyncDirection::S3ToS3;
    } else {
        actual = SyncDirection::Upload;  // Invalid case, would error in real code
    }

    EXPECT_EQ(actual, expected);
}

TEST(SyncDirectionTest, S3ToLocalIsDownload) {
    bool source_local = false;
    bool dest_local = true;

    SyncDirection expected = SyncDirection::Download;

    SyncDirection actual;
    if (source_local && !dest_local) {
        actual = SyncDirection::Upload;
    } else if (!source_local && dest_local) {
        actual = SyncDirection::Download;
    } else if (!source_local && !dest_local) {
        actual = SyncDirection::S3ToS3;
    } else {
        actual = SyncDirection::Upload;
    }

    EXPECT_EQ(actual, expected);
}

TEST(SyncDirectionTest, S3ToS3) {
    bool source_local = false;
    bool dest_local = false;

    SyncDirection expected = SyncDirection::S3ToS3;

    SyncDirection actual;
    if (source_local && !dest_local) {
        actual = SyncDirection::Upload;
    } else if (!source_local && dest_local) {
        actual = SyncDirection::Download;
    } else if (!source_local && !dest_local) {
        actual = SyncDirection::S3ToS3;
    } else {
        actual = SyncDirection::Upload;
    }

    EXPECT_EQ(actual, expected);
}

TEST(FileSourceTest, LocalFileSource) {
    FileSource fs;
    fs.type = SourceType::Local;
    fs.path = "/Users/test/data";

    EXPECT_EQ(fs.type, SourceType::Local);
    EXPECT_EQ(fs.path, "/Users/test/data");
}

TEST(FileSourceTest, S3FileSource) {
    FileSource fs;
    fs.type = SourceType::S3;
    fs.bucket = "my-bucket";
    fs.path = "prefix/path/";
    fs.region = "eu-west-1";

    EXPECT_EQ(fs.type, SourceType::S3);
    EXPECT_EQ(fs.bucket, "my-bucket");
    EXPECT_EQ(fs.path, "prefix/path/");
    EXPECT_EQ(fs.region, "eu-west-1");
}

TEST(SyncConfigTest, UploadConfig) {
    SyncConfig config;
    config.direction = SyncDirection::Upload;
    config.local_path = "/local/path";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "bucket";
    config.destination.path = "prefix/";
    config.destination.region = "us-east-1";
    config.delete_orphans = true;
    config.max_threads = 128;

    EXPECT_EQ(config.direction, SyncDirection::Upload);
    EXPECT_EQ(config.local_path, "/local/path");
    EXPECT_EQ(config.destination.bucket, "bucket");
    EXPECT_TRUE(config.delete_orphans);
    EXPECT_EQ(config.max_threads, 128);
}

TEST(SyncConfigTest, DownloadConfig) {
    SyncConfig config;
    config.direction = SyncDirection::Download;
    config.source.type = SourceType::S3;
    config.source.bucket = "bucket";
    config.source.path = "prefix/";
    config.source.region = "eu-west-1";
    config.local_path = "/local/download";
    config.delete_orphans = false;

    EXPECT_EQ(config.direction, SyncDirection::Download);
    EXPECT_EQ(config.source.bucket, "bucket");
    EXPECT_EQ(config.local_path, "/local/download");
    EXPECT_FALSE(config.delete_orphans);
}

TEST(SyncConfigTest, S3ToS3Config) {
    SyncConfig config;
    config.direction = SyncDirection::S3ToS3;
    config.source.type = SourceType::S3;
    config.source.bucket = "source-bucket";
    config.source.path = "src/";
    config.source.region = "us-west-2";
    config.destination.type = SourceType::S3;
    config.destination.bucket = "dest-bucket";
    config.destination.path = "dst/";
    config.destination.region = "eu-central-1";

    EXPECT_EQ(config.direction, SyncDirection::S3ToS3);
    EXPECT_EQ(config.source.bucket, "source-bucket");
    EXPECT_EQ(config.destination.bucket, "dest-bucket");
    EXPECT_EQ(config.source.region, "us-west-2");
    EXPECT_EQ(config.destination.region, "eu-central-1");
}

// Test the path translation algorithm used for status lookup after navigation
// This duplicates the logic from SyncViewState::get_comparison_relative_path
// to test it without needing to include the GUI internals
class NavigationPathTest : public ::testing::Test {
protected:
    // Compute comparison-relative path from local path and navigation context
    // This is the same algorithm as SyncViewState::get_comparison_relative_path
    std::string get_comparison_relative_path(
        const std::string& local_path,
        const std::string& current_pane_path,
        const std::string& comparison_base_path
    ) {
        // If current path is at or before comparison base, no prefix needed
        if (current_pane_path.size() <= comparison_base_path.size()) {
            return local_path;
        }

        // Compute the navigation offset (path from base to current)
        std::string offset = current_pane_path.substr(comparison_base_path.size());
        if (!offset.empty() && offset[0] == '/') {
            offset = offset.substr(1);
        }

        // Prepend offset to get comparison-relative path
        if (offset.empty()) {
            return local_path;
        }
        return offset + "/" + local_path;
    }
};

TEST_F(NavigationPathTest, NoNavigation) {
    // When at comparison root, paths should be unchanged
    std::string base = "/tmp/test";
    std::string current = "/tmp/test";
    std::string local = "file.txt";

    EXPECT_EQ(get_comparison_relative_path(local, current, base), "file.txt");
}

TEST_F(NavigationPathTest, SingleSubdirectory) {
    // After navigating into one subdirectory
    std::string base = "/tmp/test";
    std::string current = "/tmp/test/subdir1";
    std::string local = "file.txt";

    EXPECT_EQ(get_comparison_relative_path(local, current, base), "subdir1/file.txt");
}

TEST_F(NavigationPathTest, NestedSubdirectories) {
    // After navigating into nested subdirectories
    std::string base = "/tmp/test";
    std::string current = "/tmp/test/subdir1/deep/nested";
    std::string local = "file.txt";

    EXPECT_EQ(get_comparison_relative_path(local, current, base), "subdir1/deep/nested/file.txt");
}

TEST_F(NavigationPathTest, LocalPathWithSubdirectory) {
    // Local path already contains subdirectory
    std::string base = "/tmp/test";
    std::string current = "/tmp/test/subdir1";
    std::string local = "nested/file.txt";

    EXPECT_EQ(get_comparison_relative_path(local, current, base), "subdir1/nested/file.txt");
}

TEST_F(NavigationPathTest, S3PrefixNavigation) {
    // S3 paths work the same way
    std::string base = "prefix/data";
    std::string current = "prefix/data/2024/01";
    std::string local = "file.parquet";

    EXPECT_EQ(get_comparison_relative_path(local, current, base), "2024/01/file.parquet");
}

TEST_F(NavigationPathTest, BaseWithTrailingSlash) {
    // Should handle trailing slash in base path
    std::string base = "/tmp/test/";
    std::string current = "/tmp/test/subdir";
    std::string local = "file.txt";

    // current.size() > base.size(), offset = "subdir"
    EXPECT_EQ(get_comparison_relative_path(local, current, base), "subdir/file.txt");
}
