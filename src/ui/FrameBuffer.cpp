#include "FrameBuffer.h"

#include <cstring>

namespace cheburnet::ui {

void FrameBuffer::Resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == w_ && h == h_) return;
    w_ = w;
    h_ = h;
    cells_.assign(static_cast<size_t>(w_) * h_, CHAR_INFO{});
    prev_.assign(static_cast<size_t>(w_) * h_, CHAR_INFO{});
    prevValid_ = false;
}

void FrameBuffer::Clear(WORD attr) {
    for (auto& c : cells_) {
        c.Char.UnicodeChar = L' ';
        c.Attributes = attr;
    }
}

void FrameBuffer::PutCh(int x, int y, wchar_t ch, WORD attr) {
    if (!InBounds(x, y)) return;
    CHAR_INFO& c = cells_[Index(x, y)];
    c.Char.UnicodeChar = ch;
    c.Attributes = attr;
}

void FrameBuffer::PutText(int x, int y, std::wstring_view text, WORD attr) {
    if (y < 0 || y >= h_) return;
    int cx = x;
    for (wchar_t ch : text) {
        if (ch == L'\n') break; // single-line writer
        if (cx >= 0 && cx < w_) {
            CHAR_INFO& c = cells_[Index(cx, y)];
            c.Char.UnicodeChar = ch;
            c.Attributes = attr;
        }
        ++cx;
        if (cx >= w_) break;
    }
}

void FrameBuffer::FillRect(int x, int y, int w, int h, wchar_t ch, WORD attr) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) PutCh(x + i, y + j, ch, attr);
}

void FrameBuffer::HLine(int x, int y, int len, wchar_t ch, WORD attr) {
    for (int i = 0; i < len; ++i) PutCh(x + i, y, ch, attr);
}

void FrameBuffer::Box(int x, int y, int w, int h, WORD attr, const Glyphs& g,
                      std::wstring_view title, WORD titleAttr) {
    if (w < 2 || h < 2) return;
    PutCh(x, y, g.tl, attr);
    PutCh(x + w - 1, y, g.tr, attr);
    PutCh(x, y + h - 1, g.bl, attr);
    PutCh(x + w - 1, y + h - 1, g.br, attr);
    for (int i = 1; i < w - 1; ++i) {
        PutCh(x + i, y, g.h, attr);
        PutCh(x + i, y + h - 1, g.h, attr);
    }
    for (int j = 1; j < h - 1; ++j) {
        PutCh(x, y + j, g.v, attr);
        PutCh(x + w - 1, y + j, g.v, attr);
    }
    if (!title.empty() && w > 6) {
        const int maxLen = w - 6;
        std::wstring t = L" ";
        t.append(title.substr(0, static_cast<size_t>(maxLen > 0 ? maxLen : 0)));
        t.push_back(L' ');
        PutText(x + 2, y, t, titleAttr ? titleAttr : attr);
    }
}

int FrameBuffer::Present(HANDLE out) {
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return 0;
    const COORD bufSize{static_cast<SHORT>(w_), static_cast<SHORT>(h_)};
    int rowsWritten = 0;
    for (int y = 0; y < h_; ++y) {
        const CHAR_INFO* rowNow = &cells_[static_cast<size_t>(y) * w_];
        const CHAR_INFO* rowPrev = &prev_[static_cast<size_t>(y) * w_];
        if (prevValid_ &&
            std::memcmp(rowNow, rowPrev, static_cast<size_t>(w_) * sizeof(CHAR_INFO)) == 0) {
            continue; // unchanged row
        }
        SMALL_RECT region{0, static_cast<SHORT>(y), static_cast<SHORT>(w_ - 1),
                          static_cast<SHORT>(y)};
        const COORD src{0, static_cast<SHORT>(y)};
        ::WriteConsoleOutputW(out, cells_.data(), bufSize, src, &region);
        ++rowsWritten;
    }
    prev_ = cells_;
    prevValid_ = true;
    return rowsWritten;
}

} // namespace cheburnet::ui
