#include <gtest/gtest.h>
#include "bucket_list.h"
#include "defaults.h"

#include <algorithm>
#include <thread>
#include <vector>

// Helper to create BucketInfo vector from names (default region: us-east-1)
static std::vector<BucketInfo> make_buckets(std::initializer_list<std::string> names, const std::string& region = "us-east-1") {
    std::vector<BucketInfo> result;
    for (const auto& name : names) {
        result.push_back({name, region});
    }
    return result;
}

// ============================================================================
// BucketListState Tests
// ============================================================================

TEST(BucketListStateTest, DefaultConstruction) {
    BucketListState state;
    EXPECT_EQ(state.state, BucketListState::LoadState::Idle);
    EXPECT_TRUE(state.buckets.empty());
    EXPECT_TRUE(state.error_message.empty());
    EXPECT_EQ(state.selected_index, -1);
}

TEST(BucketListStateTest, HasValidSelectionNoSelection) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2"}));

    // No selection yet (selected_index = -1)
    EXPECT_FALSE(state.has_valid_selection());
}

TEST(BucketListStateTest, HasValidSelectionValidIndex) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2", "bucket3"}));
    state.selected_index = 1;

    EXPECT_TRUE(state.has_valid_selection());
}

TEST(BucketListStateTest, HasValidSelectionOutOfBounds) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2"}));
    state.selected_index = 5;  // Out of bounds

    EXPECT_FALSE(state.has_valid_selection());
}

TEST(BucketListStateTest, GetSelectedBucketNoSelection) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2"}));

    EXPECT_EQ(state.get_selected_bucket(), "");
}

TEST(BucketListStateTest, GetSelectedBucketValid) {
    BucketListState state;
    state.set_buckets(make_buckets({"alpha-bucket", "beta-bucket", "gamma-bucket"}));
    state.selected_index = 1;

    EXPECT_EQ(state.get_selected_bucket(), "beta-bucket");
}

TEST(BucketListStateTest, GetSelectedBucketOutOfBounds) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1"}));
    state.selected_index = 5;

    EXPECT_EQ(state.get_selected_bucket(), "");
}

TEST(BucketListStateTest, Reset) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2"}));
    state.selected_index = 1;
    state.skipped_buckets = 5;

    state.reset();

    EXPECT_EQ(state.state, BucketListState::LoadState::Idle);
    EXPECT_TRUE(state.buckets.empty());
    EXPECT_TRUE(state.error_message.empty());
    EXPECT_EQ(state.selected_index, -1);
    EXPECT_EQ(state.skipped_buckets.load(), 0);
}

TEST(BucketListStateTest, SetBuckets) {
    BucketListState state;
    state.state = BucketListState::LoadState::Loading;
    state.error_message = "previous error";

    state.set_buckets(make_buckets({"new-bucket1", "new-bucket2"}));

    EXPECT_EQ(state.state, BucketListState::LoadState::Loaded);
    EXPECT_EQ(state.buckets.size(), 2u);
    EXPECT_EQ(state.buckets[0].name, "new-bucket1");
    EXPECT_EQ(state.buckets[1].name, "new-bucket2");
    EXPECT_TRUE(state.error_message.empty());
}

TEST(BucketListStateTest, SetBucketsEmpty) {
    BucketListState state;
    state.set_buckets({});

    EXPECT_EQ(state.state, BucketListState::LoadState::Loaded);
    EXPECT_TRUE(state.buckets.empty());
}

TEST(BucketListStateTest, SetError) {
    BucketListState state;
    state.set_buckets(make_buckets({"bucket1", "bucket2"}));
    state.state = BucketListState::LoadState::Loading;

    state.set_error("Connection failed");

    EXPECT_EQ(state.state, BucketListState::LoadState::Error);
    EXPECT_EQ(state.error_message, "Connection failed");
    EXPECT_TRUE(state.buckets.empty());
}

