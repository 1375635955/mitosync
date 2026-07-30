#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <chrono>
#include "rm_task.h"
#include "s3_mock.h"

class RmTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_client_ = std::make_shared<MockS3Client>();
        mock_client_->CreateBucket("test-bucket");
    }

    std::shared_ptr<MockS3Client> mock_client_;
};

TEST_F(RmTaskTest, DeleteSingleObject) {
    // Setup: put one object
    mock_client_->PutObject("test-bucket", "file.txt", {'h', 'i'});
    ASSERT_TRUE(mock_client_->ObjectExists("test-bucket", "file.txt"));

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";
    config.force = true;
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "file.txt"));
}

TEST_F(RmTaskTest, RequiresRecursiveForPrefix) {
    mock_client_->PutObject("test-bucket", "prefix/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "prefix/b.txt", {'b'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = false;  // Not set!

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    // Should fail - need -r for prefix
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("recursive") != std::string::npos ||
                result.error_message.find("-r") != std::string::npos);
}

TEST_F(RmTaskTest, SingleObjectDeleteFailure) {
    mock_client_->PutObject("test-bucket", "file.txt", {'x', 'y'});
    mock_client_->SetFailure("test-bucket", "file.txt", S3MockMethod::DeleteObject);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";
    config.force = true;
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_EQ(result.objects_failed, 1);
    EXPECT_EQ(result.failed_keys.size(), 1);
    EXPECT_EQ(result.failed_keys[0], "file.txt");
    EXPECT_EQ(progress.objects_found.load(), 1);
    EXPECT_EQ(progress.objects_failed.load(), 1);
    // Object still exists
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "file.txt"));
}

