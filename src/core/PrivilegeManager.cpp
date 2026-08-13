#include "PrivilegeManager.h"

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace cheburnet {

bool PrivilegeManager::IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD cb = sizeof(elevation);
    bool elevated = false;
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &cb)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    ::CloseHandle(token);
    return elevated;
}

bool PrivilegeManager::RelaunchElevated(const std::wstring& extraArgs) {
    wchar_t self[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // triggers the UAC prompt
    sei.lpFile = self;
    sei.lpParameters = extraArgs.empty() ? nullptr : extraArgs.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei)) {
        // ERROR_CANCELLED == user declined UAC.
        return false;
    }
    if (sei.hProcess) ::CloseHandle(sei.hProcess);
    return true;
}

} // namespace cheburnet
