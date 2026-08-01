#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetBucketLocationRequest.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <thread>
#include <future>

#include "app_settings.h"
#include "cli_args.h"
#include "report_writer.h"
#include "aws_utils.h"
#include "s3_utils.h"
#include "crc32_hw.h"
#include "defaults.h"
#include "fd_limits.h"
#include "comparison_task.h"
#include "directory_comparison.h"
#include "url_parser.h"
#include "cached_dns_http_client.h"
#include "cloud_monitoring.h"
#include "cloud_metrics.h"
#include "aws_pricing.h"
#include "sync_task.h"
#include "rm_task.h"
#include "leftovers_task.h"
#include "duration_parse.h"

#ifdef _WIN32
#include <stdlib.h>
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

// Forward declarations
static std::string detect_bucket_region(const std::string& bucket,
                                        const std::string& profile = "",
                                        const std::string& endpoint = "");


// Track whether we've already received a signal (for double-signal detection)
static std::atomic<bool> g_signal_received{false};

// Signal handler for graceful cleanup on SIGINT/SIGTERM
// IMPORTANT: Only async-signal-safe operations here. We cannot:
// - Acquire mutexes (deadlock if signal interrupts code holding the mutex)
// - Call logging functions (not async-signal-safe)
// - Make network calls (not async-signal-safe)
static void signal_handler(int signum) {
    if (g_signal_received.exchange(true)) {
        // Second signal - force exit immediately
        std::_Exit(128 + signum);
    }
    // First signal - request shutdown via shared flag
    // Worker threads will check IsShutdownRequested() and stop processing
    // Main thread will call abort_all() after operations complete
    RequestShutdown();
}

// Set by CMake from project(mito VERSION ...). The fallback exists only so the file still
// compiles outside the project's own build (a scratch translation unit, a fuzz harness);
// a real build always defines it.
#ifndef MITO_VERSION
#define MITO_VERSION "unknown"
#endif

static void print_version() {
    std::cout << "mito " << MITO_VERSION << "\n";
}

static void print_usage(const char* program, SubCommand cmd = SubCommand::None) {
    if (cmd == SubCommand::Sync) {
        std::cout << "Usage: " << program << " sync <source> <destination> [OPTIONS]\n"
                  << "\n"
                  << "Synchronize files between local directory and S3.\n"
                  << "Direction is determined by argument order: source first, destination second.\n"
                  << "\n"
                  << "A file is skipped only when the sizes match AND the destination is newer\n"
                  << "than the source. Everything else transfers. No checksum is read to decide.\n"
                  << "Large files (>=8MB) use differential sync, transferring only changed chunks.\n"
                  << "\n"
                  << "Arguments:\n"
                  << "  source         Local path or S3 URL (s3://bucket/prefix/)\n"
                  << "  destination    Local path or S3 URL (s3://bucket/prefix/)\n"
                  << "\n"
                  << "Options:\n"
                  << "  --delete       Delete destination files not present at source\n"
                  << "  --dry-run      Preview changes without executing\n"
                  << "  -t, --threads <N>   Max concurrent operations (default: 256)\n"
                  << "  --source-profile <name>  AWS profile for the S3 source (S3->S3 / download)\n"
                  << "  --dest-profile <name>    AWS profile for the S3 destination (S3->S3 / upload)\n"
                  << "  --endpoint-url <url>     S3-compatible endpoint (e.g. Storj, MinIO)\n"
                  << "  --allow-unverified-ranges  Accept ranged reads whose response omits\n"
                  << "                             Content-Range (see docs; weakens a safety check)\n"
                  << "  -q, --quiet    Minimal output\n"
                  << "  -v, --verbose  Show detailed progress\n"
                  << "  -d, --debug    Enable debug logging\n"
                  << "  -h, --help     Show this help message\n"
                  << "\n"
                  << "Examples:\n"
                  << "  " << program << " sync ./data/ s3://my-bucket/backup/           # Upload to S3\n"
                  << "  " << program << " sync s3://my-bucket/backup/ ./data/           # Download from S3\n"
                  << "  " << program << " sync ./data/ s3://my-bucket/backup/ --delete  # Upload with delete\n"
                  << "  " << program << " sync s3://my-bucket/backup/ ./data/ --dry-run # Preview download\n";
        return;
    }

    std::cout << "Usage: " << program << " [COMMAND] [OPTIONS]\n"
              << "\n"
              << "Commands:\n"
              << "  diff       Compare files/directories between sources (default)\n"
              << "  sync       Synchronize local directory to S3\n"
              << "  rm         Delete S3 objects\n"
              << "  stats      Show S3 API usage and cost estimates\n"
              << "  leftovers  Find and abort incomplete multipart uploads\n"
              << "\n"
              << "For command-specific help: " << program << " <command> --help\n"
              << "\n"
              << "Diff usage: " << program << " [diff] <source1> <source2> [OPTIONS]\n"
              << "\n"
              << "Compare CRC32 checksums between two files or directories (local or S3).\n"
              << "\n"
              << "Sources can be:\n"
              << "  /path/to/file           - Local file\n"
              << "  /path/to/directory/     - Local directory (recursive comparison)\n"
              << "  s3://bucket/key         - S3 object (region auto-detected)\n"
              << "  s3://bucket/prefix/     - S3 prefix (recursive comparison)\n"
              << "  s3://bucket/key@region  - S3 object with explicit region\n"
              << "\n"
              << "Options:\n"
              << "  -D, --directory     Force directory comparison mode\n"
              << "  -o, --output <file> Write results to file (supports .json, .csv, .txt)\n"
              << "  --source-profile <name>  AWS profile for source1 (if S3)\n"
              << "  --dest-profile <name>    AWS profile for source2 (if S3)\n"
              << "  --endpoint-url <url>     S3-compatible endpoint (e.g. Storj, MinIO);\n"
              << "                           enables path-style addressing\n"
              << "  --allow-unverified-ranges  Accept ranged reads whose response omits\n"
              << "                           Content-Range (see docs; weakens a safety check)\n"
              << "  -q, --quiet         Minimal output (only errors and summary)\n"
              << "  -v, --verbose       Show retry warnings (hidden by default)\n"
              << "  -d, --debug         Enable debug logging\n"
              << "  -t, --threads <N>   Max threads (default: 1024, concurrency adapts to throughput)\n"
              << "  -r, --ramp-up       Gradually ramp up concurrency (helps with DNS issues)\n"
              << "  -P, --parallel-discovery          Use parallel BFS directory enumeration (default: on)\n"
              << "  --no-parallel-discovery           Disable parallel discovery (use sequential)\n"
              << "  --parallel-discovery-workers <N>  Number of parallel workers (1-128, default: 128)\n"
              << "  -h, --help          Show this help message\n"
              << "  -V, --version       Show the version and exit\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " /path/to/dir1/ /path/to/dir2/              # Directory comparison\n"
              << "  " << program << " /path/to/file s3://my-bucket/path/to/key   # Local vs S3\n"
              << "  " << program << " diff dir1/ s3://bucket/prefix/ -o results.json  # Explicit diff\n"
              << "  " << program << " sync local/ s3://bucket/backup/            # Sync to S3\n";
}


