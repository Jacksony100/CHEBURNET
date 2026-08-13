#pragma once
#include <string>

namespace cheburnet::win32 {

// Human-readable text for a Win32 error code via FormatMessageW.
// Always returns a non-empty, trimmed string ("code N: <text>").
std::wstring FormatError(unsigned long code);

// Convenience: format the current GetLastError().
std::wstring FormatLastError();

} // namespace cheburnet::win32
