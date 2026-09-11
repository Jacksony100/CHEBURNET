#include "UpdateManager.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <optional>
#include <cwctype>

#include "SignatureVerifier.h"
#include "RuntimeActivation.h"
#include "RuntimeStateStore.h"
#include "UpdatePackage.h"
#include "Version.h"
#include "../config/Strategies.h"
#include "../core/IntegrityVerifier.h"
#include "../core/RuntimePaths.h"
#include "../core/ResourceExtractor.h"
#include "../core/SecureFs.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"
#include "GeneratedProvenance.h"

namespace cheburnet::update {
namespace {

std::mutex g_manifestCacheMutex;
std::wstring g_manifestEtag;
std::optional<CheckResult> g_verifiedManifestCache;

bool SameArtifact(const Artifact& left, const Artifact& right) {
    return left.version == right.version && left.url == right.url &&
           left.sha256 == right.sha256 && left.size == right.size &&
           left.minimumSupportedVersion == right.minimumSupportedVersion &&
           left.minimumLauncherVersion == right.minimumLauncherVersion &&
           left.payloadSchema == right.payloadSchema &&
           left.strategySchema == right.strategySchema &&
           left.provider == right.provider &&
           left.upstreamReleaseUrl == right.upstreamReleaseUrl;
}

std::wstring ErrorFor(HttpStatus status) {
    switch (status) {
        case HttpStatus::Timeout: return L"тайм-аут";
        case HttpStatus::Cancelled: return L"отменено";
        case HttpStatus::TooLarge: return L"превышен лимит размера";
        case HttpStatus::InsecureRedirect: return L"небезопасная переадресация отклонена";
        case HttpStatus::RedirectLimit: return L"слишком много переадресаций";
        case HttpStatus::Truncated: return L"усечённый ответ";
        default: return L"сеть недоступна";
    }
}

} // namespace

bool IsSafeStagingFileName(std::wstring_view name) {
    if (name.empty() || name.size() > 128 || name.front() == L'.' ||
        name.back() == L'.' || name.back() == L' ') {
        return false;
    }
    for (const wchar_t c : name) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-')) {
            return false;
        }
    }
    const std::size_t dot = name.find(L'.');
    std::wstring base(name.substr(0, dot));
    for (wchar_t& c : base) c = static_cast<wchar_t>(std::towupper(c));
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return false;
    if (base.size() == 4 &&
        ((base.rfind(L"COM", 0) == 0) || (base.rfind(L"LPT", 0) == 0)) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return false;
    }
    return true;
}