static void print_rm_usage(const char* program) {
    std::cout << "Usage: " << program << " rm <s3-url> [OPTIONS]\n"
              << "\n"
              << "Delete S3 objects.\n"
              << "\n"
              << "Arguments:\n"
              << "  s3-url              S3 path (s3://bucket/key or s3://bucket/prefix/)\n"
              << "\n"
              << "Options:\n"
              << "  -r, --recursive     Delete all objects under prefix\n"
              << "  -f, --force         Actually delete (required)\n"
              << "  --endpoint-url <url> S3-compatible endpoint (e.g. Storj, MinIO)\n"
              << "  -t, --threads <N>   Max concurrent deletions (default: 256)\n"
              << "  --batch             Use batch DeleteObjects API (faster but can hit rate limits)\n"
              << "  -q, --quiet         Minimal output\n"
              << "  -v, --verbose       Show each deleted object\n"
              << "  -h, --help          Show this help\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " rm s3://bucket/file.txt -f\n"
              << "  " << program << " rm s3://bucket/prefix/ -rf\n"
              << "  " << program << " rm s3://bucket/prefix/       # dry-run\n";
}

static void print_leftovers_usage(const char* program) {
    std::cout << "Usage: " << program << " leftovers <bucket> [OPTIONS]\n"
              << "\n"
              << "List and optionally abort active multipart uploads.\n"
              << "\n"
              << "Arguments:\n"
              << "  <bucket>              S3 bucket name or s3://bucket URL\n"
              << "\n"
              << "Options:\n"
              << "  --prefix <path>       Only list uploads under this prefix\n"
              << "  --older-than <dur>    Only list uploads older than duration (e.g., 1h, 1d, 7d)\n"
              << "  --abort               Abort all matching uploads (default: list only)\n"
              << "  --region <region>     AWS region (auto-detected if not specified)\n"
              << "  --endpoint-url <url>  S3-compatible endpoint (e.g. Storj, MinIO)\n"
              << "  -v, --verbose         Show detailed output\n"
              << "  -h, --help            Show this help\n";
}

// Format bytes as human-readable string

// Format duration as human-readable string (e.g., "7d", "12h", "30m", "45s")

static void print_stats_usage(const char* program) {
    std::cout << "Usage: " << program << " stats [OPTIONS]\n"
              << "\n"
              << "Display cumulative cloud API statistics.\n"
              << "\n"
              << "Options:\n"
              << "  --reset    Clear all statistics\n"
              << "  --json     Output as JSON\n"
              << "  -h, --help Show this help\n";
}

static int run_stats_command(int argc, char* argv[]) {
    bool reset = false;
    bool json_output = false;

    // Suppress info logs for stats command
    spdlog::set_level(spdlog::level::warn);

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--reset") {
            reset = true;
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "-h" || arg == "--help") {
            print_stats_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_stats_usage(argv[0]);
            return 2;  // usage error: nothing was attempted
        }
    }

    auto& metrics = CloudMetrics::instance();
    metrics.load();

    if (reset) {
        metrics.clear();
        // clear() only resets memory, so a reset that is not saved would come
        // back on the next run (issue #41).
        if (!metrics.save()) {
            std::cerr << "Failed to save cleared statistics.\n";
            return 1;
        }
        std::cout << "Cloud statistics cleared.\n";
        return 0;
    }

    auto all_metrics = metrics.getAllMetrics();
    OperationMetrics total = metrics.getTotalMetrics();

    // Get region for cost calculation
    std::string region = metrics.getRegion();
    if (region.empty()) region = "us-east-1";
    auto& pricing_cache = AwsPricingCache::instance();

    if (json_output) {
        std::cout << "{\n";
        std::cout << "  \"total\": {\n";
        std::cout << "    \"calls\": " << total.call_count.load() << ",\n";
        std::cout << "    \"success\": " << total.success_count.load() << ",\n";
        std::cout << "    \"failures\": " << total.failure_count.load() << ",\n";
        std::cout << "    \"retries\": " << total.retry_count.load() << ",\n";
        std::cout << "    \"bytes_uploaded\": " << total.bytes_uploaded.load() << ",\n";
        std::cout << "    \"bytes_downloaded\": " << total.bytes_downloaded.load() << ",\n";
        std::cout << "    \"avg_latency_ms\": " << std::fixed << std::setprecision(2) << total.avgLatencyMs() << "\n";
        std::cout << "  },\n";
        std::cout << "  \"operations\": {\n";
        bool first = true;
        for (const auto& [op, m] : all_metrics) {
            uint64_t calls = m.call_count.load();
            if (calls == 0) continue;
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "    \"" << S3OperationTypeName(op) << "\": {\n";
            std::cout << "      \"calls\": " << calls << ",\n";
            std::cout << "      \"success\": " << m.success_count.load() << ",\n";
            std::cout << "      \"failures\": " << m.failure_count.load() << ",\n";
            std::cout << "      \"retries\": " << m.retry_count.load() << ",\n";
            std::cout << "      \"bytes_uploaded\": " << m.bytes_uploaded.load() << ",\n";
            std::cout << "      \"bytes_downloaded\": " << m.bytes_downloaded.load() << ",\n";
            std::cout << "      \"avg_latency_ms\": " << std::fixed << std::setprecision(2) << m.avgLatencyMs() << ",\n";
            double op_cost = pricing_cache.estimateOperationCost(op, m.success_count.load(), region);
            std::cout << "      \"cost_usd\": " << std::fixed << std::setprecision(6) << op_cost << "\n";
            std::cout << "    }";
        }
        std::cout << "\n  },\n";

        // Cost breakdown
        auto pricing = pricing_cache.getPricing(region);
        double data_out_cost = (total.bytes_downloaded.load() / 1e9) * pricing.data_out_per_gb;
        double request_cost = 0.0;
        for (const auto& [op, m] : all_metrics) {
            if (m.success_count.load() > 0) {
                request_cost += pricing_cache.estimateOperationCost(op, m.success_count.load(), region);
            }
        }
        double total_cost = request_cost + data_out_cost;

        std::cout << "  \"cost_breakdown\": {\n";
        std::cout << "    \"requests_usd\": " << std::fixed << std::setprecision(6) << request_cost << ",\n";
        std::cout << "    \"data_transfer_out_usd\": " << std::fixed << std::setprecision(6) << data_out_cost << ",\n";
        std::cout << "    \"total_usd\": " << std::fixed << std::setprecision(6) << total_cost << "\n";
        std::cout << "  },\n";
        std::cout << "  \"region\": \"" << region << "\"\n";
        std::cout << "}\n";
    } else {
        uint64_t total_calls = total.call_count.load();
        if (total_calls == 0) {
            std::cout << "No cloud statistics recorded yet.\n";
            return 0;
        }

        std::cout << "Cloud API Statistics\n";
        std::cout << "====================\n\n";

        std::cout << "Total:\n";
        std::cout << "  Calls:      " << total_calls << "\n";
        std::cout << "  Success:    " << total.success_count.load() << "\n";
        std::cout << "  Failures:   " << total.failure_count.load() << "\n";
        std::cout << "  Retries:    " << total.retry_count.load() << "\n";
        std::cout << "  Uploaded:   " << format_bytes(total.bytes_uploaded.load()) << "\n";
        std::cout << "  Downloaded: " << format_bytes(total.bytes_downloaded.load()) << "\n";
        std::cout << "  Avg Latency: " << std::fixed << std::setprecision(1) << total.avgLatencyMs() << " ms\n";
        std::cout << "\n";

        std::cout << "By Operation:\n";
        for (const auto& [op, m] : all_metrics) {
            uint64_t calls = m.call_count.load();
            if (calls == 0) continue;
            std::cout << "  " << std::left << std::setw(22) << S3OperationTypeName(op)
                      << " calls=" << std::setw(8) << calls
                      << " ok=" << std::setw(8) << m.success_count.load()
                      << " fail=" << std::setw(5) << m.failure_count.load()
                      << " up=" << std::setw(12) << format_bytes(m.bytes_uploaded.load())
                      << " down=" << format_bytes(m.bytes_downloaded.load()) << "\n";
        }

        // Cost breakdown
        auto pricing = pricing_cache.getPricing(region);
        double data_out_cost = (total.bytes_downloaded.load() / 1e9) * pricing.data_out_per_gb;
        double request_cost = 0.0;

        std::cout << "\nCost Breakdown";
        if (region != "us-east-1") {
            std::cout << " (" << region << ")";
        }
        std::cout << ":\n";

        // Per-operation request costs
        for (const auto& [op, m] : all_metrics) {
            uint64_t calls = m.call_count.load();
            if (calls == 0) continue;
            uint64_t success = m.success_count.load();
            double op_cost = pricing_cache.estimateOperationCost(op, success, region);
            request_cost += op_cost;
            std::cout << "  " << std::left << std::setw(22) << S3OperationTypeName(op)
                      << " " << formatCost(op_cost) << "\n";
        }

        // Data transfer
        std::cout << "  " << std::left << std::setw(22) << "Data Transfer Out"
                  << " " << formatCost(data_out_cost) << "\n";

        double total_cost = request_cost + data_out_cost;
        std::cout << "  " << std::string(22 + 10, '-') << "\n";
        std::cout << "  " << std::left << std::setw(22) << "Total"
                  << " " << formatCost(total_cost) << "\n";
    }

    return 0;
}

