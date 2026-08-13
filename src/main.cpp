#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include "app/App.h"
#include "core/RuntimePaths.h"
#include "core/PrivilegeManager.h"
#include "core/SecureFs.h"
#include "util/Logger.h"
#include "util/StringUtil.h"
#include "util/Version.h"
#include "GeneratedProvenance.h"

namespace {

bool ArgEq(const wchar_t* a, const wchar_t* b) { return std::wcscmp(a, b) == 0; }

void WriteWide(HANDLE output, std::wstring_view text) {
    if (!output || output == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (::GetConsoleMode(output, &mode)) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const DWORD request = static_cast<DWORD>(
                (std::min<std::size_t>)(text.size() - offset, 32768));
            DWORD written = 0;
            if (!::WriteConsoleW(output, text.data() + offset, request, &written, nullptr) ||
                written == 0) return;
            offset += written;
        }
        return;
    }

    // Redirected stdout/stderr is UTF-8. Console output above remains native
    // UTF-16 and therefore does not depend on the process CRT locale.
    const std::string utf8 = cheburnet::str::ToUtf8(text);
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min<std::size_t>)(utf8.size() - offset, 32768));
        DWORD written = 0;
        if (!::WriteFile(output, utf8.data() + offset, request, &written, nullptr) ||
            written == 0) return;
        offset += written;
    }
}

void WriteStdout(std::wstring_view text) { WriteWide(::GetStdHandle(STD_OUTPUT_HANDLE), text); }

void WriteStderr(std::wstring_view text) {
    WriteWide(::GetStdHandle(STD_ERROR_HANDLE), text);
    WriteWide(::GetStdHandle(STD_ERROR_HANDLE), L"\r\n");
}

void PrintVersion() {
    WriteStdout(std::wstring(CHEBURNET_PRODUCT_WNAME) + L" v" + CHEBURNET_VERSION_WSTR +
                L"\r\nConnection Console for Windows 10/11 x64\r\n"
                L"Core engine: winws / WinDivert   Launcher: C++20 / Win32\r\n"
                L"CHEBURNET LABS // ENGINEERED BY MARSHAL JACKSONY100\r\n");
}

void PrintHelp() {
    WriteStdout(std::wstring(L"CHEBURNET v") + CHEBURNET_VERSION_WSTR +
                L"\r\nИспользование: CHEBURNET.exe [флаги]\r\n\r\n"
                L"  --version         показать версию и выйти\r\n"
                L"  --no-animation    мгновенный вывод без анимаций\r\n"
                L"  --reduced-motion  спокойный режим (без glitch/typewriter/idle)\r\n"
                L"  --ascii-only      только ASCII-графика (без Unicode-рамок)\r\n"
                L"  --debug-ui        показать отладочную информацию UI\r\n"
                L"  --help            показать эту справку\r\n");
}

} // namespace

