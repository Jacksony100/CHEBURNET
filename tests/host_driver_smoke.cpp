// Opt-in host test only; never run by the ordinary CTest suite.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "core/PrivilegeManager.h"
#include "core/ResourceExtractor.h"
#include "core/RuntimePaths.h"
#include "core/SecureFs.h"
#include "util/StringUtil.h"

#ifndef CHEBURNET_E2E_INSTANCE
#error The driver smoke helper requires an isolated host E2E build
#endif

namespace {
struct ServiceFixture {
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    bool Remove() {
        if (!service) return true;
        SERVICE_STATUS status{};
        if (!::QueryServiceStatus(service, &status)) return false;
        if (status.dwCurrentState != SERVICE_STOPPED) {
            if (!::ControlService(service, SERVICE_CONTROL_STOP, &status) &&
                ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
            const auto deadline = ::GetTickCount64() + 5000;
            do {
                if (!::QueryServiceStatus(service, &status)) return false;
                if (status.dwCurrentState == SERVICE_STOPPED) break;
                ::Sleep(25);
            } while (::GetTickCount64() < deadline);
            if (status.dwCurrentState != SERVICE_STOPPED) return false;
        }
        if (!::DeleteService(service) && ::GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
            return false;
        ::CloseServiceHandle(service);
        service = nullptr;
        return true;
    }
    ~ServiceFixture() {
        if (!Remove()) std::printf("DRIVER_CLEANUP_FAILED: %lu\n", ::GetLastError());
        if (service) ::CloseServiceHandle(service);
        if (manager) ::CloseServiceHandle(manager);
    }
};
struct Module {
    HMODULE value = nullptr;
    ~Module() { if (value) ::FreeLibrary(value); }
};
template<typename Function> Function Procedure(HMODULE module, const char* name) {
    const auto address = ::GetProcAddress(module, name);
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !cheburnet::PrivilegeManager::IsElevated()) return 2;
    const std::wstring mode = argv[1];
    if (mode == L"--e2e-identity") {
        std::printf("CHEBURNET_E2E_INSTANCE:%s\n", CHEBURNET_E2E_INSTANCE);
        return 0;
    }
    const cheburnet::RuntimePaths paths;
    const auto& root = paths.ProgramDataRoot();
    if (root.empty() || root.substr(root.find_last_of(L'\\') + 1) !=
        L"CHEBURNET-E2E-" + cheburnet::str::ToUtf16(CHEBURNET_E2E_INSTANCE)) return 3;
    if (mode == L"--cleanup") {
        const auto parent = root.substr(0, root.find_last_of(L'\\'));
        const auto removed = cheburnet::securefs::RemoveTreeUnder(parent, root);
        std::printf("HOST_FIXTURE_CLEANUP: %s error=%lu\n", removed.ok ? "PASS" : "FAIL", removed.win32Error);
        return removed.ok ? 0 : 1;
    }
    if (mode != L"--driver-smoke") return 2;
    std::vector<cheburnet::RuntimeStrategy> catalog;
    if (!cheburnet::ResourceExtractor(paths).VerifyRuntime(catalog).ok) return 4;
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return 5;
    ServiceFixture fixture;
    fixture.manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!fixture.manager) return 6;
    const auto sys = paths.BinDir() + L"\\WinDivert64.sys";
    // Retain a handle to OUR exclusively created service. Never adopt an
    // existing service. Precreation also avoids the DLL's EventLog key write.
    fixture.service = ::CreateServiceW(fixture.manager, L"WinDivert", L"WinDivert",
        SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, sys.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!fixture.service) {
        std::printf("DRIVER_SMOKE: exclusive service creation refused error=%lu\n", ::GetLastError());
        return 7;
    }
    Module dll{::LoadLibraryExW((paths.BinDir() + L"\\WinDivert.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!dll.value) return 8;
    using Open = HANDLE(WINAPI*)(const char*, int, INT16, UINT64);
    using Close = BOOL(WINAPI*)(HANDLE);
    const auto open = Procedure<Open>(dll.value, "WinDivertOpen");
    const auto close = Procedure<Close>(dll.value, "WinDivertClose");
    if (!open || !close) return 9;
    // false matches no packets; SNIFF never drops original packets.
    const HANDLE handle = open("false", 0, 0, 1);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("DRIVER_SMOKE: WinDivertOpen failed error=%lu\n", ::GetLastError());
        return 10;
    }
    SERVICE_STATUS status{};
    const bool running = ::QueryServiceStatus(fixture.service, &status) &&
                         status.dwCurrentState == SERVICE_RUNNING;
    const bool closed = close(handle) != FALSE;
    const bool removed = fixture.Remove();
    std::printf("DRIVER_SMOKE: filter=false flags=SNIFF running=%d closed=%d service_removed=%d\n",
        running, closed, removed);
    return running && closed && removed ? 0 : 1;
}
