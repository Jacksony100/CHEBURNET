#include "CommandLine.h"

namespace cheburnet::cmdline {

std::wstring QuoteArgvW(const std::wstring& arg) {
    // Fast path: no quoting needed for non-empty tokens without special chars.
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring out;
    out.push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // Escape all trailing backslashes so they do not eat the closing quote.
            out.append(static_cast<size_t>(backslashes) * 2, L'\\');
            break;
        } else if (*it == L'"') {
            // Escape the backslashes and the quote itself.
            out.append(static_cast<size_t>(backslashes) * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring Build(const std::wstring& exePath, const std::vector<std::wstring>& args) {
    std::wstring cmd = QuoteArgvW(exePath);
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd.append(QuoteArgvW(a));
    }
    return cmd;
}

} // namespace cheburnet::cmdline