static int run_rm_command(int argc, char* argv[]) {
    RmCliOptions opts = parse_rm_args(argc, argv);

    if (opts.error) {
        std::cerr << "Error: " << opts.error_message << "\n\n";
        print_rm_usage(argv[0]);
        return 2;
    }

    if (opts.help) {
        print_rm_usage(argv[0]);
        return 0;
    }

    if (opts.s3_url.empty()) {
        std::cerr << "Error: S3 URL required\n\n";
        print_rm_usage(argv[0]);
        return 2;
    }

    // Check if prefix requires -r
    bool is_prefix = opts.prefix.empty() || opts.prefix.back() == '/';
    if (is_prefix && !opts.recursive) {
        std::cerr << "Error: Use -r to delete recursively\n";
        return 2;
    }

    // Disable EC2 metadata lookup
    setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

    // Initialize AWS SDK with cloud monitoring
    InitCachedDnsHttpClient();
    Aws::SDKOptions aws_options;
    aws_options.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
        GetS3MonitoringFactoryCreateFn()
    );
    Aws::InitAPI(aws_options);
    AwsShutdownGuard shutdown_guard{aws_options};

    // Load existing metrics to accumulate across command invocations
    CloudMetrics::instance().load();

    // Auto-detect region. GetBucketLocation is an AWS API; S3-compatible
    // endpoints may not implement it and often have no meaningful region, so
    // fall back to the default rather than refusing to run.
    if (opts.region.empty()) {
        opts.region = detect_bucket_region(opts.bucket, "", opts.endpoint_url);
        if (opts.region.empty()) {
            std::cerr << "Error: Failed to detect region for bucket\n";
            CloudMetrics::instance().save();
            return 2;
        }
    }

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Try to raise file descriptor limit for high concurrency
    TryRaiseFdLimit(opts.num_threads);
    FdLimits fd_limits = GetFdLimits(opts.num_threads);
    if (fd_limits.was_capped) {
        WarnIfFdLimitLow(fd_limits, opts.num_threads);
        opts.num_threads = fd_limits.max_safe_threads;
    }

    RmConfig config;
    config.bucket = opts.bucket;
    config.prefix = opts.prefix;
    config.region = opts.region;
    config.endpoint = opts.endpoint_url;
    config.recursive = opts.recursive;
    config.force = opts.force;
    config.quiet = opts.quiet;
    config.verbose = opts.verbose;
    config.batch = opts.batch;
    config.max_threads = opts.num_threads;

    RmProgress progress;

    // Run in background with progress display
    auto result_future = std::async(std::launch::async, [&]() {
        return run_rm(config, progress);
    });

    // Progress display
    while (result_future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (IsShutdownRequested()) {
            progress.cancelled = true;
        }

        if (!opts.quiet) {
            if (!progress.enumeration_done) {
                std::cout << "\rScanning: " << progress.objects_found.load() << " objects   " << std::flush;
            } else {
                size_t found = progress.objects_found.load();
                size_t deleted = progress.objects_deleted.load();
                double pct = found > 0 ? (100.0 * deleted / found) : 0.0;
                std::cout << "\rDeleting: " << deleted << "/" << found
                          << " (" << std::fixed << std::setprecision(0) << pct << "%)   " << std::flush;
            }
        }
    }

    if (!opts.quiet) std::cout << "\r                                        \r";

    RmResult result = result_future.get();

    // A summary describes what happened; when the command failed it would
    // instead assert a count that was never established - and the preview would
    // tell the user to re-run with --force, which fails the same way.
    // A cancelled run still has a summary worth printing - how much it got
    // through, and which keys are unaccounted for. Suppressing it left the
    // user with only the cancellation line and no list of what remains.
    const bool cancelled = progress.cancelled.load();
    if (!opts.quiet && (result.error_message.empty() || cancelled)) {
        if (!opts.force) {
			if(progress.objects_found.load()==1){
			std::cout << "Would delete " << progress.objects_found.load() << " object";
		}
		else{
			std::cout << "Would delete " << progress.objects_found.load() << " objects";
		}
            if (result.bytes_freed > 0) {
            	if(result.bytes_freed >= 1024.0 * 1024.0 * 1024.0){
	            	std::cout << " (" << (result.bytes_freed /(1024.0 * 1024.0* 1024.0)) << " GiB)";
	            }
	            else if(result.bytes_freed >= 1024.0 * 1024.0){
	            	std::cout << " (" << (result.bytes_freed /(1024.0* 1024.0)) << " MiB)";
	            }
	            else if(result.bytes_freed >=1024.0){
	            	std::cout << " (" << (result.bytes_freed /1024.0) << " KB)";
	            }
	            else{
	            	std::cout << " (" << (result.bytes_freed) << " B)";
	            }
            }
            std::cout << "\nUse --force to delete\n";
        } else {
        		if(result.objects_deleted==1){
	        	  std::cout << "Deleted: " << result.objects_deleted << " object";
	        }        
				else{
	        	  std::cout << "Deleted: " << result.objects_deleted << " objects";
	        }
          
            if (result.bytes_freed > 0) {
                     	if(result.bytes_freed >= 1024.0 * 1024.0 * 1024.0){
	            	std::cout << " (" << (result.bytes_freed /(1024.0 * 1024.0* 1024.0)) << " GiB)";
	            }
	            else if(result.bytes_freed >= 1024.0 * 1024.0){
	            	std::cout << " (" << (result.bytes_freed /(1024.0* 1024.0)) << " MiB)";
	            }
	            else if(result.bytes_freed >=1024.0){
	            	std::cout << " (" << (result.bytes_freed /1024.0) << " KB)";
	            }
	            else{
	            	std::cout << " (" << (result.bytes_freed) << " B)";
	            }
            }
            if (result.objects_failed > 0) {
            	if(result.objects_failed == 1){
	            	std::cout << ", " << result.objects_failed << " failure";
	            }
	            else{
            		std::cout << ", " << result.objects_failed << " failed";
            	}
            }
            std::cout << " in " << std::fixed << std::setprecision(2)
                      << result.elapsed_seconds << "s\n";
            // "Failed" covers both "still there" and "could not confirm", so
            // name the keys rather than leaving the user to guess.
            if (!result.failed_keys.empty()) {
                std::cout << "Not confirmed deleted:\n";
                size_t shown = 0;
                for (const auto& k : result.failed_keys) {
                    if (shown++ == 20) {
                        std::cout << "  ... and " << (result.failed_keys.size() - 20)
                                  << " more\n";
                        break;
                    }
                    std::cout << "  " << k << "\n";
                }
            }
        }

        // Printed for a dry run as well as a real delete: what was left alone
        // is exactly what a user checking with a dry run wants to know.
        for (const auto& note : result.warnings) {
            std::cout << "Note: " << note << "\n";
        }
    }

    if (!result.error_message.empty()) {
        std::cerr << "Error: " << result.error_message << "\n";
        CloudMetrics::instance().save();
        // 2 means "nothing was attempted", which is untrue of a run that
        // deleted half a prefix before being interrupted. Report the
        // interruption the way sync does.
        if (cancelled) return 130;
        return 2;
    }

    CloudMetrics::instance().save();
    if (cancelled) return 130;
    return result.objects_failed > 0 ? 1 : 0;
}

