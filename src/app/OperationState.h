#pragma once

#include <atomic>

namespace cheburnet {

enum class AppOperationState {
    Disconnected,
    Connecting,
    Connected,
    Disconnecting,
    Updating,
    RollingBack,
    Error
};

// Small compare/exchange state machine used to serialize every operation that
// can mutate runtime/process state. The UI is currently single-threaded, but
// the atomic gate also protects future background entry points and makes the
// no update+start / cleanup+start invariant explicit and testable.
class OperationState final {
public:
    AppOperationState Get() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    bool TryTransition(AppOperationState expected, AppOperationState desired) noexcept {
        return state_.compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    void ObserveProcess(bool alive) noexcept {
        auto state = Get();
        while (state == AppOperationState::Connected || state == AppOperationState::Disconnected ||
               state == AppOperationState::Error) {
            const auto desired = alive ? AppOperationState::Connected : AppOperationState::Disconnected;
            if (state_.compare_exchange_weak(state, desired, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) return;
        }
    }

    void Complete(AppOperationState state) noexcept {
        state_.store(state, std::memory_order_release);
    }

private:
    std::atomic<AppOperationState> state_{AppOperationState::Disconnected};
};

} // namespace cheburnet