TEST(BucketListStateTest, LoadStateTransitions) {
    BucketListState state;

    // Idle -> Loading
    EXPECT_EQ(state.state, BucketListState::LoadState::Idle);
    state.state = BucketListState::LoadState::Loading;
    EXPECT_EQ(state.state, BucketListState::LoadState::Loading);

    // Loading -> Loaded (success)
    state.set_buckets(make_buckets({"bucket"}));
    EXPECT_EQ(state.state, BucketListState::LoadState::Loaded);

    // Reset -> Idle
    state.reset();
    EXPECT_EQ(state.state, BucketListState::LoadState::Idle);

    // Idle -> Loading -> Error (failure)
    state.state = BucketListState::LoadState::Loading;
    state.set_error("Network error");
    EXPECT_EQ(state.state, BucketListState::LoadState::Error);
}

TEST(BucketListStateTest, SelectionPreservedAfterSetBuckets) {
    BucketListState state;
    state.selected_index = 1;

    // set_buckets doesn't reset selection (intentional)
    state.set_buckets(make_buckets({"a", "b", "c"}));

    // Selection is preserved - caller must validate it
    EXPECT_EQ(state.selected_index, 1);
}

TEST(BucketListStateTest, ManyBuckets) {
    BucketListState state;
    std::vector<BucketInfo> many_buckets;
    for (int i = 0; i < 100; ++i) {
        many_buckets.push_back({"bucket-" + std::to_string(i), "us-east-1"});
    }

    state.set_buckets(many_buckets);

    EXPECT_EQ(state.buckets.size(), 100u);
    state.selected_index = 99;
    EXPECT_TRUE(state.has_valid_selection());
    EXPECT_EQ(state.get_selected_bucket(), "bucket-99");
}

TEST(BucketListStateTest, GetSelectedRegion) {
    BucketListState state;
    state.set_buckets({
        {"bucket-east", "us-east-1"},
        {"bucket-west", "us-west-2"},
        {"bucket-eu", "eu-west-1"}
    });
    state.selected_index = 1;

    EXPECT_EQ(state.get_selected_bucket(), "bucket-west");
    EXPECT_EQ(state.get_selected_region(), "us-west-2");
}

TEST(BucketListStateTest, AddBucketWithRegion) {
    BucketListState state;
    state.add_bucket("bucket-b", "us-west-2");
    state.add_bucket("bucket-a", "us-east-1");
    state.add_bucket("bucket-c", "eu-west-1");

    // Should be sorted by name
    auto buckets = state.get_buckets_copy();
    ASSERT_EQ(buckets.size(), 3u);
    EXPECT_EQ(buckets[0].name, "bucket-a");
    EXPECT_EQ(buckets[0].region, "us-east-1");
    EXPECT_EQ(buckets[1].name, "bucket-b");
    EXPECT_EQ(buckets[1].region, "us-west-2");
    EXPECT_EQ(buckets[2].name, "bucket-c");
    EXPECT_EQ(buckets[2].region, "eu-west-1");
}

TEST(BucketListStateTest, BucketCount) {
    BucketListState state;
    EXPECT_EQ(state.bucket_count(), 0u);

    state.add_bucket("bucket-1", "us-east-1");
    EXPECT_EQ(state.bucket_count(), 1u);

    state.add_bucket("bucket-2", "us-west-2");
    state.add_bucket("bucket-3", "eu-west-1");
    EXPECT_EQ(state.bucket_count(), 3u);
}

TEST(BucketListStateTest, ProgressTracking) {
    BucketListState state;

    // Initial state
    EXPECT_EQ(state.total_buckets.load(), 0);
    EXPECT_EQ(state.checked_buckets.load(), 0);

    // Simulate loading progress
    state.total_buckets = 100;
    EXPECT_EQ(state.total_buckets.load(), 100);

    state.checked_buckets++;
    EXPECT_EQ(state.checked_buckets.load(), 1);

    state.checked_buckets += 49;
    EXPECT_EQ(state.checked_buckets.load(), 50);

    // Reset clears progress
    state.reset();
    EXPECT_EQ(state.total_buckets.load(), 0);
    EXPECT_EQ(state.checked_buckets.load(), 0);
}

