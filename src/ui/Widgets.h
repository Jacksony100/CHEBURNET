#pragma once
#include <string>
#include <vector>

#include "../console/Mascot.h"
#include "FrameBuffer.h"
#include "Theme.h"
#include "UiEvent.h"

namespace cheburnet::ui {

// ---- Progress bar (unicode blocks or ASCII, with a moving highlight) --------
void DrawProgressBar(FrameBuffer& fb, class Theme& th, int x, int y, int innerWidth, int percent,
                     unsigned long long elapsedMs, bool animating);

// ---- Indeterminate loading bar (a bright "comet" sweeping across) ------------
void DrawLoadingBar(FrameBuffer& fb, class Theme& th, int x, int y, int innerWidth,
                    unsigned long long elapsedMs);

// ---- Arrow-navigable menu ---------------------------------------------------
struct MenuItem {
    std::wstring label;
    std::wstring description;
    bool         enabled = true;
    wchar_t      hotkey = 0; // optional quick key (lower-case)
};

class Menu {
public:
    void SetItems(std::vector<MenuItem> items);
    void MoveUp();
    void MoveDown();
    void SelectHotkey(wchar_t ch); // jump to item with matching hotkey (if enabled)
    void SetSelected(int i);       // clamp + skip disabled (preserves nav across rebuilds)
    int  Selected() const { return sel_; }
    int  IndexForHotkey(wchar_t ch) const; // enabled item index or -1
    const MenuItem& SelectedItem() const { return items_[static_cast<size_t>(sel_)]; }
    bool Empty() const { return items_.empty(); }

    // Renders the list; selected enabled row is inverse-highlighted, disabled
    // rows muted. Returns the height used.
    int Render(FrameBuffer& fb, class Theme& th, int x, int y, int width) const;

private:
    std::vector<MenuItem> items_;
    int                   sel_ = 0;
};

// ---- Status panel (titled box, label/value grid) ----------------------------
struct StatusCell {
    std::wstring label;
    std::wstring value;
    UiColor      valueColor = UiColor::Accent;
};
// Two columns of cells inside a titled box. Returns box height.
int DrawStatusPanel(FrameBuffer& fb, class Theme& th, int x, int y, int width,
                    std::wstring_view title, const std::vector<StatusCell>& left,
                    const std::vector<StatusCell>& right);

// ---- Log viewer -------------------------------------------------------------
struct LogLine {
    std::wstring time;
    std::wstring level;
    std::wstring text;
    Severity     sev = Severity::Info;
};

class LogViewer {
public:
    void SetLines(std::vector<LogLine> lines);
    void ScrollUp(int n);
    void ScrollDown(int n);
    void CycleFilter(); // all -> INFO -> WARN -> ERROR -> TRACE -> all
    std::wstring FilterName() const;
    void Render(FrameBuffer& fb, class Theme& th, int x, int y, int w, int h) const;

private:
    std::vector<LogLine> lines_;
    int                  scroll_ = 0;  // rows scrolled up from bottom
    int                  filter_ = 0;  // 0=all
    std::vector<const LogLine*> Filtered() const;
};

// ---- Ticker (cycles status lines) ------------------------------------------
class Ticker {
public:
    void SetLines(std::vector<std::wstring> lines) { lines_ = std::move(lines); }
    std::wstring_view Current(unsigned long long elapsedMs, int periodMs) const;
    bool Empty() const { return lines_.empty(); }

private:
    std::vector<std::wstring> lines_;
};

// ---- Mascot rendering with idle blink / glint + star flicker ----------------
void DrawMascot(FrameBuffer& fb, class Theme& th, int x, int y, const MascotArt& art,
                bool blinkActive, bool starBright);

// ---- Footer branding + key hints -------------------------------------------
std::wstring BrandingLine(LayoutMode mode);
void DrawKeyHints(FrameBuffer& fb, class Theme& th, int y, std::wstring_view hints);

} // namespace cheburnet::ui
