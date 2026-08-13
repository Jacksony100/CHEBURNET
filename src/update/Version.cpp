#include "Version.h"

#include <charconv>
#include <limits>

namespace cheburnet::update {

std::optional<Version> ParseVersion(std::string_view value) {
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.remove_prefix(1);
    if (value.empty() || value.size() > 64) return std::nullopt;
    Version result;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t start = pos;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') ++pos;
        if (start == pos) return std::nullopt;
        unsigned long long part = 0;
        const auto parsed = std::from_chars(value.data() + start, value.data() + pos, part);
        if (parsed.ec != std::errc{}) return std::nullopt;
        result.parts.push_back(part);
        if (result.parts.size() > 8) return std::nullopt;
        if (pos == value.size()) break;
        if (value[pos] == '.') {
            ++pos;
            // A separator must always be followed by another numeric part.
            // Accepting "1." would create an alias of "1" and weaken the
            // updater's canonical anti-downgrade comparison.
            if (pos == value.size()) return std::nullopt;
            continue;
        }
        break;
    }
    if (result.parts.empty()) return std::nullopt;
    if (pos < value.size()) {
        result.suffix.assign(value.substr(pos));
        for (char c : result.suffix) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
            if (!allowed) return std::nullopt;
        }
        result.prerelease = result.suffix.find('-') != std::string::npos;
    }
    return result;
}

int CompareVersions(const Version& left, const Version& right) {
    const std::size_t count = left.parts.size() > right.parts.size() ? left.parts.size()
                                                                         : right.parts.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto l = i < left.parts.size() ? left.parts[i] : 0;
        const auto r = i < right.parts.size() ? right.parts[i] : 0;
        if (l < r) return -1;
        if (l > r) return 1;
    }
    if (left.suffix == right.suffix) return 0;
    if (left.suffix.empty()) return 1;
    if (right.suffix.empty()) return -1;
    return left.suffix < right.suffix ? -1 : 1;
}

} // namespace cheburnet::update
