#include "Win32Error.h"

#include <windows.h>

namespace cheburnet::win32 {

std::wstring FormatError(unsigned long code) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = ::FormatMessageW(flags, nullptr, code,
                                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (len && buffer) {
        text.assign(buffer, len);
    }
    if (buffer) ::LocalFree(buffer);

    // Trim trailing CR/LF/space that FormatMessage appends.
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
            text.back() == L'.')) {
        text.pop_back();
    }
    if (text.empty()) text = L"(описание отсутствует)";

    return L"код " + std::to_wstring(code) + L": " + text;
}

std::wstring FormatLastError() {
    return FormatError(::GetLastError());
}

} // namespace cheburnet::win32
