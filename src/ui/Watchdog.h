#pragma once
#include <atomic>
#include <functional>
#include <thread>

#include "UiEvent.h"

namespace cheburnet::ui {

// Background process watcher. Periodically calls an "is the connection still
// ours & alive" predicate and posts a WatchdogHeartbeat (ok) or a single
// ProcessStopped event when the process disappears. Runs on its own jthread and
// only touches the thread-safe UiEventQueue - never the console.
class Watchdog {
public:
    Watchdog(UiEventQueue& queue, std::function<bool()> aliveCheck, int intervalMs = 1500)
        : q_(queue), check_(std::move(aliveCheck)), intervalMs_(intervalMs) {}
    ~Watchdog() { Stop(); }

    void Start();
    void Stop();
    bool StillAlive() const { return alive_.load(); }

private:
    void Run(std::stop_token st);

    UiEventQueue&         q_;
    std::function<bool()> check_;
    int                   intervalMs_;
    std::atomic<bool>     alive_{true};
    std::jthread          thread_;
};

} // namespace cheburnet::ui
