#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace cheburnet {

enum class Color { Default, Green, BrightGreen, DarkGreen, Red, Yellow, Gray, White, DarkGray };

// Win32 console wrapper: UTF-8 output, colour attributes, absolute-position
// writes (no per-frame scrolling / newlines) for flicker-free updates. Disables
// QuickEdit (which can freeze the app on stray selection) and hides the cursor;
// the destructor restores the original console state (RAII).
class ConsoleRenderer {
public:
    ConsoleRenderer() = default;
    ~ConsoleRenderer();

    ConsoleRenderer(const ConsoleRenderer&) = delete;
    ConsoleRenderer& operator=(const ConsoleRenderer&) = delete;

    bool Init();
    void SetTitle(const std::wstring& title);
    void EnsureMinSize(int cols, int rows);

    void Clear();
    void HideCursor();
    void ShowCursor();

    void MoveTo(int x, int y);
    void SetColor(Color c);

    void Write(std::wstring_view text);
    void WriteColored(std::wstring_view text, Color c);
    void WriteAt(int x, int y, std::wstring_view text, Color c = Color::Default);
    void WriteLine(std::wstring_view text = L"", Color c = Color::Default);
    void ClearRow(int y);

    // Draw multi-line art in `body` colour, painting any `starMarker` glyph in
    // `star` colour (used for the red star on the mascot's cap).
    void DrawMascot(int x, int y, const std::vector<std::wstring>& lines, wchar_t starMarker,
                    Color body, Color star);

    int  Width() const;
    int  Height() const;
    bool Initialized() const { return initialized_; }

private:
    void* out_ = nullptr; // HANDLE
    void* in_ = nullptr;  // HANDLE
    unsigned short savedAttr_ = 0;
    unsigned long  savedInMode_ = 0;
    unsigned long  savedOutMode_ = 0;
    unsigned int   savedInCP_ = 0;
    unsigned int   savedOutCP_ = 0;
    int            savedCursorSize_ = 25;
    bool           savedCursorVisible_ = true;
    bool           initialized_ = false;
};

} // namespace cheburnet
