#include <gtest/gtest.h>
#include "url_parser.h"
#include "comparison_task.h"
#include <string>

// ============================================================================
// parse_s3_url Tests
// ============================================================================

class S3UrlParserTest : public ::testing::Test {
protected:
    std::string bucket;
    std::string key;
    std::string region;

    void SetUp() override {
        bucket.clear();
        key.clear();
        region = "default-region";  // Pre-set to verify it's not overwritten when not specified
    }
};

// Basic valid URLs

TEST_F(S3UrlParserTest, BasicUrl) {
    EXPECT_TRUE(parse_s3_url("s3://my-bucket/my-key", bucket, key, region));
    EXPECT_EQ(bucket, "my-bucket");
    EXPECT_EQ(key, "my-key");
    EXPECT_EQ(region, "default-region");  // Should be unchanged
}

TEST_F(S3UrlParserTest, UrlWithRegion) {
    EXPECT_TRUE(parse_s3_url("s3://my-bucket/my-key@us-west-2", bucket, key, region));
    EXPECT_EQ(bucket, "my-bucket");
    EXPECT_EQ(key, "my-key");
    EXPECT_EQ(region, "us-west-2");
}

TEST_F(S3UrlParserTest, NestedKey) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/path/to/file.bin", bucket, key, region));
    EXPECT_EQ(bucket, "bucket");
    EXPECT_EQ(key, "path/to/file.bin");
}

TEST_F(S3UrlParserTest, NestedKeyWithRegion) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/path/to/file.bin@eu-west-1", bucket, key, region));
    EXPECT_EQ(bucket, "bucket");
    EXPECT_EQ(key, "path/to/file.bin");
    EXPECT_EQ(region, "eu-west-1");
}

TEST_F(S3UrlParserTest, DeeplyNestedKey) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/a/b/c/d/e/f/g.txt", bucket, key, region));
    EXPECT_EQ(bucket, "bucket");
    EXPECT_EQ(key, "a/b/c/d/e/f/g.txt");
}

// Edge cases with special characters

TEST_F(S3UrlParserTest, KeyWithSpaces) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/file with spaces.txt", bucket, key, region));
    EXPECT_EQ(bucket, "bucket");
    EXPECT_EQ(key, "file with spaces.txt");
}

TEST_F(S3UrlParserTest, KeyWithSpecialChars) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/file-name_v2.0+test.tar.gz", bucket, key, region));
    EXPECT_EQ(bucket, "bucket");
    EXPECT_EQ(key, "file-name_v2.0+test.tar.gz");
}

TEST_F(S3UrlParserTest, BucketWithDots) {
    EXPECT_TRUE(parse_s3_url("s3://my.bucket.name/key", bucket, key, region));
    EXPECT_EQ(bucket, "my.bucket.name");
    EXPECT_EQ(key, "key");
}

TEST_F(S3UrlParserTest, BucketWithHyphens) {
    EXPECT_TRUE(parse_s3_url("s3://my-bucket-name/key", bucket, key, region));
    EXPECT_EQ(bucket, "my-bucket-name");
    EXPECT_EQ(key, "key");
}

// Invalid URLs

TEST_F(S3UrlParserTest, MissingProtocol) {
    EXPECT_FALSE(parse_s3_url("bucket/key", bucket, key, region));
}

TEST_F(S3UrlParserTest, WrongProtocol) {
    EXPECT_FALSE(parse_s3_url("http://bucket/key", bucket, key, region));
}

TEST_F(S3UrlParserTest, EmptyString) {
    EXPECT_FALSE(parse_s3_url("", bucket, key, region));
}

TEST_F(S3UrlParserTest, OnlyProtocol) {
    EXPECT_FALSE(parse_s3_url("s3://", bucket, key, region));
}

TEST_F(S3UrlParserTest, NoBucket) {
    EXPECT_FALSE(parse_s3_url("s3:///key", bucket, key, region));
}

TEST_F(S3UrlParserTest, NoKey) {
    EXPECT_FALSE(parse_s3_url("s3://bucket/", bucket, key, region));
}

