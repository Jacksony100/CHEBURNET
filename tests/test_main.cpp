// CHEBURNET unit tests. A tiny framework-free harness: each test returns its
// failure count; main() dispatches by name (one add_test per name in CTest).
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <charconv>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "GeneratedManifest.h"
#include "config/Config.h"
#include "config/ProgressModel.h"
#include "config/Strategies.h"
#include "console/Animation.h"
#include "console/ConsoleRenderer.h"
#include "console/Mascot.h"
#include "core/Launcher.h"
#include "core/PrivilegeManager.h"
#include "ui/Effects.h"
#include "ui/FrameBuffer.h"
#include "ui/Rng.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "update/RuntimeActivation.h"
#include "update/RuntimeStateStore.h"
#include "update/SignatureVerifier.h"
#include "update/UpdateManager.h"
#include "update/UpdateCheckService.h"
#include "update/UpdateManifest.h"
#include "update/UpdatePackage.h"
#include "update/Version.h"
#include "update/WinHttpClient.h"
#include "util/Json.h"
#include "core/IntegrityVerifier.h"
#include "core/Preflight.h"
#include "core/ProcessManager.h"
#include "core/StatusProbe.h"
#include "core/SecureFs.h"
#include "app/OperationState.h"
#include "util/CommandLine.h"
#include "util/StringUtil.h"
#include "util/Version.h"

using namespace cheburnet;

static int g_fail = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            ++g_fail;                                                    \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);        \
        }                                                                \
    } while (0)

