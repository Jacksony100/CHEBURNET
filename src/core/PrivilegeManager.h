#pragma once
#include <string>

namespace cheburnet {

// Verifies administrative elevation via the process token (not indirect signals).
// The app ships a requireAdministrator manifest, so under normal launch the
// process is already elevated; this is the authoritative confirmation.
class PrivilegeManager {
public:
    // True if the current process token is elevated.
    static bool IsElevated();

    // Relaunch self elevated via ShellExecuteExW(runas) and return true if the
    // elevated instance was started (caller should then exit). Returns false if
    // the user declined UAC or the relaunch failed. Provided as a fallback; the
    // manifest normally makes this unnecessary.
    static bool RelaunchElevated(const std::wstring& extraArgs = L"");
};

} // namespace cheburnet