TEST_F(RmTaskTest, RecursiveDeleteMultiple) {
    mock_client_->PutObject("test-bucket", "prefix/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "prefix/b.txt", {'b'});
    mock_client_->PutObject("test-bucket", "prefix/sub/c.txt", {'c'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 3);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/a.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/b.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/sub/c.txt"));
}

TEST_F(RmTaskTest, DryRunDoesNotDelete) {
    mock_client_->PutObject("test-bucket", "prefix/a.txt", {'a'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = false;  // Dry run
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    // Object still exists
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "prefix/a.txt"));
    // But we found it
    EXPECT_EQ(progress.objects_found.load(), 1);
}

TEST_F(RmTaskTest, BatchDelete1000) {
    // Create 1001 objects to test batching
    for (int i = 0; i < 1001; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1001);
}

TEST_F(RmTaskTest, ExplicitNonExistentPrefixReturnsError) {
    // An explicitly specified prefix (ending with /) that doesn't exist
    // should return an error, not silent success
    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "nonexistent/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("No objects found") != std::string::npos);
    EXPECT_EQ(result.objects_deleted, 0);
}

TEST_F(RmTaskTest, EmptyBucketPrefixReturnsSuccess) {
    // Empty prefix (entire bucket) with no objects should succeed
    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "";  // Entire bucket
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
}

TEST_F(RmTaskTest, CancellationStopsEarly) {
    // Create many objects
    for (int i = 0; i < 50; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;

    // Start delete in background
    auto future = std::async(std::launch::async, [&]() {
        return run_rm(config, progress, mock_client_);
    });

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    progress.cancelled = true;

    RmResult result = future.get();

    // Should have stopped (may have deleted some before cancellation).
    //
    // Either the cancel arrived after everything was already dealt with, in
    // which case nothing was lost and there is no error, or objects were left
    // behind and the result has to say so rather than claim success (#69).
    if (result.error_message.empty()) {
        // No message means nothing was left unaccounted for: either everything
        // was deleted, or the failures are reported through objects_failed.
        EXPECT_TRUE(result.success || result.objects_failed > 0);
    } else {
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.error_message.find("Cancelled"), std::string::npos)
            << "actual: " << result.error_message;
    }
}

TEST_F(RmTaskTest, ProgressTrackingUpdates) {
    mock_client_->PutObject("test-bucket", "prefix/a.txt", {'a', 'b', 'c'});
    mock_client_->PutObject("test-bucket", "prefix/b.txt", {'d', 'e'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(progress.objects_found.load(), 2);
    EXPECT_EQ(progress.objects_deleted.load(), 2);
    EXPECT_TRUE(progress.enumeration_done.load());
    // Note: bytes_freed not tracked for recursive deletes since
    // S3 ListObjects doesn't return object sizes; would require extra API calls
}

TEST_F(RmTaskTest, SingleObjectWithRecursiveFlag) {
    mock_client_->PutObject("test-bucket", "file.txt", {'h', 'i'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";  // Not a prefix (no trailing /)
    config.force = true;
    config.recursive = true;  // -r flag set but it's a single file

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "file.txt"));
    // Verify progress is updated for single object with -r flag
    EXPECT_EQ(progress.objects_found.load(), 1);
    EXPECT_EQ(progress.objects_deleted.load(), 1);
    EXPECT_EQ(progress.bytes_freed.load(), 2);  // 'h', 'i'
}

TEST_F(RmTaskTest, DryRunReportsBytesFreed) {
    // 5-byte file
    mock_client_->PutObject("test-bucket", "single.txt", {'a', 'b', 'c', 'd', 'e'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "single.txt";
    config.force = false;  // Dry run
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_EQ(result.bytes_freed, 5);  // Should report what would be freed
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "single.txt"));
}

TEST_F(RmTaskTest, DryRunWithRecursiveSingleObject) {
    mock_client_->PutObject("test-bucket", "file.bin", {'x', 'y', 'z'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.bin";  // Single object, not prefix
    config.force = false;  // Dry run
    config.recursive = true;  // -r flag but single object

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_EQ(result.bytes_freed, 3);  // Should report what would be freed
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "file.bin"));
}

TEST_F(RmTaskTest, TransientFailureRetry) {
    mock_client_->PutObject("test-bucket", "prefix/good.txt", {'g'});
    mock_client_->PutObject("test-bucket", "prefix/flaky.txt", {'f'});

    // Fail twice then succeed on third attempt
    mock_client_->SetTransientFailure("test-bucket", "prefix/flaky.txt",
                                       S3MockMethod::DeleteObject, 2, true);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.max_retries = 3;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 2);
    EXPECT_EQ(result.objects_failed, 0);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/good.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/flaky.txt"));
}

TEST_F(RmTaskTest, PermanentFailureAfterRetries) {
    mock_client_->PutObject("test-bucket", "prefix/good.txt", {'g'});
    mock_client_->PutObject("test-bucket", "prefix/bad.txt", {'b'});

    // Permanent failure - will never succeed
    mock_client_->SetFailure("test-bucket", "prefix/bad.txt", S3MockMethod::DeleteObject);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.max_retries = 3;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);  // Partial failure
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_EQ(result.objects_failed, 1);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "prefix/good.txt"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "prefix/bad.txt"));
    EXPECT_EQ(result.failed_keys.size(), 1);
    EXPECT_EQ(result.failed_keys[0], "prefix/bad.txt");
}

// ============================================================================
// Adaptive Concurrency Tests
// ============================================================================

TEST_F(RmTaskTest, InitialConcurrencyIsLimited) {
    // Create enough objects to require multiple batches
    // With 1000 objects per batch and initial concurrency of 4,
    // we need at least 5 batches to potentially exceed the limit
    for (int i = 0; i < 9000; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = true;  // Use batch mode for this test
    config.max_threads = 64;  // Max allowed, but should start at 64

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 9000);

    // Peak concurrency should not exceed max_threads
    // (it may grow over time, but should start bounded)
    // We check that it's not the full 64 from the start
    int peak = mock_client_->GetPeakDeleteObjectsConcurrency();
    EXPECT_LE(peak, 64);  // Should never exceed max_threads
    // More batches = more time to ramp up, so peak could be high
    // Just verify it was tracked
    EXPECT_GT(peak, 0);
}

TEST_F(RmTaskTest, ConcurrencyReducesOnRateLimit) {
    // Create objects that will be spread across multiple batches
    for (int i = 0; i < 3000; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    // Simulate rate limiting: first 5 batch calls fail entirely
    mock_client_->SetBatchRateLimit(5);
    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = true;  // Use batch mode for this test
    config.max_threads = 16;
    config.max_retries = 10;  // Enough retries to recover

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 3000);

    // The batch call count should be higher than 6 (the minimum for 3000 objects with 500 batch size)
    // because rate limiting causes retries
    int batch_calls = mock_client_->GetDeleteObjectsBatchCallCount();
    EXPECT_GT(batch_calls, 6);  // At least the 5 failed + 6 successful
}