// -------------------------------------------------------------------- args
static int test_args() {
    g_fail = 0;
    const RuntimeStrategy& s = strategies::Default();
    CHECK(s.id == "general");

    const std::wstring bin = L"C:\\rt\\bin";
    const std::wstring lists = L"C:\\rt\\lists";
    auto args = strategies::BuildArguments(s, bin, lists, GameFilterMode::Off);

    CHECK(!args.empty());
    for (auto& a : args) CHECK(a.find(L'%') == std::wstring::npos);

    auto argsGame = strategies::BuildArguments(s, bin, lists, GameFilterMode::All);
    CHECK(argsGame.size() == args.size());
    CHECK(strategies::GameFilterTcpValue(GameFilterMode::All) == L"1024-65535");
    CHECK(strategies::GameFilterUdpValue(GameFilterMode::Tcp) == L"12");

    CHECK(strategies::Exists("general"));
    CHECK(!strategies::Exists("does_not_exist"));
    // Exact upstream count and every argv are checked by the independent
    // fidelity script. Runtime code must only require a non-empty unique set.
    CHECK(!strategies::All().empty());
    std::vector<std::string> ids;
    for (const auto& strategy : strategies::All()) ids.push_back(strategy.id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    return g_fail;
}

// ---------------------------------------------------------------- quoting
static int test_quoting() {
    g_fail = 0;
    CHECK(cmdline::QuoteArgvW(L"simple") == L"simple");
    CHECK(cmdline::QuoteArgvW(L"") == L"\"\"");
    CHECK(cmdline::QuoteArgvW(L"has space") == L"\"has space\"");
    CHECK(cmdline::QuoteArgvW(L"a\\b") == L"a\\b");
    CHECK(cmdline::QuoteArgvW(L"C:\\path with space\\") == L"\"C:\\path with space\\\\\"");
    CHECK(cmdline::QuoteArgvW(L"quote\"x") == L"\"quote\\\"x\"");

    // Round-trip through CommandLineToArgvW.
    const std::wstring exe = L"C:\\Program Files\\CHEBURNET\\winws.exe";
    std::vector<std::wstring> in = {L"--wf-tcp=80,443,12", L"has space",
                                    L"C:\\rt lists\\list-general.txt", L"quote\"inside",
                                    L"trailing\\"};
    std::wstring cmd = cmdline::Build(exe, in);
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(cmd.c_str(), &argc);
    CHECK(argv != nullptr);
    if (argv) {
        CHECK(argc == static_cast<int>(in.size()) + 1);
        CHECK(std::wstring(argv[0]) == exe);
        for (int i = 0; i < static_cast<int>(in.size()) && (i + 1) < argc; ++i) {
            CHECK(std::wstring(argv[i + 1]) == in[static_cast<size_t>(i)]);
        }
        ::LocalFree(argv);
    }
    CHECK(str::ToUtf8(L"CHEBURNET Привет") ==
          "CHEBURNET \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    CHECK(str::ToUtf16("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == L"Привет");
    CHECK(str::ToUtf16(std::string("\xC0\xAF", 2)).empty()); // overlong UTF-8 rejected
    const wchar_t loneSurrogate[] = {static_cast<wchar_t>(0xD800), L'\0'};
    CHECK(str::ToUtf8(loneSurrogate).empty());
    return g_fail;
}

// ----------------------------------------------------------------- config
static int test_config() {
    g_fail = 0;
    {
        auto r = ParseConfig("{\"strategy\":\"general\",\"gameFilter\":true}");
        CHECK(r.ok && r.config.strategyId == "general" &&
              r.config.gameFilter == GameFilterMode::All);
    }
    {
        auto r = ParseConfig("{\"strategy\":\"alt2\"}");
        CHECK(r.ok && r.config.strategyId == "alt2" &&
              r.config.gameFilter == GameFilterMode::Off);
    }
    CHECK(!ParseConfig("garbage").ok);
    CHECK(!ParseConfig("{\"gameFilter\":true}").ok);            // missing strategy
    CHECK(!ParseConfig("{\"strategy\":\"bad id!\"}").ok);       // invalid chars
    CHECK(!ParseConfig("{\"strategy\":\"general\",\"gameFilter\":42}").ok);
    CHECK(!ParseConfig("{\"strategy\":\"general\",\"ui\":true}").ok);
    CHECK(!ParseConfig("{\"strategy\":\"general\",\"update\":[]}").ok);

    // Key vs value collision: a value equal to a later key name must not fool
    // the parser (regression test for the substring-key bug).
    {
        auto r = ParseConfig("{\"version\":\"strategy\",\"strategy\":\"alt3\",\"gameFilter\":false}");
        CHECK(r.ok && r.config.strategyId == "alt3");
    }
    {
        auto r = ParseConfig("{\"note\":\"gameFilter\",\"strategy\":\"general\",\"gameFilter\":true}");
        CHECK(r.ok && r.config.strategyId == "general" &&
              r.config.gameFilter == GameFilterMode::All);
    }

    CHECK(IsValidStrategyIdSyntax("general"));
    CHECK(IsValidStrategyIdSyntax("faketls_auto_alt3"));
    CHECK(!IsValidStrategyIdSyntax(""));
    CHECK(!IsValidStrategyIdSyntax("a b"));

    // Unknown-but-valid-syntax id parses, but the registry rejects it.
    auto zr = ParseConfig("{\"strategy\":\"zzz\"}");
    CHECK(zr.ok && !strategies::Exists(zr.config.strategyId));
    auto unknown = ParseConfig(
        "{\"strategy\":\"general\",\"future\":{\"nested\":true},"
        "\"ui\":{\"future_option\":1}}");
    CHECK(unknown.ok); // unknown fields are tolerated

    // Serialize -> parse round trip.
    Config c;
    c.strategyId = "faketls_auto";
    c.gameFilter = GameFilterMode::Udp;
    auto rt = ParseConfig(SerializeConfig(c));
    CHECK(rt.ok && rt.config.strategyId == "faketls_auto" &&
          rt.config.gameFilter == GameFilterMode::Udp);
    return g_fail;
}

// ----------------------------------------------------------------- sha256
static int test_sha256() {
    g_fail = 0;
    auto empty = IntegrityVerifier::Sha256Hex("", 0);
    CHECK(empty &&
          *empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    auto abc = IntegrityVerifier::Sha256Hex("abc", 3);
    CHECK(abc && *abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(IntegrityVerifier::HexEquals(
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    return g_fail;
}

// --------------------------------------------------------------- manifest
static int test_manifest() {
    g_fail = 0;
    CHECK(kEmbeddedResourceCount > 0);
    unsigned int prevId = 0;
    bool foundWinws = false, foundIpset = false;
    for (int i = 0; i < kEmbeddedResourceCount; ++i) {
        const EmbeddedResource& r = kEmbeddedResources[i];
        CHECK(r.rcId > prevId); // strictly increasing, unique
        prevId = r.rcId;
        CHECK(r.expectedSize > 0);
        CHECK(std::strlen(r.sha256) == 64);
        for (const char* p = r.sha256; *p; ++p) {
            const bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
            CHECK(hex);
        }
        std::wstring rel = r.relPath;
        CHECK(rel.rfind(L"bin\\", 0) == 0 || rel.rfind(L"lists\\", 0) == 0);
        if (std::strcmp(r.logicalName, "winws.exe") == 0) {
            foundWinws = true;
            CHECK(r.expectedSize > 0);
            CHECK(r.category == ResourceCategory::Binary);
        }
        if (std::strcmp(r.logicalName, "ipset-all.txt") == 0) {
            foundIpset = true;
            CHECK(r.category == ResourceCategory::List);
        }
    }
    CHECK(foundWinws);
    CHECK(foundIpset);
    return g_fail;
}

// ---------------------------------------------------------- secure filesystem
static int test_securefs() {
    g_fail = 0;
    wchar_t temp[MAX_PATH]{};
    CHECK(::GetTempPathW(MAX_PATH, temp) != 0);
    const std::wstring nonce = std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(::GetTickCount64());
    const std::wstring root = std::wstring(temp) + L"cheburnet-securefs-" + nonce;
    const std::wstring child = root + L"\\child";
    const std::wstring sibling = root + L"-sibling";
    CHECK(::CreateDirectoryW(root.c_str(), nullptr) != FALSE);
    CHECK(::CreateDirectoryW(child.c_str(), nullptr) != FALSE);

    CHECK(securefs::IsStrictDescendant(root, child));
    CHECK(!securefs::IsStrictDescendant(root, root));
    CHECK(!securefs::IsStrictDescendant(root, std::wstring(temp)));
    CHECK(!securefs::IsStrictDescendant(root, sibling));
    CHECK(!securefs::RemoveTreeUnder(root, root).ok);       // cleanup refuses root
    CHECK(!securefs::RemoveTreeUnder(root, std::wstring(temp)).ok); // and parent
    CHECK(!securefs::HardenObject(root + L"\\missing", securefs::ObjectKind::File).ok);

    // An existing runtime destination (including a junction/reparse object) is
    // rejected before the package is opened or any extraction is attempted.
    update::PackageInfo emptyPackage;
    CHECK(!update::ExtractPackage(root + L"\\missing.cbpkg", emptyPackage, root, child).ok);

    // A predictable legacy <target>.tmp pre-plant must remain untouched. The
    // production writer uses a CNG-random CREATE_NEW name; this operation may
    // itself fail in a non-elevated test process, which is also fail-closed.
    const std::wstring target = root + L"\\record";
    const std::wstring planted = target + L".tmp";
    HANDLE plantedFile = ::CreateFileW(planted.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(plantedFile != INVALID_HANDLE_VALUE);
    if (plantedFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        CHECK(::WriteFile(plantedFile, "sentinel", 8, &written, nullptr) != FALSE && written == 8);
        ::CloseHandle(plantedFile);
    }
    const std::string payload = "abc";
    (void)securefs::AtomicWrite(target, payload);
    const auto plantedSize = IntegrityVerifier::FileSize(planted);
    CHECK(plantedSize && *plantedSize == 8);

    // If developer mode permits an unprivileged directory symlink, verify that
    // component validation and protected directory creation both reject it.
    const std::wstring outside = std::wstring(temp) + L"cheburnet-securefs-out-" + nonce;
    const std::wstring link = root + L"\\junction";
    CHECK(::CreateDirectoryW(outside.c_str(), nullptr) != FALSE);
    constexpr DWORD kAllowUnprivilegedSymlink = 0x2;
    if (::CreateSymbolicLinkW(link.c_str(), outside.c_str(),
                              SYMBOLIC_LINK_FLAG_DIRECTORY | kAllowUnprivilegedSymlink)) {
        CHECK(!securefs::ValidatePathComponents(link).ok);
        CHECK(!securefs::EnsureProtectedDirectory(link + L"\\nested").ok);
        CHECK(::RemoveDirectoryW(link.c_str()) != FALSE);
    }

    ::DeleteFileW(target.c_str());
    ::DeleteFileW(planted.c_str());
    ::RemoveDirectoryW(child.c_str());
    ::RemoveDirectoryW(root.c_str());
    ::RemoveDirectoryW(outside.c_str());
    return g_fail;
}

// ---------------------------------------------------------- live log handle
static int test_loghandle() {
    g_fail = 0;
    wchar_t temp[MAX_PATH]{};
    CHECK(::GetTempPathW(MAX_PATH, temp) != 0);
    const std::wstring nonce = std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(::GetTickCount64());
    const std::wstring root = std::wstring(temp) + L"cheburnet-loghandle-" + nonce;
    const std::wstring path = root + L"\\cheburnet.log";

    CHECK(::CreateDirectoryW(root.c_str(), nullptr) != FALSE);
    HANDLE created = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(created != INVALID_HANDLE_VALUE);
    if (created != INVALID_HANDLE_VALUE) ::CloseHandle(created);

    HANDLE log = ::CreateFileW(
        path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    CHECK(log != INVALID_HANDLE_VALUE);
    if (log != INVALID_HANDLE_VALUE) {
        ::CloseHandle(log);
    }

    // A normal non-elevated CI token may not assign Administrators as owner.
    // When hardening is available, also exercise the production exact-ACL
    // validation through the already-open append handle.
    const auto hardenedDirectory = securefs::HardenObject(root, securefs::ObjectKind::Directory);
    const auto hardenedFile = securefs::HardenObject(path, securefs::ObjectKind::File);
    if (hardenedDirectory.ok && hardenedFile.ok) {
        HANDLE protectedLog = ::CreateFileW(
            path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        CHECK(protectedLog != INVALID_HANDLE_VALUE);
        if (protectedLog != INVALID_HANDLE_VALUE) {
            CHECK(securefs::ValidateProtectedHandle(
                      protectedLog, path, securefs::ObjectKind::File, true).ok);
            ::CloseHandle(protectedLog);
        }
        // Directory ACEs inherit into children; normal file ACEs must not carry
        // OI/CI flags. The old verifier required OI/CI for both and rejected
        // every correctly normalized protected file.
        CHECK(securefs::ValidateProtectedObject(
                  root, securefs::ObjectKind::Directory, false).ok);
        CHECK(securefs::ValidateProtectedObject(
                  path, securefs::ObjectKind::File, true).ok);
    }
    CHECK(::DeleteFileW(path.c_str()) != FALSE);
    CHECK(::RemoveDirectoryW(root.c_str()) != FALSE);
    return g_fail;
}

// ------------------------------------------------- process identity / record
static int test_process() {
    g_fail = 0;
    const unsigned long pid = ::GetCurrentProcessId();
    auto ct = probe::ProcessCreationTime(pid);
    auto img = probe::ProcessImagePath(pid);
    CHECK(ct.has_value());
    CHECK(img.has_value());
    if (!ct || !img) return g_fail;

    ProcessRecord ok{pid, *ct, *img};
    CHECK(ProcessManager::IdentityMatches(ok)); // us: matches

    ProcessRecord badTime{pid, *ct + 1, *img};
    CHECK(!ProcessManager::IdentityMatches(badTime)); // pid reuse guard

    ProcessRecord badImage{pid, *ct, L"C:\\nope\\winws.exe"};
    CHECK(!ProcessManager::IdentityMatches(badImage)); // foreign image guard

    ProcessRecord deadPid{0x7FFFFFF0ul, *ct, *img};
    CHECK(!ProcessManager::IdentityMatches(deadPid)); // not alive

    ProcessRecord empty{};
    CHECK(!ProcessManager::IdentityMatches(empty));
    CHECK((!ProcessRecord{pid, 0, *img}.valid()));
    CHECK((!ProcessRecord{pid, *ct, L""}.valid()));

    // Record image must be a winws.exe under our runtime root (anti-forged-record),
    // version-agnostic so an upgrade can still manage an old-version process.
    const std::wstring root = L"C:\\ProgramData\\CHEBURNET\\runtime";
    CHECK(ProcessManager::TrustedPidForExclusion(badTime, root) == 0);
    CHECK(ProcessManager::TrustedPidForExclusion(badImage, root) == 0);
    CHECK(ProcessManager::IsExpectedWinwsImage(
        L"C:\\ProgramData\\CHEBURNET\\runtime\\1.0.0\\bin\\winws.exe", root));
    CHECK(ProcessManager::IsExpectedWinwsImage(
        L"c:\\programdata\\cheburnet\\runtime\\0.9.0\\bin\\WINWS.EXE", root)); // other version, case-insensitive
    CHECK(!ProcessManager::IsExpectedWinwsImage(L"C:\\Windows\\System32\\lsass.exe", root));
    CHECK(!ProcessManager::IsExpectedWinwsImage(
        L"C:\\ProgramData\\CHEBURNET\\runtime\\1.0.0\\bin\\evil.exe", root)); // not winws
    CHECK(!ProcessManager::IsExpectedWinwsImage(L"C:\\Temp\\bin\\winws.exe", root)); // outside root
    CHECK(!ProcessManager::IsExpectedWinwsImage(L"", root));

    // Record persistence round trip. GitHub-hosted Windows runners may expose
    // an elevated token, while local CI normally does not. Never try to turn
    // the shared system TEMP directory itself into a protected CHEBURNET
    // directory: create and harden a unique child first.
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring recordRoot = std::wstring(tmp) + L"cheburnet-process-record-" +
                                    std::to_wstring(pid) + L"-" +
                                    std::to_wstring(::GetTickCount64());
    const std::wstring path = recordRoot + L"\\winws-run.txt";
    CHECK(::CreateDirectoryW(recordRoot.c_str(), nullptr) != FALSE);
    // Protected record writes require elevation. Under the normal non-elevated
    // unit-test runner they must fail closed; an elevated run verifies roundtrip.
    if (PrivilegeManager::IsElevated()) {
        const auto protectedRoot = securefs::EnsureProtectedDirectory(recordRoot);
        CHECK(protectedRoot.ok);
        if (protectedRoot.ok) {
            CHECK(ProcessManager::SaveRecord(path, ok));
            ProcessRecord loaded = ProcessManager::LoadRecord(path);
            CHECK(loaded.pid == ok.pid);
            CHECK(loaded.creationTime == ok.creationTime);
            CHECK(loaded.imagePath == ok.imagePath);
        }
    } else {
        CHECK(!ProcessManager::SaveRecord(path, ok));
    }
    ProcessRecord injected = ok;
    injected.imagePath += L"\nforged=1";
    CHECK(!ProcessManager::SaveRecord(path, injected));
    ProcessManager::DeleteRecord(path);
    CHECK(::RemoveDirectoryW(recordRoot.c_str()) != FALSE);
    return g_fail;
}

// ------------------------------------------------ resource integrity checks
static int test_integrity() {
    g_fail = 0;
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"cheburnet_test_blob.bin";

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ::WriteFile(h, "abc", 3, &wr, nullptr);
        ::CloseHandle(h);
    }

    auto sz = IntegrityVerifier::FileSize(path);
    CHECK(sz && *sz == 3);
    auto sha = IntegrityVerifier::Sha256File(path);
    const char* expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(sha && IntegrityVerifier::HexEquals(*sha, expected));          // valid file
    CHECK(sha && !IntegrityVerifier::HexEquals(*sha, "deadbeef"));       // corrupt detected
    CHECK(update::ValidateArtifactFile(path, 3, expected));
    CHECK(!update::ValidateArtifactFile(path, 4, expected));             // size mismatch
    CHECK(!update::ValidateArtifactFile(path, 3, std::string(64, '0'))); // hash mismatch

    const std::wstring hardLink = std::wstring(tmp) + L"cheburnet_test_blob_link.bin";
    ::DeleteFileW(hardLink.c_str());
    if (::CreateHardLinkW(hardLink.c_str(), path.c_str(), nullptr)) {
        CHECK(!IntegrityVerifier::FileSize(path).has_value());
        CHECK(!IntegrityVerifier::Sha256File(path).has_value());
        CHECK(!securefs::ValidateObject(path, securefs::ObjectKind::File, true).ok);
        ::DeleteFileW(hardLink.c_str());
    }

    ::DeleteFileW(path.c_str());
    auto missing = IntegrityVerifier::Sha256File(path);
    CHECK(!missing.has_value());                                        // missing detected
    CHECK(!IntegrityVerifier::FileSize(path).has_value());
    return g_fail;
}

// --------------------------------------------------------- preflight (UAC)
static int test_preflight() {
    g_fail = 0;
    CHECK(EvaluatePreflight(true, true, true) == PreflightResult::Ok);
    CHECK(EvaluatePreflight(false, true, true) == PreflightResult::NeedsElevation);
    CHECK(EvaluatePreflight(true, false, true) == PreflightResult::UnsupportedOs);
    CHECK(EvaluatePreflight(true, true, false) == PreflightResult::UnsupportedOs);
    CHECK(EvaluatePreflight(false, false, true) == PreflightResult::UnsupportedOs); // OS first
    return g_fail;
}

// ------------------------------------------------ animation stops on error
static int test_animation() {
    g_fail = 0;
    ConsoleRenderer con;
    con.Init(); // may be a no-op under a redirected test console; that's fine

    ProgressModel pm;
    pm.Reach(Stage::Start);
    pm.Reach(Stage::OsCheck);

    ConnectionAnimation anim(con, pm);
    anim.Start(0, 0, 20);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(!anim.Finished()); // still running while Running

    pm.MarkError(L"boom");

    bool finished = false;
    for (int i = 0; i < 40 && !finished; ++i) { // up to ~2s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        finished = anim.Finished();
    }
    CHECK(finished); // loop exited on its own because of the Error state
    CHECK(pm.State() == RunState::Error);
    CHECK(pm.Display() < 100);
    return g_fail;
}

// ---------------------------------------------- progress model invariants
static int test_progress() {
    g_fail = 0;
    {
        ProgressModel pm;
        CHECK(pm.State() == RunState::Running);
        CHECK(pm.Floor() == 0);
        CHECK(pm.Ceiling() < 100);
        const Stage seq[] = {Stage::Start,        Stage::OsCheck,      Stage::AdminCheck,
                             Stage::RuntimePrep,  Stage::ExtractVerify, Stage::ConflictCheck,
                             Stage::ArgsPrep,     Stage::LaunchProc,   Stage::ProcAlive};
        for (Stage s : seq) {
            pm.Reach(s);
            for (int k = 0; k < 200; ++k) CHECK(pm.Tick(1000) < 100);
            CHECK(pm.Ceiling() < 100);
            CHECK(pm.Display() < 100);
            CHECK(pm.State() == RunState::Running);
        }
        // At ProcAlive (90) the ceiling is 99 until Confirmed.
        CHECK(pm.Ceiling() == 99);
    }
    {
        ProgressModel pm; // error freezes below 100
        pm.Reach(Stage::Start);
        pm.Reach(Stage::ExtractVerify);
        pm.MarkError(L"x");
        CHECK(pm.State() == RunState::Error);
        CHECK(pm.Ceiling() == pm.Floor());
        CHECK(pm.Floor() == 50);
        for (int k = 0; k < 200; ++k) CHECK(pm.Tick(1000) < 100);
    }
    {
        ProgressModel pm; // only Confirmed yields 100 / Success
        pm.Reach(Stage::ProcAlive);
        CHECK(pm.Display() < 100);
        pm.Reach(Stage::Confirmed);
        CHECK(pm.State() == RunState::Success);
        CHECK(pm.Ceiling() == 100);
        CHECK(pm.Display() == 100);
        // Success must not be downgraded by a late error.
        pm.MarkError(L"late");
        CHECK(pm.State() == RunState::Success);
        CHECK(pm.Display() == 100);
    }
    return g_fail;
}

// -------------------------------------------------------------------- mascot
static int test_mascot() {
    g_fail = 0;
    const auto& full = MascotFull();
    CHECK(!full.lines.empty());
    CHECK(full.starMarker == static_cast<wchar_t>(0x2605)); // red star glyph

    int stars = 0, maxWidth = 0;
    for (const auto& line : full.lines) {
        for (wchar_t c : line)
            if (c == full.starMarker) ++stars;
        if (static_cast<int>(line.size()) > maxWidth) maxWidth = static_cast<int>(line.size());
    }
    CHECK(stars == 1);              // exactly one red star cell
    CHECK(full.width == maxWidth);  // declared width matches the art

    const auto& compact = MascotCompact();
    CHECK(!compact.lines.empty());
    int cstars = 0;
    for (const auto& line : compact.lines)
        for (wchar_t c : line)
            if (c == compact.starMarker) ++cstars;
    CHECK(cstars == 1);

    // A wide window selects the full mascot; a narrow one the compact fallback.
    CHECK(&MascotForWidth(full.width + 10) == &full);
    CHECK(&MascotForWidth(20) == &compact);
    return g_fail;
}

// -------------------------------------------------------------------- layout
static int test_layout() {
    g_fail = 0;
    using namespace cheburnet::ui;
    CHECK(LayoutForSize(100, 40) == LayoutMode::Full);
    CHECK(LayoutForSize(80, 26) == LayoutMode::Compact);
    CHECK(LayoutForSize(60, 20) == LayoutMode::Minimal);
    CHECK(LayoutForSize(92, 30) == LayoutMode::Full);   // boundary
    CHECK(LayoutForSize(91, 30) == LayoutMode::Compact); // just under
    CHECK(SizeTooSmall(50, 10));
    CHECK(SizeTooSmall(100, 14));
    CHECK(!SizeTooSmall(100, 40));
    return g_fail;
}

// --------------------------------------------------------------- checkpoints
static int test_checkpoints() {
    g_fail = 0;
    const auto& cps = ConnectCheckpoints();
    CHECK(cps.size() == 20);
    int prevPct = -1, prevPhase = 1, hundred = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        CHECK(cps[i].index == static_cast<int>(i) + 1);
        CHECK(cps[i].percent >= prevPct); // monotonic non-decreasing
        CHECK(cps[i].phase >= prevPhase); // phases in order
        CHECK(cps[i].phase >= 1 && cps[i].phase <= 5);
        if (cps[i].percent == 100) ++hundred;
        prevPct = cps[i].percent;
        prevPhase = cps[i].phase;
    }
    CHECK(hundred == 1);                     // exactly one 100%
    CHECK(cps.back().percent == 100);        // and it is the final checkpoint
    CHECK(cps[16].percent < 100);            // stability (cp17) still below 100
    return g_fail;
}

// ---------------------------------------------------------------------- menu
static int test_menu() {
    g_fail = 0;
    using namespace cheburnet::ui;
    Menu m;
    m.SetItems({{L"A", L"", true, L'a'},
                {L"B", L"", false, L'b'}, // disabled
                {L"C", L"", true, L'c'}});
    CHECK(m.Selected() == 0);
    m.MoveDown();
    CHECK(m.Selected() == 2); // skipped disabled B
    m.MoveDown();
    CHECK(m.Selected() == 0); // wrapped
    m.MoveUp();
    CHECK(m.Selected() == 2); // wrapped back, skipping B
    CHECK(m.IndexForHotkey(L'c') == 2);
    CHECK(m.IndexForHotkey(L'C') == 2); // case-insensitive
    CHECK(m.IndexForHotkey(L'b') == -1); // disabled hotkey ignored
    m.SetSelected(1);
    CHECK(m.Selected() != 1); // clamps off the disabled item
    return g_fail;
}

// -------------------------------------------------------------------- effects
static int test_effects() {
    g_fail = 0;
    using namespace cheburnet::ui;
    Rng rng;

    // Glitch recovers exactly once the active window passes.
    const std::wstring base = L"HELLO WORLD";
    CHECK(Glitch::Apply(base, rng, 3, 3, 4) == base); // frame == activeFrames
    CHECK(Glitch::Apply(base, rng, 9, 3, 4) == base); // beyond
    for (int f = 0; f < 3; ++f) {
        std::wstring g = Glitch::Apply(base, rng, f, 3, 4);
        CHECK(g.size() == base.size());  // never changes length
        CHECK(g[5] == L' ');             // whitespace preserved (layout intact)
        for (wchar_t c : g) CHECK(!(c >= 0xD800 && c <= 0xDFFF)); // no surrogate
    }

    // Blink half-on/half-off.
    CHECK(Blink::On(0, 400));
    CHECK(Blink::On(100, 400));
    CHECK(!Blink::On(250, 400));

    // Scanline reveal.
    CHECK(Scanline::Revealed(10, 0, 30, false) == 0);
    CHECK(Scanline::Revealed(10, 90, 30, false) == 3);
    CHECK(Scanline::Revealed(10, 5000, 30, false) == 10); // capped
    CHECK(Scanline::Revealed(10, 0, 30, true) == 10);      // instant

    // Typewriter (1000 cps -> 1 char/ms).
    Typewriter tw(L"ABCDE", 1000);
    CHECK(tw.Visible(0, false).size() == 0);
    CHECK(tw.Visible(3, false).size() == 3);
    CHECK(tw.Visible(99, false).size() == 5); // capped
    CHECK(tw.Visible(0, true).size() == 5);   // instant
    CHECK(tw.Done(5));
    return g_fail;
}

// --------------------------------------------------------------------- theme
static int test_theme() {
    g_fail = 0;
    using namespace cheburnet::ui;
    // Footer branding.
    CHECK(BrandingLine(LayoutMode::Full) == L"CHEBURNET LABS // РАЗРАБОТАНО MARSHAL JACKSONY100");
    CHECK(BrandingLine(LayoutMode::Compact) == L"CHEBURNET LABS // РАЗРАБОТАНО MARSHAL JACKSONY100");
    CHECK(BrandingLine(LayoutMode::Minimal) == L"АВТОР: MARSHAL JACKSONY100");

    // ASCII fallback vs unicode glyphs.
    CHECK(UnicodeGlyphs().h == L'─');
    CHECK(AsciiGlyphs().h == L'-');
    CHECK(AsciiGlyphs().barFull == L'#');
    CHECK(AsciiGlyphs().barEmpty == L'-');
    CHECK(static_cast<unsigned>(AsciiGlyphs().tl) < 128u); // ASCII stays 7-bit

    // Reduced motion disables effects.
    UiOptions o;
    o.reducedMotion = true;
    CHECK(!o.effGlitch());
    CHECK(!o.effTypewriter());
    CHECK(!o.effMascotIdle());
    UiOptions off;
    off.animations = false;
    CHECK(!off.effGlitch() && !off.effTypewriter() && !off.effBlink());
    UiOptions fast;
    fast.speed = AnimSpeed::Fast;
    CHECK(fast.scaleMs(100) == 50);
    UiOptions slow;
    slow.speed = AnimSpeed::Slow;
    CHECK(slow.scaleMs(100) == 150);

    // Colour attributes.
    Theme th(o);
    CHECK((th.Attr(UiColor::Selected) & BACKGROUND_GREEN) != 0);
    CHECK((th.Attr(UiColor::Error) & (FOREGROUND_RED | FOREGROUND_INTENSITY)) ==
          (FOREGROUND_RED | FOREGROUND_INTENSITY));
    CHECK(th.Attr(UiColor::Star) == th.Attr(UiColor::Error));
    UiOptions ascii;
    ascii.asciiOnly = true;
    Theme tha(ascii);
    CHECK(tha.G().h == L'-'); // theme honours ascii mode
    return g_fail;
}

// ---------------------------------------------------------------- framebuffer
static int test_framebuffer() {
    g_fail = 0;
    using namespace cheburnet::ui;
    FrameBuffer fb;
    fb.Resize(20, 6);
    CHECK(fb.Width() == 20 && fb.Height() == 6);
    fb.Clear(7);
    CHECK(fb.At(0, 0).Char.UnicodeChar == L' ');
    CHECK(fb.At(0, 0).Attributes == 7);
    fb.PutText(2, 1, L"AB", 3);
    CHECK(fb.At(2, 1).Char.UnicodeChar == L'A');
    CHECK(fb.At(3, 1).Char.UnicodeChar == L'B');
    CHECK(fb.At(3, 1).Attributes == 3);
    // Out-of-bounds writes are clipped, not crashes.
    fb.PutText(19, 2, L"XYZ", 3);
    CHECK(fb.At(19, 2).Char.UnicodeChar == L'X');
    fb.PutText(-3, 0, L"Q", 3); // negative x clipped
    UnicodeGlyphs();
    Glyphs g = UnicodeGlyphs();
    fb.Box(0, 0, 20, 6, 1, g);
    CHECK(fb.At(0, 0).Char.UnicodeChar == g.tl);
    CHECK(fb.At(19, 5).Char.UnicodeChar == g.br);
    // Present with no real console handle returns 0 rows and does not crash.
    CHECK(fb.Present(nullptr) == 0);
    return g_fail;
}

// ------------------------------------------------------------ config migration
static int test_configmigration() {
    g_fail = 0;
    // Old config (no ui block) loads with UI defaults.
    auto oldc = ParseConfig("{\"strategy\":\"alt2\",\"gameFilter\":true}");
    CHECK(oldc.ok);
    CHECK(oldc.config.strategyId == "alt2" && oldc.config.gameFilter == GameFilterMode::All);
    CHECK(oldc.config.ui.animations);              // default
    CHECK(!oldc.config.ui.reducedMotion);          // default
    CHECK(oldc.config.ui.speed == "normal");       // default
    CHECK(oldc.config.ui.unicodeMode == "auto");   // default

    // New config with ui block parses fully.
    auto newc = ParseConfig(
        "{\"strategy\":\"general\",\"gameFilter\":false,\"ui\":{\"animations\":false,"
        "\"animation_speed\":\"fast\",\"reduced_motion\":true,\"glitch_effects\":false,"
        "\"mascot_idle\":false,\"unicode_mode\":\"ascii\"}}");
    CHECK(newc.ok);
    CHECK(!newc.config.ui.animations && newc.config.ui.reducedMotion);
    CHECK(newc.config.ui.speed == "fast" && newc.config.ui.unicodeMode == "ascii");
    CHECK(!newc.config.ui.glitch && !newc.config.ui.mascotIdle);

    // Round-trip: serialize then parse preserves ui settings.
    auto rt = ParseConfig(SerializeConfig(newc.config));
    CHECK(rt.ok);
    CHECK(rt.config.ui.speed == "fast" && rt.config.ui.unicodeMode == "ascii");
    CHECK(!rt.config.ui.animations && rt.config.ui.reducedMotion);
    return g_fail;
}

// ------------------------------------------------------------- strict JSON
static int test_jsonstrict() {
    g_fail = 0;
    auto nested = json::Parse(
        "{\"strategy\":\"general\",\"nested\":{\"strategy\":\"evil\"},\"a\":[true,null,3]}");
    CHECK(nested.ok);
    CHECK(nested.value.Find("strategy") &&
          *nested.value.Find("strategy")->AsString() == "general");
    CHECK(!json::Parse("{\"a\":1,\"a\":2}").ok); // duplicate keys
    CHECK(!json::Parse("{\"a\":true} trailing").ok);
    CHECK(!json::Parse("{\"a\":\"\\uD800\"}").ok); // lone surrogate
    json::ParseOptions small;
    small.maxBytes = 4;
    CHECK(!json::Parse("{\"a\":1}", small).ok);

    // P2-08: nested values must never override a top-level config member.
    auto cfg = ParseConfig(
        "{\"strategy\":\"general\",\"nested\":{\"strategy\":\"alt5\","
        "\"gameFilter\":\"all\"},\"gameFilter\":\"tcp\"}");
    CHECK(cfg.ok && cfg.config.strategyId == "general");
    CHECK(cfg.config.gameFilter == GameFilterMode::Tcp);
    return g_fail;
}

// ---------------------------------------------------------- updater manifest
static std::string ValidManifest() {
    return "{\"schema\":1,\"channel\":\"stable\",\"generated_at\":"
           "\"2026-08-13T00:00:00Z\",\"launcher\":{\"version\":\"1.1.0\","
           "\"url\":\"https://example.com/CHEBURNET.exe\",\"sha256\":\"" +
           std::string(64, 'a') + "\",\"size\":123,\"minimum_supported_version\":"
           "\"1.0.0\"},\"payload\":{\"provider\":\"Flowseal/zapret-discord-youtube\","
           "\"version\":\"1.10.2\",\"url\":\"https://example.com/payload.cbp\","
           "\"sha256\":\"" + std::string(64, 'b') + "\",\"size\":456,"
           "\"minimum_launcher_version\":\"1.0.0\",\"payload_schema\":1,"
           "\"strategy_schema\":1,\"upstream_release_url\":"
           "\"https://github.com/Flowseal/zapret-discord-youtube/releases/tag/1.10.2\"},"
           "\"key_id\":\"cheburnet-release-2026\"}";
}

static int test_updatemanifest() {
    g_fail = 0;
    auto valid = update::ParseManifest(ValidManifest());
    CHECK(valid.ok);
    CHECK(update::EvaluatePayload(valid.manifest, "1.10.1", "1.0.0", true) ==
          update::Eligibility::Upgrade);
    CHECK(update::EvaluatePayload(valid.manifest, "1.10.2", "1.0.0", true) ==
          update::Eligibility::Current);
    CHECK(update::EvaluatePayload(valid.manifest, "1.10.3", "1.0.0", true) ==
          update::Eligibility::DowngradeRejected);
    CHECK(!update::ParseManifest("not json").ok);
    CHECK(!update::ParseManifest(ValidManifest(), 32).ok); // oversized
    std::string schema = ValidManifest();
    schema.replace(schema.find("\"schema\":1"), 10, "\"schema\":2");
    CHECK(!update::ParseManifest(schema).ok);
    std::string http = ValidManifest();
    http.replace(http.find("https://example.com/payload.cbp"), 5, "http");
    CHECK(!update::ParseManifest(http).ok);
    std::string badDate = ValidManifest();
    badDate.replace(badDate.find("2026-08-13T00:00:00Z"), 20, "2026-99-13T00:00:00Z");
    CHECK(!update::ParseManifest(badDate).ok);
    std::string badKey = ValidManifest();
    badKey.replace(badKey.find("cheburnet-release-2026"), 22, "CHEBURNET_RELEASE_KEY");
    CHECK(!update::ParseManifest(badKey).ok);

    auto old = valid.manifest;
    old.payload.minimumLauncherVersion = "2.0.0";
    CHECK(update::EvaluatePayload(old, "1.10.1", "1.0.0", true) ==
          update::Eligibility::LauncherTooOld);
    old = valid.manifest;
    old.payload.payloadSchema = 2;
    CHECK(update::EvaluatePayload(old, "1.10.1", "1.0.0", true) ==
          update::Eligibility::SchemaUnsupported);
    old = valid.manifest;
    old.payload.version = "1.10.2-rc1";
    CHECK(update::EvaluatePayload(old, "1.10.1", "1.0.0", true) ==
          update::Eligibility::PrereleaseRejected);
    old = valid.manifest;
    old.launcher.minimumSupportedVersion = "2.0.0";
    CHECK(update::EvaluateLauncher(old, "1.0.0", true) ==
          update::Eligibility::LauncherTooOld);

    // ---- семантика RC/stable (AC-01) ---------------------------------------
    auto rc = valid.manifest;
    rc.launcher.minimumSupportedVersion = "1.0.0-rc.1";
    rc.launcher.version = "1.0.0-rc.2";
    // RC -> более новый RC: апгрейд только для сборки канала prerelease.
    CHECK(update::EvaluateLauncher(rc, "1.0.0-rc.1", false) == update::Eligibility::Upgrade);
    CHECK(update::EvaluateLauncher(rc, "1.0.0-rc.1", true) ==
          update::Eligibility::PrereleaseRejected);
    // RC -> stable обязан быть апгрейдом на любом канале. Это и есть дефект,
    // из-за которого стабильный 1.0.0 считался «текущим» для 1.0.0-rc.3.
    rc.launcher.version = "1.0.0";
    CHECK(update::EvaluateLauncher(rc, "1.0.0-rc.3", true) == update::Eligibility::Upgrade);
    CHECK(update::EvaluateLauncher(rc, "1.0.0-rc.3", false) == update::Eligibility::Upgrade);
    CHECK(update::EvaluateLauncher(rc, "1.0.0", true) == update::Eligibility::Current);
    // stable -> RC того же ядра остаётся понижением даже на канале prerelease.
    rc.launcher.version = "1.0.0-rc.4";
    CHECK(update::EvaluateLauncher(rc, "1.0.0", false) ==
          update::Eligibility::DowngradeRejected);
    CHECK(update::EvaluateLauncher(rc, "1.0.0", true) ==
          update::Eligibility::PrereleaseRejected);
    rc.launcher.version = "1.0.1";
    CHECK(update::EvaluateLauncher(rc, "1.0.0", true) == update::Eligibility::Upgrade);
    rc.launcher.version = "1.0.0";
    CHECK(update::EvaluateLauncher(rc, "1.0.1", true) ==
          update::Eligibility::DowngradeRejected);
    rc.launcher.version = "1.1.0-rc.1";
    CHECK(update::EvaluateLauncher(rc, "1.0.0", true) ==
          update::Eligibility::PrereleaseRejected);

    // Порог minimum_supported_version стабильного выпуска обязан включать RC
    // той же базовой версии, иначе установленный RC отсекается как слишком старый.
    auto floorCase = valid.manifest;
    floorCase.launcher.version = "1.0.0";
    floorCase.launcher.minimumSupportedVersion = "1.0.0";
    CHECK(update::EvaluateLauncher(floorCase, "1.0.0-rc.3", true) ==
          update::Eligibility::LauncherTooOld);
    floorCase.launcher.minimumSupportedVersion = "1.0.0-rc.1";
    CHECK(update::EvaluateLauncher(floorCase, "1.0.0-rc.3", true) ==
          update::Eligibility::Upgrade);

    // То же для порога полезной нагрузки.
    auto payloadFloor = valid.manifest;
    payloadFloor.payload.minimumLauncherVersion = "1.0.0-rc.1";
    CHECK(update::EvaluatePayload(payloadFloor, "1.10.1", "1.0.0-rc.3", true) ==
          update::Eligibility::Upgrade);
    payloadFloor.payload.minimumLauncherVersion = "1.0.0";
    CHECK(update::EvaluatePayload(payloadFloor, "1.10.1", "1.0.0-rc.3", true) ==
          update::Eligibility::LauncherTooOld);

    // Ревизия upstream (1.10.2a) — более новый выпуск, а не понижение.
    auto revision = valid.manifest;
    revision.payload.version = "1.10.2a";
    CHECK(update::EvaluatePayload(revision, "1.10.2", "1.0.0", true) ==
          update::Eligibility::Upgrade);
    CHECK(update::EvaluatePayload(revision, "1.10.2a", "1.0.0", true) ==
          update::Eligibility::Current);
    CHECK(update::EvaluatePayload(revision, "1.10.2b", "1.0.0", true) ==
          update::Eligibility::DowngradeRejected);
    return g_fail;
}

// ----------------------------------------------------------- version ordering
static std::string ReadWholeFile(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read != 0) {
        bytes.append(buffer, read);
    }
    ::CloseHandle(file);
    return bytes;
}

static int test_updateversion() {
    g_fail = 0;

    // Сборка обязана сообщать о себе семантическую, а не только числовую
    // версию: именно по ней обновление отличает RC от стабильного выпуска.
    const auto selfVersion = update::ParseVersion(CHEBURNET_VERSION_STR);
    CHECK(selfVersion.has_value());
#if CHEBURNET_VERSION_IS_PRERELEASE
    CHECK(selfVersion && selfVersion->prerelease);
    CHECK(CHEBURNET_STABLE_CHANNEL == 0);
    CHECK(std::strcmp(CHEBURNET_VERSION_STR, CHEBURNET_VERSION_CORE_STR) != 0);
    {   // RC -> stable обязателен как Upgrade; обратное — как понижение.
        const auto stable = update::ParseVersion(CHEBURNET_VERSION_CORE_STR);
        CHECK(stable && selfVersion && update::CompareVersions(*selfVersion, *stable) < 0);
    }
#else
    CHECK(selfVersion && !selfVersion->prerelease);
    CHECK(CHEBURNET_STABLE_CHANNEL == 1);
    CHECK(std::strcmp(CHEBURNET_VERSION_STR, CHEBURNET_VERSION_CORE_STR) == 0);
#endif
    // PE-версия остаётся чисто числовой (формат PE не выражает prerelease).
    {
        const std::string pe = CHEBURNET_VERSION_PE_STR;
        int dots = 0;
        bool numeric = !pe.empty();
        for (const char c : pe) {
            if (c == '.') ++dots;
            else if (c < '0' || c > '9') numeric = false;
        }
        CHECK(numeric && dots == 3);
    }

    // Каноническая таблица порядка: те же случаи проверяет scripts/version.ps1
    // через тест version_model, поэтому обе реализации не могут разойтись.
    const std::string table = ReadWholeFile(CHEBURNET_VERSION_CASES);
    CHECK(!table.empty());
    json::ParseOptions options;
    options.maxBytes = 64 * 1024;
    options.maxValues = 4096;
    const json::ParseResult parsed = json::Parse(table, options);
    CHECK(parsed.ok);
    const json::Value* cases = parsed.ok ? parsed.value.Find("cases") : nullptr;
    const json::Value::Array* caseArray = cases ? cases->AsArray() : nullptr;
    CHECK(caseArray && caseArray->size() >= 20);
    if (caseArray) {
        for (const json::Value& entry : *caseArray) {
            const json::Value* left = entry.Find("left");
            const json::Value* right = entry.Find("right");
            const json::Value* expected = entry.Find("expected");
            const std::string* leftText = left ? left->AsString() : nullptr;
            const std::string* rightText = right ? right->AsString() : nullptr;
            const json::Number* expectedNumber = expected ? expected->AsNumber() : nullptr;
            const auto expectedValue = expectedNumber ? expectedNumber->AsInt64() : std::nullopt;
            CHECK(leftText && rightText && expectedValue);
            if (!leftText || !rightText || !expectedValue) continue;
            const auto l = update::ParseVersion(*leftText);
            const auto r = update::ParseVersion(*rightText);
            if (!l || !r) {
                ++g_fail;
                std::printf("  FAIL unparsable ordering case: %s vs %s\n",
                            leftText->c_str(), rightText->c_str());
                continue;
            }
            const int forward = update::CompareVersions(*l, *r);
            const int reverse = update::CompareVersions(*r, *l);
            const int forwardSign = forward < 0 ? -1 : (forward > 0 ? 1 : 0);
            if (forwardSign != static_cast<int>(*expectedValue) || reverse != -forward) {
                ++g_fail;
                std::printf("  FAIL ordering %s vs %s: got %d expected %lld (reverse %d)\n",
                            leftText->c_str(), rightText->c_str(), forwardSign,
                            static_cast<long long>(*expectedValue), reverse);
            }
        }
    }
    const json::Value* invalid = parsed.ok ? parsed.value.Find("invalid") : nullptr;
    const json::Value::Array* invalidArray = invalid ? invalid->AsArray() : nullptr;
    CHECK(invalidArray && invalidArray->size() >= 10);
    if (invalidArray) {
        for (const json::Value& entry : *invalidArray) {
            const std::string* text = entry.AsString();
            CHECK(text != nullptr);
            if (text && update::ParseVersion(*text)) {
                ++g_fail;
                std::printf("  FAIL accepted invalid version: '%s'\n", text->c_str());
            }
        }
    }
    return g_fail;
}

// ------------------------------------------------ serialized app operations
static int test_operationstate() {
    g_fail = 0;
    OperationState state;
    CHECK(state.Get() == AppOperationState::Disconnected);
    CHECK(state.TryTransition(AppOperationState::Disconnected,
                              AppOperationState::Connecting));
    CHECK(!state.TryTransition(AppOperationState::Disconnected,
                               AppOperationState::Updating));
    state.Complete(AppOperationState::Disconnected);
    std::atomic<int> winners{0};
    std::thread connect([&] {
        if (state.TryTransition(AppOperationState::Disconnected,
                                AppOperationState::Connecting)) ++winners;
    });
    std::thread update([&] {
        if (state.TryTransition(AppOperationState::Disconnected,
                                AppOperationState::Updating)) ++winners;
    });
    connect.join();
    update.join();
    CHECK(winners.load() == 1);
    return g_fail;
}

// --------------------------------------------------------- ECDSA trust store
static int test_updatesignature() {
    g_fail = 0;
    const std::string manifest = ValidManifest();
    const std::string signature =
        "SEWze18PnyBbvQ2urkTQNBoIMr8odGfEOkH+SKAlyoYdMRU+vjPYBddUC3odWVTc0crAyIjMDdvHT+8LeQLEWw==";
    CHECK(update::VerifyManifestSignature(manifest, signature, "cheburnet-release-2026") ==
          update::SignatureStatus::Verified);
    CHECK(update::VerifyManifestSignature(manifest + " ", signature, "cheburnet-release-2026") ==
          update::SignatureStatus::InvalidSignature);
    CHECK(update::VerifyManifestSignature(manifest, signature, "wrong-key") ==
          update::SignatureStatus::UnknownKey);
    CHECK(update::VerifyManifestSignature(manifest, "bad base64", "cheburnet-release-2026") ==
          update::SignatureStatus::InvalidEncoding);
    unsigned char wrongX[32]{};
    unsigned char wrongY[32]{};
    CHECK(update::VerifyEcdsaP256(manifest, signature, wrongX, wrongY) !=
          update::SignatureStatus::Verified);
    return g_fail;
}

// ------------------------------------------------------------- package paths
static update::PackageEntry Entry(std::string path, std::uint64_t size = 1,
                                  update::PackageEntryType type = update::PackageEntryType::Regular) {
    return {std::move(path), type, size, std::string(64, 'a')};
}

static std::vector<update::PackageEntry> BasePackage() {
    return {Entry("bin/winws.exe"), Entry("strategies/catalog.json"),
            Entry("provenance.json"), Entry("runtime-manifest.json")};
}

static int test_updatepackage() {
    g_fail = 0;
    std::string normalized;
    CHECK(update::NormalizePackagePath("bin/winws.exe", normalized));
    CHECK(!update::NormalizePackagePath("../evil", normalized));
    CHECK(!update::NormalizePackagePath("/absolute", normalized));
    CHECK(!update::NormalizePackagePath("C:/evil", normalized));
    CHECK(!update::NormalizePackagePath("//server/share", normalized));
    CHECK(!update::NormalizePackagePath("bin/file:ads", normalized));
    CHECK(!update::NormalizePackagePath("bin/CON.txt", normalized));
    CHECK(!update::NormalizePackagePath("lists/LPT1", normalized));
    CHECK(!update::NormalizePackagePath("strategies/extra.json", normalized));
    CHECK(!update::NormalizePackagePath("other/file", normalized));
    CHECK(update::ValidatePackageEntries(BasePackage()).ok);

    auto duplicate = BasePackage(); duplicate.push_back(Entry("BIN/WINWS.EXE"));
    CHECK(!update::ValidatePackageEntries(duplicate).ok);
    auto symlink = BasePackage(); symlink[0].type = update::PackageEntryType::Symlink;
    CHECK(!update::ValidatePackageEntries(symlink).ok);
    auto reparse = BasePackage(); reparse[0].type = update::PackageEntryType::Reparse;
    CHECK(!update::ValidatePackageEntries(reparse).ok);
    auto tooLarge = BasePackage(); tooLarge[0].size = 33ull * 1024ull * 1024ull;
    CHECK(!update::ValidatePackageEntries(tooLarge).ok);
    update::PackageLimits tiny; tiny.maxFiles = 2;
    CHECK(!update::ValidatePackageEntries(BasePackage(), tiny).ok);

    // CMake/CI builds a real package fixture with the release script. Exercise
    // the binary parser, per-entry metadata and trailing/truncation rejection.
#ifdef CHEBURNET_TEST_PACKAGE
    const std::wstring fixture = CHEBURNET_TEST_PACKAGE;
    auto parsed = update::ParsePackageFile(fixture);
    CHECK(parsed.ok);
    CHECK(update::ParseVersion(parsed.package.payloadVersion).has_value());
    CHECK(parsed.package.provider == "Flowseal/zapret-discord-youtube");
    CHECK(!parsed.package.entries.empty());
    CHECK(parsed.package.runtimeManifestIndex < parsed.package.entries.size());
    const auto fixtureSize = IntegrityVerifier::FileSize(fixture);
    CHECK(fixtureSize && *fixtureSize > 12);
    if (fixtureSize && *fixtureSize > 12) {
        wchar_t temp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, temp);
        const std::wstring truncated = std::wstring(temp) + L"cheburnet-truncated.cbpkg";
        const std::wstring trailing = std::wstring(temp) + L"cheburnet-trailing.cbpkg";
        HANDLE source = ::CreateFileW(fixture.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(source != INVALID_HANDLE_VALUE);
        std::vector<unsigned char> bytes(static_cast<std::size_t>(*fixtureSize));
        DWORD read = 0;
        CHECK(source != INVALID_HANDLE_VALUE &&
              ::ReadFile(source, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                         nullptr) != 0 && read == bytes.size());
        if (source != INVALID_HANDLE_VALUE) ::CloseHandle(source);
        auto writePlain = [](const std::wstring& target, const void* data, DWORD size) {
            HANDLE out = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0;
            const bool ok = out != INVALID_HANDLE_VALUE &&
                            ::WriteFile(out, data, size, &written, nullptr) && written == size;
            if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
            return ok;
        };
        CHECK(writePlain(truncated, bytes.data(), static_cast<DWORD>(bytes.size() - 1)));
        CHECK(!update::ParsePackageFile(truncated).ok);
        bytes.push_back(0x41);
        CHECK(writePlain(trailing, bytes.data(), static_cast<DWORD>(bytes.size())));
        CHECK(!update::ParsePackageFile(trailing).ok);
        ::DeleteFileW(truncated.c_str());
        ::DeleteFileW(trailing.c_str());
    }
#endif
    return g_fail;
}

// ------------------------------------------------------ runtime state record
static int test_updatestate() {
    g_fail = 0;
    update::StoredRuntimeState state;
    state.current = "1.10.2";
    state.previousKnownGood = "1.10.1";
    state.pending = "1.10.3";
    state.packageSha256 = std::string(64, 'a');
    state.manifestKeyId = "cheburnet-release-2026";
    state.lastResult = "candidate-preflight-ok";
    const auto parsed = json::Parse(update::SerializeRuntimeState(state));
    CHECK(parsed.ok);
    CHECK(parsed.value.Find("current") &&
          *parsed.value.Find("current")->AsString() == "1.10.2");
    update::StoredRuntimeState invalid = state;
    invalid.current = "../evil";
    wchar_t temp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, temp);
    CHECK(!update::SaveRuntimeState(std::wstring(temp) + L"invalid-state.json", invalid));
    return g_fail;
}

// ---------------------------------------------------------- WinHTTP policy
// ------------------------------------------------ property / mutation fuzzing
//
// Deterministic by construction: a fixed-seed xorshift PRNG mutates known-good
// inputs, so a failure always reproduces. The property under test is uniform
// across every parser: a mutated input is either parsed into a value that
// satisfies the parser's own invariants, or rejected. It must never crash,
// hang, or be accepted while violating an invariant.
namespace {

class Prng {
public:
    explicit Prng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    std::size_t Below(std::size_t bound) {
        return bound ? static_cast<std::size_t>(Next() % bound) : 0;
    }

private:
    std::uint64_t state_;
};

// Byte-level mutations that historically break hand-written parsers.
std::string MutateBytes(std::string input, Prng& prng) {
    if (input.empty()) return input;
    switch (prng.Below(7)) {
        case 0: input[prng.Below(input.size())] = static_cast<char>(prng.Next() & 0xFF); break;
        case 1: input.erase(prng.Below(input.size()), 1 + prng.Below(4)); break;
        case 2: input.insert(prng.Below(input.size()), 1 + prng.Below(4),
                             static_cast<char>(prng.Next() & 0xFF)); break;
        case 3: input.resize(prng.Below(input.size() + 1)); break;           // truncation
        case 4: input[prng.Below(input.size())] = '\0'; break;               // embedded NUL
        case 5: input.insert(prng.Below(input.size()), "\xF0\x9F\x92\xA9"); break;  // 4-byte UTF-8
        case 6: input.insert(prng.Below(input.size()), 1, '\xFF'); break;    // invalid UTF-8 lead
        default: break;
    }
    return input;
}

const char* const kVersionSeeds[] = {
    "1.0.0", "1.0.0-rc.3", "1.10.2", "1.10.2a", "v1.0.0", "1.0.0+build.7",
    "1.0.0-rc.3+abc", "0.0.1", "9.9.9.9.9.9.9.9"
};

const char* const kPackagePathSeeds[] = {
    "bin/winws.exe", "lists/list-general.txt", "strategies/catalog.json",
    "provenance.json", "runtime-manifest.json", "bin/WinDivert64.sys"
};

// CI runs a fixed seed and iteration count so a failure always reproduces.
// CHEBURNET_FUZZ_SEED / CHEBURNET_FUZZ_ITERATIONS let a maintainer run a longer
// campaign without a separate toolchain:
//   set CHEBURNET_FUZZ_ITERATIONS=2000000
//   set CHEBURNET_FUZZ_SEED=12345
//   build\tests\cheburnet_tests.exe fuzzparsers
unsigned long long FuzzEnv(const char* name, unsigned long long fallback) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    unsigned long long value = 0;
    const char* end = raw + std::strlen(raw);
    const auto parsed = std::from_chars(raw, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) return fallback;
    return value;
}

} // namespace

static int test_fuzzparsers() {
    g_fail = 0;
    const int kIterations = static_cast<int>(FuzzEnv("CHEBURNET_FUZZ_ITERATIONS", 4000));
    const std::uint64_t fuzzSeed = FuzzEnv("CHEBURNET_FUZZ_SEED", 0xC4EB2026ull);
    std::printf("  fuzz seed=%llu iterations=%d\n", static_cast<unsigned long long>(fuzzSeed),
                kIterations);
    Prng prng(fuzzSeed);

    // ---- version parser ----------------------------------------------------
    for (int i = 0; i < kIterations; ++i) {
        const std::string seed = kVersionSeeds[prng.Below(std::size(kVersionSeeds))];
        const std::string mutated = MutateBytes(seed, prng);
        const auto parsed = update::ParseVersion(mutated);
        if (!parsed) continue;
        // Accepted: the invariants the comparator relies on must hold.
        if (parsed->parts.empty() || parsed->parts.size() > 8) {
            ++g_fail;
            std::printf("  FAIL version invariant (parts) for '%s'\n", mutated.c_str());
            continue;
        }
        if (parsed->prerelease && parsed->prereleaseIds.empty()) {
            ++g_fail;
            std::printf("  FAIL version invariant (prerelease ids) for '%s'\n", mutated.c_str());
            continue;
        }
        if (parsed->revision.size() > 1) {
            ++g_fail;
            std::printf("  FAIL version invariant (revision) for '%s'\n", mutated.c_str());
            continue;
        }
        // Comparison must be a strict, antisymmetric, reflexive order.
        if (update::CompareVersions(*parsed, *parsed) != 0) {
            ++g_fail;
            std::printf("  FAIL version not reflexive for '%s'\n", mutated.c_str());
            continue;
        }
        const auto other = update::ParseVersion(kVersionSeeds[prng.Below(std::size(kVersionSeeds))]);
        if (other) {
            const int forward = update::CompareVersions(*parsed, *other);
            const int reverse = update::CompareVersions(*other, *parsed);
            if ((forward == 0) != (reverse == 0) || (forward < 0) != (reverse > 0)) {
                ++g_fail;
                std::printf("  FAIL version not antisymmetric for '%s'\n", mutated.c_str());
            }
        }
    }

    // ---- update manifest ---------------------------------------------------
    const std::string manifestSeed = ValidManifest();
    for (int i = 0; i < kIterations; ++i) {
        const std::string mutated = MutateBytes(manifestSeed, prng);
        const update::ManifestResult parsed = update::ParseManifest(mutated);
        if (!parsed.ok) continue;
        const update::Manifest& manifest = parsed.manifest;
        // Accepted manifests must still satisfy every contract the updater
        // relies on before it will touch the network or the filesystem.
        const bool contract =
            manifest.schema == 1 && manifest.channel == "stable" &&
            manifest.launcher.sha256.size() == 64 && manifest.payload.sha256.size() == 64 &&
            manifest.launcher.size > 0 && manifest.payload.size > 0 &&
            update::IsHttpsUrl(manifest.launcher.url) &&
            update::IsHttpsUrl(manifest.payload.url) &&
            update::IsHttpsUrl(manifest.payload.upstreamReleaseUrl) &&
            manifest.payload.provider == "Flowseal/zapret-discord-youtube" &&
            update::ParseVersion(manifest.launcher.version).has_value() &&
            update::ParseVersion(manifest.payload.version).has_value() &&
            !manifest.keyId.empty();
        if (!contract) {
            ++g_fail;
            std::printf("  FAIL manifest accepted while violating its contract (iteration %d)\n", i);
        }
    }

    // ---- strict JSON -------------------------------------------------------
    const std::string jsonSeed =
        "{\"a\":1,\"b\":[true,false,null,1.5e3],\"c\":{\"d\":\"\\u0416\"}}";
    for (int i = 0; i < kIterations; ++i) {
        const std::string mutated = MutateBytes(jsonSeed, prng);
        json::ParseOptions options;
        options.maxBytes = 4096;
        options.maxDepth = 8;
        options.maxValues = 128;
        const json::ParseResult parsed = json::Parse(mutated, options);
        if (parsed.ok && mutated.size() > options.maxBytes) {
            ++g_fail;
            std::printf("  FAIL json accepted an oversized document (iteration %d)\n", i);
        }
    }

    // ---- package path allowlist -------------------------------------------
    for (int i = 0; i < kIterations; ++i) {
        const std::string seed = kPackagePathSeeds[prng.Below(std::size(kPackagePathSeeds))];
        const std::string mutated = MutateBytes(seed, prng);
        std::string normalized;
        if (!update::NormalizePackagePath(mutated, normalized)) continue;
        // Accepted: the normalized path must be inside the exact allowlist,
        // relative and free of traversal. Note that ".." only means traversal
        // as a COMPLETE component: "WinDivert64.s..ys" is an ordinary name.
        const bool allowed = normalized.rfind("bin/", 0) == 0 ||
                             normalized.rfind("lists/", 0) == 0 ||
                             normalized == "strategies/catalog.json" ||
                             normalized == "provenance.json" ||
                             normalized == "runtime-manifest.json";
        bool traversal = false;
        std::size_t at = 0;
        while (at <= normalized.size()) {
            const std::size_t slash = normalized.find('/', at);
            const std::string_view component = std::string_view(normalized).substr(
                at, slash == std::string::npos ? std::string_view::npos : slash - at);
            if (component.empty() || component == "." || component == "..") traversal = true;
            if (slash == std::string::npos) break;
            at = slash + 1;
        }
        const bool safe = allowed && !traversal &&
                          normalized.find('\\') == std::string::npos &&
                          normalized.find(':') == std::string::npos &&
                          normalized.find('\0') == std::string::npos &&
                          !normalized.empty() && normalized.front() != '/';
        if (!safe) {
            ++g_fail;
            std::printf("  FAIL package path accepted unsafe normalization: '%s'\n",
                        normalized.c_str());
        }
    }

    // Explicit traversal and device-name forms must always be rejected.
    for (const char* hostile : {"../bin/winws.exe", "bin/../../evil", "bin/./winws.exe",
                                "/bin/winws.exe", "bin\\winws.exe", "C:/bin/winws.exe",
                                "bin/", "", "bin/con.exe", "bin/winws.exe ", "bin/winws.exe.",
                                "bin/..", "bin/nul", "lists/../../../etc/passwd"}) {
        std::string normalized;
        if (update::NormalizePackagePath(hostile, normalized)) {
            ++g_fail;
            std::printf("  FAIL package path accepted a hostile input: '%s'\n", hostile);
        }
    }

    // ---- staging file names ------------------------------------------------
    const wchar_t* const stagingSeeds[] = {L"CHEBURNET-new.exe", L"engine-1.10.2.cbpkg"};
    for (int i = 0; i < kIterations; ++i) {
        std::wstring seed = stagingSeeds[prng.Below(std::size(stagingSeeds))];
        switch (prng.Below(5)) {
            case 0: seed[prng.Below(seed.size())] = static_cast<wchar_t>(prng.Next() & 0xFFFF); break;
            case 1: seed.insert(prng.Below(seed.size()), 1, L'\\'); break;
            case 2: seed.insert(prng.Below(seed.size()), 1, L'.'); break;
            case 3: seed.resize(prng.Below(seed.size() + 1)); break;
            case 4: seed.insert(prng.Below(seed.size() + 1), 1, L' '); break;
            default: break;
        }
        if (!update::IsSafeStagingFileName(seed)) continue;
        const bool safe = !seed.empty() && seed.size() <= 128 && seed.front() != L'.' &&
                          seed.back() != L'.' && seed.back() != L' ' &&
                          seed.find(L'\\') == std::wstring::npos &&
                          seed.find(L'/') == std::wstring::npos &&
                          seed.find(L':') == std::wstring::npos;
        if (!safe) {
            ++g_fail;
            std::printf("  FAIL staging name accepted an unsafe value\n");
        }
    }
    return g_fail;
}

// ------------------------------------------------------- fault injection grid
//
// The transactional activation already exposes hooks for every boundary. This
// walks the whole failure grid and asserts the one invariant that matters: a
// runtime is only ever "current" if the whole transaction committed, and any
// failure leaves or restores a known-good runtime.
static int test_faultinjection() {
    g_fail = 0;
    using update::ActivationStatus;

    enum class FailAt { None, Preflight, Stop, StartCandidate, HealthCandidate, Commit,
                        StartPrevious, HealthPrevious };
    const FailAt points[] = {FailAt::None, FailAt::Preflight, FailAt::Stop,
                             FailAt::StartCandidate, FailAt::HealthCandidate, FailAt::Commit,
                             FailAt::StartPrevious, FailAt::HealthPrevious};

    for (const FailAt point : points) {
        for (const bool hasPrevious : {true, false}) {
            const update::RuntimeState before{"1.10.1", hasPrevious ? "1.10.0" : ""};
            std::vector<std::string> calls;
            bool committed = false;
            update::RuntimeState committedState;

            update::ActivationHooks hooks;
            hooks.preflight = [&](std::string_view version) {
                calls.push_back("preflight:" + std::string(version));
                return point != FailAt::Preflight;
            };
            hooks.stopCurrent = [&]() {
                calls.push_back("stop");
                return point != FailAt::Stop;
            };
            hooks.start = [&](std::string_view version) {
                calls.push_back("start:" + std::string(version));
                if (version == "1.10.2") return point != FailAt::StartCandidate;
                return point != FailAt::StartPrevious;
            };
            hooks.health = [&](std::string_view version) {
                calls.push_back("health:" + std::string(version));
                if (version == "1.10.2") return point != FailAt::HealthCandidate;
                return point != FailAt::HealthPrevious;
            };
            hooks.commit = [&](const update::RuntimeState& state) {
                calls.push_back("commit");
                if (point == FailAt::Commit) return false;
                committed = true;
                committedState = state;
                return true;
            };

            const update::ActivationResult activated =
                update::ActivateRuntime(before, "1.10.2", hooks);

            const bool isActivated = activated.status == ActivationStatus::Activated;
            // 1. The candidate becomes current only on a full commit.
            if ((activated.state.current == "1.10.2") != isActivated) {
                ++g_fail;
                std::printf("  FAIL candidate became current without committing (point %d)\n",
                            static_cast<int>(point));
            }
            if (isActivated != committed) {
                ++g_fail;
                std::printf("  FAIL commit flag disagrees with status (point %d)\n",
                            static_cast<int>(point));
            }
            // 2. Any failure leaves or restores the previous known-good runtime.
            if (!isActivated && activated.status != ActivationStatus::RollbackFailed &&
                activated.state.current != before.current) {
                ++g_fail;
                std::printf("  FAIL failure did not restore the known-good runtime (point %d)\n",
                            static_cast<int>(point));
            }
            // 3. Commit is never reached after a failed candidate start/health.
            const bool commitCalled =
                std::find(calls.begin(), calls.end(), std::string("commit")) != calls.end();
            if ((point == FailAt::StartCandidate || point == FailAt::HealthCandidate ||
                 point == FailAt::Preflight || point == FailAt::Stop) && commitCalled) {
                ++g_fail;
                std::printf("  FAIL commit reached after an earlier failure (point %d)\n",
                            static_cast<int>(point));
            }
            // 4. Nothing is stopped before preflight approves the candidate.
            if (point == FailAt::Preflight &&
                std::find(calls.begin(), calls.end(), std::string("stop")) != calls.end()) {
                ++g_fail;
                std::printf("  FAIL the running process was stopped before preflight passed\n");
            }
            // 5. Rollback never starts the candidate a second time.
            if (!isActivated) {
                const std::size_t candidateStarts = static_cast<std::size_t>(
                    std::count(calls.begin(), calls.end(), std::string("start:1.10.2")));
                if (candidateStarts > 1) {
                    ++g_fail;
                    std::printf("  FAIL rollback restarted the candidate (point %d)\n",
                                static_cast<int>(point));
                }
            }
            // 6. Committed state always records the runtime being replaced.
            if (isActivated && committedState.previousKnownGood != before.current) {
                ++g_fail;
                std::printf("  FAIL commit lost the previous known-good runtime\n");
            }
        }
    }

    // A missing hook must be treated as a failure, never as success.
    {
        const update::RuntimeState before{"1.10.1", "1.10.0"};
        update::ActivationHooks empty;
        const update::ActivationResult activated =
            update::ActivateRuntime(before, "1.10.2", empty);
        CHECK(activated.status == ActivationStatus::PreflightFailed);
        CHECK(activated.state.current == "1.10.1");
    }
    return g_fail;
}

// ------------------------------------------ asynchronous update-check service
//
// Deterministic by construction: the fake check blocks on a flag the test
// controls instead of on a sleep, so no assertion depends on timing.
namespace {

struct FakeCheck {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int>  invocations{0};
    std::atomic<bool> observedCancel{false};
    update::CheckStatus status = update::CheckStatus::Current;
    std::string payloadVersion = "1.10.2";

    update::UpdateCheckService::CheckFunction Function() {
        return [this](std::atomic<bool>& cancel) {
            ++invocations;
            entered.store(true);
            while (!release.load() && !cancel.load()) {
                std::this_thread::yield();
            }
            if (cancel.load()) observedCancel.store(true);
            update::CheckResult produced;
            produced.status = status;
            produced.message = L"fake";
            produced.manifest.payload.version = payloadVersion;
            produced.manifest.launcher.version = "1.0.0";
            return produced;
        };
    }

    void WaitUntilEntered() {
        while (!entered.load()) std::this_thread::yield();
    }
};

// Spins until the service reports a phase, so the test never sleeps.
void WaitForPhase(const update::UpdateCheckService& service, update::CheckPhase expected) {
    while (service.Phase() != expected) std::this_thread::yield();
}

} // namespace

static int test_updatecheckservice() {
    g_fail = 0;

    {   // Success: the result is delivered exactly once.
        FakeCheck fake;
        fake.status = update::CheckStatus::Available;
        update::UpdateCheckService service(fake.Function());
        CHECK(service.Phase() == update::CheckPhase::Idle);
        CHECK(service.Start());
        fake.WaitUntilEntered();
        update::CheckResult early;
        CHECK(!service.TryTakeResult(early));       // not ready yet
        CHECK(!service.Start());                    // no concurrent second check
        fake.release.store(true);
        WaitForPhase(service, update::CheckPhase::Ready);
        update::CheckResult taken;
        CHECK(service.TryTakeResult(taken));
        CHECK(taken.status == update::CheckStatus::Available);
        CHECK(taken.manifest.payload.version == "1.10.2");
        update::CheckResult again;
        CHECK(!service.TryTakeResult(again));       // delivered once only
        CHECK(service.Phase() == update::CheckPhase::Taken);
        CHECK(fake.invocations.load() == 1);
    }

    {   // Offline/timeout is a non-fatal result, not an exception or a hang.
        FakeCheck fake;
        fake.status = update::CheckStatus::Offline;
        fake.release.store(true);
        update::UpdateCheckService service(fake.Function());
        CHECK(service.Start());
        WaitForPhase(service, update::CheckPhase::Ready);
        update::CheckResult taken;
        CHECK(service.TryTakeResult(taken));
        CHECK(taken.status == update::CheckStatus::Offline);
    }

    {   // Cancellation: the worker observes the flag, Cancel joins, and no
        // result is handed out afterwards.
        FakeCheck fake;
        update::UpdateCheckService service(fake.Function());
        CHECK(service.Start());
        fake.WaitUntilEntered();
        service.Cancel();
        CHECK(fake.observedCancel.load());
        CHECK(service.Phase() == update::CheckPhase::Cancelled);
        update::CheckResult taken;
        CHECK(!service.TryTakeResult(taken));
        service.Cancel();                           // idempotent
        CHECK(!service.Start());                    // stays cancelled
    }

    {   // Shutdown while a check is pending: the destructor must cancel and
        // join, so the worker can never outlive the service.
        FakeCheck fake;
        {
            update::UpdateCheckService service(fake.Function());
            CHECK(service.Start());
            fake.WaitUntilEntered();
        }
        CHECK(fake.observedCancel.load());
    }

    {   // A result that arrives after the UI moved on is simply never taken.
        // Destruction must still be clean.
        FakeCheck fake;
        fake.release.store(true);
        {
            update::UpdateCheckService service(fake.Function());
            CHECK(service.Start());
            WaitForPhase(service, update::CheckPhase::Ready);
        }
        CHECK(fake.invocations.load() == 1);
    }

    {   // A throwing check must degrade to a non-fatal offline result.
        update::UpdateCheckService service(
            [](std::atomic<bool>&) -> update::CheckResult { throw std::runtime_error("boom"); });
        CHECK(service.Start());
        WaitForPhase(service, update::CheckPhase::Ready);
        update::CheckResult taken;
        CHECK(service.TryTakeResult(taken));
        CHECK(taken.status == update::CheckStatus::Offline);
    }

    {   // An empty check function is refused rather than started.
        update::UpdateCheckService service(update::UpdateCheckService::CheckFunction{});
        CHECK(!service.Start());
        CHECK(service.Phase() == update::CheckPhase::Idle);
    }
    return g_fail;
}

static int test_updatehttp() {
    g_fail = 0;
    CHECK(update::IsSafeStagingFileName(L"CHEBURNET-new.exe"));
    CHECK(update::IsSafeStagingFileName(L"engine-1.10.2.cbpkg"));
    CHECK(!update::IsSafeStagingFileName(L".."));
    CHECK(!update::IsSafeStagingFileName(L".hidden"));
    CHECK(!update::IsSafeStagingFileName(L"CON.exe"));
    CHECK(!update::IsSafeStagingFileName(L"payload.cbpkg."));
    CHECK(!update::IsSafeStagingFileName(L"folder\\payload.cbpkg"));
    CHECK(update::IsAllowedRedirect(L"https://example.com/file"));
    CHECK(!update::IsAllowedRedirect(L"http://example.com/file"));
    CHECK(!update::IsAllowedRedirect(L"/relative"));
    CHECK(!update::IsAllowedRedirect(L"https://example.com\\evil"));
    CHECK(!update::IsAllowedRedirect(L"https://user@example.com/file"));
    CHECK(update::ClassifyWinHttpError(ERROR_WINHTTP_TIMEOUT) == update::HttpStatus::Timeout);
    CHECK(update::ClassifyWinHttpError(ERROR_WINHTTP_CANNOT_CONNECT) ==
          update::HttpStatus::WinHttpError);
    update::WinHttpClient client;
    CHECK(client.Get(L"http://example.com", {}).status == update::HttpStatus::InvalidUrl);
    std::atomic<bool> cancelled{true};
    update::HttpOptions options;
    options.cancel = &cancelled;
    CHECK(client.Get(L"https://example.com", options).status == update::HttpStatus::Cancelled);
    return g_fail;
}

// --------------------------------------------------------------- rollback FSM
static int test_updaterollback() {
    g_fail = 0;
    using update::ActivationStatus;
    update::RuntimeState before{"1.10.1", "1.10.0"};
    std::string started;
    update::RuntimeState committed;
    update::ActivationHooks hooks{
        [](std::string_view) { return true; },
        [] { return true; },
        [&](std::string_view version) { started = version; return true; },
        [](std::string_view) { return true; },
        [&](const update::RuntimeState& state) { committed = state; return true; }};
    auto ok = update::ActivateRuntime(before, "1.10.2", hooks);
    CHECK(ok.status == ActivationStatus::Activated && committed.current == "1.10.2");
    CHECK(committed.previousKnownGood == "1.10.1");

    bool candidateAttempted = false;
    hooks.start = [&](std::string_view version) {
        if (version == "1.10.2") { candidateAttempted = true; return false; }
        started = version; return true;
    };
    auto startFail = update::ActivateRuntime(before, "1.10.2", hooks);
    CHECK(candidateAttempted && startFail.status == ActivationStatus::RolledBack);
    CHECK(started == "1.10.1" && startFail.state.current == "1.10.1");

    hooks.start = [&](std::string_view version) { started = version; return true; };
    hooks.health = [](std::string_view version) { return version != "1.10.2"; };
    auto healthFail = update::ActivateRuntime(before, "1.10.2", hooks);
    CHECK(healthFail.status == ActivationStatus::RolledBack);
    hooks.health = [](std::string_view) { return true; };
    hooks.commit = [](const update::RuntimeState&) { return false; };
    auto commitFail = update::ActivateRuntime(before, "1.10.2", hooks);
    CHECK(commitFail.status == ActivationStatus::RolledBack);
    CHECK(commitFail.state.current == "1.10.1");
    hooks.commit = [](const update::RuntimeState&) { return false; };
    hooks.start = [](std::string_view version) { return version == "1.10.2"; };
    auto rollbackFail = update::ActivateRuntime(before, "1.10.2", hooks);
    CHECK(rollbackFail.status == ActivationStatus::RollbackFailed);
    return g_fail;
}

int wmain(int argc, wchar_t** argv) {
    // Verification helper: dump the built winws argv for one strategy using
    // sentinel dirs, one token per line, so an independent BAT parser can diff.
    //   cheburnet_tests --dump <id> <0|1 gameFilter>
    if (argc >= 3 && std::wcscmp(argv[1], L"--dump") == 0) {
        const std::string id = str::ToUtf8(argv[2]);
        GameFilterMode game = GameFilterMode::Off;
        if (argc >= 4 && std::wcscmp(argv[3], L"all") == 0) game = GameFilterMode::All;
        else if (argc >= 4 && std::wcscmp(argv[3], L"tcp") == 0) game = GameFilterMode::Tcp;
        else if (argc >= 4 && std::wcscmp(argv[3], L"udp") == 0) game = GameFilterMode::Udp;
        const RuntimeStrategy* s = strategies::Find(id);
        if (!s) {
            std::printf("NO_SUCH_STRATEGY\n");
            return 3;
        }
        auto args = strategies::BuildArguments(*s, L"BIN", L"LISTS", game);
        for (auto& a : args) std::printf("%s\n", str::ToUtf8(a).c_str());
        return 0;
    }

    struct Entry { const wchar_t* name; int (*fn)(); };
    const Entry tests[] = {
        {L"args", test_args},         {L"quoting", test_quoting},
        {L"config", test_config},     {L"sha256", test_sha256},
        {L"manifest", test_manifest}, {L"process", test_process},
        {L"integrity", test_integrity}, {L"preflight", test_preflight},
        {L"securefs", test_securefs}, {L"loghandle", test_loghandle},
        {L"animation", test_animation}, {L"progress", test_progress},
        {L"mascot", test_mascot},       {L"layout", test_layout},
        {L"checkpoints", test_checkpoints}, {L"menu", test_menu},
        {L"effects", test_effects},     {L"theme", test_theme},
        {L"framebuffer", test_framebuffer}, {L"configmigration", test_configmigration},
        {L"jsonstrict", test_jsonstrict}, {L"updatemanifest", test_updatemanifest},
        {L"operationstate", test_operationstate},
        {L"updateversion", test_updateversion}, {L"updatesignature", test_updatesignature},
        {L"updatepackage", test_updatepackage}, {L"updaterollback", test_updaterollback},
        {L"updatestate", test_updatestate},
        {L"updatehttp", test_updatehttp},
        {L"updatecheckservice", test_updatecheckservice},
        {L"fuzzparsers", test_fuzzparsers},
        {L"faultinjection", test_faultinjection},
    };

    int failures = 0;
    auto runOne = [&](const Entry& e) {
        std::printf("[ RUN  ] %ls\n", e.name);
        const int f = e.fn();
        failures += f;
        std::printf("[ %s ] %ls\n", f == 0 ? "PASS" : "FAIL", e.name);
    };

    if (argc >= 2) {
        bool found = false;
        for (const auto& e : tests) {
            if (std::wcscmp(e.name, argv[1]) == 0) {
                runOne(e);
                found = true;
            }
        }
        if (!found) {
            std::printf("unknown test: %ls\n", argv[1]);
            return 2;
        }
    } else {
        for (const auto& e : tests) runOne(e);
    }
    return failures == 0 ? 0 : 1;
}
