#include "cli_args.h"

#include <cctype>
#include <exception>
#include <string>

#include "url_parser.h"

bool wants_version(int argc, char* argv[]) {
    if (argc < 2) return false;
    std::string arg = argv[1];
    return arg == "--version" || arg == "-V";
}

SubCommand detect_subcommand(int argc, char* argv[]) {
    if (argc < 2) return SubCommand::None;
    std::string arg = argv[1];
    if (arg == "diff") return SubCommand::Diff;
    if (arg == "sync") return SubCommand::Sync;
    if (arg == "rm") return SubCommand::Rm;
    if (arg == "stats") return SubCommand::Stats;
    if (arg == "leftovers") return SubCommand::Leftovers;
    return SubCommand::None;  // Legacy: treat as diff
}

SyncCliOptions parse_sync_args(int argc, char* argv[]) {
    SyncCliOptions opts;

    // Skip program name and "sync" subcommand
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
        } else if (arg == "-d" || arg == "--debug") {
            opts.debug = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "--allow-unverified-ranges") {
            opts.allow_unverified_ranges = true;
        } else if (arg == "--delete") {
            opts.delete_orphans = true;
        } else if (arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 >= argc) {
                opts.error = true;
                opts.error_message = "--threads requires a number argument";
                return opts;
            }
            try {
                opts.num_threads = std::stoi(argv[++i]);
                if (opts.num_threads < 1) {
                    opts.error = true;
                    opts.error_message = "--threads must be at least 1";
                    return opts;
                }
            } catch (const std::exception&) {
                opts.error = true;
                opts.error_message = "--threads requires a valid number";
                return opts;
            }
        } else if (arg == "--source-profile") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--source-profile requires a profile name"; return opts; }
            opts.source_profile = argv[++i];
        } else if (arg == "--dest-profile") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--dest-profile requires a profile name"; return opts; }
            opts.dest_profile = argv[++i];
        } else if (arg == "--endpoint-url") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--endpoint-url requires a URL"; return opts; }
            opts.endpoint_url = argv[++i];
        } else if (arg[0] == '-') {
            opts.error = true;
            opts.error_message = "Unknown option: " + arg;
            return opts;
        } else {
            // Positional argument - detect type (local vs S3)
            // Case-insensitive check for s3:// prefix
            bool is_s3 = (arg.size() >= 5 &&
                          std::tolower(static_cast<unsigned char>(arg[0])) == 's' &&
                          std::tolower(static_cast<unsigned char>(arg[1])) == '3' &&
                          arg[2] == ':' && arg[3] == '/' && arg[4] == '/');

            if (!opts.has_local && !opts.has_s3_source) {
                // First positional argument
                if (is_s3) {
                    std::string parse_error;
                    opts.s3_source = parse_source(arg, "", parse_error);
                    if (!parse_error.empty()) {
                        opts.error = true;
                        opts.error_message = parse_error;
                        return opts;
                    }
                    opts.has_s3_source = true;
                    opts.direction = SyncDirection::Download;  // S3 first = download (may change to S3ToS3)
                } else {
                    opts.local_path = arg;
                    opts.has_local = true;
                    opts.direction = SyncDirection::Upload;    // Local first = upload
                }
            } else if (opts.has_local && !opts.has_s3_source) {
                // Second argument, first was local -> expect S3 destination
                if (!is_s3) {
                    opts.error = true;
                    opts.error_message = "Second argument must be an S3 URL (s3://bucket/prefix/)";
                    return opts;
                }
                std::string parse_error;
                opts.s3_source = parse_source(arg, "", parse_error);  // Acts as destination for upload
                if (!parse_error.empty()) {
                    opts.error = true;
                    opts.error_message = parse_error;
                    return opts;
                }
                opts.has_s3_source = true;
            } else if (opts.has_s3_source && !opts.has_local && !opts.has_s3_dest) {
                // Second argument, first was S3 -> could be local or S3 (S3-to-S3)
                if (is_s3) {
                    // S3-to-S3 sync
                    std::string parse_error;
                    opts.s3_dest = parse_source(arg, "", parse_error);
                    if (!parse_error.empty()) {
                        opts.error = true;
                        opts.error_message = parse_error;
                        return opts;
                    }
                    opts.has_s3_dest = true;
                    opts.direction = SyncDirection::S3ToS3;
                } else {
                    opts.local_path = arg;
                    opts.has_local = true;
                    // direction stays Download
                }
            } else {
                opts.error = true;
                opts.error_message = "Too many arguments";
                return opts;
            }
        }
    }

    // Direction is final now; map the per-side profile flags onto the correct
    // S3 FileSource(s). s3_source is overloaded (see apply_sync_profiles).
    apply_sync_profiles(opts.direction, opts.source_profile, opts.dest_profile,
                        opts.s3_source, opts.s3_dest);

    // One endpoint serves every S3 side: an S3-compatible gateway is a single
    // host, unlike credentials which legitimately differ per account.
    if (!opts.endpoint_url.empty()) {
        if (opts.s3_source.type == SourceType::S3) opts.s3_source.endpoint = opts.endpoint_url;
        if (opts.s3_dest.type == SourceType::S3) opts.s3_dest.endpoint = opts.endpoint_url;
    }

    return opts;
}

