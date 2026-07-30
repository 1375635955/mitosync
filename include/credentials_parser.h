#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <algorithm>

// Parse AWS credentials file and extract profile names
// Profile names are lines that match the pattern: [profile-name]
inline std::vector<std::string> parse_aws_credential_profiles(std::istream& input) {
    std::vector<std::string> profiles;
    std::string line;

    while (std::getline(input, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Trim trailing whitespace
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            line = line.substr(0, end + 1);
        }

        // Check for profile section header: [profile-name]
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            std::string profile_name = line.substr(1, line.size() - 2);
            // Trim whitespace inside brackets
            start = profile_name.find_first_not_of(" \t");
            if (start == std::string::npos) {
                // All whitespace, skip
                continue;
            }
            end = profile_name.find_last_not_of(" \t");
            profile_name = profile_name.substr(start, end - start + 1);
            if (!profile_name.empty()) {
                profiles.push_back(profile_name);
            }
        }
    }

    return profiles;
}

// Get the default AWS credentials file path
inline std::string get_aws_credentials_path() {
    // Check AWS_SHARED_CREDENTIALS_FILE environment variable first
    const char* env_path = std::getenv("AWS_SHARED_CREDENTIALS_FILE");
    if (env_path && env_path[0] != '\0') {
        return env_path;
    }

    // Default to ~/.aws/credentials
    const char* home = std::getenv("HOME");
    if (!home) {
        home = std::getenv("USERPROFILE");  // Windows fallback
    }
    if (home) {
        return std::string(home) + "/.aws/credentials";
    }
    return "";
}

// Load credential profiles from the default AWS credentials file
inline std::vector<std::string> load_aws_credential_profiles() {
    std::string path = get_aws_credentials_path();
    if (path.empty()) {
        return {};
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }

    return parse_aws_credential_profiles(file);
}

// Credentials list state for GUI
struct CredentialsListState {
    enum class LoadState { Idle, Loading, Loaded, Error };
    LoadState state = LoadState::Idle;
    std::vector<std::string> profiles;
    std::string error_message;
    int selected_index[2] = {-1, -1};  // Selected profile index for each source (-1 = none/default)

    // Check if a source has a valid profile selection
    bool has_valid_selection(int source_idx) const {
        if (source_idx < 0 || source_idx > 1) return false;
        return selected_index[source_idx] >= 0 &&
               static_cast<size_t>(selected_index[source_idx]) < profiles.size();
    }

    // Get the selected profile name for a source, or empty string for default
    std::string get_selected_profile(int source_idx) const {
        if (!has_valid_selection(source_idx)) return "";
        return profiles[static_cast<size_t>(selected_index[source_idx])];
    }

    // Reset the state
    void reset() {
        state = LoadState::Idle;
        profiles.clear();
        error_message.clear();
        selected_index[0] = -1;
        selected_index[1] = -1;
    }

    // Set profiles from a successful load
    void set_profiles(std::vector<std::string> profile_list) {
        profiles = std::move(profile_list);
        state = LoadState::Loaded;
        error_message.clear();
    }

    // Set error from a failed load
    void set_error(const std::string& error) {
        error_message = error;
        state = LoadState::Error;
        profiles.clear();
    }

    // Find index of a profile by name, returns -1 if not found
    int find_profile_index(const std::string& name) const {
        for (size_t i = 0; i < profiles.size(); ++i) {
            if (profiles[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};
