#pragma once
#include <string>
#include <vector>

namespace cheburnet::cmdline {

// Quote a single argument for a Windows command line so that
// CommandLineToArgvW reconstructs it byte-for-byte. Implements the canonical
// backslash/quote algorithm (Microsoft "Everyone quotes... the wrong way").
std::wstring QuoteArgvW(const std::wstring& arg);

// Build a full command line: quoted executable path followed by each already
// individual argument token, space-separated. Each element of `args` is treated
// as ONE argv token (e.g. L"--hostlist=C:\\path\\list.txt").
std::wstring Build(const std::wstring& exePath, const std::vector<std::wstring>& args);

} // namespace cheburnet::cmdline