static int run_leftovers_command(int argc, char* argv[]) {
    LeftoversConfig config;
    bool help = false;

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            help = true;
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--abort") {
            config.abort_uploads = true;
        } else if (arg == "--prefix" && i + 1 < argc) {
            config.prefix = argv[++i];
        } else if (arg == "--older-than" && i + 1 < argc) {
            auto dur = parse_duration(argv[++i]);
            if (!dur) {
                std::cerr << "Error: Invalid duration format. Use 1h, 1d, 7d, etc.\n";
                return 2;
            }
            config.older_than = *dur;
        } else if (arg == "--region" && i + 1 < argc) {
            config.region = argv[++i];
        } else if (arg == "--endpoint-url" && i + 1 < argc) {
            config.endpoint = argv[++i];
        } else if (arg.rfind("s3://", 0) == 0 || (arg[0] != '-' && config.bucket.empty())) {
            // Parse bucket from s3:// URL or plain name
            if (arg.rfind("s3://", 0) == 0) {
                arg = arg.substr(5);  // Remove s3://
            }
            // Extract bucket (before first /)
            auto slash = arg.find('/');
            if (slash != std::string::npos) {
                config.bucket = arg.substr(0, slash);
                if (config.prefix.empty()) {
                    config.prefix = arg.substr(slash + 1);
                }
            } else {
                config.bucket = arg;
            }
        } else {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            print_leftovers_usage(argv[0]);
            return 2;
        }
    }

    if (help) {
        print_leftovers_usage(argv[0]);
        return 0;
    }

    if (config.bucket.empty()) {
        std::cerr << "Error: Bucket required\n\n";
        print_leftovers_usage(argv[0]);
        return 2;
    }

    // Disable EC2 metadata lookup
    setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

    // Initialize AWS SDK with monitoring
    InitCachedDnsHttpClient();
    Aws::SDKOptions options;
    options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Off;
    options.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
        GetS3MonitoringFactoryCreateFn()
    );
    Aws::InitAPI(options);

    // Load existing metrics to accumulate across command invocations. Without
    // this the save() at the end writes a CloudMetrics that only ever saw this
    // one run, overwriting everything the user had accumulated (issue #35).
    CloudMetrics::instance().load();

    // Auto-detect region if not specified. Skipped for S3-compatible endpoints,
    // which may not implement GetBucketLocation (see run_rm_command).
    if (config.region.empty()) {
        config.region = detect_bucket_region(config.bucket, "", config.endpoint);
        if (config.region.empty()) {
            std::cerr << "Error: Could not detect bucket region. Specify with --region.\n";
            // The region probe made real calls; record them like rm does.
            CloudMetrics::instance().save();
            Aws::ShutdownAPI(options);
            return 1;
        }
    }

    // addRegion, not setRegion: the region list is cumulative in the same way
    // the counters are, and load() has just read the regions every previous run
    // recorded. Replacing it would keep the user's accumulated calls but reprice
    // all of them under this one run's region.
    CloudMetrics::instance().addRegion(config.region);

    // Run leftovers
    LeftoversProgress progress;
    auto result = run_leftovers(config, progress, nullptr);

    // Print results
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << "\n";
        // A failed run still made calls that cost money.
        CloudMetrics::instance().save();
        Aws::ShutdownAPI(options);
        return 1;
    }

    if (config.abort_uploads) {
        std::cout << "\nAborted " << result.uploads_aborted << " multipart uploads";
        if (result.abort_failures > 0) {
            std::cout << " (" << result.abort_failures << " failures)";
        }
    } else {
        std::cout << "\n" << result.uploads_listed << " active multipart uploads";
        if (!config.prefix.empty()) {
            std::cout << " (prefix: " << config.prefix << ")";
        }
        if (config.older_than.count() > 0) {
            std::cout << " (older than: " << format_duration(config.older_than) << ")";
        }
    }
    std::cout << "\n";

    CloudMetrics::instance().save();
    Aws::ShutdownAPI(options);
    return (result.abort_failures > 0) ? 1 : 0;
}

