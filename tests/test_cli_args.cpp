#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cli_args.h"

// Build an argv array from a vector of strings. The parsers take char* argv[],
// so the backing strings must outlive the call.
class Args {
public:
    explicit Args(std::vector<std::string> args) : storage_(std::move(args)) {
        pointers_.reserve(storage_.size() + 1);
        for (auto& s : storage_) pointers_.push_back(s.data());
        pointers_.push_back(nullptr);
    }
    int argc() const { return static_cast<int>(storage_.size()); }
    char** argv() { return pointers_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<char*> pointers_;
};

// ============================================================================
// wants_version
// ============================================================================

TEST(WantsVersionTest, AcceptsBothSpellingsInArgv1) {
    for (const char* flag : {"--version", "-V"}) {
        Args a({"mito", flag});
        EXPECT_TRUE(wants_version(a.argc(), a.argv())) << "for " << flag;
    }
}

TEST(WantsVersionTest, IgnoresItAnywhereButArgv1) {
    // `mito rm --version s3://bucket/key` is a typo, not a version request. Answering it
    // would print a version and exit 0 for a command line that asked to delete something.
    Args a({"mito", "rm", "--version", "s3://bucket/key"});
    EXPECT_FALSE(wants_version(a.argc(), a.argv()));

    Args b({"mito", "diff", "a", "-V"});
    EXPECT_FALSE(wants_version(b.argc(), b.argv()));
}

TEST(WantsVersionTest, HandlesNoArgumentsAndNearMisses) {
    Args none({"mito"});
    EXPECT_FALSE(wants_version(none.argc(), none.argv()));

    // Lower-case -v is --verbose and must stay that way.
    for (const char* flag : {"-v", "--versions", "version", "--ver"}) {
        Args a({"mito", flag});
        EXPECT_FALSE(wants_version(a.argc(), a.argv())) << "for " << flag;
    }
}

// ============================================================================
// detect_subcommand
// ============================================================================

TEST(DetectSubcommandTest, RecognisesEverySubcommand) {
    const std::pair<const char*, SubCommand> cases[] = {
        {"diff", SubCommand::Diff},
        {"sync", SubCommand::Sync},
        {"rm", SubCommand::Rm},
        {"stats", SubCommand::Stats},
        {"leftovers", SubCommand::Leftovers},
    };
    for (const auto& [word, expected] : cases) {
        Args a({"mito", word});
        EXPECT_EQ(detect_subcommand(a.argc(), a.argv()), expected) << "for " << word;
    }
}

TEST(DetectSubcommandTest, NoArgumentsIsNone) {
    Args a({"mito"});
    EXPECT_EQ(detect_subcommand(a.argc(), a.argv()), SubCommand::None);
}

TEST(DetectSubcommandTest, UnknownWordFallsBackToLegacyDiffMode) {
    Args a({"mito", "/some/path"});
    EXPECT_EQ(detect_subcommand(a.argc(), a.argv()), SubCommand::None);
}

TEST(DetectSubcommandTest, IsCaseSensitive) {
    Args a({"mito", "SYNC"});
    EXPECT_EQ(detect_subcommand(a.argc(), a.argv()), SubCommand::None);
}

TEST(DetectSubcommandTest, OnlyInspectsTheFirstArgument) {
    // "sync" in second position is a source path, not a subcommand.
    Args a({"mito", "diff", "sync"});
    EXPECT_EQ(detect_subcommand(a.argc(), a.argv()), SubCommand::Diff);
}

// ============================================================================
// parse_args (diff / default command)
// ============================================================================

TEST(ParseArgsTest, DefaultsAreSane) {
    Args a({"mito"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_FALSE(o.error);
    EXPECT_FALSE(o.help);
    EXPECT_FALSE(o.has_source_a);
    EXPECT_FALSE(o.has_source_b);
    EXPECT_EQ(o.num_threads, 1024);
    EXPECT_TRUE(o.parallel_discovery);
    EXPECT_EQ(o.parallel_discovery_workers, 128);
}

TEST(ParseArgsTest, TwoLocalSources) {
    Args a({"mito", "/tmp/a", "/tmp/b"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.has_source_a);
    EXPECT_TRUE(o.has_source_b);
    EXPECT_EQ(o.source_a.type, SourceType::Local);
    EXPECT_EQ(o.source_b.type, SourceType::Local);
    EXPECT_EQ(o.source_a.path, "/tmp/a");
    EXPECT_EQ(o.source_b.path, "/tmp/b");
}

TEST(ParseArgsTest, ThirdSourceIsRejected) {
    Args a({"mito", "/tmp/a", "/tmp/b", "/tmp/c"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Too many sources"), std::string::npos);
}

TEST(ParseArgsTest, ShortAndLongFlagsAgree) {
    Args shortf({"mito", "-d", "-q", "-v", "-D", "-r", "-P"});
    CliOptions s = parse_args(shortf.argc(), shortf.argv());
    Args longf({"mito", "--debug", "--quiet", "--verbose", "--directory",
                "--ramp-up", "--parallel-discovery"});
    CliOptions l = parse_args(longf.argc(), longf.argv());

    EXPECT_EQ(s.debug, l.debug);
    EXPECT_EQ(s.quiet, l.quiet);
    EXPECT_EQ(s.verbose, l.verbose);
    EXPECT_EQ(s.directory_mode, l.directory_mode);
    EXPECT_EQ(s.ramp_up, l.ramp_up);
    EXPECT_EQ(s.parallel_discovery, l.parallel_discovery);
    EXPECT_TRUE(s.debug && s.quiet && s.verbose && s.directory_mode && s.ramp_up);
}

TEST(ParseArgsTest, NoParallelDiscoveryDisablesIt) {
    Args a({"mito", "--no-parallel-discovery"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_FALSE(o.error);
    EXPECT_FALSE(o.parallel_discovery);
}

TEST(ParseArgsTest, ThreadsAcceptsZeroButNotNegative) {
    // parse_args uses < 0 (unlike sync/rm which require >= 1)
    Args zero({"mito", "--threads", "0"});
    CliOptions z = parse_args(zero.argc(), zero.argv());
    EXPECT_FALSE(z.error);
    EXPECT_EQ(z.num_threads, 0);

    Args neg({"mito", "--threads", "-1"});
    CliOptions n = parse_args(neg.argc(), neg.argv());
    EXPECT_TRUE(n.error);
}

TEST(ParseArgsTest, ThreadsRejectsNonNumericAndMissingValue) {
    Args bad({"mito", "--threads", "lots"});
    EXPECT_TRUE(parse_args(bad.argc(), bad.argv()).error);

    Args missing({"mito", "--threads"});
    CliOptions m = parse_args(missing.argc(), missing.argv());
    EXPECT_TRUE(m.error);
    EXPECT_NE(m.error_message.find("requires a number"), std::string::npos);
}

TEST(ParseArgsTest, DiscoveryWorkersEnforcesOneTo128) {
    for (const char* v : {"1", "64", "128"}) {
        Args a({"mito", "--parallel-discovery-workers", v});
        CliOptions o = parse_args(a.argc(), a.argv());
        EXPECT_FALSE(o.error) << "value " << v;
    }
    for (const char* v : {"0", "129"}) {
        Args a({"mito", "--parallel-discovery-workers", v});
        CliOptions o = parse_args(a.argc(), a.argv());
        EXPECT_TRUE(o.error) << "value " << v;
        EXPECT_NE(o.error_message.find("1-128"), std::string::npos);
    }
}

TEST(ParseArgsTest, OutputFileIsCaptured) {
    Args a({"mito", "-o", "results.json"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_FALSE(o.error);
    EXPECT_EQ(o.output_file, "results.json");

    Args missing({"mito", "--output"});
    EXPECT_TRUE(parse_args(missing.argc(), missing.argv()).error);
}

TEST(ParseArgsTest, UnknownOptionIsRejected) {
    Args a({"mito", "--nonsense"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Unknown option"), std::string::npos);
}

// The GUI was removed from main; -g/--gui must no longer be accepted.
TEST(ParseArgsTest, GuiFlagIsNoLongerAccepted) {
    for (const char* flag : {"-g", "--gui"}) {
        Args a({"mito", flag});
        CliOptions o = parse_args(a.argc(), a.argv());
        EXPECT_TRUE(o.error) << flag << " should be rejected";
        EXPECT_NE(o.error_message.find("Unknown option"), std::string::npos);
    }
}

TEST(ParseArgsTest, ProfileAppliesToS3SidesOnly) {
    Args a({"mito", "s3://bucket-a/x", "s3://bucket-b/y",
            "--source-profile", "acctA", "--dest-profile", "acctB"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.source_a.profile, "acctA");
    EXPECT_EQ(o.source_b.profile, "acctB");
}

TEST(ParseArgsTest, ProfileIsIgnoredForLocalSources) {
    Args a({"mito", "/tmp/a", "/tmp/b", "--source-profile", "acctA", "--dest-profile", "acctB"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.source_a.profile.empty());
    EXPECT_TRUE(o.source_b.profile.empty());
}

TEST(ParseArgsTest, ProfileFlagsRequireAValue) {
    Args s({"mito", "--source-profile"});
    EXPECT_TRUE(parse_args(s.argc(), s.argv()).error);
    Args d({"mito", "--dest-profile"});
    EXPECT_TRUE(parse_args(d.argc(), d.argv()).error);
}

TEST(ParseArgsTest, S3UrlWithExplicitRegionIsParsed) {
    Args a({"mito", "s3://my-bucket/key@eu-west-1", "/tmp/b"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.source_a.type, SourceType::S3);
    EXPECT_EQ(o.source_a.bucket, "my-bucket");
    EXPECT_EQ(o.source_a.region, "eu-west-1");
}

TEST(ParseArgsTest, FlagsMayFollowPositionals) {
    Args a({"mito", "/tmp/a", "/tmp/b", "--debug"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.debug);
    EXPECT_TRUE(o.has_source_b);
}

// ============================================================================
// parse_sync_args
// ============================================================================

TEST(ParseSyncArgsTest, LocalThenS3IsUpload) {
    Args a({"mito", "sync", "/data/", "s3://bucket/backup/"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Upload);
    EXPECT_TRUE(o.has_local);
    EXPECT_TRUE(o.has_s3_source);
    EXPECT_EQ(o.local_path, "/data/");
    EXPECT_EQ(o.s3_source.bucket, "bucket");
}

TEST(ParseSyncArgsTest, S3ThenLocalIsDownload) {
    Args a({"mito", "sync", "s3://bucket/backup/", "/data/"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Download);
    EXPECT_TRUE(o.has_local);
    EXPECT_TRUE(o.has_s3_source);
}

TEST(ParseSyncArgsTest, S3ThenS3IsServerSideCopy) {
    Args a({"mito", "sync", "s3://src/x/", "s3://dst/y/"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::S3ToS3);
    EXPECT_TRUE(o.has_s3_source);
    EXPECT_TRUE(o.has_s3_dest);
    EXPECT_EQ(o.s3_source.bucket, "src");
    EXPECT_EQ(o.s3_dest.bucket, "dst");
}

TEST(ParseSyncArgsTest, S3PrefixIsCaseInsensitive) {
    Args a({"mito", "sync", "S3://bucket/x/", "/data/"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Download);
    EXPECT_TRUE(o.has_s3_source);
}

TEST(ParseSyncArgsTest, LocalThenLocalIsRejected) {
    Args a({"mito", "sync", "/data/", "/other/"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("must be an S3 URL"), std::string::npos);
}

TEST(ParseSyncArgsTest, ThirdPositionalIsRejected) {
    Args a({"mito", "sync", "/data/", "s3://bucket/x/", "extra"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Too many arguments"), std::string::npos);
}

TEST(ParseSyncArgsTest, DeleteAndDryRunFlags) {
    Args a({"mito", "sync", "/data/", "s3://bucket/x/", "--delete", "--dry-run"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.delete_orphans);
    EXPECT_TRUE(o.dry_run);
}

TEST(ParseSyncArgsTest, ThreadsMustBeAtLeastOne) {
    Args zero({"mito", "sync", "--threads", "0"});
    SyncCliOptions z = parse_sync_args(zero.argc(), zero.argv());
    EXPECT_TRUE(z.error);
    EXPECT_NE(z.error_message.find("at least 1"), std::string::npos);

    Args ok({"mito", "sync", "--threads", "8"});
    SyncCliOptions o = parse_sync_args(ok.argc(), ok.argv());
    EXPECT_FALSE(o.error);
    EXPECT_EQ(o.num_threads, 8);
}

TEST(ParseSyncArgsTest, DefaultThreadsIs256) {
    Args a({"mito", "sync"});
    EXPECT_EQ(parse_sync_args(a.argc(), a.argv()).num_threads, 256);
}

TEST(ParseSyncArgsTest, UploadUsesDestProfileForTheS3Side) {
    // Local -> S3: the S3 endpoint is the destination.
    Args a({"mito", "sync", "/data/", "s3://bucket/x/", "--dest-profile", "acctB"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Upload);
    EXPECT_EQ(o.s3_source.profile, "acctB");
}

TEST(ParseSyncArgsTest, DownloadUsesSourceProfileForTheS3Side) {
    Args a({"mito", "sync", "s3://bucket/x/", "/data/", "--source-profile", "acctA"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Download);
    EXPECT_EQ(o.s3_source.profile, "acctA");
}

TEST(ParseSyncArgsTest, S3ToS3UsesBothProfiles) {
    Args a({"mito", "sync", "s3://src/x/", "s3://dst/y/",
            "--source-profile", "acctA", "--dest-profile", "acctB"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.s3_source.profile, "acctA");
    EXPECT_EQ(o.s3_dest.profile, "acctB");
}

TEST(ParseSyncArgsTest, UnknownOptionIsRejected) {
    Args a({"mito", "sync", "--recursive"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Unknown option"), std::string::npos);
}

TEST(ParseSyncArgsTest, HelpShortCircuitsNothingButIsRecorded) {
    Args a({"mito", "sync", "--help"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    EXPECT_TRUE(o.help);
    EXPECT_FALSE(o.error);
}

// ============================================================================
// parse_rm_args
// ============================================================================

TEST(ParseRmArgsTest, ParsesBucketPrefixAndRegion) {
    Args a({"mito", "rm", "s3://my-bucket/old/data/@us-east-1"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.bucket, "my-bucket");
    EXPECT_EQ(o.prefix, "old/data/");
    EXPECT_EQ(o.region, "us-east-1");
}

TEST(ParseRmArgsTest, LocalPathIsRejected) {
    Args a({"mito", "rm", "/tmp/local"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Invalid S3 URL"), std::string::npos);
}

TEST(ParseRmArgsTest, ForceAndRecursiveFlags) {
    Args a({"mito", "rm", "s3://b/p/", "--recursive", "--force"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.recursive);
    EXPECT_TRUE(o.force);
}

TEST(ParseRmArgsTest, CombinedShortFlagsSetBoth) {
    for (const char* combo : {"-rf", "-fr"}) {
        Args a({"mito", "rm", "s3://b/p/", combo});
        RmCliOptions o = parse_rm_args(a.argc(), a.argv());
        ASSERT_FALSE(o.error) << combo << ": " << o.error_message;
        EXPECT_TRUE(o.recursive) << combo;
        EXPECT_TRUE(o.force) << combo;
    }
}

TEST(ParseRmArgsTest, ForceDefaultsOffSoDeletionIsOptIn) {
    Args a({"mito", "rm", "s3://b/p/"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_FALSE(o.force);
    EXPECT_FALSE(o.recursive);
}

TEST(ParseRmArgsTest, BatchFlagIsAccepted) {
    Args a({"mito", "rm", "s3://b/p/", "--batch"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.batch);
}

TEST(ParseRmArgsTest, SecondUrlIsRejected) {
    Args a({"mito", "rm", "s3://b/p/", "s3://b/q/"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Too many arguments"), std::string::npos);
}

TEST(ParseRmArgsTest, ThreadsMustBeAtLeastOne) {
    Args a({"mito", "rm", "s3://b/p/", "--threads", "0"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
}

TEST(ParseRmArgsTest, UnknownOptionIsRejected) {
    Args a({"mito", "rm", "s3://b/p/", "--delete"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("Unknown option"), std::string::npos);
}

// ============================================================================
// --endpoint-url (S3-compatible gateways: Storj, MinIO, Ceph)
// ============================================================================

TEST(EndpointUrlTest, DiffAppliesTheEndpointToBothS3Sources) {
    Args a({"mito", "s3://bucket-a/x", "s3://bucket-b/y",
            "--endpoint-url", "https://gateway.storjshare.io"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.endpoint_url, "https://gateway.storjshare.io");
    EXPECT_EQ(o.source_a.endpoint, "https://gateway.storjshare.io");
    EXPECT_EQ(o.source_b.endpoint, "https://gateway.storjshare.io");
}

TEST(EndpointUrlTest, DiffLeavesLocalSourcesAlone) {
    Args a({"mito", "/tmp/a", "s3://bucket/y", "--endpoint-url", "https://example.test"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.source_a.endpoint.empty()) << "a local path has no endpoint";
    EXPECT_EQ(o.source_b.endpoint, "https://example.test");
}

TEST(EndpointUrlTest, AbsentFlagLeavesEveryEndpointEmpty) {
    Args a({"mito", "s3://bucket-a/x", "s3://bucket-b/y"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_TRUE(o.endpoint_url.empty());
    EXPECT_TRUE(o.source_a.endpoint.empty());
    EXPECT_TRUE(o.source_b.endpoint.empty());
}

TEST(EndpointUrlTest, DiffRequiresAValue) {
    Args a({"mito", "--endpoint-url"});
    CliOptions o = parse_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("requires a URL"), std::string::npos);
}

TEST(EndpointUrlTest, SyncUploadAppliesItToTheS3Side) {
    Args a({"mito", "sync", "/data/", "s3://bucket/x/",
            "--endpoint-url", "https://gateway.storjshare.io"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.direction, SyncDirection::Upload);
    EXPECT_EQ(o.s3_source.endpoint, "https://gateway.storjshare.io");
}

TEST(EndpointUrlTest, SyncS3ToS3AppliesItToBothSides) {
    Args a({"mito", "sync", "s3://src/x/", "s3://dst/y/",
            "--endpoint-url", "https://gateway.storjshare.io"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.s3_source.endpoint, "https://gateway.storjshare.io");
    EXPECT_EQ(o.s3_dest.endpoint, "https://gateway.storjshare.io");
}

TEST(EndpointUrlTest, SyncCombinesEndpointWithPerSideProfiles) {
    // One gateway host, two credential sets: the flags are independent.
    Args a({"mito", "sync", "s3://src/x/", "s3://dst/y/",
            "--endpoint-url", "https://gw.test",
            "--source-profile", "acctA", "--dest-profile", "acctB"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.s3_source.endpoint, "https://gw.test");
    EXPECT_EQ(o.s3_dest.endpoint, "https://gw.test");
    EXPECT_EQ(o.s3_source.profile, "acctA");
    EXPECT_EQ(o.s3_dest.profile, "acctB");
}

TEST(EndpointUrlTest, SyncRequiresAValue) {
    Args a({"mito", "sync", "--endpoint-url"});
    SyncCliOptions o = parse_sync_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
    EXPECT_NE(o.error_message.find("requires a URL"), std::string::npos);
}

TEST(EndpointUrlTest, RmAcceptsTheFlag) {
    Args a({"mito", "rm", "s3://bucket/prefix/", "--endpoint-url", "https://gw.test"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.endpoint_url, "https://gw.test");
    EXPECT_EQ(o.bucket, "bucket");
}

TEST(EndpointUrlTest, RmRequiresAValue) {
    Args a({"mito", "rm", "s3://b/p/", "--endpoint-url"});
    RmCliOptions o = parse_rm_args(a.argc(), a.argv());
    EXPECT_TRUE(o.error);
}

TEST(EndpointUrlTest, EndpointIsPreservedAlongsideAnExplicitRegion) {
    // @region still wins where the user pins it.
    Args a({"mito", "s3://bucket/x@eu-west-1", "/tmp/b", "--endpoint-url", "https://gw.test"});
    CliOptions o = parse_args(a.argc(), a.argv());
    ASSERT_FALSE(o.error) << o.error_message;
    EXPECT_EQ(o.source_a.region, "eu-west-1");
    EXPECT_EQ(o.source_a.endpoint, "https://gw.test");
}
