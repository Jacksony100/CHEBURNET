#pragma once

#include <string>
#include <string_view>
#include <atomic>
#include <functional>

#include "UpdateManifest.h"
#include "WinHttpClient.h"
#include "../config/Config.h"
#include "../core/ProcessManager.h"

namespace cheburnet {
class RuntimePaths;
}

namespace cheburnet::update {

inline constexpr wchar_t kDefaultManifestUrl[] =
    L"https://github.com/Jacksony100/CHEBURNET/releases/latest/download/update-manifest.json";
inline constexpr wchar_t kDefaultSignatureUrl[] =
    L"https://github.com/Jacksony100/CHEBURNET/releases/latest/download/update-manifest.json.sig";

enum class CheckStatus { Current, Available, Offline, Disabled, Rejected, NotModified };

enum class PayloadApplyStatus {
    Applied, DownloadRejected, PackageRejected, PreflightRejected, StopRejected,
    StartRejected, HealthRejected, CommitRejected, RolledBack, RollbackFailed
};

struct PayloadApplyResult {
    PayloadApplyStatus status = PayloadApplyStatus::PackageRejected;
    std::wstring message;
    std::string previousVersion;
    std::string candidateVersion;
};

struct CheckResult {
    CheckStatus status = CheckStatus::Offline;
    std::wstring message;
    Manifest manifest;
    Eligibility launcher = Eligibility::InvalidVersion;
    Eligibility payload = Eligibility::InvalidVersion;
};

// Staging is intentionally restricted to a single conservative ASCII file
// name. This closes dot-segment, device-name and trailing-dot/space aliases.
bool IsSafeStagingFileName(std::wstring_view name);

class UpdateManager {
public:
    explicit UpdateManager(const RuntimePaths& paths) : paths_(paths) {}

    // Fetch manifest + detached signature independently, verify exact bytes,
    // then parse and evaluate versions. No unsigned fallback exists.
    //
    // `cancel` is an optional cooperative cancellation flag passed straight to
    // the HTTP client, so a pending request is aborted instead of waiting out
    // its timeout. It affects transport only: every trust check is unchanged.
    CheckResult CheckNow(bool enabled = true, std::atomic<bool>* cancel = nullptr) const;

    // Download a signed-manifest artifact to hardened staging, enforcing size
    // and SHA-256. This does not execute or silently apply elevated binaries.
    bool DownloadVerified(const Artifact& artifact, const std::wstring& finalName,
                          std::wstring& stagedPath, std::wstring& error,
                          std::atomic<bool>* cancel = nullptr,
                          std::function<void(std::uint64_t, std::uint64_t)> progress = {}) const;

    // Install a previously verified CHEBURNET payload package into a new
    // version directory and transactionally switch a running connection.
    // `manager` must be the process-wide manager owned by App.
    PayloadApplyResult ApplyPayload(const Artifact& artifact,
                                    const std::wstring& stagedPackage,
                                    ProcessManager& manager,
                                    const std::string& strategyId,
                                    GameFilterMode gameFilter,
                                    std::string_view manifestKeyId) const;

private:
    const RuntimePaths& paths_;
};

} // namespace cheburnet::update
