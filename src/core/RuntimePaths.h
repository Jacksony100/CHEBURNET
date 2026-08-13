#pragma once
#include <string>

namespace cheburnet {

// Centralises every on-disk location CHEBURNET uses. All runtime files live
// under %ProgramData%\CHEBURNET; nothing is written to the user's own folders.
class RuntimePaths {
public:
    RuntimePaths();
    explicit RuntimePaths(std::wstring runtimeVersion);

    const std::wstring& ProgramDataRoot() const { return root_; }        // %ProgramData%\CHEBURNET
    const std::wstring& LogsDir() const { return logsDir_; }             // ...\logs
    const std::wstring& ConfigPath() const { return configPath_; }       // ...\config.json
    const std::wstring& RuntimeRoot() const { return runtimeRoot_; }      // ...\runtime
    const std::wstring& UpdatesDir() const { return updatesDir_; }
    const std::wstring& UserDir() const { return userDir_; }
    const std::wstring& RuntimeVersionDir() const { return versionDir_; } // ...\runtime\<version>
    const std::wstring& BinDir() const { return binDir_; }                // ...\runtime\<version>\bin
    const std::wstring& ListsDir() const { return listsDir_; }            // ...\runtime\<version>\lists
    const std::wstring& RuntimeVersion() const { return runtimeVersion_; }

    std::wstring WinwsExePath() const;   // ...\bin\winws.exe
    std::wstring DiagnosticsReportPath() const; // ...\logs\diagnostics.txt
    std::wstring ActiveRuntimePath() const; // ...\active-runtime.json
    std::wstring StrategyCatalogPath() const;

    // Recursively create a directory chain. Returns true if the dir now exists.
    static bool EnsureDir(const std::wstring& path);

    // True if the path exists (file or directory).
    static bool Exists(const std::wstring& path);

private:
    std::wstring root_;
    std::wstring logsDir_;
    std::wstring configPath_;
    std::wstring runtimeRoot_;
    std::wstring updatesDir_;
    std::wstring userDir_;
    std::wstring versionDir_;
    std::wstring binDir_;
    std::wstring listsDir_;
    std::wstring runtimeVersion_;
};

} // namespace cheburnet
