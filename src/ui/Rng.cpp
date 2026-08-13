#include "Rng.h"

#include <windows.h>

namespace cheburnet::ui {

Rng::Rng() {
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    gen_.seed(static_cast<std::mt19937::result_type>(c.QuadPart ^ ::GetCurrentThreadId()));
}

int Rng::Range(int lo, int hi) {
    if (hi < lo) return lo;
    std::uniform_int_distribution<int> d(lo, hi);
    return d(gen_);
}

bool Rng::Chance(int percent) {
    if (percent <= 0) return false;
    if (percent >= 100) return true;
    return Range(1, 100) <= percent;
}

double Rng::Unit() {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    return d(gen_);
}

} // namespace cheburnet::ui
