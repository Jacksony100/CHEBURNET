#pragma once
#include <string>
#include <string_view>

#include "Rng.h"
#include "Theme.h"

namespace cheburnet::ui {

// Typewriter: reveals a short phrase character-by-character over time.
class Typewriter {
public:
    Typewriter(std::wstring text, int charsPerSec) : text_(std::move(text)), cps_(charsPerSec) {}
    // Visible prefix for the elapsed time. If `instant`, returns the whole text.
    std::wstring_view Visible(unsigned long long elapsedMs, bool instant) const;
    bool Done(unsigned long long elapsedMs) const;
    const std::wstring& Text() const { return text_; }

private:
    size_t VisibleCount(unsigned long long elapsedMs, bool instant) const;
    std::wstring text_;
    int          cps_;
};

// Glitch: returns a short-lived corrupted copy of `base` that always restores to
// exactly `base` once the active window passes. Only safe BMP glyphs are used -
// never control characters or surrogate halves.
class Glitch {
public:
    // frame in [0, activeFrames): corrupt ~intensity cells. frame >= activeFrames
    // (or reduced motion): returns base unchanged.
    static std::wstring Apply(const std::wstring& base, Rng& rng, int frame, int activeFrames,
                              int intensity);
};

// Blink: on/off state for a period (half on, half off).
class Blink {
public:
    static bool On(unsigned long long elapsedMs, int periodMs);
};

// Spinner: rotating frame from the theme glyph set.
class Spinner {
public:
    static wchar_t Frame(const Glyphs& g, unsigned long long elapsedMs, int stepMs);
};

// Scanline: number of rows revealed top-to-bottom for a reveal of totalRows.
class Scanline {
public:
    static int Revealed(int totalRows, unsigned long long elapsedMs, int rowMs, bool instant);
};

} // namespace cheburnet::ui