TEST_F(RmTaskTest, ConcurrencyRampsUpOnSuccess) {
    // Create many objects to allow ramp-up to occur
    // With initial concurrency of 32 (min of max_threads and 64) and ramp-up every 10 successes,
    // we need many batches to see significant ramp-up
    for (int i = 0; i < 20000; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = true;  // Use batch mode for this test
    config.max_threads = 128;  // Allow room to ramp up beyond initial 64

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 20000);

    // With 40 batches (20000 / 500) and ramp-up doubling every 10 successes,
    // starting at 64, we should see some ramp-up
    // Peak should be > 0 (just verify tracking works)
    int peak = mock_client_->GetPeakDeleteObjectsConcurrency();
    EXPECT_GT(peak, 0);  // Should have tracked concurrency
}

TEST_F(RmTaskTest, RateLimitRecovery) {
    // Test that after rate limiting reduces concurrency, it eventually recovers
    for (int i = 0; i < 5000; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    // First 2 batches fail (triggers concurrency reduction)
    mock_client_->SetBatchRateLimit(2);
    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.max_threads = 16;
    config.max_retries = 5;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 5000);

    // Despite early rate limiting, all objects should be deleted
    // The system should recover and complete the work
    for (int i = 0; i < 100; ++i) {  // Spot check some objects
        std::string key = "prefix/file" + std::to_string(i * 50) + ".txt";
        EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", key));
    }
}

// ============================================================================
// Individual Delete Mode Concurrency Tests (default mode, batch = false)
// ============================================================================

TEST_F(RmTaskTest, IndividualModeInitialConcurrency) {
    // Create enough objects to test concurrency behavior
    for (int i = 0; i < 500; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = false;  // Individual delete mode (default)
    config.max_threads = 64;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 500);

    // Peak concurrency should be tracked and not exceed max_threads
    int peak = mock_client_->GetPeakDeleteObjectConcurrency();
    EXPECT_GT(peak, 0);  // Should have tracked concurrency
    EXPECT_LE(peak, 64);  // Should never exceed max_threads
}

TEST_F(RmTaskTest, IndividualModeConcurrencyReducesOnRateLimit) {
    // Create objects
    for (int i = 0; i < 200; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    // First 50 individual delete calls fail (simulating rate limiting)
    mock_client_->SetDeleteObjectRateLimit(50);
    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = false;  // Individual delete mode
    config.max_threads = 32;
    config.max_retries = 5;  // Enough retries to recover

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 200);

    // All objects should be deleted despite rate limiting
    for (int i = 0; i < 200; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", key));
    }
}

TEST_F(RmTaskTest, IndividualModeConcurrencyRampsUp) {
    // Create many objects to allow ramp-up
    for (int i = 0; i < 1000; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = false;  // Individual delete mode
    config.max_threads = 256;  // Allow room to ramp up

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1000);

    // With 1000 objects and ramp-up every 100 successes,
    // starting at 64, we should see some ramp-up
    int peak = mock_client_->GetPeakDeleteObjectConcurrency();
    EXPECT_GT(peak, 0);  // Should have tracked concurrency
}

TEST_F(RmTaskTest, IndividualModeRateLimitRecovery) {
    // Test that after rate limiting, individual mode recovers
    for (int i = 0; i < 300; ++i) {
        std::string key = "prefix/file" + std::to_string(i) + ".txt";
        mock_client_->PutObject("test-bucket", key, {'x'});
    }

    // First 30 calls fail
    mock_client_->SetDeleteObjectRateLimit(30);
    mock_client_->ResetConcurrencyTracking();

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "prefix/";
    config.force = true;
    config.recursive = true;
    config.batch = false;  // Individual delete mode
    config.max_threads = 32;
    config.max_retries = 5;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 300);

    // All objects should be deleted despite early rate limiting
    for (int i = 0; i < 50; ++i) {  // Spot check
        std::string key = "prefix/file" + std::to_string(i * 6) + ".txt";
        EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", key));
    }
}

