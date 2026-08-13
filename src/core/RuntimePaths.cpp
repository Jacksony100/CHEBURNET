#include "RuntimePaths.h"

#include <windows.h>
#include <shlobj.h>

#include "../util/Version.h"
#include "GeneratedProvenance.h"
#include "../update/RuntimeStateStore.h"
#include "../util/StringUtil.h"
#include "SecureFs.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace cheburnet {
namespace {

std::wstring ProgramDataDir() {
    // The known-folder API is the only trusted source for this elevated path.
    // Environment variables are attacker-controlled input and must not redirect
    // privileged runtime/update writes.
    PWSTR path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path)) && path) {
        std::wstring result(path);
        ::CoTaskMemFree(path);
        return result;
    }
    if (path) ::CoTaskMemFree(path);
    return {};
}

} // namespace

RuntimePaths::RuntimePaths() : RuntimePaths(L"") {}

RuntimePaths::RuntimePaths(std::wstring runtimeVersion) {
    const std::wstring programData = ProgramDataDir();
    if (programData.empty()) return;
    root_ = programData + L"\\" CHEBURNET_PRODUCT_WNAME;
    logsDir_ = root_ + L"\\logs";
    configPath_ = root_ + L"\\user\\config.json";
    runtimeRoot_ = root_ + L"\\runtime";
    updatesDir_ = root_ + L"\\updates";
    userDir_ = root_ + L"\\user";
    if (runtimeVersion.empty()) {
        const auto state = update::LoadRuntimeState(root_ + L"\\active-runtime.json");
        if (state.ok) {
            // Only a committed current version is selected at process start.
            // A crash with `pending` set cannot promote unconfirmed code.
            runtimeVersion = str::ToUtf16(state.state.current);
        }
    }
    if (runtimeVersion.empty()) runtimeVersion = upstream::kVersion;
    runtimeVersion_ = std::move(runtimeVersion);
    versionDir_ = runtimeRoot_ + L"\\" + runtimeVersion_;
    binDir_ = versionDir_ + L"\\bin";
    listsDir_ = versionDir_ + L"\\lists";
}

std::wstring RuntimePaths::WinwsExePath() const {
    return binDir_ + L"\\winws.exe";
}

std::wstring RuntimePaths::DiagnosticsReportPath() const {
    return logsDir_ + L"\\diagnostics.txt";
}

std::wstring RuntimePaths::ActiveRuntimePath() const { return root_ + L"\\active-runtime.json"; }

std::wstring RuntimePaths::StrategyCatalogPath() const {
    return versionDir_ + L"\\strategies\\catalog.json";
}

bool RuntimePaths::Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RuntimePaths::EnsureDir(const std::wstring& path) {
    return !path.empty() && securefs::EnsureProtectedDirectory(path).ok;
}

} // namespace cheburnet