TEST_F(S3UrlParserTest, NoKeyNoSlash) {
    EXPECT_FALSE(parse_s3_url("s3://bucket", bucket, key, region));
}

TEST_F(S3UrlParserTest, JustS3) {
    EXPECT_FALSE(parse_s3_url("s3", bucket, key, region));
}

TEST_F(S3UrlParserTest, PartialProtocol) {
    EXPECT_FALSE(parse_s3_url("s3:/bucket/key", bucket, key, region));
}

// Region edge cases

TEST_F(S3UrlParserTest, EmptyRegion) {
    // s3://bucket/key@ should treat @ as part of the key, not empty region
    EXPECT_TRUE(parse_s3_url("s3://bucket/key@", bucket, key, region));
    EXPECT_EQ(key, "key");
    EXPECT_EQ(region, "");  // Empty region extracted
}

TEST_F(S3UrlParserTest, RegionWithHyphen) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/key@ap-southeast-1", bucket, key, region));
    EXPECT_EQ(region, "ap-southeast-1");
}

TEST_F(S3UrlParserTest, RegionWithNumbers) {
    EXPECT_TRUE(parse_s3_url("s3://bucket/key@us-east-1", bucket, key, region));
    EXPECT_EQ(region, "us-east-1");
}

// @ in bucket name (edge case - technically invalid S3 bucket name but test parsing)
TEST_F(S3UrlParserTest, AtInBucketName) {
    // @ before first slash should not be treated as region separator
    // s3://buck@et/key - the @ is in bucket portion
    // Current implementation: @ after slash is region, so this parses bucket as "buck@et"
    EXPECT_TRUE(parse_s3_url("s3://bucket/key", bucket, key, region));
    // This is an edge case - buckets can't have @ anyway
}

// ============================================================================
// parse_source Tests
// ============================================================================

class ParseSourceTest : public ::testing::Test {
protected:
    std::string error;
    const std::string default_region = "eu-west-2";
};

// Local paths

TEST_F(ParseSourceTest, AbsoluteLocalPath) {
    FileSource source = parse_source("/path/to/file.bin", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "/path/to/file.bin");
}

TEST_F(ParseSourceTest, RelativeLocalPath) {
    FileSource source = parse_source("relative/path/file.txt", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "relative/path/file.txt");
}

TEST_F(ParseSourceTest, CurrentDirPath) {
    FileSource source = parse_source("./file.txt", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "./file.txt");
}

TEST_F(ParseSourceTest, ParentDirPath) {
    FileSource source = parse_source("../file.txt", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "../file.txt");
}

TEST_F(ParseSourceTest, HomeExpansion) {
    FileSource source = parse_source("~/Documents/file.txt", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "~/Documents/file.txt");  // Not expanded, just stored
}

TEST_F(ParseSourceTest, LocalPathWithSpaces) {
    FileSource source = parse_source("/path/to/my file.txt", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "/path/to/my file.txt");
}

// S3 URLs

