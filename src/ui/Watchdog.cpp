#include "Watchdog.h"

#include <chrono>

namespace cheburnet::ui {

void Watchdog::Start() {
    alive_.store(true);
    thread_ = std::jthread([this](std::stop_token st) { Run(st); });
}

void Watchdog::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Watchdog::Run(std::stop_token st) {
    using namespace std::chrono_literals;
    // An exception escaping a jthread entry calls std::terminate; a background
    // allocation/lock failure must instead just stop the watcher quietly.
    try {
    while (!st.stop_requested()) {
        // Sleep the interval in small slices so Stop() is responsive.
        int slept = 0;
        while (slept < intervalMs_ && !st.stop_requested()) {
            std::this_thread::sleep_for(100ms);
            slept += 100;
        }
        if (st.stop_requested()) break;

        const bool ok = check_ ? check_() : false;
        if (ok) {
            UiEvent e;
            e.type = UiEventType::WatchdogHeartbeat;
            e.severity = Severity::Ok;
            q_.Push(std::move(e));
        } else {
            alive_.store(false);
            UiEvent e;
            e.type = UiEventType::ProcessStopped;
            e.severity = Severity::Error;
            e.label = L"Процесс winws.exe завершился";
            q_.Push(std::move(e));
            break;
        }
    }
    } catch (...) {
        alive_.store(false); // no allocation in the handler
    }
}

} // namespace cheburnet::ui
