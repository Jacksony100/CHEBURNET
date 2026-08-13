#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "RuntimePaths.h"

namespace cheburnet {

// Identity of a launched winws.exe. pid alone is unsafe (reuse), so we also
// pin the creation time and full image path.
struct ProcessRecord {
    unsigned long      pid = 0;
    unsigned long long creationTime = 0; // FILETIME packed as uint64
    std::wstring       imagePath;
    bool valid() const { return pid != 0 && creationTime != 0 && !imagePath.empty(); }
};

enum class StartStatus {
    Ok,
    InvalidExecutable,
    RedirectFailed,
    JobFailed,
    CreateFailed,
    AssignJobFailed,
    ResumeFailed,
    IdentityFailed,
    ExitedEarly,
    RecordFailed
};

struct StartResult {
    StartStatus   status = StartStatus::CreateFailed;
    unsigned long win32Error = 0;
    unsigned long exitCode = 0; // meaningful when ExitedEarly
    ProcessRecord record;
};

enum class StopStatus { Stopped, NotRunning, IdentityMismatch, Failed };

struct StopResult {
    StopStatus    status = StopStatus::Failed;
    unsigned long win32Error = 0;
};

// Owns the winws.exe child process. Launch uses CreateProcessW with an explicit
// inherited-handle list (only the redirected std handles), a Job Object, and a
// persisted identity record so a later CHEBURNET instance can still recognise
// and stop the same process. Stop is always identity-verified: a pid whose
// creation time or image path no longer matches the record is never terminated.
class ProcessManager {
public:
    explicit ProcessManager(const RuntimePaths& paths);
    ~ProcessManager();

    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    StartResult Start(const std::wstring& exePath, const std::vector<std::wstring>& args,
                      const std::wstring& workingDir, const std::wstring& stdoutLog,
                      const std::wstring& stderrLog, unsigned stabilizeMs = 2500);

    // Identity-verified terminate of the recorded process.
    StopResult Stop();

    // Release ownership without terminating (winws keeps running after we exit).
    void Detach();

    // True if the recorded process (this run or a persisted one) is alive & ours.
    bool IsConnectedByUs() const;

    // Returns a COPY of the current record (thread-safe snapshot).
    ProcessRecord       Record() const;
    const std::wstring& RecordPath() const { return recordPath_; }

    static ProcessRecord LoadRecord(const std::wstring& path);
    static bool          SaveRecord(const std::wstring& path, const ProcessRecord& r);
    static void          DeleteRecord(const std::wstring& path);
    static bool          IdentityMatches(const ProcessRecord& r);

    // A stale or forged record must never hide a foreign winws.exe from the
    // conflict scan. Only a fully verified identity under our runtime root may
    // contribute an exclusion PID.
    static unsigned long TrustedPidForExclusion(const ProcessRecord& r,
                                                const std::wstring& runtimeRoot);

    // True only if `image` is a winws.exe located under our runtime root
    // (any version): it must start with `runtimeRoot`\ and end with \bin\winws.exe
    // (case-insensitive). A record pointing at any other image is untrusted and
    // never terminated - this stops a forged record from designating a
    // foreign/SYSTEM process for kill, while still allowing an upgraded launcher
    // to manage a winws started by a previous version.
    static bool IsExpectedWinwsImage(const std::wstring& image, const std::wstring& runtimeRoot);

private:
    void CloseHandles(); // caller must hold mutex_

    const RuntimePaths& paths_;
    std::wstring        recordPath_;
    std::wstring        runtimeRoot_; // %ProgramData%\CHEBURNET\runtime
    mutable std::mutex  mutex_; // guards record_, process_, job_
    ProcessRecord       record_;
    void*               process_ = nullptr; // HANDLE (current run only)
    void*               job_ = nullptr;     // HANDLE (current run only)
};

} // namespace cheburnet
