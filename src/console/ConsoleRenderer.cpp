#include "ConsoleRenderer.h"

#include <windows.h>

namespace cheburnet {
namespace {

WORD AttrFor(Color c) {
    switch (c) {
        case Color::Green:       return FOREGROUND_GREEN;
        case Color::DarkGreen:   return FOREGROUND_GREEN;
        case Color::BrightGreen: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Color::Red:         return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case Color::Yellow:      return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Color::Gray:        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case Color::White:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case Color::DarkGray:    return FOREGROUND_INTENSITY;
        case Color::Default:
        default:                 return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    }
}

} // namespace

ConsoleRenderer::~ConsoleRenderer() {
    if (!initialized_) return;
    HANDLE out = static_cast<HANDLE>(out_);
    HANDLE in = static_cast<HANDLE>(in_);
    // Restore cursor.
    CONSOLE_CURSOR_INFO ci{};
    ci.dwSize = savedCursorSize_ > 0 ? static_cast<DWORD>(savedCursorSize_) : 25;
    ci.bVisible = savedCursorVisible_ ? TRUE : FALSE;
    ::SetConsoleCursorInfo(out, &ci);
    ::SetConsoleTextAttribute(out, savedAttr_);
    if (in) ::SetConsoleMode(in, savedInMode_);
    if (out && savedOutMode_) ::SetConsoleMode(out, savedOutMode_);
    if (savedOutCP_) ::SetConsoleOutputCP(savedOutCP_);
    if (savedInCP_) ::SetConsoleCP(savedInCP_);
}

bool ConsoleRenderer::Init() {
    out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    in_ = ::GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = static_cast<HANDLE>(out_);
    HANDLE in = static_cast<HANDLE>(in_);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) return false;

    savedOutCP_ = ::GetConsoleOutputCP();
    savedInCP_ = ::GetConsoleCP();
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (::GetConsoleScreenBufferInfo(out, &sbi)) savedAttr_ = sbi.wAttributes;

    CONSOLE_CURSOR_INFO ci{};
    if (::GetConsoleCursorInfo(out, &ci)) {
        savedCursorSize_ = static_cast<int>(ci.dwSize);
        savedCursorVisible_ = ci.bVisible != FALSE;
    }

    if (in) {
        ::GetConsoleMode(in, &savedInMode_);
        DWORD mode = savedInMode_;
        mode |= ENABLE_EXTENDED_FLAGS;
        mode &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE); // avoid selection-freeze
        mode &= ~static_cast<DWORD>(ENABLE_MOUSE_INPUT);
        ::SetConsoleMode(in, mode);
    }
    if (out) {
        ::GetConsoleMode(out, &savedOutMode_);
        DWORD om = savedOutMode_ | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        ::SetConsoleMode(out, om); // VT is optional; classic attrs still used
    }

    // Dark background, bright-green foreground as the base palette.
    ::SetConsoleTextAttribute(out, AttrFor(Color::Default));
    initialized_ = true;
    return true;
}

void ConsoleRenderer::SetTitle(const std::wstring& title) {
    ::SetConsoleTitleW(title.c_str());
}

void ConsoleRenderer::EnsureMinSize(int cols, int rows) {
    HANDLE out = static_cast<HANDLE>(out_);
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (!::GetConsoleScreenBufferInfo(out, &sbi)) return;
    const int curCols = sbi.srWindow.Right - sbi.srWindow.Left + 1;
    const int curRows = sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
    const int wantCols = curCols < cols ? cols : curCols;
    const int wantRows = curRows < rows ? rows : curRows;
    if (wantCols == curCols && wantRows == curRows) return;

    COORD buf{static_cast<SHORT>(wantCols), static_cast<SHORT>(wantRows + 2)};
    ::SetConsoleScreenBufferSize(out, buf); // may fail if window bigger; ignore
    SMALL_RECT win{0, 0, static_cast<SHORT>(wantCols - 1), static_cast<SHORT>(wantRows - 1)};
    ::SetConsoleWindowInfo(out, TRUE, &win);
    ::SetConsoleScreenBufferSize(out, buf);
}