static int run_sync_command(int argc, char* argv[]) {
    SyncCliOptions opts = parse_sync_args(argc, argv);

    if (opts.error) {
        std::cerr << "Error: " << opts.error_message << "\n\n";
        print_usage(argv[0], SubCommand::Sync);
        return 2;  // usage error: nothing was attempted
    }

    if (opts.help) {
        print_usage(argv[0], SubCommand::Sync);
        return 0;
    }

    g_app_settings.allow_unverified_ranges = opts.allow_unverified_ranges;
    if (opts.allow_unverified_ranges) {
        spdlog::warn("--allow-unverified-ranges: a ranged read whose response omits "
                     "Content-Range will be accepted, so bytes that cannot be placed are "
                     "trusted. Use only with an endpoint known to omit the header.");
    }

    // Warn about profile flags that don't apply to the chosen direction (they
    // were silently dropped by apply_sync_profiles). Only S3->S3 uses both sides.
    switch (opts.direction) {
        case SyncDirection::Upload:  // S3 side is the destination
            if (!opts.source_profile.empty())
                std::cerr << "Warning: --source-profile is ignored for an upload "
                             "(local source has no AWS profile); use --dest-profile "
                             "for the S3 destination.\n";
            break;
        case SyncDirection::Download:  // S3 side is the source
            if (!opts.dest_profile.empty())
                std::cerr << "Warning: --dest-profile is ignored for a download "
                             "(local destination has no AWS profile); use --source-profile "
                             "for the S3 source.\n";
            break;
        case SyncDirection::LocalToLocal:
            if (!opts.source_profile.empty() || !opts.dest_profile.empty())
                std::cerr << "Warning: --source-profile/--dest-profile are ignored for a "
                             "local-to-local sync (no S3 side).\n";
            break;
        case SyncDirection::S3ToS3:
            break;  // both sides apply
    }

    // Validate required arguments based on direction
    if (opts.direction == SyncDirection::S3ToS3) {
        if (!opts.has_s3_source || !opts.has_s3_dest) {
            std::cerr << "Error: S3-to-S3 sync requires two S3 URLs\n\n";
            print_usage(argv[0], SubCommand::Sync);
            return 2;  // usage error: nothing was attempted
        }
    } else {
        if (!opts.has_local || !opts.has_s3_source) {
            std::cerr << "Error: sync requires a local path and S3 URL\n\n";
            print_usage(argv[0], SubCommand::Sync);
            return 2;  // usage error: nothing was attempted
        }
    }

    // For upload, verify local path exists and is a directory
    // For download, create local path if it doesn't exist
    // S3-to-S3 skips local path validation
    std::error_code ec;
    if (opts.direction == SyncDirection::Upload) {
        if (!std::filesystem::is_directory(opts.local_path, ec)) {
            std::cerr << "Error: '" << opts.local_path << "' is not a directory\n";
            return 2;  // usage error: the argument is wrong, nothing was attempted
        }
    } else if (opts.direction == SyncDirection::Download) {
        // Download direction - create directory if it doesn't exist
        if (!std::filesystem::exists(opts.local_path, ec)) {
            std::filesystem::create_directories(opts.local_path, ec);
            if (ec) {
                std::cerr << "Error: Failed to create directory '" << opts.local_path << "': " << ec.message() << "\n";
                return 1;
            }
        } else if (!std::filesystem::is_directory(opts.local_path, ec)) {
            std::cerr << "Error: '" << opts.local_path << "' exists but is not a directory\n";
            return 2;  // usage error: the argument is wrong, nothing was attempted
        }
    }
    // S3-to-S3 doesn't need local path validation

    // Set up logging
    if (opts.debug) {
        spdlog::set_level(spdlog::level::debug);
    } else if (opts.quiet) {
        spdlog::set_level(spdlog::level::warn);
    }

    // Disable EC2 metadata lookup
    setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

    // Initialize AWS SDK
    InitCachedDnsHttpClient();
    Aws::SDKOptions aws_options;
    aws_options.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
        GetS3MonitoringFactoryCreateFn()
    );
    Aws::InitAPI(aws_options);
    AwsShutdownGuard shutdown_guard{aws_options};

    // Load existing metrics to accumulate across command invocations
    CloudMetrics::instance().load();

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Auto-detect S3 region(s) if not specified
    if (opts.s3_source.region.empty()) {
        opts.s3_source.region = detect_bucket_region(opts.s3_source.bucket, opts.s3_source.profile, opts.s3_source.endpoint);
        if (opts.s3_source.region.empty()) {
            std::cerr << "Error: Failed to detect region for bucket '"
                      << opts.s3_source.bucket << "'\n";
            CloudMetrics::instance().save();
            return 1;
        }
    }
    if (opts.direction == SyncDirection::S3ToS3 && opts.s3_dest.region.empty()) {
        opts.s3_dest.region = detect_bucket_region(opts.s3_dest.bucket, opts.s3_dest.profile, opts.s3_dest.endpoint);
        if (opts.s3_dest.region.empty()) {
            std::cerr << "Error: Failed to detect region for bucket '"
                      << opts.s3_dest.bucket << "'\n";
            CloudMetrics::instance().save();
            return 1;
        }
    }

    // Warm up DNS cache
    WarmupS3Dns(opts.s3_source.bucket, opts.s3_source.region, opts.s3_source.endpoint);
    if (opts.direction == SyncDirection::S3ToS3 &&
        (opts.s3_dest.bucket != opts.s3_source.bucket ||
         opts.s3_dest.region != opts.s3_source.region)) {
        WarmupS3Dns(opts.s3_dest.bucket, opts.s3_dest.region, opts.s3_dest.endpoint);
    }

    // Try to raise file descriptor limit for high concurrency
    TryRaiseFdLimit(opts.num_threads);
    FdLimits fd_limits = GetFdLimits(opts.num_threads);
    if (fd_limits.was_capped) {
        WarnIfFdLimitLow(fd_limits, opts.num_threads);
        opts.num_threads = fd_limits.max_safe_threads;
    }

    // Build config
    SyncConfig config;
    config.direction = opts.direction;
    config.local_path = opts.local_path;
    if (opts.direction == SyncDirection::S3ToS3) {
        config.source = opts.s3_source;
        config.destination = opts.s3_dest;
    } else if (opts.direction == SyncDirection::Upload) {
        config.destination = opts.s3_source;
    } else {
        config.source = opts.s3_source;
    }
    config.delete_orphans = opts.delete_orphans;
    config.dry_run = opts.dry_run;
    config.max_threads = opts.num_threads;
    config.debug = opts.debug;
    config.quiet = opts.quiet;
    config.verbose = opts.verbose;

    // Set up region tracking for cost estimation
    auto& metrics = CloudMetrics::instance();
    // See run_leftovers_command: setRegion here discarded the regions load()
    // had just read, repricing the whole accumulated history.
    metrics.addRegion(opts.s3_source.region);
    if (opts.direction == SyncDirection::S3ToS3 && opts.s3_source.region != opts.s3_dest.region) {
        metrics.addRegion(opts.s3_dest.region);
    }

    SyncProgress progress;

    // Run sync in background thread with progress display
    auto result_future = std::async(std::launch::async, [&]() {
        return run_sync(config, progress);
    });

    // Track start time for transfer rate calculation
    size_t last_bytes = 0;
    auto last_rate_time = std::chrono::steady_clock::now();
    double current_rate_mbps = 0.0;  // Persist rate between updates

    // Progress display loop
    while (result_future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (IsShutdownRequested()) {
            progress.cancelled = true;
        }

        if (!opts.quiet) {
            if (!progress.scanning_done) {
                if (opts.direction == SyncDirection::S3ToS3) {
                    // S3-to-S3: show source and dest S3 counts
                    std::cout << "\rScanning: " << progress.files_scanned_s3.load()
                              << " source / " << progress.files_scanned_dest.load() << " dest   " << std::flush;
                } else {
                    std::cout << "\rScanning: " << progress.files_scanned_local.load()
                              << " local / " << progress.files_scanned_s3.load() << " S3   " << std::flush;
                }
            } else {
                size_t done = progress.files_processed.load();
                size_t total = progress.files_total.load();
                double pct = total > 0 ? (100.0 * done / total) : 0.0;

                // For S3-to-S3, track server-side copy progress (not client bandwidth)
                size_t bytes = (opts.direction == SyncDirection::S3ToS3)
                    ? progress.bytes_copied_server_side.load()
                    : progress.bytes_transferred.load();

                // Calculate transfer rate (bytes/sec over last interval)
                auto now = std::chrono::steady_clock::now();
                double interval_sec = std::chrono::duration<double>(now - last_rate_time).count();
                if (interval_sec >= 0.5) {  // Update rate every 0.5 seconds
                    size_t bytes_delta = bytes - last_bytes;
                    current_rate_mbps = (bytes_delta / (1024.0 * 1024.0)) / interval_sec;
                    last_bytes = bytes;
                    last_rate_time = now;
                }

                size_t in_flight = progress.files_in_flight.load();
                std::cout << "\rSyncing: " << done << "/" << total
                          << " (" << std::fixed << std::setprecision(0) << pct << "%)";
                if (in_flight > 0) {
                    std::cout << " [" << in_flight << " in flight]";
                }
                if (current_rate_mbps > 0.01) {
                    // For S3-to-S3, clarify this is server-side copy rate
                    if (opts.direction == SyncDirection::S3ToS3) {
                        std::cout << " " << std::setprecision(1) << current_rate_mbps << " MiB/s (copy)";
                    } else {
                        std::cout << " " << std::setprecision(1) << current_rate_mbps << " MiB/s";
                    }
                }
                std::cout << "   " << std::flush;
            }
        }
    }

    // Clear progress line
    if (!opts.quiet) std::cout << "\r                                                              \r";

    SyncResult result = result_future.get();

    // Cleanup any orphaned multipart uploads
    UploadRegistry::instance().abort_all();

    // A cancellation that reached the transfer phase still moved data, and the
    // summary below is the only record of what it managed, so print that first
    // and report the interruption after it. Everything else that fails this
    // early is a hard error with nothing to summarise.
    const bool interrupted_mid_transfer = progress.cancelled && progress.scanning_done;

    if (!result.success && !result.error_message.empty() && !interrupted_mid_transfer) {
        std::cerr << "Error: " << result.error_message << "\n";
        CloudMetrics::instance().save();
        return 1;
    }

    // Print summary
    if (!opts.quiet) {
        if (opts.dry_run) {
            std::cout << "Dry run - no changes made\n";
        }

        // Show appropriate counts based on sync direction
        if (opts.direction == SyncDirection::Upload) {
            std::cout << "Synced: " << result.files_uploaded << " uploaded, "
                      << result.files_diff_uploaded << " diff-uploaded, "
                      << result.files_skipped << " skipped";
        } else if (opts.direction == SyncDirection::S3ToS3) {
            std::cout << "Synced: " << result.files_copied << " copied, "
                      << result.files_skipped << " skipped";
        } else {
            std::cout << "Synced: " << result.files_downloaded << " downloaded, "
                      << result.files_diff_downloaded << " diff-downloaded, "
                      << result.files_skipped << " skipped";
        }
        if (opts.delete_orphans) {
            std::cout << ", " << result.files_deleted << " deleted";
        }
        if (result.files_failed > 0) {
            std::cout << ", " << result.files_failed << " failed";
        }
        std::cout << "\n";

        // Print transfer stats
        if (opts.direction == SyncDirection::S3ToS3) {
            // S3-to-S3: show server-side copy stats (no client bandwidth)
            if (result.bytes_copied_server_side > 0) {
                double copied_mb = result.bytes_copied_server_side / (1024.0 * 1024.0);
                double rate_mbps = result.elapsed_seconds > 0
                    ? copied_mb / result.elapsed_seconds : 0.0;

                std::cout << std::fixed << std::setprecision(1)
                          << "Copied: " << copied_mb << " MiB (server-side)";
                std::cout << " in " << std::setprecision(1) << result.elapsed_seconds << "s";
                if (rate_mbps > 0.01) {
                    std::cout << " (" << rate_mbps << " MiB/s)";
                }
                std::cout << "\n";
            }
        } else if (result.bytes_transferred > 0 || result.bytes_saved > 0) {
            double transferred_mb = result.bytes_transferred / (1024.0 * 1024.0);
            double saved_mb = result.bytes_saved / (1024.0 * 1024.0);
            double rate_mbps = result.elapsed_seconds > 0
                ? transferred_mb / result.elapsed_seconds : 0.0;

            std::cout << std::fixed << std::setprecision(1)
                      << "Transferred: " << transferred_mb << " MiB";
            if (result.bytes_saved > 0) {
                const char* diff_type = (opts.direction == SyncDirection::Upload)
                    ? "diff-upload" : "diff-download";
                std::cout << " (saved " << saved_mb << " MiB via " << diff_type << ")";
            }
            std::cout << " in " << std::setprecision(1) << result.elapsed_seconds << "s";
            if (rate_mbps > 0.01) {
                std::cout << " (" << rate_mbps << " MiB/s)";
            }
            std::cout << "\n";
        }
    }

    if (!result.success && !result.error_message.empty()) {
        std::cerr << "Error: " << result.error_message << "\n";
    }

    CloudMetrics::instance().save();
    return result.success ? 0 : 1;
}


