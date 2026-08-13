#include "Logger.h"

#include <windows.h>

#include <cwchar>
#include <algorithm>

#include "StringUtil.h"
#include "Version.h"
#include "../core/SecureFs.h"

namespace cheburnet {
namespace {

std::mutex        g_mutex;
HANDLE            g_file = INVALID_HANDLE_VALUE;
std::wstring      g_path;
LogLevel          g_minLevel = LogLevel::Debug;
constexpr long long kMaxLogBytes = 2 * 1024 * 1024; // rotate at 2 MiB

const wchar_t* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return L"ОТЛАДКА";
        case LogLevel::Info:  return L"СВЕДЕНИЯ";
        case LogLevel::Warn:  return L"ПРЕДУПРЕЖДЕНИЕ";
        case LogLevel::Error: return L"ОШИБКА";
    }
    return L"СВЕДЕНИЯ";
}

bool WriteAllUtf8_NoLock(const std::string& utf8) {
    if (g_file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(utf8.size() - offset, 1024u * 1024u));
        DWORD written = 0;
        if (!::WriteFile(g_file, utf8.data() + offset, request, &written, nullptr) ||
            written != request)
            return false;
        offset += written;
    }
    return true;
}

bool PrepareProtectedLogPath_NoLock() {
    const DWORD attributes = ::GetFileAttributesW(g_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        const std::string empty;
        if (!securefs::AtomicWrite(g_path, empty).ok) return false;
    } else {
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return false;
        if (!securefs::HardenObject(g_path, securefs::ObjectKind::File).ok) return false;
    }
    return securefs::ValidateProtectedObject(
               g_path, securefs::ObjectKind::File, true).ok;
}

bool OpenProtectedLog_NoLock() {
    // Apply and verify the ACL before taking the long-lived append handle.
    // Once open, validate that exact no-follow handle rather than reopening by
    // name (which caused a sharing violation and would introduce a race).
    if (!PrepareProtectedLogPath_NoLock()) return false;
    HANDLE candidate = ::CreateFileW(
        g_path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (candidate == INVALID_HANDLE_VALUE) return false;
    const securefs::Result verified = securefs::ValidateProtectedHandle(
        candidate, g_path, securefs::ObjectKind::File, true);
    if (!verified.ok) {
        ::CloseHandle(candidate);
        return false;
    }
    g_file = candidate;
    return true;
}

} // namespace

std::wstring Logger::Timestamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t buf[32];
    ::swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

bool Logger::RotateIfNeeded_NoLock() {
    if (g_file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(g_file, &size)) return false;
    if (size.QuadPart < kMaxLogBytes) return true;

    ::CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;

    const std::wstring rolled = g_path + L".1";
    const DWORD rolledAttributes = ::GetFileAttributesW(rolled.c_str());
    if (rolledAttributes != INVALID_FILE_ATTRIBUTES) {
        if (!securefs::ValidateObject(rolled, securefs::ObjectKind::File, true).ok ||
            !::DeleteFileW(rolled.c_str())) return false;
    }
    if (!::MoveFileExW(g_path.c_str(), rolled.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    if (!securefs::HardenObject(rolled, securefs::ObjectKind::File).ok) return false;

    return OpenProtectedLog_NoLock();
}

bool Logger::Init(const std::wstring& logDir, LogLevel minLevel) {
    std::scoped_lock lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_minLevel = minLevel;

    // The pre-logger bootstrap must already have created and protected this
    // directory. Logger never creates an elevated path itself.
    if (!securefs::ValidatePathComponents(logDir).ok ||
        !securefs::ValidateProtectedObject(
            logDir, securefs::ObjectKind::Directory, false).ok) return false;

    g_path = logDir + L"\\cheburnet.log";
    if (!OpenProtectedLog_NoLock()) return false;

    if (!RotateIfNeeded_NoLock()) return false;

    // Session banner (BOM only if brand-new empty file to keep UTF-8 tools happy).
    LARGE_INTEGER size{};
    ::GetFileSizeEx(g_file, &size);
    std::string banner;
    if (size.QuadPart == 0) banner += "\xEF\xBB\xBF";
    banner += str::ToUtf8(L"[" + Timestamp() + L"] [СВЕДЕНИЯ] ===== CHEBURNET " +
                          std::wstring(CHEBURNET_VERSION_WSTR) + L" начало сеанса =====\r\n");
    if (!WriteAllUtf8_NoLock(banner) || !::FlushFileBuffers(g_file)) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

void Logger::Shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        const std::string tail =
            str::ToUtf8(L"[" + Timestamp() + L"] [СВЕДЕНИЯ] ===== завершение сеанса =====\r\n");
        (void)WriteAllUtf8_NoLock(tail);
        ::FlushFileBuffers(g_file);
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void Logger::Log(LogLevel level, std::wstring_view message) {
    std::scoped_lock lock(g_mutex);
    if (g_file == INVALID_HANDLE_VALUE) return;
    if (static_cast<int>(level) < static_cast<int>(g_minLevel)) return;
    if (!RotateIfNeeded_NoLock()) return;
    std::wstring line = L"[" + Timestamp() + L"] [" + LevelName(level) + L"] ";
    line.append(message);
    line += L"\r\n";
    if (!WriteAllUtf8_NoLock(str::ToUtf8(line))) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

std::wstring Logger::LogFilePath() {
    std::scoped_lock lock(g_mutex);
    return g_path;
}

} // namespace cheburnet