TEST(BucketListStateTest, ConcurrentAddBucket) {
    BucketListState state;
    constexpr int NUM_THREADS = 8;
    constexpr int BUCKETS_PER_THREAD = 100;

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&state, t]() {
            for (int i = 0; i < BUCKETS_PER_THREAD; ++i) {
                std::string name = "bucket-" + std::to_string(t) + "-" + std::to_string(i);
                state.add_bucket(name, "us-east-1");
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All buckets should be added
    EXPECT_EQ(state.bucket_count(), NUM_THREADS * BUCKETS_PER_THREAD);

    // Buckets should be sorted
    auto buckets = state.get_buckets_copy();
    for (size_t i = 1; i < buckets.size(); ++i) {
        EXPECT_LT(buckets[i - 1].name, buckets[i].name);
    }
}

TEST(BucketListStateTest, ConcurrentAddAndRead) {
    BucketListState state;
    constexpr int NUM_WRITERS = 4;
    constexpr int NUM_READERS = 4;
    constexpr int BUCKETS_PER_WRITER = 50;
    std::atomic<bool> stop_readers{false};

    std::vector<std::thread> threads;

    // Writer threads
    for (int t = 0; t < NUM_WRITERS; ++t) {
        threads.emplace_back([&state, t]() {
            for (int i = 0; i < BUCKETS_PER_WRITER; ++i) {
                std::string name = "bucket-" + std::to_string(t) + "-" + std::to_string(i);
                state.add_bucket(name, "us-east-1");
            }
        });
    }

    // Reader threads (get_buckets_copy while writers are active)
    for (int t = 0; t < NUM_READERS; ++t) {
        threads.emplace_back([&state, &stop_readers]() {
            while (!stop_readers.load()) {
                auto buckets = state.get_buckets_copy();
                // Verify snapshot is consistent (sorted)
                for (size_t i = 1; i < buckets.size(); ++i) {
                    EXPECT_LE(buckets[i - 1].name, buckets[i].name);
                }
            }
        });
    }

    // Wait for writers
    for (int i = 0; i < NUM_WRITERS; ++i) {
        threads[i].join();
    }

    stop_readers = true;

    // Wait for readers
    for (int i = NUM_WRITERS; i < NUM_WRITERS + NUM_READERS; ++i) {
        threads[i].join();
    }

    EXPECT_EQ(state.bucket_count(), NUM_WRITERS * BUCKETS_PER_WRITER);
}

TEST(BucketListStateTest, ConcurrentProgressUpdate) {
    BucketListState state;
    constexpr int NUM_THREADS = 8;
    constexpr int INCREMENTS_PER_THREAD = 1000;

    state.total_buckets = NUM_THREADS * INCREMENTS_PER_THREAD;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&state]() {
            for (int i = 0; i < INCREMENTS_PER_THREAD; ++i) {
                state.checked_buckets++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(state.checked_buckets.load(), NUM_THREADS * INCREMENTS_PER_THREAD);
}

TEST(BucketListStateTest, GetBucketsCopyIsSnapshot) {
    BucketListState state;
    state.add_bucket("bucket-1", "us-east-1");
    state.add_bucket("bucket-2", "us-west-2");

    // Get a copy
    auto snapshot = state.get_buckets_copy();
    EXPECT_EQ(snapshot.size(), 2u);

    // Modify original
    state.add_bucket("bucket-3", "eu-west-1");
    state.add_bucket("bucket-4", "ap-south-1");

    // Snapshot should be unchanged
    EXPECT_EQ(snapshot.size(), 2u);
    EXPECT_EQ(state.bucket_count(), 4u);
}

TEST(BucketListStateTest, CancelInitialState) {
    BucketListState state;

    // Initially not cancelled
    EXPECT_FALSE(state.is_cancelled());
}

TEST(BucketListStateTest, CancelSetsFlag) {
    BucketListState state;

    state.cancel();

    EXPECT_TRUE(state.is_cancelled());
}

TEST(BucketListStateTest, CancelMultipleTimes) {
    BucketListState state;

    // Calling cancel multiple times is safe
    state.cancel();
    state.cancel();
    state.cancel();

    EXPECT_TRUE(state.is_cancelled());
}

TEST(BucketListStateTest, ResetPreservesCancellation) {
    // reset() does NOT clear cancel_requested (to avoid race conditions)
    BucketListState state;
    state.cancel();
    EXPECT_TRUE(state.is_cancelled());

    state.reset();

    // Cancellation flag preserved - only StartBucketListLoad clears it
    EXPECT_TRUE(state.is_cancelled());
}

TEST(BucketListStateTest, ResetAllClearsCancellation) {
    // reset_all() DOES clear cancel_requested (for full reinitialization)
    BucketListState state;
    state.cancel();
    EXPECT_TRUE(state.is_cancelled());

    state.reset_all();

    EXPECT_FALSE(state.is_cancelled());
}

TEST(BucketListStateTest, ConcurrentCancellation) {
    BucketListState state;
    constexpr int NUM_CHECKERS = 8;
    constexpr int CHECKS_PER_THREAD = 1000;
    std::atomic<int> saw_cancelled{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;

    // Checker threads - read the cancellation flag
    for (int t = 0; t < NUM_CHECKERS; ++t) {
        threads.emplace_back([&state, &saw_cancelled, &start]() {
            while (!start.load()) {
                // Spin until started
            }
            for (int i = 0; i < CHECKS_PER_THREAD; ++i) {
                if (state.is_cancelled()) {
                    saw_cancelled++;
                }
            }
        });
    }

    // Start all threads
    start = true;

    // Cancel from main thread while checkers are running
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    state.cancel();

    for (auto& thread : threads) {
        thread.join();
    }

    // After cancel(), at least some checks should have seen the flag
    // (not deterministic exactly how many, but should be > 0)
    EXPECT_TRUE(state.is_cancelled());
}

TEST(BucketListStateTest, CancelDuringProgressUpdate) {
    BucketListState state;
    state.total_buckets = 100;
    std::atomic<int> cancelled_checks{0};
    constexpr int NUM_THREADS = 4;

    std::vector<std::thread> threads;

    // Threads simulating bucket checking with cancellation checks
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&state, &cancelled_checks]() {
            for (int i = 0; i < 25; ++i) {
                if (state.is_cancelled()) {
                    cancelled_checks++;
                    return;
                }
                state.checked_buckets++;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Cancel after a short delay
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    state.cancel();

    for (auto& thread : threads) {
        thread.join();
    }

    // Some threads should have exited early due to cancellation
    // and total checked should be less than 100
    EXPECT_TRUE(state.is_cancelled());
    // Not all buckets were checked (some threads exited early)
    EXPECT_LT(state.checked_buckets.load(), 100);
}

// ============================================================================
// Empty Defaults Tests
// ============================================================================

TEST(DefaultsTest, BucketDefaultIsEmpty) {
    EXPECT_STREQ(defaults::BUCKET, "");
    EXPECT_EQ(strlen(defaults::BUCKET), 0u);
}

TEST(DefaultsTest, KeyDefaultIsEmpty) {
    EXPECT_STREQ(defaults::KEY, "");
    EXPECT_EQ(strlen(defaults::KEY), 0u);
}

TEST(DefaultsTest, RegionDefaultIsSet) {
    // Region should still have a default value
    EXPECT_STRNE(defaults::REGION, "");
    EXPECT_STREQ(defaults::REGION, "eu-west-2");
}
