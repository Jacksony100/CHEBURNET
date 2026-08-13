#include "Theme.h"

namespace cheburnet::ui {

WORD Theme::Attr(UiColor c) const {
    constexpr WORD G = FOREGROUND_GREEN;
    constexpr WORD Gi = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    constexpr WORD Wi = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    constexpr WORD Y = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    constexpr WORD R = FOREGROUND_RED | FOREGROUND_INTENSITY;
    constexpr WORD DG = FOREGROUND_INTENSITY; // dark gray
    constexpr WORD SEL = BACKGROUND_GREEN | BACKGROUND_INTENSITY; // black fg on bright green bg
    constexpr WORD SELDIM = BACKGROUND_GREEN;                     // black fg on dim green bg
    switch (c) {
        case UiColor::Primary:     return Gi;
        case UiColor::PrimaryDim:  return G;
        case UiColor::Accent:      return Wi;
        case UiColor::Warning:     return Y;
        case UiColor::Error:       return R;
        case UiColor::Muted:       return DG;
        case UiColor::Selected:    return SEL;
        case UiColor::SelectedDim: return SELDIM;
        case UiColor::Star:        return R;
        case UiColor::Default:
        default:                   return Gi;
    }
}

LayoutMode LayoutForSize(int w, int h) {
    if (w >= 92 && h >= 30) return LayoutMode::Full;
    if (w >= 70 && h >= 24) return LayoutMode::Compact;
    return LayoutMode::Minimal;
}

bool SizeTooSmall(int w, int h) {
    return w < 54 || h < 15;
}

int UiOptions::scaleMs(int nominalMs) const {
    switch (speed) {
        case AnimSpeed::Slow:   return nominalMs * 3 / 2;
        case AnimSpeed::Fast:   return nominalMs / 2;
        case AnimSpeed::Normal:
        default:                return nominalMs;
    }
}

const Glyphs& UnicodeGlyphs() {
    static const Glyphs g = {
        L'┌', L'┐', L'└', L'┘', L'─', L'│', L'├', L'┤', L'┬', L'┴',
        L'█', L'░', L'▓',
        L'•', L'●', L'▌', L'›',
        L"⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏",
    };
    return g;
}

const Glyphs& AsciiGlyphs() {
    static const Glyphs g = {
        L'+', L'+', L'+', L'+', L'-', L'|', L'+', L'+', L'+', L'+',
        L'#', L'-', L'=',
        L'*', L'*', L'_', L'>',
        L"|/-\\",
    };
    return g;
}

} // namespace cheburnet::ui
