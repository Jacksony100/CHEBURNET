#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cheburnet::update {

struct Version {
    std::vector<unsigned long long> parts;
    std::string suffix;
    bool prerelease = false;
};

std::optional<Version> ParseVersion(std::string_view value);
int CompareVersions(const Version& left, const Version& right);

} // namespace cheburnet::update
