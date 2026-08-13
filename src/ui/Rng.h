#pragma once
#include <random>

namespace cheburnet::ui {

// Small non-crypto RNG for cosmetic timing (blink intervals, glitch cells).
// Seeded from the high-resolution counter; never affects business logic.
class Rng {
public:
    Rng();
    int    Range(int lo, int hi);     // inclusive
    bool   Chance(int percent);       // true with probability percent/100
    double Unit();                    // [0,1)

private:
    std::mt19937 gen_;
};

} // namespace cheburnet::ui