// CHEBURNET launcher entry point (console subsystem, Unicode).
int wmain(int argc, wchar_t** argv) {
    cheburnet::CliFlags flags;
    for (int i = 1; i < argc; ++i) {
        if (ArgEq(argv[i], L"--version") || ArgEq(argv[i], L"-v")) {
            PrintVersion();
            return 0;
        }
        if (ArgEq(argv[i], L"--help") || ArgEq(argv[i], L"-h") || ArgEq(argv[i], L"/?")) {
            PrintHelp();
            return 0;
        }
        if (ArgEq(argv[i], L"--no-animation")) flags.noAnimation = true;
        else if (ArgEq(argv[i], L"--reduced-motion")) flags.reducedMotion = true;
        else if (ArgEq(argv[i], L"--ascii-only")) flags.asciiOnly = true;
        else if (ArgEq(argv[i], L"--debug-ui")) flags.debugUi = true;
    }

    // DLL search-order hardening for the launcher itself: only load DLLs from
    // System32. winws.exe and its DLLs are launched separately from the
    // ACL-locked runtime directory with an explicit working directory.
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const wchar_t* message = L"Не удалось включить безопасный поиск DLL. Запуск отклонён.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — безопасный запуск",
                      MB_OK | MB_ICONERROR);
        return 6;
    }

    PSECURITY_DESCRIPTOR mutexDescriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
            &mutexDescriptor, nullptr)) {
        const wchar_t* message = L"Не удалось создать security descriptor блокировки запуска.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — безопасный запуск",
                      MB_OK | MB_ICONERROR);
        return 7;
    }
    SECURITY_ATTRIBUTES mutexSecurity{};
    mutexSecurity.nLength = sizeof(mutexSecurity);
    mutexSecurity.lpSecurityDescriptor = mutexDescriptor;
    mutexSecurity.bInheritHandle = FALSE;
    HANDLE instanceMutex = ::CreateMutexW(&mutexSecurity, FALSE,
                                           L"Global\\CHEBURNET.PublicRelease.Instance.v1");
    const DWORD mutexError = ::GetLastError();
    ::LocalFree(mutexDescriptor);
    if (!instanceMutex || mutexError == ERROR_ALREADY_EXISTS) {
        if (instanceMutex) ::CloseHandle(instanceMutex);
        const wchar_t* message = L"Другой экземпляр CHEBURNET уже выполняется.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET", MB_OK | MB_ICONINFORMATION);
        return 5;
    }

    if (!cheburnet::PrivilegeManager::IsElevated()) {
        const wchar_t* message = L"CHEBURNET требует подтверждённые права администратора.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — безопасный запуск", MB_OK | MB_ICONERROR);
        ::CloseHandle(instanceMutex);
        return 2;
    }

    // Bootstrap fixed CHEBURNET-owned roots before any trusted state file is
    // interpreted. The active runtime is selected later inside App.
    const std::wstring embeddedVersion(cheburnet::upstream::kVersion);
    cheburnet::RuntimePaths paths(embeddedVersion);
    if (paths.ProgramDataRoot().empty()) {
        const wchar_t* message = L"Не удалось получить доверенный путь ProgramData.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — безопасный запуск",
                      MB_OK | MB_ICONERROR);
        ::CloseHandle(instanceMutex);
        return 3;
    }
    const cheburnet::securefs::Result bootstrap = cheburnet::securefs::BootstrapProtectedTree(
        paths.ProgramDataRoot(), paths.LogsDir(), paths.RuntimeRoot(), paths.UpdatesDir(),
        paths.UserDir());
    if (!bootstrap.ok) {
        const std::wstring message = L"Защищённый bootstrap отклонён:\n" + bootstrap.detail;
        WriteStderr(message);
        ::MessageBoxW(nullptr, message.c_str(), L"CHEBURNET — UPDATE REJECTED",
                      MB_OK | MB_ICONERROR);
        ::CloseHandle(instanceMutex);
        return 3;
    }
    if (!cheburnet::Logger::Init(paths.LogsDir())) {
        const wchar_t* message = L"Не удалось создать защищённый журнал. Запуск отклонён.";
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — безопасный запуск", MB_OK | MB_ICONERROR);
        ::CloseHandle(instanceMutex);
        return 4;
    }

    int code = 0;
    try {
        cheburnet::App app(flags);
        code = app.Run();
    } catch (const std::exception& error) {
        const std::wstring message = L"Критическая ошибка CHEBURNET: " +
                                     cheburnet::str::ToUtf16(error.what());
        cheburnet::Logger::Error(message);
        WriteStderr(message);
        ::MessageBoxW(nullptr, message.c_str(), L"CHEBURNET — FATAL", MB_OK | MB_ICONERROR);
        code = 1;
    } catch (...) {
        const wchar_t* message = L"Критическая неизвестная ошибка CHEBURNET.";
        cheburnet::Logger::Error(message);
        WriteStderr(message);
        ::MessageBoxW(nullptr, message, L"CHEBURNET — FATAL", MB_OK | MB_ICONERROR);
        code = 1;
    }

    cheburnet::Logger::Shutdown();
    ::CloseHandle(instanceMutex);
    return code;
}
