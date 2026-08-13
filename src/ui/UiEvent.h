#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cheburnet::ui {

enum class UiEventType {
    StageStarted,
    StageOk,
    StageFailed,
    ProcessStarted,
    ProcessStopped,
    WatchdogHeartbeat,
    StrategyChanged,
    Info,
};

enum class Severity { Info, Ok, Warn, Error };

// Work threads (connect pipeline, watchdog) push these; the UI thread drains and
// renders them. Nothing on a work thread ever draws directly.
struct UiEvent {
    UiEventType  type = UiEventType::Info;
    int          checkpoint = 0; // 1..20 during connect, else 0
    int          percent = 0;    // 0..100, monotonic; 100 only after confirm
    Severity     severity = Severity::Info;
    std::wstring label;
    std::wstring detail;
};

// Thread-safe multi-producer / single-consumer queue.
class UiEventQueue {
public:
    void Push(UiEvent e) {
        std::scoped_lock lock(m_);
        q_.push_back(std::move(e));
    }
    std::vector<UiEvent> Drain() {
        std::scoped_lock lock(m_);
        std::vector<UiEvent> out(q_.begin(), q_.end());
        q_.clear();
        return out;
    }
    bool Empty() const {
        std::scoped_lock lock(m_);
        return q_.empty();
    }

private:
    mutable std::mutex   m_;
    std::deque<UiEvent>  q_;
};

} // namespace cheburnet::ui
