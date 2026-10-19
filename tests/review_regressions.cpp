#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "app/Diagnostics.h"
#include "app/OperationState.h"
#include "app/RuntimeRecovery.h"
#include "core/PrivilegeManager.h"
#include "core/Launcher.h"
#include "core/ProcessManager.h"
#include "core/StatusProbe.h"
#include "core/ResourceExtractor.h"
#include "core/SecureFs.h"
#include "update/UpdateCheckService.h"
#include "update/WinHttpClient.h"
#include "util/Json.h"
#include "util/StringUtil.h"
#include "GeneratedProvenance.h"
#include "GeneratedStrategies.h"

using namespace cheburnet;
namespace fs = std::filesystem;
static int failures = 0;
#define REQUIRE(condition) do { if (!(condition)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

struct Fixture {
    std::wstring parent;
    std::wstring root;
    Fixture() {
        wchar_t buffer[MAX_PATH]{};
        REQUIRE(::GetTempPathW(MAX_PATH, buffer) != 0);
        parent = fs::absolute(buffer).wstring();
        root = (fs::path(parent) / (L"cheburnet-review-" + std::to_wstring(::GetCurrentProcessId()) +
                 L"-" + std::to_wstring(::GetTickCount64()))).wstring();
        REQUIRE(securefs::IsStrictDescendant(parent, root));
        REQUIRE(::CreateDirectoryW(root.c_str(), nullptr) != FALSE);
    }
    ~Fixture() {
        if (securefs::IsStrictDescendant(parent, root))
            REQUIRE(securefs::RemoveTreeUnder(parent, root).ok);
    }
    void Put(const std::wstring& relative, const std::string& text = "fixture") {
        const fs::path target = fs::path(root) / relative;
        fs::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary);
        file << text;
        REQUIRE(file.good());
    }
};

static int Inventory() {
    Fixture fixture;
    fixture.Put(L"bin\\winws.exe");
    const std::vector<std::wstring> allow{L"bin\\winws.exe", L"lists\\vendor.txt"};
    std::wstring unexpected;
    REQUIRE(ValidateRuntimeInventory(fixture.root, allow, unexpected));
    fixture.Put(L"bin\\wlanapi.dll", "inert marker");
    REQUIRE(!ValidateRuntimeInventory(fixture.root, allow, unexpected));
    REQUIRE(unexpected == L"bin\\wlanapi.dll");
    REQUIRE(fs::remove(fs::path(fixture.root) / L"bin\\wlanapi.dll"));
    fixture.Put(L"bin\\nested\\other.dll");
    REQUIRE(!ValidateRuntimeInventory(fixture.root, allow, unexpected));
    REQUIRE(fs::remove(fs::path(fixture.root) / L"bin\\nested\\other.dll"));
    REQUIRE(fs::remove(fs::path(fixture.root) / L"bin\\nested"));
    const auto link = (fs::path(fixture.root) / L"lists\\vendor.txt").wstring();
    fs::create_directories(fs::path(link).parent_path());
    REQUIRE(::CreateHardLinkW(link.c_str(),
                (fs::path(fixture.root) / L"bin\\winws.exe").c_str(), nullptr) != FALSE);
    REQUIRE(!ValidateRuntimeInventory(fixture.root, allow, unexpected));
    REQUIRE(fs::remove(link));
    // Existing weak security must not be silently adopted.
    REQUIRE(!securefs::EnsureTrustedDirectory(fixture.root + L"\\untrusted").ok);
    REQUIRE(!fs::exists(fs::path(fixture.root) / L"untrusted"));
    return failures;
}