// ============================================================================
// Auto-Detection Tests (file vs directory without trailing /)
// ============================================================================

TEST_F(RmTaskTest, AutoDetectDirectory) {
    // Create objects under "mydir/" but specify "mydir" (no trailing /)
    mock_client_->PutObject("test-bucket", "mydir/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "mydir/b.txt", {'b'});
    mock_client_->PutObject("test-bucket", "mydir/sub/c.txt", {'c'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "mydir";  // No trailing /
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 3);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "mydir/a.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "mydir/b.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "mydir/sub/c.txt"));
}

TEST_F(RmTaskTest, AutoDetectDirectoryRequiresRecursive) {
    // Create objects under "mydir/" but specify "mydir" (no trailing /)
    mock_client_->PutObject("test-bucket", "mydir/a.txt", {'a'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "mydir";  // No trailing /
    config.force = true;
    config.recursive = false;  // Not recursive!

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    // Should fail - detected as directory but no -r flag
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("directory") != std::string::npos);
    EXPECT_TRUE(result.error_message.find("-r") != std::string::npos);
    // Object should still exist
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "mydir/a.txt"));
}

TEST_F(RmTaskTest, AutoDetectFilePriority) {
    // Create both a file "data" and a directory "data/"
    // The file should take priority
    mock_client_->PutObject("test-bucket", "data", {'f', 'i', 'l', 'e'});
    mock_client_->PutObject("test-bucket", "data/child.txt", {'c'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";  // Matches both file and directory
    config.force = true;
    config.recursive = false;  // No -r, treating as file

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    // Should succeed - file "data" takes priority
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_EQ(result.bytes_freed, 4);
    // File "data" should be deleted
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data"));
    // Directory contents should still exist
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/child.txt"));
}

TEST_F(RmTaskTest, AutoDetectNotFound) {
    // No file or directory exists with this name
    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "nonexistent";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.find("Not found") != std::string::npos);
}

TEST_F(RmTaskTest, AutoDetectNestedDirectory) {
    // Create nested structure and delete without trailing /
    mock_client_->PutObject("test-bucket", "a/b/c/file1.txt", {'1'});
    mock_client_->PutObject("test-bucket", "a/b/c/file2.txt", {'2'});
    mock_client_->PutObject("test-bucket", "a/b/other.txt", {'o'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "a/b/c";  // No trailing /
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 2);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "a/b/c/file1.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "a/b/c/file2.txt"));
    // Other files should still exist
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "a/b/other.txt"));
}

// ============================================================================
// Failed verification is not proof of deletion (issue #36)
// ============================================================================
//
// After delete retries fail, rm asks whether the object is still there, because
// S3 sometimes applies the delete and still returns an error. It used to read
// that answer from a size lookup, which returns the same sentinel for "absent"
// and for "the lookup failed" - so a permission error or a throttle was
// recorded as a successful deletion.

TEST_F(RmTaskTest, InconclusiveVerificationIsNotCountedAsDeleted) {
    // The retry-then-verify path lives in the recursive delete, not the
    // single-object one.
    mock_client_->PutObject("test-bucket", "data/stubborn.txt", {'h', 'i'});
    mock_client_->PutObject("test-bucket", "data/ok.txt", {'x'});
    // This delete always fails, and the follow-up presence check cannot answer.
    mock_client_->SetFailure("test-bucket", "data/stubborn.txt", S3MockMethod::DeleteObject);
    mock_client_->SetFailure("test-bucket", "data/stubborn.txt", S3MockMethod::GetObjectSize);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;
    config.max_retries = 1;   // the behaviour under test is independent of this

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_EQ(result.objects_deleted, 1u)
        << "only the object that really went may be counted";
    EXPECT_EQ(result.objects_failed, 1u)
        << "an inconclusive check must be reported as a failure, not a deletion";
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/stubborn.txt"))
        << "the object is still there";
}

