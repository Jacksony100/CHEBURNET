#pragma once
#include <atomic>
#include <thread>

#include "../config/ProgressModel.h"
#include "ConsoleRenderer.h"

namespace cheburnet {

// Renders the connection animation - a "Подключение..." label with 1..8 cycling
// dots and a segmented green progress bar with a percentage - on its own
// std::jthread. Reads the shared ProgressModel; the bar can only reach 100%
// after Reach(Confirmed). Stops immediately when the model enters the Error
// state. Absolute-position writes keep it flicker-free.
class ConnectionAnimation {
public:
    ConnectionAnimation(ConsoleRenderer& con, ProgressModel& progress)
        : con_(con), progress_(progress) {}
    ~ConnectionAnimation() { Stop(); }

    ConnectionAnimation(const ConnectionAnimation&) = delete;
    ConnectionAnimation& operator=(const ConnectionAnimation&) = delete;

    // originY: first row used by the animation block (label + bar + hint).
    void Start(int originX, int originY, int barWidth);
    void Stop();

    // True once the render loop has exited (e.g. on error or success). Lets the
    // stop-on-error behaviour be observed without a real console.
    bool Finished() const { return finished_.load(); }

private:
    void Run(std::stop_token st, int originX, int originY, int barWidth);

    ConsoleRenderer&  con_;
    ProgressModel&    progress_;
    std::atomic<bool> finished_{false};
    std::jthread      thread_;
};

} // namespace cheburnet
