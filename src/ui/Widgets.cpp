#include "Widgets.h"

#include <algorithm>
#include <cwchar>

namespace cheburnet::ui {
namespace {

WORD SevAttr(Theme& th, Severity s) {
    switch (s) {
        case Severity::Ok:    return th.Attr(UiColor::Primary);
        case Severity::Warn:  return th.Attr(UiColor::Warning);
        case Severity::Error: return th.Attr(UiColor::Error);
        case Severity::Info:
        default:              return th.Attr(UiColor::PrimaryDim);
    }
}

void PutClipped(FrameBuffer& fb, int x, int y, std::wstring_view s, WORD attr, int maxW) {
    if (maxW <= 0) return;
    if (static_cast<int>(s.size()) > maxW) s = s.substr(0, static_cast<size_t>(maxW));
    fb.PutText(x, y, s, attr);
}

} // namespace

// ---------------------------------------------------------------- ProgressBar
void DrawProgressBar(FrameBuffer& fb, Theme& th, int x, int y, int innerWidth, int percent,
                     unsigned long long elapsedMs, bool animating) {
    if (innerWidth < 1) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const Glyphs& g = th.G();
    int filled = percent * innerWidth / 100;
    if (filled > innerWidth) filled = innerWidth;

    fb.PutCh(x, y, L'[', th.Attr(UiColor::Muted));
    for (int i = 0; i < innerWidth; ++i) {
        const bool on = i < filled;
        fb.PutCh(x + 1 + i, y, on ? g.barFull : g.barEmpty,
                 th.Attr(on ? UiColor::Primary : UiColor::PrimaryDim));
    }
    if (animating && filled > 0) {
        const int hi = static_cast<int>((elapsedMs / 70ull) % static_cast<unsigned long long>(filled));
        fb.PutCh(x + 1 + hi, y, g.barHi, th.Attr(UiColor::Accent));
    }
    fb.PutCh(x + 1 + innerWidth, y, L']', th.Attr(UiColor::Muted));
    wchar_t pct[8];
    ::swprintf(pct, 8, L"%3d%%", percent);
    fb.PutText(x + innerWidth + 3, y, pct, th.Attr(UiColor::Accent));
}

void DrawLoadingBar(FrameBuffer& fb, Theme& th, int x, int y, int innerWidth,
                    unsigned long long elapsedMs) {
    if (innerWidth < 1) return;
    const Glyphs& g = th.G();
    fb.PutCh(x, y, L'[', th.Attr(UiColor::Muted));
    const int seg = innerWidth >= 12 ? 6 : 3;
    const int pos = static_cast<int>((elapsedMs / 45ull) %
                                     static_cast<unsigned long long>(innerWidth));
    for (int i = 0; i < innerWidth; ++i) {
        int d = i - pos;
        if (d < 0) d += innerWidth;
        const bool head = d < seg;
        fb.PutCh(x + 1 + i, y, head ? g.barFull : g.barEmpty,
                 th.Attr(head ? UiColor::Accent : UiColor::PrimaryDim));
    }
    fb.PutCh(x + 1 + innerWidth, y, L']', th.Attr(UiColor::Muted));
}

// ----------------------------------------------------------------------- Menu
void Menu::SetItems(std::vector<MenuItem> items) {
    items_ = std::move(items);
    sel_ = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled) {
            sel_ = static_cast<int>(i);
            break;
        }
    }
}

void Menu::MoveUp() {
    if (items_.empty()) return;
    for (int step = 1; step <= static_cast<int>(items_.size()); ++step) {
        int i = (sel_ - step % static_cast<int>(items_.size()) + static_cast<int>(items_.size())) %
                static_cast<int>(items_.size());
        if (items_[static_cast<size_t>(i)].enabled) {
            sel_ = i;
            return;
        }
    }
}

