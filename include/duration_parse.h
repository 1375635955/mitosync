#pragma once

#include <chrono>
#include <optional>
#include <string>

// Parse duration string like "1h", "7d", "30m"
// Returns nullopt on parse error
std::optional<std::chrono::seconds> parse_duration(const std::string& str);
