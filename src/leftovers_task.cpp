#include "leftovers_task.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <ctime>

LeftoversResult run_leftovers(
    const LeftoversConfig& config,
    LeftoversProgress& progress,
    std::shared_ptr<IS3Client> s3_client
) {
    LeftoversResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        if (!s3_client) {
            s3_client = CreateS3Client(config.region, config.endpoint, 128);
            if (!s3_client) {
                result.error_message = "Failed to create S3 client";
                return result;
            }
        }

        auto now = std::chrono::system_clock::now();
        std::string key_marker;
        std::string upload_id_marker;

        bool header_printed = false;

        do {
            if (progress.cancelled.load()) {
                result.error_message = "Cancelled";
                break;
            }

            // Listed bucket-wide and filtered below, rather than asking the
            // service to filter.
            //
            // MinIO answers a prefix-filtered ListMultipartUploads with nothing
            // unless the prefix is the whole key: an upload at p/x/unfinished.bin
            // is found by prefix "" and by "p/x/unfinished.bin", and missed by
            // "p/", "p/x" and "p/x/". Passing the prefix through therefore made
            // `leftovers --prefix` report a clean bucket while an upload sat in
            // it - the worst answer a cleanup command can give, because the user
            // stops looking (issue #104).
            //
            // AWS honours the prefix, so this trades some listing for not
            // depending on the endpoint getting it right. Uploads in flight are
            // transient and few by nature - a bucket with enough of them to make
            // this expensive is one where leftovers has plenty to report anyway.
            auto list_result = s3_client->ListMultipartUploads(
                config.bucket,
                "",
                key_marker,
                upload_id_marker,
                1000
            );

            if (!list_result.success) {
                result.error_message = list_result.error_message;
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            for (const auto& upload : list_result.uploads) {
                // The prefix filter the service was not asked to apply. An
                // empty prefix matches everything, which is the whole-bucket
                // case.
                if (!config.prefix.empty() &&
                    upload.key.compare(0, config.prefix.size(), config.prefix) != 0) {
                    continue;
                }

                // Apply older_than filter
                if (config.older_than.count() > 0) {
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - upload.initiated);
                    if (age < config.older_than) {
                        continue;  // Skip uploads that are too new
                    }
                }

                // Print header on first upload
                if (!header_printed && (config.verbose || !config.abort_uploads)) {
                    std::cout << "KEY" << std::string(30, ' ') << "UPLOAD_ID" << std::string(6, ' ') << "INITIATED\n";
                    header_printed = true;
                }

                // Print upload info
                if (config.verbose || !config.abort_uploads) {
                    auto initiated_time = std::chrono::system_clock::to_time_t(upload.initiated);
                    std::tm tm;
#ifdef _WIN32
                    gmtime_s(&tm, &initiated_time);
#else
                    gmtime_r(&initiated_time, &tm);
#endif
                    char time_buf[32];
                    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &tm);

                    std::cout << upload.key << "  " << upload.upload_id.substr(0, 12) << "...  " << time_buf << "\n";
                }

                result.uploads_listed++;
                progress.uploads_found++;

                // Abort if requested
                if (config.abort_uploads) {
                    if (s3_client->AbortMultipartUpload(config.bucket, upload.key, upload.upload_id)) {
                        result.uploads_aborted++;
                        progress.uploads_aborted++;
                        if (config.verbose) {
                            spdlog::info("Aborted: {} ({})", upload.key, upload.upload_id);
                        }
                    } else {
                        result.abort_failures++;
                        progress.abort_failures++;
                        spdlog::warn("Failed to abort: {} ({})", upload.key, upload.upload_id);
                    }
                }
            }

            key_marker = list_result.next_key_marker;
            upload_id_marker = list_result.next_upload_id_marker;

            if (!list_result.is_truncated) {
                break;
            }
        } while (true);

        result.success = true;
    } catch (const std::exception& e) {
        result.error_message = e.what();
        spdlog::error("Exception in run_leftovers: {}", e.what());
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
    return result;
}
