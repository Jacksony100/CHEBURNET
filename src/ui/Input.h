#pragma once
#include <windows.h>

namespace cheburnet::ui {

enum class Key {
    None, Up, Down, Left, Right, Enter, Esc, Backspace, Space, Tab,
    PageUp, PageDown, Home, End, Delete, F1, Char
};

struct KeyEvent {
    Key     key = Key::None;
    wchar_t ch = 0; // valid when key == Key::Char
    bool    resized = false;
};

// Raw console input reader in non-line mode: arrows, editing keys, F1, printable
// characters and window-resize events. QuickEdit stays disabled (set by the
// console owner). Does not echo.
class Input {
public:
    Input();
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Wait up to timeoutMs for a key or resize. Returns an event with
    // key==None (and resized possibly true) on timeout.
    KeyEvent Poll(int timeoutMs);

    // Discard pending input records.
    void Drain();

private:
    void*  handle_; // HANDLE
    DWORD  savedMode_ = 0;
    bool   modeSaved_ = false;
};

} // namespace cheburnet::ui
