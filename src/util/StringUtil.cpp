#include "StringUtil.h"

#include <windows.h>

#include <cctype>
#include <climits>

namespace cheburnet::str {

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    if (w.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                             static_cast<int>(w.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                              static_cast<int>(w.size()), out.data(), needed,
                              nullptr, nullptr) != needed) return {};
    return out;
}

std::wstring ToUtf16(std::string_view s) {
    if (s.empty()) return {};
    if (s.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), out.data(), needed) != needed) return {};
    return out;
}

bool IEqualsAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool IEqualsAscii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::wstring ToLowerAsciiW(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

} // namespace cheburnet::str