// Detect bucket region using GetBucketLocation API
// Returns empty string on failure
static std::string detect_bucket_region(const std::string& bucket,
                                        const std::string& profile,
                                        const std::string& endpoint) {
    // GetBucketLocation is an AWS API. An S3-compatible endpoint may not
    // implement it and usually has no meaningful region - and the credentials
    // belong to that gateway, not to AWS, so asking AWS would both fail and
    // leak an attempt. Answer from the default without a round trip.
    if (!endpoint.empty()) {
        return defaults::REGION;
    }

    try {
        Aws::Client::ClientConfiguration config;
        config.requestTimeoutMs = 10000;
        config.connectTimeoutMs = 5000;

        std::unique_ptr<Aws::S3::S3Client> client;
        if (!profile.empty()) {
            auto creds = std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(profile.c_str());
            client = std::make_unique<Aws::S3::S3Client>(creds, nullptr, config);  // profile-aware creds
        } else {
            client = std::make_unique<Aws::S3::S3Client>(config);                  // default chain (unchanged)
        }

        Aws::S3::Model::GetBucketLocationRequest request;
        request.SetBucket(bucket.c_str());

        auto outcome = client->GetBucketLocation(request);
        if (!outcome.IsSuccess()) {
            spdlog::error("Failed to detect region for bucket '{}': {}",
                bucket, outcome.GetError().GetMessage());
            return "";
        }

        std::string region = Aws::S3::Model::BucketLocationConstraintMapper::GetNameForBucketLocationConstraint(
            outcome.GetResult().GetLocationConstraint()
        );

        // Handle special cases
        if (region.empty()) {
            region = "us-east-1";  // us-east-1 returns empty
        } else if (region == "EU") {
            region = "eu-west-1";  // Legacy EU region
        }

        return region;
    } catch (const std::exception& e) {
        spdlog::error("Exception detecting region for bucket '{}': {}", bucket, e.what());
        return "";
    }
}

