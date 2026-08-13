#include "StatusProbe.h"

#include <windows.h>
#include <tlhelp32.h>

#include "../util/StringUtil.h"

#pragma comment(lib, "advapi32.lib")

namespace cheburnet::probe {

ServiceState QueryService(const std::wstring& name) {
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return ServiceState::Other;

    ServiceState state = ServiceState::NotInstalled;
    SC_HANDLE svc = ::OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS ss{};
        if (::QueryServiceStatus(svc, &ss)) {
            switch (ss.dwCurrentState) {
                case SERVICE_RUNNING:      state = ServiceState::Running; break;
                case SERVICE_STOPPED:      state = ServiceState::Stopped; break;
                case SERVICE_STOP_PENDING: state = ServiceState::StopPending; break;
                default:                   state = ServiceState::Other; break;
            }
        } else {
            state = ServiceState::Other;
        }
        ::CloseServiceHandle(svc);
    } else {
        const DWORD err = ::GetLastError();
        state = (err == ERROR_SERVICE_DOES_NOT_EXIST) ? ServiceState::NotInstalled
                                                      : ServiceState::Other;
    }
    ::CloseServiceHandle(scm);
    return state;
}

bool ServiceActive(const std::wstring& name) {
    const ServiceState s = QueryService(name);
    return s == ServiceState::Running || s == ServiceState::StopPending;
}

std::vector<ProcEntry> FindProcesses(const std::wstring& exeName) {
    std::vector<ProcEntry> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (str::IEqualsAscii(std::wstring_view(pe.szExeFile),
                                  std::wstring_view(exeName))) {
                out.push_back({pe.th32ProcessID, pe.szExeFile});
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return out;
}

std::optional<std::wstring> ProcessImagePath(unsigned long pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::nullopt;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::optional<std::wstring> result;
    if (::QueryFullProcessImageNameW(h, 0, buf, &size)) {
        result = std::wstring(buf, size);
    }
    ::CloseHandle(h);
    return result;
}

std::optional<unsigned long long> ProcessCreationTime(unsigned long pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::nullopt;
    FILETIME create{}, exit{}, kernel{}, user{};
    std::optional<unsigned long long> result;
    if (::GetProcessTimes(h, &create, &exit, &kernel, &user)) {
        ULARGE_INTEGER li;
        li.LowPart = create.dwLowDateTime;
        li.HighPart = create.dwHighDateTime;
        result = li.QuadPart;
    }
    ::CloseHandle(h);
    return result;
}

bool ProcessAlive(unsigned long pid) {
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const DWORD wait = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return wait == WAIT_TIMEOUT; // still running
}

bool Is64BitWindows() {
#if defined(_WIN64)
    return true; // 64-bit process only runs on 64-bit Windows
#else
    BOOL wow = FALSE;
    return ::IsWow64Process(::GetCurrentProcess(), &wow) && wow;
#endif
}

bool IsWindows10OrGreater() {
    // Use the version helper via RtlGetVersion to avoid manifest-dependent lies.
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionPtr>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                return vi.dwMajorVersion >= 10;
            }
        }
    }
    // OS support is a launch-security prerequisite. An indeterminate result is
    // not evidence that the host satisfies it.
    return false;
}

} // namespace cheburnet::probe
