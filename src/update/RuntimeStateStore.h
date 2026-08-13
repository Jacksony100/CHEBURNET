#pragma once

#include <string>

namespace cheburnet::update {

struct StoredRuntimeState {
    int schema = 1;
    std::string current;
    std::string previousKnownGood;
    std::string pending;
    std::string packageSha256;
    std::string manifestKeyId;
    std::string lastResult;
};

struct StateResult {
    bool ok = false;
    bool missing = false;
    StoredRuntimeState state;
    std::string error;
};

StateResult LoadRuntimeState(const std::wstring& path);
bool SaveRuntimeState(const std::wstring& path, const StoredRuntimeState& state,
                      std::wstring* error = nullptr);
std::string SerializeRuntimeState(const StoredRuntimeState& state);

} // namespace cheburnet::update
