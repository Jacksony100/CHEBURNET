#include "Input.h"

namespace cheburnet::ui {

Input::Input() {
    handle_ = ::GetStdHandle(STD_INPUT_HANDLE);
    HANDLE h = static_cast<HANDLE>(handle_);
    DWORD mode = 0;
    if (::GetConsoleMode(h, &mode)) {
        savedMode_ = mode;
        modeSaved_ = true;
        mode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
        mode &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE);
        mode &= ~static_cast<DWORD>(ENABLE_MOUSE_INPUT);
        mode &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        ::SetConsoleMode(h, mode);
    }
}

Input::~Input() {
    if (modeSaved_) ::SetConsoleMode(static_cast<HANDLE>(handle_), savedMode_);
}

void Input::Drain() {
    HANDLE h = static_cast<HANDLE>(handle_);
    ::FlushConsoleInputBuffer(h);
}

KeyEvent Input::Poll(int timeoutMs) {
    KeyEvent ev;
    HANDLE h = static_cast<HANDLE>(handle_);
    const DWORD waited = ::WaitForSingleObject(h, timeoutMs < 0 ? INFINITE
                                                                 : static_cast<DWORD>(timeoutMs));
    if (waited != WAIT_OBJECT_0) return ev;

    INPUT_RECORD rec;
    DWORD read = 0;
    if (!::ReadConsoleInputW(h, &rec, 1, &read) || read == 0) return ev;

    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        ev.resized = true;
        return ev;
    }
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return ev;

    const KEY_EVENT_RECORD& k = rec.Event.KeyEvent;
    switch (k.wVirtualKeyCode) {
        case VK_UP:     ev.key = Key::Up; return ev;
        case VK_DOWN:   ev.key = Key::Down; return ev;
        case VK_LEFT:   ev.key = Key::Left; return ev;
        case VK_RIGHT:  ev.key = Key::Right; return ev;
        case VK_RETURN: ev.key = Key::Enter; return ev;
        case VK_ESCAPE: ev.key = Key::Esc; return ev;
        case VK_BACK:   ev.key = Key::Backspace; return ev;
        case VK_TAB:    ev.key = Key::Tab; return ev;
        case VK_PRIOR:  ev.key = Key::PageUp; return ev;
        case VK_NEXT:   ev.key = Key::PageDown; return ev;
        case VK_HOME:   ev.key = Key::Home; return ev;
        case VK_END:    ev.key = Key::End; return ev;
        case VK_DELETE: ev.key = Key::Delete; return ev;
        case VK_F1:     ev.key = Key::F1; return ev;
        default: break;
    }
    const wchar_t ch = k.uChar.UnicodeChar;
    if (ch == L' ') {
        ev.key = Key::Space;
        ev.ch = L' ';
    } else if (ch >= 0x20 && ch != 0x7F) {
        ev.key = Key::Char;
        ev.ch = ch;
    }
    return ev;
}

} // namespace cheburnet::ui