CheckResult UpdateManager::CheckNow(bool enabled, std::atomic<bool>* cancel) const {
    CheckResult result;
    if (!enabled) {
        result.status = CheckStatus::Disabled;
        result.message = L"Проверка обновлений отключена.";
        return result;
    }
    WinHttpClient client;
    HttpOptions manifestOptions;
    manifestOptions.maxBytes = 64 * 1024;
    manifestOptions.cancel = cancel;
    {
        std::scoped_lock lock(g_manifestCacheMutex);
        manifestOptions.etag = g_manifestEtag;
    }
    HttpResult manifest = client.Get(kDefaultManifestUrl, manifestOptions);
    if (manifest.status == HttpStatus::NotModified) {
        std::scoped_lock lock(g_manifestCacheMutex);
        if (g_verifiedManifestCache) {
            result = *g_verifiedManifestCache;
            result.message += L" (проверенный кэш ETag, HTTP 304)";
            Logger::Info(L"манифест не изменён; используется проверенный кэш процесса");
            return result;
        }
        result.status = CheckStatus::Rejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: HTTP 304 без проверенного кэша манифеста.";
        Logger::Error(result.message);
        return result;
    }
    if (manifest.status != HttpStatus::Ok) {
        result.status = CheckStatus::Offline;
        result.message = L"Проверка обновлений: " + ErrorFor(manifest.status);
        Logger::Warn(result.message);
        return result;
    }
    HttpOptions signatureOptions;
    signatureOptions.maxBytes = 1024;
    signatureOptions.cancel = cancel;
    HttpResult signature = client.Get(kDefaultSignatureUrl, signatureOptions);
    if (signature.status != HttpStatus::Ok) {
        result.status = CheckStatus::Rejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: отдельная подпись недоступна.";
        Logger::Error(result.message);
        return result;
    }
    const std::string manifestBytes(manifest.body.begin(), manifest.body.end());
    const std::string signatureBytes(signature.body.begin(), signature.body.end());

    // Parse once only to obtain an untrusted key_id selector. Nothing else is
    // trusted until the exact bytes verify against an embedded key.
    const ManifestResult untrusted = ParseManifest(manifestBytes);
    if (!untrusted.ok ||
        VerifyManifestSignature(manifestBytes, signatureBytes, untrusted.manifest.keyId) !=
            SignatureStatus::Verified) {
        result.status = CheckStatus::Rejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: подпись манифеста недействительна.";
        Logger::Error(result.message);
        return result;
    }
    result.manifest = untrusted.manifest;
    // Канал берётся из авторитетной модели версии этой сборки, а не из
    // константы. Стабильная сборка никогда не принимает предварительную
    // версию; RC-сборка может перейти на новый RC или на стабильный выпуск.
    constexpr bool kStableChannel = CHEBURNET_STABLE_CHANNEL != 0;
    result.launcher = EvaluateLauncher(result.manifest, CHEBURNET_VERSION_STR, kStableChannel);
    const StateResult runtimeState = LoadRuntimeState(paths_.ActiveRuntimePath());
    if (!runtimeState.ok && !runtimeState.missing) {
        result.status = CheckStatus::Rejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: доверенное состояние среды недействительно.";
        Logger::Error(result.message);
        return result;
    }
    const std::string currentPayload = runtimeState.ok
                                           ? runtimeState.state.current
                                           : str::ToUtf8(upstream::kVersion);
    result.payload = EvaluatePayload(result.manifest, currentPayload,
                                     CHEBURNET_VERSION_STR, kStableChannel);
    if (result.launcher == Eligibility::DowngradeRejected ||
        result.payload == Eligibility::DowngradeRejected ||
        result.launcher == Eligibility::PrereleaseRejected ||
        result.payload == Eligibility::PrereleaseRejected ||
        result.payload == Eligibility::SchemaUnsupported ||
        result.launcher == Eligibility::LauncherTooOld ||
        result.payload == Eligibility::LauncherTooOld ||
        result.launcher == Eligibility::InvalidVersion ||
        result.payload == Eligibility::InvalidVersion) {
        result.status = CheckStatus::Rejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: запрещено понижение/предварительная версия либо несовместима схема.";
        Logger::Error(result.message);
        return result;
    }
    if (result.launcher == Eligibility::Upgrade || result.payload == Eligibility::Upgrade) {
        result.status = CheckStatus::Available;
        result.message = L"Доступно проверенное обновление.";
    } else {
        result.status = CheckStatus::Current;
        result.message = L"CHEBURNET и движок актуальны.";
    }
    Logger::Info(L"подпись обновления проверена; программа=" +
                 str::ToUtf16(result.manifest.launcher.version) + L" движок=" +
                 str::ToUtf16(result.manifest.payload.version));
    // Cache the authenticated interpretation regardless of whether the server
    // supplied an ETag. ETag is only a network optimization; it must never be
    // a prerequisite for binding privileged actions to verified metadata.
    {
        std::scoped_lock lock(g_manifestCacheMutex);
        g_manifestEtag = manifest.etag;
        g_verifiedManifestCache = result;
    }
    return result;
}

bool UpdateManager::DownloadVerified(const Artifact& artifact, const std::wstring& finalName,
                                     std::wstring& stagedPath, std::wstring& error,
                                     std::atomic<bool>* cancel,
                                     std::function<void(std::uint64_t, std::uint64_t)> progress) const {
    {
        std::scoped_lock lock(g_manifestCacheMutex);
        if (!g_verifiedManifestCache ||
            (!SameArtifact(artifact, g_verifiedManifestCache->manifest.launcher) &&
             !SameArtifact(artifact, g_verifiedManifestCache->manifest.payload))) {
        error = L"Скачивание отклонено: артефакт не связан с проверенным манифестом.";
            return false;
        }
    }
    if (!IsSafeStagingFileName(finalName)) {
        error = L"Недопустимое имя файла в защищённой области.";
        return false;
    }
    if (!securefs::EnsureProtectedDirectory(paths_.UpdatesDir()).ok) {
        error = L"Проверка ACL или пути защищённой области завершилась ошибкой.";
        return false;
    }
    HttpOptions options;
    options.maxBytes = artifact.size;
    options.cancel = cancel;
    options.progress = std::move(progress);
    stagedPath = paths_.UpdatesDir() + L"\\" + finalName;
    HttpResult download = WinHttpClient{}.DownloadToProtectedFile(
        str::ToUtf16(artifact.url), options, stagedPath, artifact.size, artifact.sha256);
    if (download.status != HttpStatus::Ok) {
        error = L"Скачивание отклонено: " + ErrorFor(download.status);
        stagedPath.clear();
        return false;
    }
    if (!ValidateArtifactFile(stagedPath, artifact.size, artifact.sha256)) {
        error = L"Скачивание отклонено: SHA-256 не совпадает.";
        stagedPath.clear();
        return false;
    }
    Logger::Info(L"проверенное обновление помещено в защищённую область: " + finalName + L" байт=" +
                 std::to_wstring(artifact.size));
    return true;
}

