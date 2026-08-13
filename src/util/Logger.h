#pragma once
#include <mutex>
#include <string>
#include <string_view>

namespace cheburnet {

enum class LogLevel { Debug, Info, Warn, Error };

// Thread-safe file logger writing UTF-8 to <logDir>\cheburnet.log with
// size-based rotation. Never logs secrets - callers must not pass tokens or
// passwords. Safe to call before Init (messages are dropped, never crash).
class Logger {
public:
    // Initialise only inside a directory secured by the pre-logger bootstrap.
    static bool Init(const std::wstring& logDir, LogLevel minLevel = LogLevel::Debug);
    static void Shutdown();

    static void Log(LogLevel level, std::wstring_view message);

    static void Debug(std::wstring_view m) { Log(LogLevel::Debug, m); }
    static void Info(std::wstring_view m)  { Log(LogLevel::Info, m); }
    static void Warn(std::wstring_view m)  { Log(LogLevel::Warn, m); }
    static void Error(std::wstring_view m) { Log(LogLevel::Error, m); }

    static std::wstring LogFilePath();

private:
    static bool RotateIfNeeded_NoLock();
    static std::wstring Timestamp();
};

} // namespace cheburnet
