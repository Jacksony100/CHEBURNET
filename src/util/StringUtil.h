#pragma once
#include <string>
#include <string_view>

namespace cheburnet::str {

// UTF-16 (Windows wide) <-> UTF-8 conversions built on the Win32 code-page API.
// All internal Windows strings are UTF-16; UTF-8 is used only for log files and
// the minimal JSON config on disk.
std::string  ToUtf8(std::wstring_view w);
std::wstring ToUtf16(std::string_view s);

// Case-insensitive ASCII compare (used for hex hashes and file names).
bool IEqualsAscii(std::string_view a, std::string_view b);
bool IEqualsAscii(std::wstring_view a, std::wstring_view b);

// Lower-case ASCII letters (non-ASCII left unchanged).
std::string  ToLowerAscii(std::string_view s);
std::wstring ToLowerAsciiW(std::wstring_view s);

// Trim ASCII whitespace from both ends.
std::string_view Trim(std::string_view s);

} // namespace cheburnet::str
