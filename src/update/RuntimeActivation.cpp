#include "RuntimeActivation.h"

namespace cheburnet::update {

ActivationResult ActivateRuntime(const RuntimeState& before, std::string_view candidate,
                                 const ActivationHooks& hooks) {
    ActivationResult result;
    result.state = before;
    if (!hooks.preflight || !hooks.preflight(candidate)) {
        result.status = ActivationStatus::PreflightFailed;
        return result;
    }
    if (!hooks.stopCurrent || !hooks.stopCurrent()) {
        result.status = ActivationStatus::StopFailed;
        return result;
    }
    auto rollback = [&]() {
        if (!before.current.empty() && hooks.start && hooks.start(before.current) &&
            hooks.health && hooks.health(before.current)) {
            result.state = before;
            result.status = ActivationStatus::RolledBack;
        } else {
            result.status = ActivationStatus::RollbackFailed;
        }
    };
    if (!hooks.start || !hooks.start(candidate)) {
        result.status = ActivationStatus::StartFailed;
        rollback();
        return result;
    }
    if (!hooks.health || !hooks.health(candidate)) {
        result.status = ActivationStatus::HealthFailed;
        rollback();
        return result;
    }
    RuntimeState committed{std::string(candidate), before.current};
    if (!hooks.commit || !hooks.commit(committed)) {
        result.status = ActivationStatus::CommitFailed;
        rollback();
        return result;
    }
    result.state = std::move(committed);
    result.status = ActivationStatus::Activated;
    return result;
}

} // namespace cheburnet::update
