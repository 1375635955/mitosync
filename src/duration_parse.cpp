#include "duration_parse.h"
#include <cctype>
#include <stdexcept>
#include <limits>

std::optional<std::chrono::seconds> parse_duration(const std::string& str) {
    if (str.empty()) return std::nullopt;

    // Find where the number ends
    size_t i = 0;
    while (i < str.size() && std::isdigit(str[i])) {
        i++;
    }

    if (i == 0 || i >= str.size()) {
        return std::nullopt;  // No number or no unit
    }

    // Parse unit (must be single character at end)
    char unit = str[i];
    if (i + 1 != str.size()) {
        return std::nullopt;  // Extra characters after unit
    }

    // Parse number with overflow protection
    int64_t value;
    try {
        value = std::stoll(str.substr(0, i));
    } catch (const std::out_of_range&) {
        return std::nullopt;  // Number too large
    } catch (const std::invalid_argument&) {
        return std::nullopt;  // Invalid number
    }

    if (value < 0) {
        return std::nullopt;  // Negative values not allowed
    }

    // Apply multiplier with overflow check
    int64_t multiplier;
    switch (unit) {
        case 's': multiplier = 1; break;
        case 'm': multiplier = 60; break;
        case 'h': multiplier = 3600; break;
        case 'd': multiplier = 86400; break;
        default: return std::nullopt;
    }

    // Check for overflow before multiplication
    if (value > std::numeric_limits<int64_t>::max() / multiplier) {
        return std::nullopt;  // Would overflow
    }

    return std::chrono::seconds(value * multiplier);
}