void Menu::MoveDown() {
    if (items_.empty()) return;
    for (int step = 1; step <= static_cast<int>(items_.size()); ++step) {
        int i = (sel_ + step) % static_cast<int>(items_.size());
        if (items_[static_cast<size_t>(i)].enabled) {
            sel_ = i;
            return;
        }
    }
}

void Menu::SelectHotkey(wchar_t ch) {
    const int i = IndexForHotkey(ch);
    if (i >= 0) sel_ = i;
}

int Menu::IndexForHotkey(wchar_t ch) const {
    const wchar_t lower = (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch | 0x20) : ch;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].enabled && items_[i].hotkey == lower) return static_cast<int>(i);
    }
    return -1;
}

void Menu::SetSelected(int i) {
    if (items_.empty()) return;
    if (i < 0) i = 0;
    if (i >= static_cast<int>(items_.size())) i = static_cast<int>(items_.size()) - 1;
    sel_ = i;
    if (!items_[static_cast<size_t>(sel_)].enabled) {
        MoveDown();
        if (!items_[static_cast<size_t>(sel_)].enabled) MoveUp();
    }
}

int Menu::Render(FrameBuffer& fb, Theme& th, int x, int y, int width) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& it = items_[i];
        const bool selected = static_cast<int>(i) == sel_;
        WORD attr;
        if (!it.enabled)
            attr = th.Attr(UiColor::Muted);
        else if (selected)
            attr = th.Attr(UiColor::Selected);
        else
            attr = th.Attr(UiColor::Primary);
        // Fixed 3-column prefix (" > label") so the label never shifts between
        // selected and unselected rows.
        std::wstring row = L" ";
        row += selected ? th.G().arrow : L' ';
        row += L' ';
        row += it.label;
        if (!it.enabled) row += L"  —";
        if (static_cast<int>(row.size()) < width)
            row.append(static_cast<size_t>(width) - row.size(), L' ');
        else
            row = row.substr(0, static_cast<size_t>(width));
        fb.PutText(x, y + static_cast<int>(i), row, attr);
    }
    return static_cast<int>(items_.size());
}

// ---------------------------------------------------------------- StatusPanel
int DrawStatusPanel(FrameBuffer& fb, Theme& th, int x, int y, int width, std::wstring_view title,
                    const std::vector<StatusCell>& left, const std::vector<StatusCell>& right) {
    const int rows = std::max(static_cast<int>(left.size()), static_cast<int>(right.size()));
    const int h = rows + 2;
    fb.Box(x, y, width, h, th.Attr(UiColor::PrimaryDim), th.G(), title, th.Attr(UiColor::Primary));
    const int colW = (width - 4) / 2;
    const int lx = x + 2;
    const int rx = x + 2 + colW;
    auto cell = [&](int cx, int ry, const StatusCell& c) {
        PutClipped(fb, cx, ry, c.label, th.Attr(UiColor::Muted), colW);
        const int vo = static_cast<int>(c.label.size());
        PutClipped(fb, cx + vo, ry, c.value, th.Attr(c.valueColor), colW - vo);
    };
    for (int i = 0; i < rows; ++i) {
        const int ry = y + 1 + i;
        if (i < static_cast<int>(left.size())) cell(lx, ry, left[static_cast<size_t>(i)]);
        if (i < static_cast<int>(right.size())) cell(rx, ry, right[static_cast<size_t>(i)]);
    }
    return h;
}

