#include "rm_task.h"

#include <unordered_map>
#include "s3_interface.h"
#include "directory_comparison.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <thread>
#include <random>

RmResult run_rm(
    const RmConfig& config,
    RmProgress& progress,
    std::shared_ptr<IS3Client> s3_client
) {
    RmResult result;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Create S3 client if not provided
        if (!s3_client) {
            s3_client = CreateS3Client(config.region, config.endpoint, 128);
            if (!s3_client) {
                result.error_message = "Failed to create S3 client";
                return result;
            }
        }

        // Auto-detect whether path is a file or directory
        // - If path ends with "/", it's definitely a directory
        // - Otherwise, check if object exists (file) or if prefix has children (directory)
        bool explicit_prefix = config.prefix.empty() || config.prefix.back() == '/';
        bool is_file = false;
        bool is_directory = false;
        int64_t file_size = -1;
        std::string effective_prefix = config.prefix;

        if (explicit_prefix) {
            // Explicit directory (ends with /)
            is_directory = true;
        } else {
            // Need to detect: check if it's a file first.
            //
            // A size that comes back proves the object is there, so the
            // ordinary case is still one round trip. What must not happen is
            // reading -1 as "not an object": GetObjectSize returns it for
            // *any* failure - throttling, AccessDenied, a bad endpoint, a
            // dropped connection - so a single transient error used to send rm
            // looking for a prefix instead. In a bucket where both "key" and
            // "key/..." exist that silently retargets the delete: it wipes the
            // prefix contents and leaves the object the user named (issue
            // #59). CheckObjectPresence is what distinguishes the two.
            file_size = s3_client->GetObjectSize(config.bucket, config.prefix);

            S3ObjectPresence presence = S3ObjectPresence::Exists;
            if (file_size >= 0) {
                // A size came back, so the object is definitely there. No
                // second call needed - this is the ordinary case.
                is_file = true;
            } else {
                // -1 only means the lookup did not produce a size. Ask what
                // actually happened before acting on it.
                presence = s3_client->CheckObjectPresence(config.bucket, config.prefix);
            }

            if (presence == S3ObjectPresence::Unknown) {
                result.error_message =
                    "Could not determine whether s3://" + config.bucket + "/" +
                    config.prefix + " is an object or a prefix, so it is not clear "
                    "what would be deleted. Nothing was deleted. Retry, or name the "
                    "prefix explicitly as \"" + config.prefix + "/\" to skip the "
                    "object lookup.";
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds =
                    std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            if (!is_file && presence == S3ObjectPresence::Exists) {
                // The object is there but its size is not. Presence is
                // authoritative, so the delete goes ahead; only the
                // freed-bytes figure suffers.
                is_file = true;
                spdlog::warn("Could not read the size of s3://{}/{}; reporting "
                             "0 bytes freed for it",
                             config.bucket, config.prefix);
                file_size = 0;
            } else if (!is_file) {
                // The service said the key does not exist. Only now is it safe
                // to ask whether "key/" is a prefix.
                std::string dir_prefix = config.prefix + "/";
                auto list_result = s3_client->ListObjects(config.bucket, dir_prefix, "/", "", 1);
                if (!list_result.success) {
                    // Same trap one level down: a failed listing is not an
                    // empty one, and must not become "Not found".
                    result.error_message =
                        "Could not list s3://" + config.bucket + "/" + dir_prefix +
                        " to determine whether it is a prefix: " +
                        list_result.error_message;
                    auto end_time = std::chrono::high_resolution_clock::now();
                    result.elapsed_seconds =
                        std::chrono::duration<double>(end_time - start_time).count();
                    return result;
                }
                // is_truncated matters even with nothing on this page: S3 may
                // return an empty truncated page, and "there are more results"
                // is not "there is nothing here".
                if (!list_result.objects.empty() ||
                    !list_result.common_prefixes.empty() ||
                    list_result.is_truncated) {
                    is_directory = true;
                    effective_prefix = dir_prefix;
                }
            }
        }

        // Neither file nor directory found
        if (!is_file && !is_directory) {
            result.error_message = "Not found: " + config.prefix;
            auto end_time = std::chrono::high_resolution_clock::now();
            result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
            return result;
        }

        // Directory without recursive flag is an error
        if (is_directory && !config.recursive) {
            result.error_message = config.prefix + " is a directory, use -r to delete recursively";
            auto end_time = std::chrono::high_resolution_clock::now();
            result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
            return result;
        }

        // Handle single file deletion
        if (is_file) {
            progress.objects_found = 1;

            // -r was passed and the name resolved to one object. Deleting that
            // object is the right target - the user named it, and #59 made the
            // resolution reliable - but -r then had no effect at all, and
            // saying nothing let the user believe the same-named prefix went
            // with it. Checked before the dry-run branch so a dry run reports
            // it too, which is where a user is most likely to be checking
            // (issue #72).
            if (config.recursive) {
                const std::string sibling = config.prefix + "/";
                auto peek = s3_client->ListObjects(config.bucket, sibling, "/", "", 1);
                if (!peek.success) {
                    // Informational only. A failed peek must not turn a delete
                    // that works into a failure, so this reports the doubt
                    // rather than the conclusion.
                    result.warnings.push_back(
                        "Could not check whether s3://" + config.bucket + "/" + sibling +
                        " also exists; if it does, it was not touched.");
                } else if (!peek.objects.empty() || !peek.common_prefixes.empty() ||
                           peek.is_truncated) {
                    // is_truncated matters even with an empty page, for the same
                    // reason it does in the resolution above.
                    result.warnings.push_back(
                        "\"" + config.prefix + "\" is an object, so -r had no effect: \"" +
                        sibling + "\" also exists and was not touched. Delete it with: "
                        "mito rm -r s3://" + config.bucket + "/" + sibling);
                }
            }

            if (!config.force) {
                // Dry run - just report what would be deleted
                result.success = true;
                result.objects_deleted = 0;
                result.bytes_freed = file_size;
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            if (s3_client->DeleteObject(config.bucket, config.prefix)) {
                result.objects_deleted = 1;
                result.bytes_freed = file_size;
                progress.objects_deleted = 1;
                progress.bytes_freed = file_size;
                result.success = true;
            } else {
                result.objects_failed = 1;
                result.failed_keys.push_back(config.prefix);
                progress.objects_failed = 1;
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
            return result;
        }

        // Handle directory deletion (recursive)
        if (is_directory) {
            // Enumerate objects under prefix (use effective_prefix which has "/" appended if needed)
            bool listing_complete = true;
            // Markers included: "delete everything under this prefix" covers the
            // zero-byte folder objects other clients create, at every depth.
            // Comparison leaves them out, which is right for comparison and was
            // wrong here (issue #71).
            std::vector<DirectoryEntry> entries = parallel_enumerate_s3_prefix(
                config.bucket, effective_prefix, true, 64,
                progress.objects_found, progress.cancelled, s3_client,
                &listing_complete, /*include_directory_markers=*/true
            );

            progress.enumeration_done = true;

            if (progress.cancelled) {
                result.error_message = "Cancelled";
                return result;
            }

            // A partial listing means an unknown number of objects were never
            // considered. Deleting the subset and reporting success tells the
            // caller the prefix is gone when it is not - and a fully failed
            // listing would otherwise surface as the misleading "No objects
            // found under ...".
            if (!listing_complete) {
                result.error_message =
                    "Could not list s3://" + config.bucket + "/" + effective_prefix +
                    " completely, so an unknown number of objects would be left behind. "
                    "Refusing to delete a partial listing. The failures are logged above.";
                spdlog::error("{}", result.error_message);
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            if (entries.empty()) {
                // For explicit prefixes (ending with /), empty = not found
                // Exception: empty prefix means "entire bucket" which is valid
                if (!effective_prefix.empty()) {
                    result.error_message = "No objects found under: " + effective_prefix;
                    auto end_time = std::chrono::high_resolution_clock::now();
                    result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                    return result;
                }
                result.success = true;
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            // Dry run - don't delete
            if (!config.force) {
                result.success = true;
                for (const auto& e : entries) {
                    result.bytes_freed += e.size;
                }
                auto end_time = std::chrono::high_resolution_clock::now();
                result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
                return result;
            }

            // Shared state for parallel deletion
            std::atomic<size_t> deleted_count{0};
            std::atomic<size_t> failed_count{0};
            std::atomic<size_t> bytes_freed_atomic{0};
            std::mutex failed_mutex;
            std::vector<std::string> all_failed;

            // Adaptive concurrency control
            size_t max_concurrency = static_cast<size_t>(config.max_threads);
            size_t initial_concurrency = std::min(max_concurrency, static_cast<size_t>(64));
            std::atomic<size_t> current_max_concurrency{initial_concurrency};
            std::atomic<size_t> in_flight{0};
            std::atomic<size_t> consecutive_successes{0};
            std::mutex concurrency_mutex;
            std::condition_variable concurrency_cv;

            // Thread pool must be created AFTER all shared state
            auto pool = std::make_unique<boost::asio::thread_pool>(max_concurrency);

            // Lambda for batch deletion - defined at pool scope to ensure proper lifetime
            auto process_batch = [&](std::vector<std::string> keys, std::vector<int64_t> sizes) {
                struct Guard {
                    std::atomic<size_t>& in_flight;
                    std::mutex& mutex;
                    std::condition_variable& cv;
                    ~Guard() {
                        --in_flight;
                        std::lock_guard<std::mutex> lk(mutex);
                        cv.notify_one();
                    }
                } guard{in_flight, concurrency_mutex, concurrency_cv};

                if (progress.cancelled) {
                    failed_count += keys.size();
                    progress.objects_failed += keys.size();
                    std::lock_guard<std::mutex> lock(failed_mutex);
                    all_failed.insert(all_failed.end(), keys.begin(), keys.end());
                    return;
                }

                std::vector<std::string> to_delete = keys;
                // to_delete shrinks as attempts succeed, so sizes cannot be
                // indexed positionally alongside it.
                std::unordered_map<std::string, int64_t> size_of;
                for (size_t i = 0; i < keys.size() && i < sizes.size(); ++i) {
                    size_of[keys[i]] = sizes[i];
                }
                int retry_delay_ms = 500;
                thread_local std::mt19937 rng(std::random_device{}());

                bool had_total_failure = false;
                for (int attempt = 0; attempt <= config.max_retries && !to_delete.empty(); ++attempt) {
                    if (attempt > 0) {
                        int jitter = retry_delay_ms / 4;
                        std::uniform_int_distribution<int> dist(0, jitter);
                        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms + dist(rng)));
                        retry_delay_ms = std::min(retry_delay_ms * 2, 8000);
                    }

                    std::vector<std::string> failed = s3_client->DeleteObjects(config.bucket, to_delete);

                    if (failed.size() == to_delete.size() && !to_delete.empty()) {
                        had_total_failure = true;
                    }

                    size_t succeeded = to_delete.size() - failed.size();
                    deleted_count += succeeded;
                    progress.objects_deleted += succeeded;

                    for (size_t i = 0; i < keys.size(); ++i) {
                        bool was_deleted = std::find(failed.begin(), failed.end(), keys[i]) == failed.end();
                        if (was_deleted && std::find(to_delete.begin(), to_delete.end(), keys[i]) != to_delete.end()) {
                            bytes_freed_atomic += sizes[i];
                            progress.bytes_freed += sizes[i];
                        }
                    }
                    to_delete = failed;
                }

                if (had_total_failure && !progress.cancelled) {
                    consecutive_successes.store(0);
                    size_t current = current_max_concurrency.load();
                    size_t new_max = std::max(current / 2, static_cast<size_t>(16));
                    while (new_max < current) {
                        if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                            if (config.verbose) spdlog::info("Rate limited - reducing concurrency to {}", new_max);
                            break;
                        }
                        new_max = std::max(current / 2, static_cast<size_t>(16));
                    }
                } else if (!progress.cancelled) {
                    size_t successes = consecutive_successes.fetch_add(1) + 1;
                    if (successes % 10 == 0) {
                        size_t current = current_max_concurrency.load();
                        if (current < max_concurrency) {
                            size_t new_max = std::min(current * 2, max_concurrency);
                            if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                                spdlog::debug("Increasing batch concurrency to {}", new_max);
                            }
                        }
                    }
                }

                if (!to_delete.empty() && had_total_failure) {
                    // Verify every key. The previous code checked only the first
                    // and credited the whole batch - up to 1000 objects - to one
                    // lookup, which could itself have failed for an unrelated
                    // reason. Anything not confirmed absent stays failed.
                    // Bounded: a batch fails wholesale when the service is
                    // unavailable, and the presence check is subject to the same
                    // condition. Probing every key would issue hundreds of
                    // sequential retrying HEADs at a service that just shed the
                    // delete - minutes to hours per batch. Give up after a run
                    // of inconclusive answers, which is itself the signal that
                    // the service cannot answer.
                    constexpr size_t MAX_CONSECUTIVE_UNKNOWN = 3;
                    std::vector<std::string> still_unverified;
                    size_t confirmed_deleted = 0;
                    size_t confirmed_bytes = 0;
                    size_t consecutive_unknown = 0;
                    bool gave_up = false;

                    for (size_t vi = 0; vi < to_delete.size(); ++vi) {
                        const auto& k = to_delete[vi];
                        if (gave_up || progress.cancelled) {
                            still_unverified.push_back(k);
                            continue;
                        }
                        auto presence = s3_client->CheckObjectPresence(config.bucket, k);
                        if (presence == S3ObjectPresence::NotFound) {
                            ++confirmed_deleted;
                            auto sit = size_of.find(k);
                            if (sit != size_of.end() && sit->second > 0)
                                confirmed_bytes += static_cast<size_t>(sit->second);
                            consecutive_unknown = 0;
                        } else {
                            still_unverified.push_back(k);
                            if (presence == S3ObjectPresence::Unknown &&
                                ++consecutive_unknown >= MAX_CONSECUTIVE_UNKNOWN) {
                                gave_up = true;
                                spdlog::warn("Giving up verifying this batch after {} "
                                             "inconclusive checks; the remaining {} keys "
                                             "are reported as failed",
                                             consecutive_unknown,
                                             to_delete.size() - vi - 1);
                            }
                        }
                    }
                    if (confirmed_deleted > 0) {
                        deleted_count += confirmed_deleted;
                        progress.objects_deleted += confirmed_deleted;
                        // The individual path credits bytes for a verified
                        // deletion; the batch path must not silently drop them.
                        bytes_freed_atomic += confirmed_bytes;
                        progress.bytes_freed += confirmed_bytes;
                        spdlog::debug("Rate-limited batch: {} of {} confirmed deleted",
                                      confirmed_deleted, to_delete.size());
                    }
                    to_delete = std::move(still_unverified);
                }

                if (!to_delete.empty()) {
                    failed_count += to_delete.size();
                    progress.objects_failed += to_delete.size();
                    std::lock_guard<std::mutex> lock(failed_mutex);
                    all_failed.insert(all_failed.end(), to_delete.begin(), to_delete.end());
                }
            };

            if (config.batch) {
                // ===== BATCH DELETE MODE =====
                // Use 500 instead of S3's max 1000 to reduce partial-failure scope
                // and provide more granular progress updates
                constexpr size_t BATCH_SIZE = 500;
                std::vector<std::string> batch;
                std::vector<int64_t> batch_sizes;

                std::vector<std::pair<std::vector<std::string>, std::vector<int64_t>>> batches;
                for (const auto& entry : entries) {
                    if (progress.cancelled) break;
                    std::string key = effective_prefix + entry.relative_path;
                    batch.push_back(key);
                    batch_sizes.push_back(entry.size);
                    if (batch.size() >= BATCH_SIZE) {
                        batches.emplace_back(std::move(batch), std::move(batch_sizes));
                        batch.clear();
                        batch_sizes.clear();
                    }
                }
                if (!batch.empty()) {
                    batches.emplace_back(std::move(batch), std::move(batch_sizes));
                }

                for (auto& [keys, sizes] : batches) {
                    if (progress.cancelled) break;
                    {
                        std::unique_lock<std::mutex> lock(concurrency_mutex);
                        concurrency_cv.wait(lock, [&] {
                            return in_flight < current_max_concurrency || progress.cancelled;
                        });
                    }
                    if (progress.cancelled) break;
                    ++in_flight;
                    boost::asio::post(*pool, [&, k = std::move(keys), s = std::move(sizes)]() mutable {
                        process_batch(std::move(k), std::move(s));
                    });
                }
            } else {
                // ===== INDIVIDUAL DELETE MODE (default) =====
                // Use individual DeleteObject calls with adaptive concurrency
                for (const auto& entry : entries) {
                    if (progress.cancelled) break;

                    std::string key = effective_prefix + entry.relative_path;

                    {
                        std::unique_lock<std::mutex> lock(concurrency_mutex);
                        concurrency_cv.wait(lock, [&] {
                            return in_flight < current_max_concurrency || progress.cancelled;
                        });
                    }
                    if (progress.cancelled) break;

                    ++in_flight;
                    // Inline the task to avoid lambda lifetime issues
                    boost::asio::post(*pool, [&, k = key, sz = entry.size]() {
                        struct Guard {
                            std::atomic<size_t>& in_flight;
                            std::mutex& mutex;
                            std::condition_variable& cv;
                            ~Guard() {
                                --in_flight;
                                std::lock_guard<std::mutex> lk(mutex);
                                cv.notify_one();
                            }
                        } guard{in_flight, concurrency_mutex, concurrency_cv};

                        if (progress.cancelled) {
                            failed_count++;
                            progress.objects_failed++;
                            std::lock_guard<std::mutex> lock(failed_mutex);
                            all_failed.push_back(k);
                            return;
                        }

                        // Retry with exponential backoff
                        thread_local std::mt19937 rng(std::random_device{}());
                        int backoff_ms = 100;
                        bool success = false;

                        for (int attempt = 0; attempt <= config.max_retries; ++attempt) {
                            if (progress.cancelled) {
                                failed_count++;
                                progress.objects_failed++;
                                std::lock_guard<std::mutex> lock(failed_mutex);
                                all_failed.push_back(k);
                                return;
                            }

                            if (attempt > 0) {
                                int jitter = backoff_ms / 4;
                                std::uniform_int_distribution<int> dist(0, jitter);
                                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms + dist(rng)));
                                backoff_ms = std::min(backoff_ms * 2, 4000);
                            }

                            if (s3_client->DeleteObject(config.bucket, k)) {
                                success = true;
                                break;
                            }
                        }

                        if (success) {
                            deleted_count++;
                            progress.objects_deleted++;
                            bytes_freed_atomic += sz;
                            progress.bytes_freed += sz;

                            // Adaptive concurrency: increase on success
                            size_t successes = consecutive_successes.fetch_add(1) + 1;
                            if (successes % 50 == 0) {
                                size_t current = current_max_concurrency.load();
                                if (current < max_concurrency) {
                                    size_t new_max = std::min(current * 2, max_concurrency);
                                    if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                                        spdlog::debug("Increasing delete concurrency to {}", new_max);
                                    }
                                }
                            }
                        } else {
                            // All retries failed. S3 may still have applied the
                            // delete before erroring (rate limiting), so ask
                            // whether the object is there - but only a definite
                            // NotFound counts. A size lookup cannot be used here:
                            // it returns the same sentinel for "absent" and for
                            // "the lookup itself failed", so a permission error
                            // or a throttle would be recorded as a deletion.
                            auto presence = s3_client->CheckObjectPresence(config.bucket, k);
                            if (presence == S3ObjectPresence::NotFound) {
                                // Confirmed gone - the delete did apply
                                deleted_count++;
                                progress.objects_deleted++;
                                bytes_freed_atomic += sz;
                                progress.bytes_freed += sz;
                                spdlog::debug("Rate-limited delete verified as successful: {}", k);

                                size_t successes = consecutive_successes.fetch_add(1) + 1;
                                if (successes % 50 == 0) {
                                    size_t current = current_max_concurrency.load();
                                    if (current < max_concurrency) {
                                        size_t new_max = std::min(current * 2, max_concurrency);
                                        current_max_concurrency.compare_exchange_weak(current, new_max);
                                    }
                                }
                            } else {
                                // Object still exists - genuine failure
                                consecutive_successes.store(0);
                                size_t current = current_max_concurrency.load();
                                size_t new_max = std::max(current / 2, static_cast<size_t>(16));
                                while (new_max < current) {
                                    if (current_max_concurrency.compare_exchange_weak(current, new_max)) {
                                        spdlog::debug("Reducing delete concurrency to {}", new_max);
                                        break;
                                    }
                                    new_max = std::max(current / 2, static_cast<size_t>(16));
                                }

                                failed_count++;
                                progress.objects_failed++;
                                std::lock_guard<std::mutex> lock(failed_mutex);
                                all_failed.push_back(k);
                            }
                        }
                    });
                }
            }

            // Wait for all to complete
            {
                std::unique_lock<std::mutex> lock(concurrency_mutex);
                concurrency_cv.wait(lock, [&] { return in_flight == 0; });
            }

            pool->join();

            result.objects_deleted = deleted_count.load();
            result.objects_failed = failed_count.load();
            result.bytes_freed = bytes_freed_atomic.load();
            result.failed_keys = std::move(all_failed);
            result.success = (result.objects_failed == 0);

            // A cancelled run breaks out of the posting loop, so everything it
            // never got to is neither deleted nor counted as failed. Judging
            // the run by objects_failed alone then reported success with
            // almost the whole prefix still in the bucket, and the CLI exited
            // 0 - so a script that deletes and then moves on believed the
            // objects were gone.
            if (progress.cancelled) {
                // Judge it by what is confirmed gone, not by objects_failed.
                // A task that was posted and then saw the cancel books itself
                // as failed without ever calling S3, so counting those as
                // "attempted" drove the remainder to zero and said nothing
                // while objects were still there.
                const int64_t total = static_cast<int64_t>(entries.size());
                const int64_t remaining = total - result.objects_deleted;
                if (remaining > 0) {
                    result.success = false;
                    result.error_message =
                        "Cancelled after deleting " + std::to_string(result.objects_deleted) +
                        " of " + std::to_string(total) + " objects under s3://" +
                        config.bucket + "/" + effective_prefix + ". " +
                        std::to_string(remaining) + " were not deleted and are still there.";
                }
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        result.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
        return result;

    } catch (const std::exception& e) {
        result.error_message = std::string("Exception: ") + e.what();
        return result;
    }
}