RmCliOptions parse_rm_args(int argc, char* argv[]) {
    RmCliOptions opts;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
        } else if (arg == "-r" || arg == "--recursive") {
            opts.recursive = true;
        } else if (arg == "-f" || arg == "--force") {
            opts.force = true;
        } else if (arg == "-q" || arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 >= argc) {
                opts.error = true;
                opts.error_message = "--threads requires a number";
                return opts;
            }
            try {
                opts.num_threads = std::stoi(argv[++i]);
                if (opts.num_threads < 1) {
                    opts.error = true;
                    opts.error_message = "--threads must be at least 1";
                    return opts;
                }
            } catch (const std::exception&) {
                opts.error = true;
                opts.error_message = "--threads requires a valid number";
                return opts;
            }
        } else if (arg == "-rf" || arg == "-fr") {
            opts.recursive = true;
            opts.force = true;
        } else if (arg == "--endpoint-url") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--endpoint-url requires a URL"; return opts; }
            opts.endpoint_url = argv[++i];
        } else if (arg == "--batch") {
            opts.batch = true;
        } else if (arg[0] == '-') {
            opts.error = true;
            opts.error_message = "Unknown option: " + arg;
            return opts;
        } else {
            // Positional - S3 URL
            if (opts.s3_url.empty()) {
                opts.s3_url = arg;
                std::string parse_error;
                FileSource source = parse_source(arg, "", parse_error);
                if (!parse_error.empty() || source.type != SourceType::S3) {
                    opts.error = true;
                    opts.error_message = "Invalid S3 URL: " + arg;
                    return opts;
                }
                opts.bucket = source.bucket;
                opts.prefix = source.path;
                opts.region = source.region;
            } else {
                opts.error = true;
                opts.error_message = "Too many arguments";
                return opts;
            }
        }
    }

    return opts;
}

CliOptions parse_args(int argc, char* argv[]) {
    CliOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
        } else if (arg == "-d" || arg == "--debug") {
            opts.debug = true;
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 >= argc) {
                opts.error = true;
                opts.error_message = "--threads requires a number argument";
                return opts;
            }
            try {
                opts.num_threads = std::stoi(argv[++i]);
                if (opts.num_threads < 0) {
                    opts.error = true;
                    opts.error_message = "--threads must be non-negative";
                    return opts;
                }
            } catch (const std::exception&) {
                opts.error = true;
                opts.error_message = "--threads requires a valid number";
                return opts;
            }
        } else if (arg == "-r" || arg == "--ramp-up") {
            opts.ramp_up = true;
        } else if (arg == "--allow-unverified-ranges") {
            opts.allow_unverified_ranges = true;
        } else if (arg == "-D" || arg == "--directory") {
            opts.directory_mode = true;
        } else if (arg == "-q" || arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-P" || arg == "--parallel-discovery") {
            opts.parallel_discovery = true;
        } else if (arg == "--no-parallel-discovery") {
            opts.parallel_discovery = false;
        } else if (arg == "--parallel-discovery-workers") {
            if (i + 1 >= argc) {
                opts.error = true;
                opts.error_message = "--parallel-discovery-workers requires a number argument";
                return opts;
            }
            try {
                opts.parallel_discovery_workers = std::stoi(argv[++i]);
                if (opts.parallel_discovery_workers < 1 || opts.parallel_discovery_workers > 128) {
                    opts.error = true;
                    opts.error_message = "--parallel-discovery-workers must be 1-128";
                    return opts;
                }
            } catch (const std::exception&) {
                opts.error = true;
                opts.error_message = "--parallel-discovery-workers requires a valid number";
                return opts;
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                opts.error = true;
                opts.error_message = "--output requires a filename argument";
                return opts;
            }
            opts.output_file = argv[++i];
        } else if (arg == "--source-profile") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--source-profile requires a profile name"; return opts; }
            opts.source_profile = argv[++i];
        } else if (arg == "--dest-profile") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--dest-profile requires a profile name"; return opts; }
            opts.dest_profile = argv[++i];
        } else if (arg == "--endpoint-url") {
            if (i + 1 >= argc) { opts.error = true; opts.error_message = "--endpoint-url requires a URL"; return opts; }
            opts.endpoint_url = argv[++i];
        } else if (arg[0] == '-') {
            opts.error = true;
            opts.error_message = "Unknown option: " + arg;
            return opts;
        } else {
            // Positional argument - source
            // Pass empty default_region so we can auto-detect later
            std::string parse_error;
            FileSource source = parse_source(arg, "", parse_error);

            if (!parse_error.empty()) {
                opts.error = true;
                opts.error_message = parse_error;
                return opts;
            }

            if (!opts.has_source_a) {
                opts.source_a = source;
                opts.has_source_a = true;
            } else if (!opts.has_source_b) {
                opts.source_b = source;
                opts.has_source_b = true;
            } else {
                opts.error = true;
                opts.error_message = "Too many sources specified (expected 2)";
                return opts;
            }
        }
    }

    // Apply per-side profile flags onto the parsed sources (S3 sides only;
    // empty flag = leave as-is).
    if (!opts.source_profile.empty() && opts.source_a.type == SourceType::S3)
        opts.source_a.profile = opts.source_profile;
    if (!opts.dest_profile.empty() && opts.source_b.type == SourceType::S3)
        opts.source_b.profile = opts.dest_profile;

    // One endpoint serves every S3 source (see parse_sync_args).
    if (!opts.endpoint_url.empty()) {
        if (opts.source_a.type == SourceType::S3) opts.source_a.endpoint = opts.endpoint_url;
        if (opts.source_b.type == SourceType::S3) opts.source_b.endpoint = opts.endpoint_url;
    }

    return opts;
}
