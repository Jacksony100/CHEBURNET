#pragma once
#include <windows.h>

#include <string_view>
#include <vector>

#include "Theme.h"

namespace cheburnet::ui {

// Off-screen character grid. All drawing goes here; Present() pushes only the
// rows that changed to the console via WriteConsoleOutputW (one atomic write per
// changed row) - flicker-free, and idle frames cost nothing when nothing moved.
// Every cell is exactly one column, so ambiguous-width glyphs never shift layout.
class FrameBuffer {
public:
    void Resize(int w, int h);
    int  Width() const { return w_; }
    int  Height() const { return h_; }

    void Clear(WORD attr);
    void PutCh(int x, int y, wchar_t ch, WORD attr);
    void PutText(int x, int y, std::wstring_view text, WORD attr);
    void FillRect(int x, int y, int w, int h, wchar_t ch, WORD attr);
    void HLine(int x, int y, int len, wchar_t ch, WORD attr);
    void Box(int x, int y, int w, int h, WORD attr, const Glyphs& g,
             std::wstring_view title = {}, WORD titleAttr = 0);

    // Present changed rows to `out`. Returns number of rows written (for tests).
    int Present(HANDLE out);

    // Force the next Present to redraw everything (e.g. after a resize).
    void Invalidate() { prevValid_ = false; }

    // Test access: raw cell at (x,y).
    const CHAR_INFO& At(int x, int y) const { return cells_[Index(x, y)]; }

private:
    int  Index(int x, int y) const { return y * w_ + x; }
    bool InBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

    int                    w_ = 0, h_ = 0;
    std::vector<CHAR_INFO> cells_;
    std::vector<CHAR_INFO> prev_;
    bool                   prevValid_ = false;
};

} // namespace cheburnet::ui