static int Cleanup() {
    update::StateResult state;
    std::vector<std::wstring> keep{L"must be cleared"};
    REQUIRE(!BuildRuntimeKeepSet(state, L"2.0.0", L"1.0.0", L"", keep));
    REQUIRE(keep.empty());
    state.missing = true;
    REQUIRE(!BuildRuntimeKeepSet(state, L"2.0.0", L"1.0.0", L"", keep));
    state.ok = true;
    state.missing = false;
    state.state.current = "2.0.0";
    state.state.previousKnownGood = "1.9.0";
    state.state.pending = "3.0.0";
    REQUIRE(BuildRuntimeKeepSet(state, L"2.1.0", L"1.0.0", L"1.8.0", keep));
    for (const auto version : {L"2.0.0", L"1.9.0", L"3.0.0", L"2.1.0", L"1.0.0", L"1.8.0"})
        REQUIRE(std::find(keep.begin(), keep.end(), version) != keep.end());

    Fixture fixture;
    fixture.Put(L"runtime\\2.0.0\\keep.txt", "current");
    fixture.Put(L"runtime\\1.9.0\\keep.txt", "previous");
    const auto paths = RuntimePaths::ForOwnedRoot(fixture.root, L"2.0.0");
    ResourceExtractor extractor(paths);
    auto result = extractor.CleanupOldVersions();
    REQUIRE(!result.ok && result.removed == 0);
    fixture.Put(L"active-runtime.json", "{broken");
    result = extractor.CleanupOldVersions();
    REQUIRE(!result.ok && result.removed == 0);
    REQUIRE(fs::exists(fs::path(fixture.root) / L"runtime\\2.0.0\\keep.txt"));
    REQUIRE(fs::exists(fs::path(fixture.root) / L"runtime\\1.9.0\\keep.txt"));
    return failures;
}

static int Recovery() {
    std::vector<int> calls;
    const auto stop = [&] { calls.push_back(1); return true; };
    const auto commit = [&] { calls.push_back(2); return true; };
    REQUIRE(CommitIntegrityRecovery(true, L"current", L"current", L"previous", stop, commit));
    REQUIRE((calls == std::vector<int>{1, 2}));
    calls.clear();
    REQUIRE(!CommitIntegrityRecovery(true, L"unknown", L"current", L"previous", stop, commit));
    REQUIRE(calls.empty());
    REQUIRE(!CommitIntegrityRecovery(true, L"current", L"current", L"previous",
        [&] { calls.push_back(1); return false; }, commit));
    REQUIRE((calls == std::vector<int>{1}));
    calls.clear();
    REQUIRE(CommitIntegrityRecovery(true, L"previous", L"current", L"previous", stop, commit));
    REQUIRE((calls == std::vector<int>{2}));
    calls.clear();
    REQUIRE(CommitIntegrityRecovery(false, L"", L"current", L"previous", stop, commit));
    REQUIRE((calls == std::vector<int>{2}));
    REQUIRE(!CommitIntegrityRecovery(false, L"", L"current", L"previous", stop, [] { return false; }));
    return failures;
}

static int ProcessState() {
    OperationState state;
    state.Complete(AppOperationState::Connected);
    state.ObserveProcess(false); // engine died while the main menu was open
    REQUIRE(state.TryTransition(AppOperationState::Disconnected, AppOperationState::Connecting));
    state.ObserveProcess(false);
    REQUIRE(state.Get() == AppOperationState::Connecting);
    state.Complete(AppOperationState::Updating);
    state.ObserveProcess(true);
    REQUIRE(state.Get() == AppOperationState::Updating);
    state.Complete(AppOperationState::Error);
    state.ObserveProcess(false);
    REQUIRE(state.Get() == AppOperationState::Disconnected);
    return failures;
}

static int Notifications() {
    update::CheckResult result;
    result.status = update::CheckStatus::Available;
    result.payload = update::Eligibility::Upgrade;
    result.manifest.payload.version = "2.0.0";
    REQUIRE(update::ShouldNotify(result, ""));
    REQUIRE(!update::ShouldNotify(result, "2.0.0"));
    result.launcher = update::Eligibility::Upgrade;
    REQUIRE(update::ShouldNotify(result, "2.0.0"));
    result.status = update::CheckStatus::Current;
    REQUIRE(!update::ShouldNotify(result, ""));
    result.status = update::CheckStatus::Rejected;
    REQUIRE(update::ShouldNotify(result, ""));
    return failures;
}

static int Privacy() {
    for (const auto address : {L"2001:db8::1", L"fe80::abcd", L"::1", L"::",
             L"fe80::1234%12", L"::ffff:192.0.2.1", L"2001:db8:0:1:2:3:4:5"}) {
        const std::wstring input = L"peer=[" + std::wstring(address) + L"]:443";
        const auto output = diag::Redact(input, {});
        REQUIRE(output.find(address) == std::wstring::npos);
        REQUIRE(output.find(L"]:443") != std::wstring::npos);
    }
    REQUIRE(diag::Redact(L"duration=12:34 hash=abcdef normal", {}) ==
                         L"duration=12:34 hash=abcdef normal");
    return failures;
}

