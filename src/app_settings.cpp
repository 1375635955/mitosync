#include "app_settings.h"

#include <cstdlib>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <pwd.h>
#endif

#ifndef _WIN32
// Thread-safe helper to get home directory via getpwuid_r
// Returns empty string on failure
static std::string getHomeDirectory() {
    // First try $HOME (almost always set)
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home);
    }

    // Fallback to getpwuid_r (thread-safe unlike getpwuid)
    struct passwd pwd;
    struct passwd* result = nullptr;
    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) {
        bufsize = 16384;  // Default if sysconf fails
    }

    std::vector<char> buf(static_cast<size_t>(bufsize));
    if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 && result) {
        return std::string(result->pw_dir);
    }

    return std::string();
}

// Create directory and all parent directories if needed
// Similar to mkdir -p
static void mkdirRecursive(const std::string& path) {
    if (path.empty()) return;

    // Find each component and create if needed
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty() && parent != "/") {
            mkdir(parent.c_str(), 0755);  // Ignore errors (may already exist)
        }
    }
    // Create final directory
    mkdir(path.c_str(), 0755);
}
#endif

// Get platform-specific application data directory
std::string GetAppDataDirectory() {
    std::string data_dir;

#ifdef _WIN32
    // Windows: %APPDATA%\MitoSync
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        data_dir = std::string(path) + "\\MitoSync";
    } else {
        // Fallback
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            data_dir = std::string(appdata) + "\\MitoSync";
        }
    }
#elif defined(__APPLE__)
    // macOS: ~/Library/Application Support/MitoSync
    std::string home = getHomeDirectory();
    if (!home.empty()) {
        data_dir = home + "/Library/Application Support/MitoSync";
    }
#else
    // Linux: $XDG_DATA_HOME/mitosync or ~/.local/share/mitosync
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        data_dir = std::string(xdg_data) + "/mitosync";
    } else {
        std::string home = getHomeDirectory();
        if (!home.empty()) {
            data_dir = home + "/.local/share/mitosync";
        }
    }
#endif

    // Fallback to current directory if all else fails
    if (data_dir.empty()) {
        data_dir = ".";
    }

    // Create the app directory if it doesn't exist
#ifdef _WIN32
    // Windows: parent (%APPDATA%) is always created by OS
    mkdir(data_dir.c_str(), 0755);
#else
    // macOS/Linux: create parent directories if needed
    // On minimal Linux installs, ~/.local/share may not exist
    mkdirRecursive(data_dir);
#endif

    return data_dir;
}

// Global settings instance
// Default values are defined in the struct
AppSettings g_app_settings;

// Global verbose flag - controls retry warning visibility in CLI mode
// Default: false (retry warnings suppressed)
bool g_verbose = false;