// ------------------------------------------------------------------ LogViewer
void LogViewer::SetLines(std::vector<LogLine> lines) {
    lines_ = std::move(lines);
    scroll_ = 0;
}
void LogViewer::ScrollUp(int n) { scroll_ += n; }
void LogViewer::ScrollDown(int n) {
    scroll_ -= n;
    if (scroll_ < 0) scroll_ = 0;
}
void LogViewer::CycleFilter() {
    filter_ = (filter_ + 1) % 4;
    scroll_ = 0;
}
std::wstring LogViewer::FilterName() const {
    switch (filter_) {
        case 1:  return L"СВЕДЕНИЯ";
        case 2:  return L"ПРЕДУПРЕЖДЕНИЯ";
        case 3:  return L"ОШИБКИ";
        default: return L"ВСЕ";
    }
}
std::vector<const LogLine*> LogViewer::Filtered() const {
    std::vector<const LogLine*> out;
    for (const auto& l : lines_) {
        bool keep = true;
        if (filter_ == 1) keep = (l.sev == Severity::Info || l.sev == Severity::Ok);
        else if (filter_ == 2) keep = (l.sev == Severity::Warn);
        else if (filter_ == 3) keep = (l.sev == Severity::Error);
        if (keep) out.push_back(&l);
    }
    return out;
}
void LogViewer::Render(FrameBuffer& fb, Theme& th, int x, int y, int w, int h) const {
    auto flt = Filtered();
    const int total = static_cast<int>(flt.size());
    int first = total - h - scroll_;
    if (first < 0) first = 0;
    int shown = std::min(h, total - first);
    if (shown < 0) shown = 0;
    for (int i = 0; i < shown; ++i) {
        const LogLine* l = flt[static_cast<size_t>(first + i)];
        const int ry = y + i;
        int cx = x;
        PutClipped(fb, cx, ry, l->time, th.Attr(UiColor::Muted), 12);
        cx += 10;
        PutClipped(fb, cx, ry, l->level, SevAttr(const_cast<Theme&>(th), l->sev), 7);
        cx += 8;
        PutClipped(fb, cx, ry, l->text, th.Attr(UiColor::PrimaryDim), w - (cx - x));
    }
}

// --------------------------------------------------------------------- Ticker
std::wstring_view Ticker::Current(unsigned long long elapsedMs, int periodMs) const {
    if (lines_.empty()) return {};
    if (periodMs <= 0) periodMs = 1;
    const size_t idx = static_cast<size_t>((elapsedMs / static_cast<unsigned long long>(periodMs)) %
                                           lines_.size());
    return lines_[idx];
}

// --------------------------------------------------------------------- Mascot
void DrawMascot(FrameBuffer& fb, Theme& th, int x, int y, const MascotArt& art, bool blinkActive,
                bool starBright) {
    const WORD body = th.Attr(UiColor::Primary);
    const WORD starC = th.Attr(starBright ? UiColor::Accent : UiColor::Star);
    for (size_t r = 0; r < art.lines.size(); ++r) {
        const std::wstring& line = art.lines[r];
        fb.PutText(x, y + static_cast<int>(r), line, body);
        for (size_t c = 0; c < line.size(); ++c) {
            if (line[c] == art.starMarker)
                fb.PutCh(x + static_cast<int>(c), y + static_cast<int>(r), art.starMarker, starC);
        }
    }
    if (blinkActive) {
        for (const MascotEye& e : art.eyes) {
            if (art.blinkSwap) {
                fb.PutCh(x + e.x, y + e.y, art.eyeClosed, body);
            } else if (art.glint) {
                wchar_t gch = L'*';
                if (e.y >= 0 && e.y < static_cast<int>(art.lines.size()) && e.x >= 0 &&
                    e.x < static_cast<int>(art.lines[static_cast<size_t>(e.y)].size())) {
                    gch = art.lines[static_cast<size_t>(e.y)][static_cast<size_t>(e.x)];
                }
                fb.PutCh(x + e.x, y + e.y, gch, th.Attr(UiColor::Accent));
            }
        }
    }
}

// -------------------------------------------------------------------- Footer
std::wstring BrandingLine(LayoutMode mode) {
    if (mode == LayoutMode::Minimal) return L"АВТОР: MARSHAL JACKSONY100";
    return L"CHEBURNET LABS // РАЗРАБОТАНО MARSHAL JACKSONY100";
}

void DrawKeyHints(FrameBuffer& fb, Theme& th, int y, std::wstring_view hints) {
    fb.PutText(2, y, hints, th.Attr(UiColor::Muted));
}

} // namespace cheburnet::ui