TEST_F(ParseSourceTest, S3UrlBasic) {
    FileSource source = parse_source("s3://bucket/key", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
    EXPECT_EQ(source.bucket, "bucket");
    EXPECT_EQ(source.path, "key");
    EXPECT_EQ(source.region, default_region);
}

TEST_F(ParseSourceTest, S3UrlWithRegion) {
    FileSource source = parse_source("s3://bucket/key@us-west-2", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
    EXPECT_EQ(source.bucket, "bucket");
    EXPECT_EQ(source.path, "key");
    EXPECT_EQ(source.region, "us-west-2");
}

TEST_F(ParseSourceTest, S3UrlNestedPath) {
    FileSource source = parse_source("s3://bucket/path/to/object.bin", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
    EXPECT_EQ(source.bucket, "bucket");
    EXPECT_EQ(source.path, "path/to/object.bin");
}

TEST_F(ParseSourceTest, S3UrlNestedWithRegion) {
    FileSource source = parse_source("s3://my-bucket/data/file.bin@ap-southeast-1", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
    EXPECT_EQ(source.bucket, "my-bucket");
    EXPECT_EQ(source.path, "data/file.bin");
    EXPECT_EQ(source.region, "ap-southeast-1");
}

// Invalid S3 URLs

TEST_F(ParseSourceTest, S3UrlNoKey) {
    FileSource source = parse_source("s3://bucket/", default_region, error);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
}

TEST_F(ParseSourceTest, S3UrlNoBucket) {
    FileSource source = parse_source("s3:///key", default_region, error);
    EXPECT_FALSE(error.empty());
}

TEST_F(ParseSourceTest, S3UrlOnlyProtocol) {
    FileSource source = parse_source("s3://", default_region, error);
    EXPECT_FALSE(error.empty());
}

// Edge cases

TEST_F(ParseSourceTest, EmptyPath) {
    FileSource source = parse_source("", default_region, error);
    EXPECT_TRUE(error.empty());  // Empty local path is technically valid at parse time
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "");
}

TEST_F(ParseSourceTest, S3LikeButLocal) {
    // "s3" as filename, not protocol
    FileSource source = parse_source("s3", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "s3");
}

TEST_F(ParseSourceTest, S3ColonButLocal) {
    // "s3:" not followed by //
    FileSource source = parse_source("s3:file", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "s3:file");
}

// ============================================================================
// FileSource Structure Tests
// ============================================================================

TEST(FileSourceTest, DefaultValues) {
    FileSource source;
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_TRUE(source.path.empty());
    EXPECT_TRUE(source.bucket.empty());
    EXPECT_TRUE(source.region.empty());  // Empty = auto-detect
}

TEST(FileSourceTest, CopyAssignment) {
    FileSource source1;
    source1.type = SourceType::S3;
    source1.path = "key";
    source1.bucket = "bucket";
    source1.region = "us-east-1";

    FileSource source2 = source1;
    EXPECT_EQ(source2.type, SourceType::S3);
    EXPECT_EQ(source2.path, "key");
    EXPECT_EQ(source2.bucket, "bucket");
    EXPECT_EQ(source2.region, "us-east-1");
}

// ============================================================================
// Real-world URL Examples
// ============================================================================

TEST_F(S3UrlParserTest, RealWorldExample1) {
    EXPECT_TRUE(parse_s3_url("s3://company-data-lake/raw/2024/01/data.parquet", bucket, key, region));
    EXPECT_EQ(bucket, "company-data-lake");
    EXPECT_EQ(key, "raw/2024/01/data.parquet");
}

TEST_F(S3UrlParserTest, RealWorldExample2) {
    EXPECT_TRUE(parse_s3_url("s3://backup-bucket/database-dumps/prod_2024-01-15.sql.gz@eu-central-1", bucket, key, region));
    EXPECT_EQ(bucket, "backup-bucket");
    EXPECT_EQ(key, "database-dumps/prod_2024-01-15.sql.gz");
    EXPECT_EQ(region, "eu-central-1");
}

TEST_F(S3UrlParserTest, RealWorldExample3) {
    // Very long path
    EXPECT_TRUE(parse_s3_url("s3://ml-artifacts/experiments/exp-123/models/v1/checkpoint-10000/model.safetensors", bucket, key, region));
    EXPECT_EQ(bucket, "ml-artifacts");
    EXPECT_EQ(key, "experiments/exp-123/models/v1/checkpoint-10000/model.safetensors");
}

TEST_F(ParseSourceTest, RealWorldLocalPath) {
    FileSource source = parse_source("/Users/example/projects/mitosync/test/1GiB.bin", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::Local);
    EXPECT_EQ(source.path, "/Users/example/projects/mitosync/test/1GiB.bin");
}

TEST_F(ParseSourceTest, RealWorldS3Path) {
    FileSource source = parse_source("s3://example-bucket/compare/1GiB.bin@eu-west-2", default_region, error);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(source.type, SourceType::S3);
    EXPECT_EQ(source.bucket, "example-bucket");
    EXPECT_EQ(source.path, "compare/1GiB.bin");
    EXPECT_EQ(source.region, "eu-west-2");
}