// Format FileCompareStatus as string with actual paths for ONLY cases

// Write results to JSON file

// Write results to CSV file

// Write results to text file

// Write results to file based on extension

// Print directory comparison results to console

// Auto-detect regions for S3 sources that don't have explicit regions
// Returns true on success, false if any detection failed
static bool auto_detect_regions(FileSource& source_a, FileSource& source_b) {
    bool success = true;

    if (source_a.type == SourceType::S3 && source_a.region.empty()) {
        source_a.region = detect_bucket_region(source_a.bucket, source_a.profile, source_a.endpoint);
        if (source_a.region.empty()) {
            success = false;
        }
    }

    if (source_b.type == SourceType::S3 && source_b.region.empty()) {
        // Optimize: if same bucket as source_a, reuse the region
        if (source_a.type == SourceType::S3 && source_b.bucket == source_a.bucket && !source_a.region.empty()) {
            source_b.region = source_a.region;
        } else {
            source_b.region = detect_bucket_region(source_b.bucket, source_b.profile, source_b.endpoint);
            if (source_b.region.empty()) {
                success = false;
            }
        }
    }

    return success;
}

int main(int argc, char* argv[]) {
    // Answered before anything else looks at the arguments. See wants_version() for why
    // it is argv[1] only.
    if (wants_version(argc, argv)) {
        print_version();
        return 0;
    }

    // Check for subcommand first
    SubCommand cmd = detect_subcommand(argc, argv);

    if (cmd == SubCommand::Sync) {
        return run_sync_command(argc, argv);
    }

    if (cmd == SubCommand::Rm) {
        return run_rm_command(argc, argv);
    }

    if (cmd == SubCommand::Stats) {
        return run_stats_command(argc, argv);
    }

    if (cmd == SubCommand::Leftovers) {
        return run_leftovers_command(argc, argv);
    }

    // Handle legacy/diff mode
    // Adjust argc/argv if "diff" subcommand was used explicitly
    int adj_argc = argc;
    char** adj_argv = argv;
    if (cmd == SubCommand::Diff && argc > 1) {
        adj_argc = argc - 1;
        adj_argv = argv + 1;
        adj_argv[0] = argv[0];  // Keep program name
    }

    CliOptions opts = parse_args(adj_argc, adj_argv);

    if (opts.error) {
        std::cerr << "Error: " << opts.error_message << "\n\n";
        print_usage(argv[0]);
        return 2;  // usage error: nothing was attempted
    }

    if (opts.help) {
        print_usage(argv[0]);
        return 0;
    }

    // Validate required arguments for CLI mode
    if (!opts.has_source_a || !opts.has_source_b) {
        std::cerr << "Error: two sources are required for comparison\n\n";
        print_usage(argv[0]);
        return 2;  // usage error: nothing was attempted
    }

    // Set up logging level
    if (opts.debug) {
        spdlog::set_level(spdlog::level::debug);
    } else if (opts.quiet) {
        spdlog::set_level(spdlog::level::warn);
    }

    // Set global verbose flag for retry warnings
    g_verbose = opts.verbose || opts.debug;
    g_app_settings.allow_unverified_ranges = opts.allow_unverified_ranges;
    if (opts.allow_unverified_ranges) {
        spdlog::warn("--allow-unverified-ranges: a ranged read whose response omits "
                     "Content-Range will be accepted, so bytes that cannot be placed are "
                     "trusted. Use only with an endpoint known to omit the header.");
    }

    // Disable EC2 metadata lookup (avoid timeouts on non-EC2 machines)
    setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

    // IMPORTANT: Initialize custom HTTP client factory BEFORE Aws::InitAPI()
    // The AWS SDK only allows setting the HTTP client factory before initialization.
    // After Aws::InitAPI() is called, SetHttpClientFactory() has no effect.
    InitCachedDnsHttpClient();

    Aws::SDKOptions aws_options;

    // Register S3 monitoring to track API calls and latency
    aws_options.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
        GetS3MonitoringFactoryCreateFn()
    );

    Aws::InitAPI(aws_options);
    AwsShutdownGuard shutdown_guard{aws_options};

    // Load existing metrics to accumulate across command invocations
    CloudMetrics::instance().load();

    // Register signal handlers for graceful cleanup
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Auto-detect regions for S3 sources without explicit region
    if (!auto_detect_regions(opts.source_a, opts.source_b)) {
        spdlog::error("Failed to auto-detect bucket region(s). Use explicit @region syntax.");
        CloudMetrics::instance().save();
        return 1;
    }

    // Pre-warm DNS cache for S3 buckets
    if (opts.source_a.type == SourceType::S3) {
        WarmupS3Dns(opts.source_a.bucket, opts.source_a.region, opts.source_a.endpoint);
    }
    if (opts.source_b.type == SourceType::S3 &&
        (opts.source_b.bucket != opts.source_a.bucket || opts.source_b.region != opts.source_a.region)) {
        WarmupS3Dns(opts.source_b.bucket, opts.source_b.region, opts.source_b.endpoint);
    }

    spdlog::debug("Comparing: {} <-> {}", source_to_string(opts.source_a), source_to_string(opts.source_b));
    spdlog::debug("CRC32 implementation: {}", hw_crc32_name());

    // Check file descriptor limits and adjust thread count if needed
    int effective_threads = opts.num_threads;
    if (opts.num_threads > 0) {
        TryRaiseFdLimit(opts.num_threads);
        FdLimits fd_limits = GetFdLimits(opts.num_threads);
        if (fd_limits.was_capped) {
            effective_threads = fd_limits.max_safe_threads;
            spdlog::warn("Thread count capped to {} due to file descriptor limits", effective_threads);
        }
    }
    spdlog::debug("Using {} threads", effective_threads);

    // Check if we should use directory comparison mode
    bool is_dir_a = is_directory_source(opts.source_a);
    bool is_dir_b = is_directory_source(opts.source_b);

    // For S3 without trailing slash: check if key exists as object
    // If one source is clearly a directory, check the other via API
    if (!is_dir_a && opts.source_a.type == SourceType::S3 && is_dir_b) {
        is_dir_a = is_s3_prefix(opts.source_a);
    }
    if (!is_dir_b && opts.source_b.type == SourceType::S3 && is_dir_a) {
        is_dir_b = is_s3_prefix(opts.source_b);
    }
    // If both are S3 without trailing slashes, check both
    if (!is_dir_a && !is_dir_b &&
        opts.source_a.type == SourceType::S3 && opts.source_b.type == SourceType::S3) {
        is_dir_a = is_s3_prefix(opts.source_a);
        is_dir_b = is_s3_prefix(opts.source_b);
    }

    if (opts.directory_mode || (is_dir_a && is_dir_b)) {
        if (is_dir_a != is_dir_b && !opts.directory_mode) {
            std::cerr << "Error: one source is a directory and one is a file\n";
            CloudMetrics::instance().save();
            return 1;
        }

        DirectoryComparisonConfig dir_config;
        dir_config.source_a = opts.source_a;
        dir_config.source_b = opts.source_b;
        dir_config.recursive = true;
        dir_config.debug = opts.debug;
        dir_config.num_threads = effective_threads;
        dir_config.ramp_up = opts.ramp_up;
        dir_config.parallel_discovery = opts.parallel_discovery;
        dir_config.parallel_discovery_workers = opts.parallel_discovery_workers;

        DirectoryComparisonProgress dir_progress;

        // Suppress info logs during comparison to keep progress display clean
        auto saved_log_level = spdlog::default_logger()->level();
        if (!opts.debug) {
            spdlog::set_level(spdlog::level::warn);
        }

        auto dir_result_future = std::async(std::launch::async, [&dir_config, &dir_progress]() {
            return run_directory_comparison(dir_config, dir_progress);
        });

        // Progress display loop for directory comparison
        bool shutdown_msg_shown = false;
        while (dir_result_future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (IsShutdownRequested() && !shutdown_msg_shown) {
                if (!opts.quiet) std::cout << "\n";
                std::cerr << "Interrupt received, stopping...\n";
                dir_progress.cancelled = true;
                shutdown_msg_shown = true;
            }
            if (!shutdown_msg_shown && !opts.quiet) {
                if (!dir_progress.scanning_done) {
                    std::cout << "\rScanning: " << dir_progress.files_scanned_a.load()
                              << " / " << dir_progress.files_scanned_b.load() << " files   " << std::flush;
                } else {
                    size_t compared = dir_progress.files_compared.load();
                    size_t total = dir_progress.total_files.load();
                    double pct = total > 0 ? (100.0 * compared / total) : 0.0;
                    std::cout << "\rComparing: " << compared << "/" << total
                              << " (" << std::fixed << std::setprecision(0) << pct << "%)   " << std::flush;
                }
            }
        }
        if (!opts.quiet) std::cout << "\r                                              \r";

        // Restore log level
        spdlog::set_level(saved_log_level);

        DirectoryComparisonResult dir_result = dir_result_future.get();

        // Cleanup any orphaned uploads
        UploadRegistry::instance().abort_all();

        if (IsShutdownRequested()) {
            spdlog::info("Operation interrupted, cleanup complete.");
            CloudMetrics::instance().save();
            return 130;
        }

        if (!dir_result.success) {
            spdlog::error("{}", dir_result.error_message);
            CloudMetrics::instance().save();
            return 1;
        }

        // Write results to file if requested
        if (!opts.output_file.empty()) {
            std::string source_a_str = source_to_string(opts.source_a);
            std::string source_b_str = source_to_string(opts.source_b);
            if (write_results_to_file(opts.output_file, dir_result, source_a_str, source_b_str)) {
                if (!opts.quiet) {
                    std::cout << "Results written to: " << opts.output_file << "\n";
                }
            } else {
                std::cerr << "Error: Failed to write results to " << opts.output_file << "\n";
            }
        }

        print_directory_result(dir_result, source_to_string(opts.source_a), source_to_string(opts.source_b));

        // Return 0 if all files match, 1 if there are differences/errors
        bool all_ok = (dir_result.mismatched_files == 0 &&
                       dir_result.only_in_a == 0 &&
                       dir_result.only_in_b == 0 &&
                       dir_result.errors == 0);
        CloudMetrics::instance().save();
        return all_ok ? 0 : 1;
    }

    // Regular file comparison
    ComparisonConfig config;
    config.source_a = opts.source_a;
    config.source_b = opts.source_b;
    config.debug = opts.debug;
    config.num_threads = effective_threads;
    config.ramp_up = opts.ramp_up;

    ComparisonProgress progress;

    // Run comparison in background thread so we can display progress
    auto result_future = std::async(std::launch::async, [&config, &progress]() {
        return run_comparison(config, progress);
    });

    // Progress display loop
    bool shutdown_msg_shown = false;
    while (result_future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (IsShutdownRequested() && !shutdown_msg_shown) {
            std::cout << "\n";
            spdlog::info("Interrupt received, waiting for in-flight requests to complete...");
            shutdown_msg_shown = true;
        }
        if (!shutdown_msg_shown) {
            std::cout << "\rSource A progress: " << std::fixed << std::setprecision(1)
                      << (progress.source_a_done ? 100.0 : progress.source_a_progress.load()) << "%";
            if (progress.source_a_done) std::cout << " (done)";
            std::cout << " | Source B progress: " << std::fixed << std::setprecision(1)
                      << (progress.source_b_done ? 100.0 : progress.source_b_progress.load()) << "%";
            if (progress.source_b_done) std::cout << " (done)";
            std::cout << std::flush;
        }
    }
    if (!shutdown_msg_shown) {
        std::cout << "\rSource A progress: 100.0% (done) | Source B progress: 100.0% (done)          \n";
    }

    ComparisonResult result = result_future.get();

    // Cleanup any orphaned uploads before AWS SDK shutdown
    // This must happen while the SDK is still active
    UploadRegistry::instance().abort_all();

    // Check if we were interrupted by a signal
    if (IsShutdownRequested()) {
        spdlog::info("Operation interrupted, cleanup complete.");
        CloudMetrics::instance().save();
        return 130;  // Standard exit code for SIGINT (128 + 2)
    }

    // Report results
    if (!result.success) {
        spdlog::error("{}", result.error_message);
        CloudMetrics::instance().save();
        return 1;
    }

    spdlog::info("Source A CRC32 time: {}", format_duration(result.source_a_elapsed));
    spdlog::info("Source B CRC32 time: {}", format_duration(result.source_b_elapsed));
    spdlog::info("Total parallel time: {}", format_duration(result.total_elapsed));

    // Show file sizes if they differ
    if (result.size_a != result.size_b) {
        spdlog::info("Source A size: {} bytes", result.size_a);
        spdlog::info("Source B size: {} bytes", result.size_b);
    }

    CloudMetrics::instance().save();

    if (result.all_match) {
        spdlog::info("All CRC32 values match.");
        return 0;
    } else {
        if (!result.error_message.empty()) {
            spdlog::error("{}", result.error_message);
        }
        for (size_t idx : result.mismatched_chunks) {
            spdlog::warn("DIFF at chunk {}: A=0x{:x}, B=0x{:x}",
                idx + 1, result.source_a_crcs[idx], result.source_b_crcs[idx]);
        }
        for (size_t idx : result.extra_chunks_in_a) {
            spdlog::warn("EXTRA in A: chunk {} (missing in B)", idx + 1);
        }
        for (size_t idx : result.extra_chunks_in_b) {
            spdlog::warn("EXTRA in B: chunk {} (missing in A)", idx + 1);
        }
        return 1;
    }
}