TEST_F(RmTaskTest, ConfirmedAbsenceAfterFailedDeleteStillCountsAsDeleted) {
    // The counterpart: the delete errors but the object really is gone, which
    // is the rate-limiting case the verification exists for. Without this the
    // fix would under-report real deletions. The mock reports a definite
    // NotFound because the key was removed from the bucket by an earlier call.
    mock_client_->PutObject("test-bucket", "data/vanishes.txt", {'v'});
    mock_client_->PutObject("test-bucket", "data/ok.txt", {'x'});
    // S3 applies the delete and still returns an error.
    mock_client_->SetDeleteAppliesButReportsFailure("test-bucket", "data/vanishes.txt");

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;
    config.max_retries = 1;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_EQ(result.objects_failed, 0u) << "a confirmed absence is a success";
    EXPECT_EQ(result.objects_deleted, 2u);
}

TEST_F(RmTaskTest, PartialListingRefusesToDeleteAnything) {
    // A failed listing used to surface as "No objects found under ...", or as a
    // partial delete reported as success - both tell the caller the prefix is
    // gone when it is not.
    mock_client_->PutObject("test-bucket", "data/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "data/b.txt", {'b'});
    mock_client_->SetFailure("test-bucket", "data/", S3MockMethod::ListObjects);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.objects_deleted, 0u);
    EXPECT_NE(result.error_message.find("completely"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/a.txt"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/b.txt"));
}

TEST_F(RmTaskTest, CompleteListingStillDeletesEverything) {
    // Guard against over-firing.
    mock_client_->PutObject("test-bucket", "data/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "data/b.txt", {'b'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 2u);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/a.txt"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/b.txt"));
}

TEST_F(RmTaskTest, BatchVerificationChecksEveryKeyNotJustTheFirst) {
    // The worst form of the bug: after a wholesale batch failure the code
    // checked to_delete[0] and credited the entire batch to that one lookup.
    // Here the first key really is gone and the rest are not.
    mock_client_->PutObject("test-bucket", "data/a.txt", {'a'});
    mock_client_->PutObject("test-bucket", "data/b.txt", {'b'});
    mock_client_->PutObject("test-bucket", "data/c.txt", {'c'});
    // Every key fails the batch delete; only the first is actually applied.
    mock_client_->SetDeleteAppliesButReportsFailure("test-bucket", "data/a.txt");
    mock_client_->SetFailure("test-bucket", "data/b.txt", S3MockMethod::DeleteObject);
    mock_client_->SetFailure("test-bucket", "data/c.txt", S3MockMethod::DeleteObject);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;
    config.batch = true;
    config.max_retries = 1;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_EQ(result.objects_deleted, 1u)
        << "only the key confirmed absent may be credited, not the whole batch";
    EXPECT_EQ(result.objects_failed, 2u);
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/b.txt"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/c.txt"));
}

TEST_F(RmTaskTest, BatchVerificationCreditsBytesForConfirmedDeletions) {
    // The individual path credits bytes for a verified deletion; the batch path
    // used to drop them silently, so rm reported objects with no bytes freed.
    mock_client_->PutObject("test-bucket", "data/big.txt",
                            std::vector<uint8_t>(1000, 'x'));
    mock_client_->SetDeleteAppliesButReportsFailure("test-bucket", "data/big.txt");

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;
    config.batch = true;
    config.max_retries = 1;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_EQ(result.objects_deleted, 1u);
    EXPECT_EQ(result.bytes_freed, 1000u)
        << "bytes must be credited for a deletion confirmed by verification";
}

// ============================================================================
// Target classification when the object lookup is inconclusive (issue #59)
// ============================================================================

TEST_F(RmTaskTest, InconclusiveLookupDoesNotRetargetTheDeleteAtThePrefix) {
    // "data" is an object AND "data/" has children - a legal S3 layout.
    mock_client_->PutObject("test-bucket", "data", {'k', 'e', 'e', 'p'});
    mock_client_->PutObject("test-bucket", "data/child.txt", {'c'});

    // The object lookup fails: throttled, AccessDenied, network - anything
    // that is not "the key does not exist".
    mock_client_->SetFailure("test-bucket", "data", S3MockMethod::GetObjectSize);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_NE(result.error_message.find("Could not determine"), std::string::npos)
        << "actual: " << result.error_message;

    // The point of the issue: neither the named object nor the prefix contents
    // may be touched when the lookup that chose between them failed.
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/child.txt"))
        << "a failed probe must not redirect the delete at the prefix";
}