void ConsoleRenderer::Clear() {
    HANDLE out = static_cast<HANDLE>(out_);
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (!::GetConsoleScreenBufferInfo(out, &sbi)) return;
    const DWORD cells = static_cast<DWORD>(sbi.dwSize.X) * static_cast<DWORD>(sbi.dwSize.Y);
    COORD origin{0, 0};
    DWORD written = 0;
    ::FillConsoleOutputCharacterW(out, L' ', cells, origin, &written);
    ::FillConsoleOutputAttribute(out, AttrFor(Color::Default), cells, origin, &written);
    ::SetConsoleCursorPosition(out, origin);
}

void ConsoleRenderer::HideCursor() {
    HANDLE out = static_cast<HANDLE>(out_);
    CONSOLE_CURSOR_INFO ci{};
    ::GetConsoleCursorInfo(out, &ci);
    ci.bVisible = FALSE;
    ::SetConsoleCursorInfo(out, &ci);
}

void ConsoleRenderer::ShowCursor() {
    HANDLE out = static_cast<HANDLE>(out_);
    CONSOLE_CURSOR_INFO ci{};
    ::GetConsoleCursorInfo(out, &ci);
    ci.dwSize = savedCursorSize_ > 0 ? static_cast<DWORD>(savedCursorSize_) : 25;
    ci.bVisible = TRUE;
    ::SetConsoleCursorInfo(out, &ci);
}

void ConsoleRenderer::MoveTo(int x, int y) {
    COORD c{static_cast<SHORT>(x), static_cast<SHORT>(y)};
    ::SetConsoleCursorPosition(static_cast<HANDLE>(out_), c);
}

void ConsoleRenderer::SetColor(Color c) {
    ::SetConsoleTextAttribute(static_cast<HANDLE>(out_), AttrFor(c));
}

void ConsoleRenderer::Write(std::wstring_view text) {
    if (text.empty()) return;
    DWORD written = 0;
    ::WriteConsoleW(static_cast<HANDLE>(out_), text.data(), static_cast<DWORD>(text.size()),
                    &written, nullptr);
}

void ConsoleRenderer::WriteColored(std::wstring_view text, Color c) {
    SetColor(c);
    Write(text);
}

void ConsoleRenderer::WriteAt(int x, int y, std::wstring_view text, Color c) {
    MoveTo(x, y);
    WriteColored(text, c);
}

void ConsoleRenderer::WriteLine(std::wstring_view text, Color c) {
    WriteColored(text, c);
    Write(L"\r\n");
}

void ConsoleRenderer::ClearRow(int y) {
    HANDLE out = static_cast<HANDLE>(out_);
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (!::GetConsoleScreenBufferInfo(out, &sbi)) return;
    COORD c{0, static_cast<SHORT>(y)};
    DWORD written = 0;
    ::FillConsoleOutputCharacterW(out, L' ', static_cast<DWORD>(sbi.dwSize.X), c, &written);
    ::FillConsoleOutputAttribute(out, AttrFor(Color::Default), static_cast<DWORD>(sbi.dwSize.X), c,
                                 &written);
}

void ConsoleRenderer::DrawMascot(int x, int y, const std::vector<std::wstring>& lines,
                                 wchar_t starMarker, Color body, Color star) {
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& line = lines[i];
        const int ly = y + static_cast<int>(i);
        // Draw the whole line in the body colour first.
        WriteAt(x, ly, line, body);
        // Overpaint any star markers in the star colour.
        for (size_t j = 0; j < line.size(); ++j) {
            if (line[j] == starMarker) {
                const wchar_t s[2] = {starMarker, 0};
                WriteAt(x + static_cast<int>(j), ly, s, star);
            }
        }
    }
}

int ConsoleRenderer::Width() const {
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (::GetConsoleScreenBufferInfo(static_cast<HANDLE>(out_), &sbi))
        return sbi.srWindow.Right - sbi.srWindow.Left + 1;
    return 80;
}

int ConsoleRenderer::Height() const {
    CONSOLE_SCREEN_BUFFER_INFO sbi{};
    if (::GetConsoleScreenBufferInfo(static_cast<HANDLE>(out_), &sbi))
        return sbi.srWindow.Bottom - sbi.srWindow.Top + 1;
    return 25;
}

} // namespace cheburnet
