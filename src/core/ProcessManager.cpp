#include "ProcessManager.h"

#include <windows.h>

#include <memory>
#include <charconv>

#include "../util/CommandLine.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Win32Error.h"
#include "StatusProbe.h"
#include "SecureFs.h"

namespace cheburnet {
namespace {

std::optional<unsigned long long> CreationTimeOf(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetProcessTimes(h, &c, &e, &k, &u)) return std::nullopt;
    ULARGE_INTEGER li;
    li.LowPart = c.dwLowDateTime;
    li.HighPart = c.dwHighDateTime;
    return li.QuadPart;
}

std::optional<std::wstring> ImagePathOf(HANDLE h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buf));
    if (::QueryFullProcessImageNameW(h, 0, buf, &size)) return std::wstring(buf, size);
    return std::nullopt;
}

bool HandleIsSafeRegularFile(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return handle != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(handle, &info) &&
           (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                     FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
           info.nNumberOfLinks == 1;
}

HANDLE OpenProtectedRedirect(const std::wstring& path, const std::wstring& logsRoot,
                             SECURITY_ATTRIBUTES* attributes) {
    if (!securefs::IsStrictDescendant(logsRoot, path)) return INVALID_HANDLE_VALUE;
    const std::string empty;
    if (!securefs::AtomicWrite(path, empty).ok) return INVALID_HANDLE_VALUE;
    HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ, attributes,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!HandleIsSafeRegularFile(handle) ||
        !securefs::ValidateProtectedHandle(
             handle, path, securefs::ObjectKind::File, true).ok) {
        if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

} // namespace

ProcessManager::ProcessManager(const RuntimePaths& paths) : paths_(paths) {
    recordPath_ = paths_.ProgramDataRoot() + L"\\winws-run.txt";
    runtimeRoot_ = paths_.RuntimeRoot();
    record_ = LoadRecord(recordPath_);

    // Never trust a record that names an image outside our runtime tree.
    // This defeats a forged record that tries to point us at a foreign process.
    if (record_.valid() && !IsExpectedWinwsImage(record_.imagePath, runtimeRoot_)) {
        Logger::Warn(L"запись запуска winws с неожиданным образом проигнорирована: " + record_.imagePath);
        record_ = {};
    }
    if (record_.valid() && !IdentityMatches(record_)) {
        // Stale record (process gone or pid reused) - clear it.
        DeleteRecord(recordPath_);
        record_ = {};
    }
}

bool ProcessManager::IsExpectedWinwsImage(const std::wstring& image,
                                          const std::wstring& runtimeRoot) {
    if (image.empty() || runtimeRoot.empty()) return false;
    const std::wstring img = str::ToLowerAsciiW(image);
    std::wstring root = str::ToLowerAsciiW(runtimeRoot);
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    // Must live under the runtime root and be a winws.exe in a bin\ folder.
    if (img.rfind(root, 0) != 0) return false;
    const std::wstring suffix = L"\\bin\\winws.exe";
    if (img.size() < suffix.size()) return false;
    return img.compare(img.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ProcessManager::~ProcessManager() {
    std::scoped_lock lock(mutex_);
    CloseHandles();
}

void ProcessManager::CloseHandles() {
    if (process_) {
        ::CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_) {
        ::CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
}

StartResult ProcessManager::Start(const std::wstring& exePath,
                                  const std::vector<std::wstring>& args,
                                  const std::wstring& workingDir, const std::wstring& stdoutLog,
                                  const std::wstring& stderrLog, unsigned stabilizeMs) {
    std::scoped_lock lock(mutex_);
    StartResult res;

    const auto executableCheck = securefs::ValidateObject(
        exePath, securefs::ObjectKind::File, true);
    if (!executableCheck.ok || !IsExpectedWinwsImage(exePath, runtimeRoot_)) {
        res.status = StartStatus::InvalidExecutable;
        res.win32Error = executableCheck.win32Error ? executableCheck.win32Error
                                                   : ERROR_ACCESS_DENIED;
        return res;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    HANDLE hOut = OpenProtectedRedirect(stdoutLog, paths_.LogsDir(), &sa);
    HANDLE hErr = OpenProtectedRedirect(stderrLog, paths_.LogsDir(), &sa);
    auto closeStd = [&] {
        if (hIn != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
        if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) ::CloseHandle(hErr);
    };
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE ||
        hErr == INVALID_HANDLE_VALUE) {
        res.status = StartStatus::RedirectFailed;
        res.win32Error = ::GetLastError();
        closeStd();
        Logger::Error(L"запуск: не удалось открыть дескрипторы перенаправления: " +
                      win32::FormatError(res.win32Error));
        return res;
    }

    // Attribute list restricting inheritance to exactly the three std handles.
    // The sizing call must actually produce a size: if it does not, the
    // allocation below would be zero-length and every later attribute call
    // would write through a pointer with no storage behind it. Fail closed
    // instead of creating an elevated child with unrestricted inheritance.
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0 || attrSize > 64 * 1024) {
        res.win32Error = ::GetLastError();
        closeStd();
        Logger::Error(L"запуск: некорректный размер списка атрибутов процесса");
        return res;
    }
    auto attrBuf = std::make_unique<BYTE[]>(attrSize);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.get());
    if (attrList == nullptr) {
        res.win32Error = ERROR_NOT_ENOUGH_MEMORY;
        closeStd();
        return res;
    }
    if (!::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        res.win32Error = ::GetLastError();
        closeStd();
        return res;
    }
    HANDLE inheritList[3] = {hIn, hOut, hErr};
    if (!::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                                     sizeof(inheritList), nullptr, nullptr)) {
        res.win32Error = ::GetLastError();
        ::DeleteProcThreadAttributeList(attrList);
        closeStd();
        return res;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = hIn;
    si.StartupInfo.hStdOutput = hOut;
    si.StartupInfo.hStdError = hErr;
    si.lpAttributeList = attrList;

    const std::wstring cmd = cmdline::Build(exePath, args);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // Kill-on-close protects every uncommitted path; it is cleared only after
    // identity, stabilization and the protected process record all succeed.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        res.status = StartStatus::JobFailed;
        res.win32Error = ::GetLastError();
        ::DeleteProcThreadAttributeList(attrList);
        closeStd();
        return res;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &jobLimits, sizeof(jobLimits))) {
        res.status = StartStatus::JobFailed;
        res.win32Error = ::GetLastError();
        ::CloseHandle(job);
        ::DeleteProcThreadAttributeList(attrList);
        closeStd();
        return res;
    }

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    const BOOL ok = ::CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr,
                                     /*bInheritHandles*/ TRUE, flags, nullptr, workingDir.c_str(),
                                     &si.StartupInfo, &pi);
    const DWORD createErr = ok ? 0 : ::GetLastError();

    ::DeleteProcThreadAttributeList(attrList);
    closeStd(); // child holds its own copies

    if (!ok) {
        res.status = StartStatus::CreateFailed;
        res.win32Error = createErr;
        ::CloseHandle(job);
        Logger::Error(L"CreateProcessW завершился ошибкой: " + win32::FormatError(createErr));
        return res;
    }

    auto rollback = [&] {
        ::TerminateProcess(pi.hProcess, ERROR_PROCESS_ABORTED);
        // Closing a still kill-on-close Job is the kernel fallback when the
        // direct terminate request fails. Close it before waiting so the wait
        // confirms either termination mechanism actually completed.
        ::CloseHandle(job);
        job = nullptr;
        ::WaitForSingleObject(pi.hProcess, 5000);
        if (pi.hThread) ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        DeleteRecord(recordPath_);
        record_ = {};
    };

    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        res.status = StartStatus::AssignJobFailed;
        res.win32Error = ::GetLastError();
        rollback();
        return res;
    }
    // Pin the identity while the child is still suspended. No unverified code
    // is allowed to execute between CreateProcessW and this check.
    record_ = {};
    record_.pid = pi.dwProcessId;
    const auto creationTime = CreationTimeOf(pi.hProcess);
    const auto imagePath = ImagePathOf(pi.hProcess);
    if (!creationTime || !imagePath ||
        !str::IEqualsAscii(std::wstring_view(*imagePath), std::wstring_view(exePath)) ||
        !IsExpectedWinwsImage(*imagePath, runtimeRoot_)) {
        res.status = StartStatus::IdentityFailed;
        res.win32Error = ERROR_INVALID_DATA;
        rollback();
        return res;
    }
    record_.creationTime = *creationTime;
    record_.imagePath = *imagePath;

    if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        res.status = StartStatus::ResumeFailed;
        res.win32Error = ::GetLastError();
        rollback();
        return res;
    }
    ::CloseHandle(pi.hThread);
    pi.hThread = nullptr;

    // Stabilisation window: confirm the process does not die immediately.
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, stabilizeMs);
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        res.status = StartStatus::ExitedEarly;
        res.exitCode = code;
        res.record = record_;
        Logger::Error(L"winws.exe завершился слишком рано, код=" + std::to_wstring(code));
        DeleteRecord(recordPath_);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        record_ = {};
        return res;
    }

    if (waited != WAIT_TIMEOUT || !IdentityMatches(record_)) {
        res.status = StartStatus::IdentityFailed;
        res.win32Error = waited == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_DATA;
        rollback();
        return res;
    }

    if (!SaveRecord(recordPath_, record_)) {
        res.status = StartStatus::RecordFailed;
        res.win32Error = ERROR_WRITE_FAULT;
        rollback();
        return res;
    }

    // Transaction is committed: closing/detaching the launcher must no longer
    // kill a healthy engine. Until this exact point, closing the job is a
    // kernel-enforced rollback fallback even if TerminateProcess itself fails.
    jobLimits.BasicLimitInformation.LimitFlags = 0;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &jobLimits, sizeof(jobLimits))) {
        res.status = StartStatus::JobFailed;
        res.win32Error = ::GetLastError();
        rollback();
        return res;
    }

    CloseHandles();
    process_ = pi.hProcess;
    job_ = job;
    Logger::Info(L"winws.exe запущен транзакционно, PID=" + std::to_wstring(record_.pid));

    res.status = StartStatus::Ok;
    res.record = record_;
    return res;
}

