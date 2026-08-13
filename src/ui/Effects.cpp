#include "Effects.h"

#include <cwchar>

namespace cheburnet::ui {
namespace {
// Safe printable ASCII glyphs used for the glitch overlay - no control chars,
// no surrogates, all single-cell.
constexpr wchar_t kGlitchChars[] = L"#%&$@*?/\\|<>=+~^:;";
constexpr int kGlitchCount = static_cast<int>(sizeof(kGlitchChars) / sizeof(wchar_t)) - 1;
} // namespace

size_t Typewriter::VisibleCount(unsigned long long elapsedMs, bool instant) const {
    if (instant || cps_ <= 0) return text_.size();
    const unsigned long long chars = elapsedMs * static_cast<unsigned long long>(cps_) / 1000ull;
    return chars >= text_.size() ? text_.size() : static_cast<size_t>(chars);
}

std::wstring_view Typewriter::Visible(unsigned long long elapsedMs, bool instant) const {
    return std::wstring_view(text_).substr(0, VisibleCount(elapsedMs, instant));
}

bool Typewriter::Done(unsigned long long elapsedMs) const {
    return VisibleCount(elapsedMs, false) >= text_.size();
}

std::wstring Glitch::Apply(const std::wstring& base, Rng& rng, int frame, int activeFrames,
                           int intensity) {
    if (frame < 0 || frame >= activeFrames || base.empty()) {
        return base; // recovered: exact original
    }
    std::wstring out = base;
    const int n = intensity < 1 ? 1 : intensity;
    for (int k = 0; k < n; ++k) {
        const int pos = rng.Range(0, static_cast<int>(out.size()) - 1);
        // Never corrupt whitespace (keeps layout) or a surrogate half.
        const wchar_t c = out[static_cast<size_t>(pos)];
        if (c == L' ' || (c >= 0xD800 && c <= 0xDFFF)) continue;
        out[static_cast<size_t>(pos)] = kGlitchChars[rng.Range(0, kGlitchCount - 1)];
    }
    return out;
}

bool Blink::On(unsigned long long elapsedMs, int periodMs) {
    if (periodMs <= 0) return true;
    return (elapsedMs % static_cast<unsigned long long>(periodMs)) <
           static_cast<unsigned long long>(periodMs) / 2;
}

wchar_t Spinner::Frame(const Glyphs& g, unsigned long long elapsedMs, int stepMs) {
    const int len = static_cast<int>(std::wcslen(g.spinner));
    if (len == 0) return L' ';
    if (stepMs <= 0) stepMs = 1;
    const int idx = static_cast<int>((elapsedMs / static_cast<unsigned long long>(stepMs)) %
                                     static_cast<unsigned long long>(len));
    return g.spinner[idx];
}

int Scanline::Revealed(int totalRows, unsigned long long elapsedMs, int rowMs, bool instant) {
    if (instant || rowMs <= 0) return totalRows;
    const unsigned long long rows = elapsedMs / static_cast<unsigned long long>(rowMs);
    if (rows >= static_cast<unsigned long long>(totalRows)) return totalRows;
    return static_cast<int>(rows);
}

} // namespace cheburnet::ui
