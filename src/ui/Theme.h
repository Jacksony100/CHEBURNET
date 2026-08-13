#pragma once
#include <windows.h>

#include <string>

namespace cheburnet::ui {

// Centralised semantic colours. Never scatter raw WORD attributes across the UI.
enum class UiColor {
    Default,      // primary green on black
    Primary,      // bright green (main text / headings)
    PrimaryDim,   // dim green (frames, secondary)
    Accent,       // bright white (values, emphasis)
    Warning,      // yellow
    Error,        // red
    Muted,        // dark gray (disabled, hints)
    Selected,     // black on green (inverse highlight)
    SelectedDim,  // black on dim-green
    Star,         // red (the mascot cap star)
};

// Animation speed multiplier source.
enum class AnimSpeed { Slow, Normal, Fast };

// Console width tiers.
enum class LayoutMode { Full, Compact, Minimal };

// Pure layout selection by console size (unit tested).
LayoutMode LayoutForSize(int w, int h);
bool       SizeTooSmall(int w, int h);

// Glyph set - Unicode (box drawing, blocks) or ASCII fallback.
struct Glyphs {
    wchar_t tl, tr, bl, br, h, v, ltee, rtee, ttee, btee;
    wchar_t barFull, barEmpty, barHi;
    wchar_t bullet, heartbeat, cursorBlock, arrow;
    const wchar_t* spinner; // null-terminated
};

const Glyphs& UnicodeGlyphs();
const Glyphs& AsciiGlyphs();

// Runtime UI preferences (from config.json "ui" block + CLI flags).
struct UiOptions {
    bool      animations = true;
    bool      reducedMotion = false;
    bool      glitch = true;
    bool      mascotIdle = true;
    bool      asciiOnly = false; // force ASCII glyphs
    bool      debugUi = false;
    AnimSpeed speed = AnimSpeed::Normal;

    // Effective helpers (reduced motion overrides individual toggles).
    bool effGlitch() const { return animations && glitch && !reducedMotion; }
    bool effTypewriter() const { return animations && !reducedMotion; }
    bool effMascotIdle() const { return animations && mascotIdle && !reducedMotion; }
    bool effBlink() const { return animations && !reducedMotion; }
    // Frame delay in ms for the given nominal ms, scaled by speed.
    int scaleMs(int nominalMs) const;
};

// Theme: maps UiColor -> console attribute and selects the glyph set.
class Theme {
public:
    explicit Theme(const UiOptions& opts) : opts_(opts) {}

    WORD Attr(UiColor c) const;
    const Glyphs& G() const { return opts_.asciiOnly ? AsciiGlyphs() : UnicodeGlyphs(); }
    const UiOptions& Options() const { return opts_; }
    void SetOptions(const UiOptions& o) { opts_ = o; }

private:
    UiOptions opts_;
};

} // namespace cheburnet::ui