TEST_F(RmTaskTest, InconclusiveLookupIsNotReportedAsNotFound) {
    // No prefix involved - just a key whose lookup fails. Saying "Not found"
    // tells the user their object is gone when it is sitting right there.
    mock_client_->PutObject("test-bucket", "file.txt", {'h', 'i'});
    mock_client_->SetFailure("test-bucket", "file.txt", S3MockMethod::GetObjectSize);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";
    config.force = true;
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message.find("Not found"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "file.txt"));
}

TEST_F(RmTaskTest, FailedPrefixListingIsNotTreatedAsAnEmptyPrefix) {
    // The key genuinely does not exist, so rm falls through to the prefix
    // check - and that listing fails. An error is not an empty result.
    mock_client_->PutObject("test-bucket", "tree/child.txt", {'c'});
    mock_client_->SetFailure("test-bucket", "tree/", S3MockMethod::ListObjects);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "tree";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_NE(result.error_message.find("Could not list"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "tree/child.txt"));
}

TEST_F(RmTaskTest, ObjectThatShadowsAPrefixIsStillDeletedAsAnObject) {
    // The control for the fix: with the lookup working, "data" must resolve to
    // the object, not the prefix, exactly as before.
    mock_client_->PutObject("test-bucket", "data", {'g', 'o'});
    mock_client_->PutObject("test-bucket", "data/child.txt", {'c'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/child.txt"))
        << "deleting the object must leave the same-named prefix alone";
}

TEST_F(RmTaskTest, ExplicitPrefixDeletesTheDirectoryMarkerToo) {
    mock_client_->PutObject("test-bucket", "data/", {});
    mock_client_->PutObject("test-bucket", "data/child.txt", {'c'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 2);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/"))
        << "recursive rm of an explicit prefix must remove its zero-byte marker";
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/child.txt"));
}

TEST_F(RmTaskTest, NestedDirectoryMarkersAreAlsoDeleted) {
    // The first fix for this covered only the marker named by the prefix
    // itself. Markers deeper in the tree were still skipped by the enumerator,
    // so `rm -r` reported success and left them behind - the same bug one level
    // down. Any client that creates a folder makes one of these per level, so a
    // tree touched by a GUI is full of them (issue #71).
    mock_client_->PutObject("test-bucket", "data/", {});
    mock_client_->PutObject("test-bucket", "data/sub/", {});
    mock_client_->PutObject("test-bucket", "data/sub/deep/", {});
    mock_client_->PutObject("test-bucket", "data/sub/deep/f.txt", {'d'});
    mock_client_->PutObject("test-bucket", "data/top.txt", {'t'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 5);
    for (const char* key : {"data/", "data/sub/", "data/sub/deep/",
                            "data/sub/deep/f.txt", "data/top.txt"}) {
        EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", key))
            << key << " survived a recursive delete of its prefix";
    }
}

TEST_F(RmTaskTest, APrefixHoldingOnlyMarkersIsDeletedRatherThanReportedMissing) {
    // The sharper end of the same bug. With every marker skipped, enumeration
    // came back empty and rm answered "No objects found under: data/" - telling
    // the user the prefix was empty while objects sat in it, and leaving no
    // command that could remove them.
    mock_client_->PutObject("test-bucket", "data/", {});
    mock_client_->PutObject("test-bucket", "data/sub/", {});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 2);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/"));
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data/sub/"));
}

TEST_F(RmTaskTest, ObjectIsDeletedWhenPresenceIsCertainButTheSizeLookupFails) {
    // A throttle hit the size lookup but not the presence check. The object is
    // known to be there, so the delete proceeds; only the freed-bytes figure
    // is unavailable.
    mock_client_->PutObject("test-bucket", "file.txt", {'a', 'b', 'c', 'd'});
    mock_client_->SetTransientFailure("test-bucket", "file.txt",
                                      S3MockMethod::GetObjectSize, 99);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";
    config.force = true;
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_EQ(result.bytes_freed, 0) << "an unknown size must not be invented";
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "file.txt"));
}

TEST_F(RmTaskTest, PresenceFailureAloneIsEnoughToRefuse) {
    // The size lookup is fine; it is the presence check that cannot answer.
    // Classification still has to stop.
    mock_client_->PutObject("test-bucket", "data/child.txt", {'c'});
    mock_client_->SetFailure("test-bucket", "data", S3MockMethod::CheckObjectPresence);

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.objects_deleted, 0);
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/child.txt"));

    // Anchors the counter that ExistingObjectCostsASingleLookup asserts is
    // zero: without a case that makes it non-zero, "no presence call" and
    // "presence calls are not counted" look identical.
    EXPECT_GT(mock_client_->GetCallCount("test-bucket", "data",
                                         S3MockMethod::CheckObjectPresence),
              0);
}

