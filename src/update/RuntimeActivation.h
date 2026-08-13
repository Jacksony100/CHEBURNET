#pragma once

#include <functional>
#include <string>

namespace cheburnet::update {

struct RuntimeState {
    std::string current;
    std::string previousKnownGood;
};

enum class ActivationStatus { Activated, PreflightFailed, StopFailed, StartFailed,
                              HealthFailed, CommitFailed, RolledBack, RollbackFailed };

struct ActivationResult {
    ActivationStatus status = ActivationStatus::PreflightFailed;
    RuntimeState state;
};

struct ActivationHooks {
    std::function<bool(std::string_view)> preflight;
    std::function<bool()> stopCurrent;
    std::function<bool(std::string_view)> start;
    std::function<bool(std::string_view)> health;
    std::function<bool(const RuntimeState&)> commit;
};

ActivationResult ActivateRuntime(const RuntimeState& before, std::string_view candidate,
                                 const ActivationHooks& hooks);

} // namespace cheburnet::update
