#pragma once

#include <vector>
#include <string>
#include <future>
#include <utility>
#include <atomic>
#include <mutex>
#include <algorithm>

// Bucket info with name and region
struct BucketInfo {
    std::string name;
    std::string region;

    bool operator<(const BucketInfo& other) const {
        return name < other.name;
    }
};

// Bucket list state for GUI (extracted for testability)
struct BucketListState {
    enum class LoadState { Idle, Loading, Loaded, Error };
    LoadState state = LoadState::Idle;
    std::vector<BucketInfo> buckets;
    std::string error_message;
    std::future<std::pair<bool, std::vector<std::string>>> load_future;
    int selected_index = -1;  // Selected bucket index (-1 = none)

    // Progress tracking for loading
    std::atomic<int> total_buckets{0};
    std::atomic<int> checked_buckets{0};
    std::atomic<int> skipped_buckets{0};  // Buckets skipped due to access denied

    // Cancellation flag for async loading
    std::atomic<bool> cancel_requested{false};

    // Mutex for thread-safe bucket additions during loading (mutable for const methods)
    mutable std::mutex buckets_mutex;

    // Check if there is a valid bucket selection
    bool has_valid_selection() const {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        return selected_index >= 0 &&
               static_cast<size_t>(selected_index) < buckets.size();
    }

    // Get the selected bucket name, or empty string if none
    std::string get_selected_bucket() const {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        if (selected_index < 0 || static_cast<size_t>(selected_index) >= buckets.size()) {
            return "";
        }
        return buckets[static_cast<size_t>(selected_index)].name;
    }

    // Get the selected bucket's region, or empty string if none
    std::string get_selected_region() const {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        if (selected_index < 0 || static_cast<size_t>(selected_index) >= buckets.size()) {
            return "";
        }
        return buckets[static_cast<size_t>(selected_index)].region;
    }

    // Reset the state to idle (does NOT clear cancel_requested - that's handled by StartBucketListLoad)
    void reset() {
        state = LoadState::Idle;
        {
            std::lock_guard<std::mutex> lock(buckets_mutex);
            buckets.clear();
        }
        error_message.clear();
        selected_index = -1;
        total_buckets = 0;
        checked_buckets = 0;
        skipped_buckets = 0;
    }

    // Full reset including cancellation flag (use when completely reinitializing)
    void reset_all() {
        cancel_requested = false;
        reset();
    }

    // Request cancellation of ongoing loading
    void cancel() {
        cancel_requested = true;
    }

    // Check if cancellation was requested
    bool is_cancelled() const {
        return cancel_requested.load();
    }

    // Thread-safe add bucket with region (used during progressive loading)
    void add_bucket(const std::string& bucket_name, const std::string& region) {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        BucketInfo info{bucket_name, region};
        // Insert in sorted order by name
        auto it = std::lower_bound(buckets.begin(), buckets.end(), info);
        buckets.insert(it, info);
    }

    // Thread-safe get bucket count
    size_t bucket_count() const {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        return buckets.size();
    }

    // Thread-safe copy of buckets for UI rendering
    std::vector<BucketInfo> get_buckets_copy() const {
        std::lock_guard<std::mutex> lock(buckets_mutex);
        return buckets;
    }

    // Set buckets from a successful load
    void set_buckets(std::vector<BucketInfo> bucket_list) {
        buckets = std::move(bucket_list);
        state = LoadState::Loaded;
        error_message.clear();
    }

    // Set error from a failed load
    void set_error(const std::string& error) {
        error_message = error;
        state = LoadState::Error;
        buckets.clear();
    }
};