bool ProcessManager::IdentityMatches(const ProcessRecord& r) {
    if (!r.valid()) return false;
    if (!probe::ProcessAlive(r.pid)) return false;
    auto ct = probe::ProcessCreationTime(r.pid);
    if (!ct || *ct != r.creationTime) return false;
    auto img = probe::ProcessImagePath(r.pid);
    if (!img) return false;
    return str::IEqualsAscii(std::wstring_view(*img), std::wstring_view(r.imagePath));
}

unsigned long ProcessManager::TrustedPidForExclusion(const ProcessRecord& r,
                                                     const std::wstring& runtimeRoot) {
    return r.valid() && IsExpectedWinwsImage(r.imagePath, runtimeRoot) && IdentityMatches(r)
               ? r.pid
               : 0;
}

StopResult ProcessManager::Stop() {
    std::scoped_lock lock(mutex_);
    StopResult res;
    if (!record_.valid()) {
        res.status = StopStatus::NotRunning;
        CloseHandles();
        return res;
    }

    // Never terminate a process whose recorded image is not a winws.exe under
    // our runtime tree.
    if (!IsExpectedWinwsImage(record_.imagePath, runtimeRoot_)) {
        res.status = StopStatus::IdentityMismatch;
        Logger::Warn(L"остановка отклонена: образ в записи не является нашим winws.exe: " + record_.imagePath);
        CloseHandles();
        return res;
    }

    // Re-verify identity right before terminating (guards pid reuse / TOCTOU).
    if (!probe::ProcessAlive(record_.pid)) {
        res.status = StopStatus::NotRunning;
        DeleteRecord(recordPath_);
        CloseHandles();
        record_ = {};
        return res;
    }
    if (!IdentityMatches(record_)) {
        res.status = StopStatus::IdentityMismatch;
        Logger::Warn(L"остановка отклонена: PID " + std::to_wstring(record_.pid) +
                     L" no longer matches the recorded winws identity");
        // Do not delete the record blindly; leave for diagnostics.
        CloseHandles();
        return res;
    }

    HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE, record_.pid);
    if (!h) {
        res.status = StopStatus::Failed;
        res.win32Error = ::GetLastError();
        Logger::Error(L"остановка: OpenProcess завершился ошибкой: " + win32::FormatError(res.win32Error));
        return res;
    }

    // Final image+ctime confirmation on the opened handle.
    auto ct = CreationTimeOf(h);
    auto img = ImagePathOf(h);
    if (!ct || *ct != record_.creationTime || !img ||
        !str::IEqualsAscii(std::wstring_view(*img), std::wstring_view(record_.imagePath))) {
        ::CloseHandle(h);
        res.status = StopStatus::IdentityMismatch;
        Logger::Warn(L"остановка отклонена после повторной проверки: PID не совпал " +
                     std::to_wstring(record_.pid));
        return res;
    }

    // winws provides no clean-shutdown IPC; terminate the verified process.
    if (!::TerminateProcess(h, 0)) {
        res.status = StopStatus::Failed;
        res.win32Error = ::GetLastError();
        ::CloseHandle(h);
        Logger::Error(L"TerminateProcess завершился ошибкой: " + win32::FormatError(res.win32Error));
        return res;
    }
    const DWORD stopped = ::WaitForSingleObject(h, 5000);
    if (stopped != WAIT_OBJECT_0) {
        res.status = StopStatus::Failed;
        res.win32Error = stopped == WAIT_FAILED ? ::GetLastError() : ERROR_TIMEOUT;
        ::CloseHandle(h);
        Logger::Error(L"остановка: завершение процесса не подтверждено: " +
                      win32::FormatError(res.win32Error));
        return res;
    }
    ::CloseHandle(h);

    Logger::Info(L"winws.exe (PID " + std::to_wstring(record_.pid) + L") остановлен");
    DeleteRecord(recordPath_);
    CloseHandles();
    record_ = {};
    res.status = StopStatus::Stopped;
    return res;
}

