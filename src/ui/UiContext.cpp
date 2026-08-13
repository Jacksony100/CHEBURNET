#include "UiContext.h"

namespace cheburnet::ui {
namespace {
// Fixed target console size. Big enough for the full mascot (86) + panels;
// clamped down to the largest that fits the current display/font.
constexpr int kTargetW = 110;
constexpr int kTargetH = 38;
} // namespace

UiContext::UiContext(const UiOptions& opts) : theme_(opts) {
    out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    savedOutputCp_ = ::GetConsoleOutputCP();
    savedInputCp_ = ::GetConsoleCP();
    if (::GetConsoleMode(out_, &savedOutputMode_)) outputModeSaved_ = true;
    if (::GetConsoleCursorInfo(out_, &savedCursor_)) cursorSaved_ = true;
    ::SetConsoleOutputCP(CP_UTF8);

    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (::GetConsoleScreenBufferInfo(out_, &sbi)) {
        savedBufferSize_ = sbi.dwSize;
        savedWindow_ = sbi.srWindow;
        savedAttributes_ = sbi.wAttributes;
        attributesSaved_ = true;
        saved_ = true;
    }
    hwnd_ = ::GetConsoleWindow();
    if (hwnd_) {
        savedStyle_ = ::GetWindowLongW(hwnd_, GWL_STYLE);
        styleSaved_ = true;
    }
    LockSize();
}

UiContext::~UiContext() {
    // Restore resizability and the original buffer/window (scrollback returns).
    if (styleSaved_ && hwnd_) ::SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_);
    if (saved_) {
        SMALL_RECT tiny{0, 0, 1, 1};
        ::SetConsoleWindowInfo(out_, TRUE, &tiny);
        ::SetConsoleScreenBufferSize(out_, savedBufferSize_);
        ::SetConsoleWindowInfo(out_, TRUE, &savedWindow_);
    }
    if (attributesSaved_) ::SetConsoleTextAttribute(out_, savedAttributes_);
    if (cursorSaved_) ::SetConsoleCursorInfo(out_, &savedCursor_);
    if (outputModeSaved_) ::SetConsoleMode(out_, savedOutputMode_);
    if (savedOutputCp_ != 0) ::SetConsoleOutputCP(savedOutputCp_);
    if (savedInputCp_ != 0) ::SetConsoleCP(savedInputCp_);
}

void UiContext::LockSize() {
    // Desired size clamped to the largest that fits the current font/monitor.
    const COORD largest = ::GetLargestConsoleWindowSize(out_);
    int desW = kTargetW, desH = kTargetH;
    if (largest.X > 0 && desW > largest.X) desW = largest.X;
    if (largest.Y > 0 && desH > largest.Y) desH = largest.Y;
    if (desW < 20) desW = 20;
    if (desH < 10) desH = 10;

    // Shrink window, set buffer, grow window - the order the API constraints need.
    SMALL_RECT tiny{0, 0, 1, 1};
    ::SetConsoleWindowInfo(out_, TRUE, &tiny);
    ::SetConsoleScreenBufferSize(out_, COORD{static_cast<SHORT>(desW), static_cast<SHORT>(desH)});
    SMALL_RECT win{0, 0, static_cast<SHORT>(desW - 1), static_cast<SHORT>(desH - 1)};
    ::SetConsoleWindowInfo(out_, TRUE, &win);

    // Read the ACTUAL window the terminal gave us (Windows Terminal may keep its
    // own size), and make the buffer exactly equal to it so (0,0) is the top-left
    // and there is no scrollback offset. w_/h_ come from the actual window, so
    // SyncSize() will not thrash trying to re-force a size the terminal ignores.
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (::GetConsoleScreenBufferInfo(out_, &sbi)) {
        int aw = sbi.srWindow.Right - sbi.srWindow.Left + 1;
        int ah = sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
        if (aw < 1) aw = 1;
        if (ah < 1) ah = 1;
        if (sbi.dwSize.X != aw || sbi.dwSize.Y != ah) {
            ::SetConsoleScreenBufferSize(out_, COORD{static_cast<SHORT>(aw), static_cast<SHORT>(ah)});
            if (::GetConsoleScreenBufferInfo(out_, &sbi)) {
                aw = sbi.srWindow.Right - sbi.srWindow.Left + 1;
                ah = sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
                if (aw < 1) aw = 1;
                if (ah < 1) ah = 1;
            }
        }
        w_ = aw;
        h_ = ah;
    } else {
        w_ = desW;
        h_ = desH;
    }
    fb_.Resize(w_, h_);
    fb_.Invalidate();

    // Disable resize / maximize on classic conhost (true fixed window; no-op on
    // Windows Terminal, whose window it does not own).
    if (hwnd_) {
        const LONG style = ::GetWindowLongW(hwnd_, GWL_STYLE);
        ::SetWindowLongW(hwnd_, GWL_STYLE,
                         style & ~(WS_MAXIMIZEBOX | WS_THICKFRAME | WS_SIZEBOX));
    }
}

bool UiContext::SyncSize() {
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (!::GetConsoleScreenBufferInfo(out_, &sbi)) {
        if (w_ == 0) {
            w_ = kTargetW;
            h_ = kTargetH;
            fb_.Resize(w_, h_);
        }
        return false;
    }
    int winW = sbi.srWindow.Right - sbi.srWindow.Left + 1;
    int winH = sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
    if (winW < 1) winW = 1;
    if (winH < 1) winH = 1;

    // FAST PATH: size unchanged -> no console API calls, no relayout, no lag.
    if (winW == w_ && winH == h_) return false;

    // Genuine resize (only possible on terminals we cannot lock): re-lock and
    // relayout once.
    LockSize();
    return true;
}

LayoutMode UiContext::Layout() const { return LayoutForSize(w_, h_); }

bool UiContext::TooSmall() const { return SizeTooSmall(w_, h_); }

int UiContext::CenterX(int contentWidth) const {
    int x = (w_ - contentWidth) / 2;
    return x < 0 ? 0 : x;
}

void UiContext::PutCentered(int y, std::wstring_view text, UiColor c) {
    fb_.PutText(CenterX(static_cast<int>(text.size())), y, text, theme_.Attr(c));
}

} // namespace cheburnet::ui