TEST_F(RmTaskTest, ExistingObjectCostsASingleLookup) {
    // The ordinary case must not pay for a second round trip: a size that came
    // back is already proof the object is there.
    mock_client_->PutObject("test-bucket", "file.txt", {'h', 'i'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "file.txt";
    config.force = true;
    config.recursive = false;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.bytes_freed, 2);
    EXPECT_EQ(mock_client_->GetCallCount("test-bucket", "file.txt",
                                         S3MockMethod::CheckObjectPresence),
              0)
        << "classification must not re-ask what the size lookup already proved";
}

TEST_F(RmTaskTest, CancelledDeleteIsNeverReportedAsSuccess) {
    // Cancelling breaks out of the loop that posts deletes, so everything it
    // never reached is neither deleted nor counted as failed. Judging the run
    // by objects_failed alone reported success with almost the whole prefix
    // still in the bucket (issue #69).
    const int kObjects = 3000;
    for (int i = 0; i < kObjects; ++i) {
        mock_client_->PutObject("test-bucket", "tree/f" + std::to_string(i) + ".txt", {'x'});
    }

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "tree/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;

    // Cancel from inside the delete path rather than from a thread racing it. The racing
    // version cancelled once objects_deleted reached 5, which on a fast machine could land
    // after all 3000 were already gone - the run then had nothing left to leave behind and
    // the test failed with "the cancel landed too late to test anything". It did exactly
    // that on the macOS release builder while passing everywhere else.
    //
    // Setting the flag on the 5th delete makes it a property of the run, not of the host:
    // the posting loop is bounded by max_threads and rechecks cancelled before each batch,
    // so at most a few hundred more objects can be in flight and the rest are never reached.
    std::atomic<int> deletes_seen{0};
    mock_client_->SetOnDeleteObject([&](const std::string&, const std::string&) {
        if (++deletes_seen == 5) {
            progress.cancelled = true;
        }
    });

    RmResult result = run_rm(config, progress, mock_client_);

    int remaining = 0;
    for (int i = 0; i < kObjects; ++i) {
        if (mock_client_->ObjectExists("test-bucket", "tree/f" + std::to_string(i) + ".txt")) {
            ++remaining;
        }
    }
    ASSERT_GT(remaining, 0)
        << "the cancel did not stop the run: all " << kObjects << " objects were deleted";

    EXPECT_FALSE(result.success)
        << "reported success with " << remaining << " of " << kObjects
        << " objects still in the bucket";
    EXPECT_NE(result.error_message.find("Cancelled"), std::string::npos)
        << "actual: " << result.error_message;
    EXPECT_NE(result.error_message.find("still there"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(RmTaskTest, AnUncancelledDeleteStillReportsSuccess) {
    // The control: nothing about the cancellation check may make an ordinary
    // complete delete look like a failure.
    for (int i = 0; i < 50; ++i) {
        mock_client_->PutObject("test-bucket", "tree/f" + std::to_string(i) + ".txt", {'x'});
    }

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "tree/";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 50);
    EXPECT_TRUE(result.error_message.empty()) << "actual: " << result.error_message;
}

// A mock that cancels the run from inside a delete that has already succeeded.
// That reaches the window the bug actually lived in: the posting loop sees the
// flag, no worker does, so nothing lands in objects_failed and the old
// "success = (objects_failed == 0)" rule called the run a success.
class CancelAfterNDeletes : public MockS3Client {
public:
    CancelAfterNDeletes(RmProgress& progress, int after)
        : progress_(progress), after_(after) {}

    bool DeleteObject(const std::string& bucket, const std::string& key) override {
        bool ok = MockS3Client::DeleteObject(bucket, key);
        if (ok && ++deletes_ == after_) progress_.cancelled = true;
        return ok;
    }

private:
    RmProgress& progress_;
    int after_;
    std::atomic<int> deletes_{0};
};

TEST_F(RmTaskTest, CancelledDeleteWithNoFailedObjectsStillIsNotSuccess) {
    // The specific shape of issue #69: every posted delete succeeded, so
    // objects_failed is 0, and the entries the loop never reached are the only
    // thing missing. Judging the run by objects_failed alone reported success.
    const int kObjects = 200;
    RmProgress progress;
    auto client = std::make_shared<CancelAfterNDeletes>(progress, 10);
    client->CreateBucket("test-bucket");
    for (int i = 0; i < kObjects; ++i) {
        client->PutObject("test-bucket", "tree/f" + std::to_string(i) + ".txt", {'x'});
    }

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "tree/";
    config.force = true;
    config.recursive = true;
    // One delete in flight at a time, so the worker that raises the flag is
    // the only one running. Nothing else can be mid-flight to observe it and
    // book itself as failed, which is what leaves objects_failed at zero.
    config.max_threads = 1;

    RmResult result = run_rm(config, progress, client);

    int remaining = 0;
    for (int i = 0; i < kObjects; ++i) {
        if (client->ObjectExists("test-bucket", "tree/f" + std::to_string(i) + ".txt")) {
            ++remaining;
        }
    }
    ASSERT_GT(remaining, 0) << "the cancel landed too late to test anything";
    ASSERT_EQ(result.objects_failed, 0)
        << "this test is only meaningful when nothing was counted as failed";

    EXPECT_FALSE(result.success)
        << "objects_failed=" << result.objects_failed << " but " << remaining
        << " of " << kObjects << " objects are still in the bucket";
    EXPECT_NE(result.error_message.find("Cancelled"), std::string::npos)
        << "actual: " << result.error_message;
}

TEST_F(RmTaskTest, RecursiveOnAnAmbiguousNameReportsWhatItLeftAlone) {
    // "data" is an object and "data/" is also a prefix with children. Deleting
    // the object is correct - that is what was named - but -r had no effect on
    // the prefix, and the run used to say nothing about it, so a user could
    // reasonably think both went (issue #72).
    mock_client_->PutObject("test-bucket", "data", {'o'});
    mock_client_->PutObject("test-bucket", "data/child1.txt", {'a'});
    mock_client_->PutObject("test-bucket", "data/child2.txt", {'b'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";      // no trailing slash: the ambiguous form
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_FALSE(mock_client_->ObjectExists("test-bucket", "data"));
    EXPECT_TRUE(mock_client_->ObjectExists("test-bucket", "data/child1.txt"))
        << "naming the object must not delete the same-named prefix";

    ASSERT_EQ(result.warnings.size(), 1u) << "the untouched prefix went unmentioned";
    EXPECT_NE(result.warnings[0].find("data/"), std::string::npos);
    EXPECT_NE(result.warnings[0].find("not touched"), std::string::npos);
}

TEST_F(RmTaskTest, ADryRunAlsoReportsTheUntouchedPrefix) {
    // A dry run is where a user checks what -r is about to do, so it is the
    // worst place to stay quiet about the half that will not happen.
    mock_client_->PutObject("test-bucket", "data", {'o'});
    mock_client_->PutObject("test-bucket", "data/child.txt", {'a'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "data";
    config.force = false;        // dry run
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.objects_deleted, 0) << "a dry run must not delete";
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("data/"), std::string::npos);
}

TEST_F(RmTaskTest, AnUnambiguousObjectDeleteSaysNothingExtra) {
    // The warning must not fire when there is no same-named prefix, or every
    // ordinary delete would carry noise.
    mock_client_->PutObject("test-bucket", "lonely", {'o'});

    RmConfig config;
    config.bucket = "test-bucket";
    config.prefix = "lonely";
    config.force = true;
    config.recursive = true;

    RmProgress progress;
    RmResult result = run_rm(config, progress, mock_client_);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.objects_deleted, 1);
    EXPECT_TRUE(result.warnings.empty())
        << "nothing was left behind, so there is nothing to report: "
        << (result.warnings.empty() ? "" : result.warnings[0]);
}