// Actual WinHTTP pending I/O against a loopback TCP peer that never completes
// TLS. No certificate bypass, external network, driver or machine trust changes.
static int HttpCancellation() {
    WSADATA data{};
    REQUIRE(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    for (const bool cancelRequest : {true, false}) {
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener != INVALID_SOCKET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener, 1) == 0);
        int size = sizeof(address);
        REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        std::promise<void> accepted;
        auto acceptedFuture = accepted.get_future();
        std::jthread server([&](std::stop_token token) {
            while (!token.stop_requested()) {
                fd_set set;
                FD_ZERO(&set);
                FD_SET(listener, &set);
                timeval timeout{0, 50000};
                if (::select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
                const SOCKET peer = ::accept(listener, nullptr, nullptr);
                if (peer == INVALID_SOCKET) continue;
                accepted.set_value();
                while (!token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ::closesocket(peer);
                return;
            }
        });
        std::atomic<bool> cancel{false};
        update::HttpOptions options;
        options.useSystemProxy = false;
        options.cancel = &cancel;
        options.totalTimeoutMs = cancelRequest ? 8000 : 600;
        const auto url = L"https://127.0.0.1:" + std::to_wstring(ntohs(address.sin_port)) + L"/";
        auto response = std::async(std::launch::async, [&] {
            return update::WinHttpClient{}.Get(url, options);
        });
        REQUIRE(acceptedFuture.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
        const auto start = std::chrono::steady_clock::now();
        if (cancelRequest) cancel.store(true);
        REQUIRE(response.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto result = response.get();
        REQUIRE(result.status == (cancelRequest ? update::HttpStatus::Cancelled : update::HttpStatus::Timeout));
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        server.request_stop();
        server.join();
        ::closesocket(listener);
    }
    ::WSACleanup();
    return failures;
}

static int ElevatedRuntime() {
    if (!PrivilegeManager::IsElevated()) {
        std::puts("SKIP: requires an elevated token; fixture is isolated from ProgramData");
        return 77;
    }
    Fixture fixture;
    auto paths = RuntimePaths::ForOwnedRoot(fixture.root, upstream::kVersion);
    REQUIRE(securefs::BootstrapProtectedTree(paths.ProgramDataRoot(), paths.LogsDir(),
              paths.RuntimeRoot(), paths.UpdatesDir(), paths.UserDir()).ok);
    ResourceExtractor extractor(paths);
    REQUIRE(extractor.EnsureExtracted().ok);
    std::vector<RuntimeStrategy> catalog;
    std::vector<RuntimeFile> files;
    REQUIRE(extractor.VerifyRuntime(catalog, &files).ok);
    REQUIRE(catalog.size() == static_cast<std::size_t>(kGeneratedStrategyCount));
    REQUIRE(!files.empty());
    const auto bundle = diag::BuildBundle(paths, Config{});
    for (const auto& entry : bundle.entries) if (entry.name == "summary.json") {
        const auto summary = json::Parse(entry.content);
        REQUIRE(summary.ok);
        const auto* runtime = summary.value.Find("runtime");
        REQUIRE(runtime != nullptr);
        if (runtime) {
            REQUIRE(*runtime->Find("integrity")->AsString() == str::ToUtf8(L"проверена"));
            REQUIRE(*runtime->Find("strategies_available")->AsString() == std::to_string(catalog.size()));
        }
    }
    // No code is ever loaded from this file.
    REQUIRE(securefs::AtomicWrite(paths.BinDir() + L"\\wlanapi.dll", std::string("inert")).ok);
    std::wstring bad;
    REQUIRE(!extractor.EnsureExtracted().ok);
    REQUIRE(!extractor.VerifyBinaries(bad));
    REQUIRE(!extractor.VerifyRuntime(catalog).ok);
    REQUIRE(fs::remove(fs::path(paths.BinDir()) / L"wlanapi.dll"));
    // A previously untrusted directory cannot acquire trust through extraction.
    const auto other = paths.RuntimeRoot() + L"\\preplanted";
    REQUIRE(::CreateDirectoryW(other.c_str(), nullptr) != FALSE);
    REQUIRE(!securefs::EnsureTrustedDirectory(other).ok); // inherited DACL is not protected
    // Start only this harmless test executable under a winws filename, never
    // the bundled packet engine. The fixture owns every file and process.
    auto processPaths = RuntimePaths::ForOwnedRoot(fixture.root, L"2.0.0");
    REQUIRE(securefs::EnsureTrustedDirectory(processPaths.RuntimeVersionDir()).ok);
    REQUIRE(securefs::EnsureTrustedDirectory(processPaths.BinDir()).ok);
    wchar_t self[32768]{};
    REQUIRE(::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) != 0);
    REQUIRE(::CopyFileW(self, processPaths.WinwsExePath().c_str(), TRUE));
    REQUIRE(securefs::HardenObject(processPaths.WinwsExePath(), securefs::ObjectKind::File).ok);
    ProcessManager managed(processPaths);
    struct StopOnExit {
        ProcessManager& process;
        ~StopOnExit() { process.Stop(); }
    } stopGuard{managed};
    const auto started = managed.Start(processPaths.WinwsExePath(), {L"--wait"},
        processPaths.BinDir(), processPaths.LogsDir() + L"\\stdout.log",
        processPaths.LogsDir() + L"\\stderr.log", 50);
    REQUIRE(started.status == StartStatus::Ok);
    REQUIRE(CommitIntegrityRecovery(started.status == StartStatus::Ok,
        started.record.imagePath, processPaths.WinwsExePath(), paths.WinwsExePath(),
        [&] { return managed.Stop().status == StopStatus::Stopped; },
        [&] { return !probe::ProcessAlive(started.record.pid); }));
    REQUIRE(!managed.IsConnectedByUs());
    // Resuming an old session must not bypass runtime verification. Replace
    // only the inert, not-running embedded EXE with this test helper, start
    // --wait, then exercise the real Launcher adoption path.
    REQUIRE(::CopyFileW(self, paths.WinwsExePath().c_str(), FALSE));
    REQUIRE(securefs::HardenObject(paths.WinwsExePath(), securefs::ObjectKind::File).ok);
    ProcessManager resumed(paths);
    StopOnExit resumedGuard{resumed};
    const auto oldSession = resumed.Start(paths.WinwsExePath(), {L"--wait"}, paths.BinDir(),
        paths.LogsDir() + L"\\resume-out.log", paths.LogsDir() + L"\\resume-err.log", 50);
    REQUIRE(oldSession.status == StartStatus::Ok);
    ui::UiEventQueue events;
    Launcher launcher(paths, resumed);
    const auto adopted = launcher.Connect(strategies::Default(), GameFilterMode::Off, events);
    REQUIRE(!adopted.success());
    REQUIRE(!resumed.IsConnectedByUs());
    REQUIRE(!probe::ProcessAlive(oldSession.record.pid));
    REQUIRE(extractor.EnsureExtracted().ok);
    // A corrupt state authorizes no deletions even with an elevated token.
    REQUIRE(securefs::AtomicWrite(paths.ActiveRuntimePath(), std::string("{broken")).ok);
    const auto cleanup = extractor.CleanupOldVersions();
    REQUIRE(!cleanup.ok && cleanup.removed == 0);
    REQUIRE(fs::exists(paths.WinwsExePath()));
    return failures;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::wstring mode = argv[1];
    // Harmless child used by the E2E contract test. No engine is started.
    if (mode == L"--verify-install") { std::puts("CHEBURNET_INSTALL_VERIFIED"); return 0; }
    if (mode == L"--wait") { ::Sleep(INFINITE); return 0; }
    if (mode == L"inventory") return Inventory();
    if (mode == L"cleanup") return Cleanup();
    if (mode == L"recovery") return Recovery();
    if (mode == L"processstate") return ProcessState();
    if (mode == L"notifications") return Notifications();
    if (mode == L"privacy") return Privacy();
    if (mode == L"httpsmoke") {
        update::HttpOptions options;
        options.totalTimeoutMs = 15000;
        const auto response = update::WinHttpClient{}.Get(
            L"https://raw.githubusercontent.com/Jacksony100/CHEBURNET/6a371e5c825c6dbce7cf2553158ada084f2d1e87/README.md",
            options);
        REQUIRE(response.status == update::HttpStatus::Ok);
        REQUIRE(response.httpStatus == 200 && response.body.size() > 1000);
        std::printf("HTTPS smoke: HTTP %lu, bytes=%zu, status=%d\n",
            response.httpStatus, response.body.size(), static_cast<int>(response.status));
        return failures;
    }
    if (mode == L"httpcancel") return HttpCancellation();
    if (mode == L"elevated") return ElevatedRuntime();
    return 2;
}
