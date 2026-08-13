#pragma once
#include <windows.h>

namespace cheburnet::ui {

// Monotonic millisecond clock for animation timing (GetTickCount64-based).
class AnimationClock {
public:
    AnimationClock() : start_(::GetTickCount64()) {}
    void Reset() { start_ = ::GetTickCount64(); }
    unsigned long long ElapsedMs() const { return ::GetTickCount64() - start_; }

private:
    unsigned long long start_;
};

// Format milliseconds as HH:MM:SS (for session uptime).
inline void FormatUptime(unsigned long long ms, wchar_t out[9]) {
    unsigned long long s = ms / 1000;
    unsigned h = static_cast<unsigned>(s / 3600);
    unsigned m = static_cast<unsigned>((s % 3600) / 60);
    unsigned sec = static_cast<unsigned>(s % 60);
    if (h > 99) h = 99;
    ::swprintf(out, 9, L"%02u:%02u:%02u", h, m, sec);
}

} // namespace cheburnet::ui