void ProcessManager::Detach() {
    // Keep the on-disk record so a later instance can find winws; just drop our
    // owned handles. The job has no kill-on-close, so the child keeps running.
    std::scoped_lock lock(mutex_);
    CloseHandles();
}

bool ProcessManager::IsConnectedByUs() const {
    std::scoped_lock lock(mutex_);
    return record_.valid() && IdentityMatches(record_);
}

ProcessRecord ProcessManager::Record() const {
    std::scoped_lock lock(mutex_);
    return record_;
}

// ---- Record persistence ----------------------------------------------------

ProcessRecord ProcessManager::LoadRecord(const std::wstring& path) {
    ProcessRecord r;
    if (!securefs::ValidateProtectedObject(path, securefs::ObjectKind::File, true).ok)
        return r;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) {
        ::CloseHandle(h);
        return r;
    }
    std::string content;
    char buf[2048];
    DWORD read = 0;
    for (;;) {
        if (!::ReadFile(h, buf, sizeof(buf), &read, nullptr)) {
            ::CloseHandle(h);
            return {};
        }
        if (read == 0) break;
        content.append(buf, read);
        if (content.size() > 8192) {
            ::CloseHandle(h);
            return {};
        }
    }
    ::CloseHandle(h);

    // Parse an exact, duplicate-free key=value record. Corruption or extra
    // fields invalidate the entire identity rather than being ignored.
    bool havePid = false, haveTime = false, haveImage = false;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line =
            content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
        auto trimmed = str::Trim(line);
        size_t eq = trimmed.find('=');
        if (eq == std::string_view::npos) return {};
        std::string_view key = trimmed.substr(0, eq);
        std::string_view val = trimmed.substr(eq + 1);
        if (key == "pid") {
            if (havePid) return {};
            unsigned long parsed = 0;
            const auto converted = std::from_chars(val.data(), val.data() + val.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != val.data() + val.size()) return {};
            r.pid = parsed;
            havePid = true;
        } else if (key == "ctime") {
            if (haveTime) return {};
            unsigned long long parsed = 0;
            const auto converted = std::from_chars(val.data(), val.data() + val.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != val.data() + val.size()) return {};
            r.creationTime = parsed;
            haveTime = true;
        } else if (key == "image") {
            if (haveImage || val.empty() || val.size() > 1024) return {};
            r.imagePath = str::ToUtf16(val);
            haveImage = true;
        } else {
            return {};
        }
    }
    return havePid && haveTime && haveImage && r.valid() ? r : ProcessRecord{};
}

bool ProcessManager::SaveRecord(const std::wstring& path, const ProcessRecord& r) {
    if (!r.valid() || r.imagePath.find_first_of(L"\r\n") != std::wstring::npos) return false;
    const std::string image = str::ToUtf8(r.imagePath);
    if (image.empty()) return false;
    std::string out;
    out += "pid=" + std::to_string(r.pid) + "\n";
    out += "ctime=" + std::to_string(r.creationTime) + "\n";
    out += "image=" + image + "\n";

    return securefs::AtomicWrite(path, out).ok;
}

void ProcessManager::DeleteRecord(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (!securefs::ValidateObject(path, securefs::ObjectKind::File, true).ok) return;
    ::DeleteFileW(path.c_str());
}

} // namespace cheburnet
