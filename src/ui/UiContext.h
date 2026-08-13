#pragma once
#include <windows.h>

#include <string_view>

#include "FrameBuffer.h"
#include "Input.h"
#include "Rng.h"
#include "Theme.h"

namespace cheburnet::ui {

// Owns the presentation surface: sizes the console so buffer == window (origin
// 0,0), holds the FrameBuffer, Input, Theme and Rng, and picks a LayoutMode from
// the current size. Restores the original console buffer/window on destruction.
class UiContext {
public:
    explicit UiContext(const UiOptions& opts);
    ~UiContext();

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    // Re-read the console size; returns true if the size changed.
    bool SyncSize();

    int         Width() const { return w_; }
    int         Height() const { return h_; }
    LayoutMode  Layout() const;
    bool        TooSmall() const;

    FrameBuffer&  FB() { return fb_; }
    Input&        In() { return input_; }
    class Theme&  Th() { return theme_; }
    Rng&          Rand() { return rng_; }

    WORD Attr(UiColor c) { return theme_.Attr(c); }
    const Glyphs& G() { return theme_.G(); }
    const UiOptions& Options() const { return theme_.Options(); }
    void SetOptions(const UiOptions& o) { theme_.SetOptions(o); }

    void Present() { fb_.Present(out_); }
    void Invalidate() { fb_.Invalidate(); }

    int  CenterX(int contentWidth) const;
    void PutCentered(int y, std::wstring_view text, UiColor c);

private:
    // Lock the console to a fixed size (buffer == window at origin) once, and
    // disable resize on classic conhost. Cheap: only re-run on a real change.
    void LockSize();

    HANDLE                out_;
    class Theme           theme_;
    FrameBuffer           fb_;
    Input                 input_;
    Rng                   rng_;
    int                   w_ = 0, h_ = 0;
    COORD                 savedBufferSize_{};
    SMALL_RECT            savedWindow_{};
    bool                  saved_ = false;
    HWND                  hwnd_ = nullptr;
    long                  savedStyle_ = 0;
    bool                  styleSaved_ = false;
    UINT                  savedOutputCp_ = 0;
    UINT                  savedInputCp_ = 0;
    DWORD                 savedOutputMode_ = 0;
    bool                  outputModeSaved_ = false;
    CONSOLE_CURSOR_INFO   savedCursor_{};
    bool                  cursorSaved_ = false;
    WORD                  savedAttributes_ = 0;
    bool                  attributesSaved_ = false;
};

} // namespace cheburnet::ui