PayloadApplyResult UpdateManager::ApplyPayload(const Artifact& artifact,
                                               const std::wstring& stagedPackage,
                                               ProcessManager& manager,
                                               const std::string& strategyId,
                                               GameFilterMode gameFilter,
                                               std::string_view manifestKeyId) const {
    PayloadApplyResult result;
    result.candidateVersion = artifact.version;
    {
        // A payload object is actionable only while it is byte-for-byte equal
        // to the payload from this process's most recently authenticated
        // manifest. This prevents an internal call site from constructing an
        // unsigned Artifact and reaching privileged extraction.
        std::scoped_lock lock(g_manifestCacheMutex);
        const Artifact* trusted = g_verifiedManifestCache
                                      ? &g_verifiedManifestCache->manifest.payload
                                      : nullptr;
        if (!trusted || manifestKeyId != g_verifiedManifestCache->manifest.keyId ||
            !SameArtifact(*trusted, artifact)) {
            result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: пакет движка не связан с проверенным манифестом.";
            return result;
        }
    }
    const StateResult loaded = LoadRuntimeState(paths_.ActiveRuntimePath());
    StoredRuntimeState stored;
    if (loaded.ok) {
        stored = loaded.state;
    } else if (loaded.missing) {
        stored.current = str::ToUtf8(upstream::kVersion);
    } else {
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: состояние активной среды повреждено.";
        return result;
    }
    result.previousVersion = stored.current;
    const auto current = ParseVersion(stored.current);
    const auto candidate = ParseVersion(artifact.version);
    if (!current || !candidate || CompareVersions(*candidate, *current) <= 0 ||
        artifact.payloadSchema != CHEBURNET_PAYLOAD_SCHEMA ||
        artifact.strategySchema != CHEBURNET_STRATEGY_SCHEMA) {
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: запрещено понижение либо несовместимы схема/версия.";
        return result;
    }

    // Re-establish the signed artifact contract immediately before parsing and
    // extracting. ApplyPayload never accepts an arbitrary path supplied by a
    // caller and never relies solely on an earlier download-time check.
    const std::size_t stagedSlash = stagedPackage.find_last_of(L"\\/");
    if (!securefs::IsStrictDescendant(paths_.UpdatesDir(), stagedPackage) ||
        stagedSlash == std::wstring::npos ||
        !securefs::ValidatePathComponents(stagedPackage.substr(0, stagedSlash)).ok ||
        !ValidateArtifactFile(stagedPackage, artifact.size, artifact.sha256)) {
        result.status = PayloadApplyStatus::PackageRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: не прошла проверка пути, размера или SHA-256 пакета.";
        return result;
    }

    const PackageResult parsed = ParsePackageFile(stagedPackage);
    if (!parsed.ok || parsed.package.payloadVersion != artifact.version ||
        parsed.package.provider != artifact.provider ||
        parsed.package.payloadSchema != artifact.payloadSchema ||
        parsed.package.strategySchema != artifact.strategySchema) {
        result.status = PayloadApplyStatus::PackageRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: заголовок пакета не совпадает с подписанным манифестом.";
        return result;
    }
    const RuntimePaths candidatePaths(str::ToUtf16(artifact.version));
    if (RuntimePaths::Exists(candidatePaths.RuntimeVersionDir())) {
        const securefs::Result removed =
            securefs::RemoveTreeUnder(paths_.RuntimeRoot(), candidatePaths.RuntimeVersionDir());
        if (!removed.ok) {
            result.status = PayloadApplyStatus::PackageRejected;
            result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: существующая среда-кандидат небезопасна.";
            return result;
        }
    }
    const PackageValidation extracted = ExtractPackage(
        stagedPackage, parsed.package, paths_.RuntimeRoot(), candidatePaths.RuntimeVersionDir());
    if (!extracted.ok) {
        result.status = PayloadApplyStatus::PackageRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: безопасная распаковка не пройдена: " +
                         str::ToUtf16(extracted.error);
        return result;
    }
    const PackageValidation verified = VerifyExtractedPackage(parsed.package,
                                                               candidatePaths.RuntimeVersionDir());
    std::vector<RuntimeStrategy> candidateCatalog;
    std::wstring preflightError;
    ResourceExtractor candidateExtractor(candidatePaths);
    const ExtractionResult runtimeVerified = candidateExtractor.VerifyInstalledRuntime(candidateCatalog);
    if (!verified.ok || !runtimeVerified.ok || candidateCatalog.empty()) {
        securefs::RemoveTreeUnder(paths_.RuntimeRoot(), candidatePaths.RuntimeVersionDir());
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: предварительная проверка рабочей среды не пройдена: " +
                         (runtimeVerified.error.empty() ? str::ToUtf16(verified.error)
                                                        : runtimeVerified.error);
        return result;
    }
    const RuntimeStrategy* selected = nullptr;
    for (const RuntimeStrategy& strategy : candidateCatalog) {
        if (strategy.id == strategyId) selected = &strategy;
    }
    if (!selected) {
        for (const RuntimeStrategy& strategy : candidateCatalog) {
            if (strategy.id == "general") selected = &strategy;
        }
    }
    if (!selected) {
        securefs::RemoveTreeUnder(paths_.RuntimeRoot(), candidatePaths.RuntimeVersionDir());
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: кандидат не содержит пригодной стратегии.";
        return result;
    }
    const bool wasRunning = manager.IsConnectedByUs();
    StoredRuntimeState pending = stored;
    pending.pending = artifact.version;
    pending.packageSha256 = artifact.sha256;
    pending.manifestKeyId = std::string(manifestKeyId);
    pending.lastResult = "candidate-preflight-ok";
    if (!SaveRuntimeState(paths_.ActiveRuntimePath(), pending, &preflightError)) {
        securefs::RemoveTreeUnder(paths_.RuntimeRoot(), candidatePaths.RuntimeVersionDir());
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: не удалось записать состояние pending: " + preflightError;
        return result;
    }

    RuntimeState before{stored.current, stored.previousKnownGood};
    std::unique_ptr<ProcessManager> candidateManager;
    std::vector<RuntimeStrategy> rollbackCatalog;
    const RuntimeStrategy* rollbackStrategy = nullptr;
    const RuntimePaths previousPaths(str::ToUtf16(stored.current));
    if (wasRunning && stored.current != str::ToUtf8(upstream::kVersion)) {
        ResourceExtractor previousExtractor(previousPaths);
        ExtractionResult previousVerified = previousExtractor.VerifyInstalledRuntime(rollbackCatalog);
        if (!previousVerified.ok) {
            result.status = PayloadApplyStatus::PreflightRejected;
            result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: проверка предыдущей рабочей среды не пройдена.";
            pending.pending.clear();
            pending.lastResult = "previous-runtime-preflight-failed";
            if (!SaveRuntimeState(paths_.ActiveRuntimePath(), pending)) {
                result.status = PayloadApplyStatus::RollbackFailed;
                result.message = L"ОШИБКА ОБНОВЛЕНИЯ: предыдущая среда неисправна, состояние ожидания не очищено.";
            }
            return result;
        }
    }
    if (wasRunning) {
        if (stored.current == str::ToUtf8(upstream::kVersion)) {
            rollbackCatalog = strategies::All();
        }
        for (const RuntimeStrategy& strategy : rollbackCatalog) {
            if (strategy.id == strategyId) rollbackStrategy = &strategy;
        }
        if (!rollbackStrategy && !rollbackCatalog.empty()) rollbackStrategy = &rollbackCatalog.front();
    }

    auto launchVersion = [&](std::string_view version) {
        if (!wasRunning) return true;
        const bool isCandidate = version == artifact.version;
        if (!isCandidate && candidateManager) {
            const StopResult stopped = candidateManager->Stop();
            if (stopped.status != StopStatus::Stopped &&
                stopped.status != StopStatus::NotRunning) return false;
        }
        const RuntimePaths& runPaths = isCandidate ? candidatePaths : previousPaths;
        const RuntimeStrategy* strategy = isCandidate ? selected : rollbackStrategy;
        if (!strategy) return false;
        auto created = std::make_unique<ProcessManager>(runPaths);
        const std::vector<std::wstring> args = strategies::BuildArguments(
            *strategy, runPaths.BinDir(), runPaths.ListsDir(), gameFilter,
            runPaths.UserDir() + L"\\lists");
        StartResult started = created->Start(
            runPaths.WinwsExePath(), args, runPaths.BinDir(),
            runPaths.LogsDir() + L"\\winws-stdout.log",
            runPaths.LogsDir() + L"\\winws-stderr.log", 2500);
        if (started.status != StartStatus::Ok) return false;
        if (isCandidate) candidateManager = std::move(created);
        else created->Detach();
        return true;
    };
    auto healthVersion = [&](std::string_view version) {
        if (!wasRunning) return true;
        if (version == artifact.version)
            return candidateManager && candidateManager->IsConnectedByUs();
        ProcessRecord record = ProcessManager::LoadRecord(
            previousPaths.ProgramDataRoot() + L"\\winws-run.txt");
        return record.valid() && ProcessManager::IdentityMatches(record);
    };
    ActivationHooks hooks;
    hooks.preflight = [&](std::string_view version) {
        return version == artifact.version && runtimeVerified.ok;
    };
    hooks.stopCurrent = [&]() {
        if (!wasRunning) return true;
        const StopResult stopped = manager.Stop();
        return stopped.status == StopStatus::Stopped || stopped.status == StopStatus::NotRunning;
    };
    hooks.start = launchVersion;
    hooks.health = healthVersion;
    hooks.commit = [&](const RuntimeState& committed) {
        StoredRuntimeState finalState = stored;
        finalState.current = committed.current;
        finalState.previousKnownGood = committed.previousKnownGood;
        finalState.pending.clear();
        finalState.packageSha256 = artifact.sha256;
        finalState.manifestKeyId = std::string(manifestKeyId);
        finalState.lastResult = "payload-update-committed";
        return SaveRuntimeState(paths_.ActiveRuntimePath(), finalState);
    };
    const ActivationResult activated = ActivateRuntime(before, artifact.version, hooks);
    if (activated.status == ActivationStatus::Activated) {
        if (candidateManager) candidateManager->Detach();
        strategies::ActivateCatalog(std::move(candidateCatalog));
        result.status = PayloadApplyStatus::Applied;
        result.message = wasRunning
                             ? L"Движок обновлён и прошёл стабилизацию/проверку состояния."
                             : L"Движок установлен; версия активна для следующего подключения.";
        Logger::Info(L"обновление движка зафиксировано: " + str::ToUtf16(artifact.version));
        return result;
    }

    if (candidateManager && candidateManager->IsConnectedByUs()) candidateManager->Stop();
    StoredRuntimeState rolledBack = stored;
    rolledBack.pending.clear();
    rolledBack.packageSha256 = artifact.sha256;
    rolledBack.lastResult = activated.status == ActivationStatus::RollbackFailed
                                ? "payload-rollback-failed"
                                : "payload-update-rolled-back";
    const bool stateRestored = SaveRuntimeState(paths_.ActiveRuntimePath(), rolledBack);
    if (!stateRestored || activated.status == ActivationStatus::RollbackFailed) {
        result.status = PayloadApplyStatus::RollbackFailed;
        result.message = L"ОШИБКА ОБНОВЛЕНИЯ: кандидат неисправен, откат не удалось подтвердить.";
    } else if (activated.status == ActivationStatus::StopFailed) {
        result.status = PayloadApplyStatus::StopRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: доверенный процесс не удалось безопасно остановить.";
    } else if (activated.status == ActivationStatus::PreflightFailed) {
        result.status = PayloadApplyStatus::PreflightRejected;
        result.message = L"ОБНОВЛЕНИЕ ОТКЛОНЕНО: предварительная проверка кандидата не пройдена.";
    } else {
        result.status = PayloadApplyStatus::RolledBack;
        result.message = L"[ ОБНОВЛЕНИЕ ОТКАЧЕНО ] Кандидат не прошёл запуск/проверку состояния; "
                         L"восстановлена предыдущая рабочая среда.";
    }
    Logger::Error(result.message);
    return result;
}

} // namespace cheburnet::update
