#include "Diagnostics.h"

#include <windows.h>
#include <lmcons.h>
#include <wintrust.h>
#include <softpub.h>
#include <cwctype>

#include <algorithm>
#include <array>

#include "../config/Strategies.h"
#include "../core/ResourceExtractor.h"
#include "../core/RuntimePaths.h"
#include "../update/RuntimeStateStore.h"
#include "../util/Json.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"
#include "GeneratedManifest.h"
#include "GeneratedProvenance.h"

namespace cheburnet::diag {
namespace {

constexpr wchar_t kRedactedUser[] = L"<ПОЛЬЗОВАТЕЛЬ>";
constexpr wchar_t kRedactedMachine[] = L"<КОМПЬЮТЕР>";
constexpr wchar_t kRedactedPath[] = L"<ПРОФИЛЬ>";
constexpr wchar_t kRedactedAddress[] = L"<АДРЕС>";
constexpr wchar_t kRedactedSecret[] = L"<СКРЫТО>";

bool IEqualsAt(std::wstring_view haystack, std::size_t at, std::wstring_view needle) {
    if (needle.empty() || at + needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i) {
        const wchar_t a = static_cast<wchar_t>(::towlower(haystack[at + i]));
        const wchar_t b = static_cast<wchar_t>(::towlower(needle[i]));
        if (a != b) return false;
    }
    return true;
}

void ReplaceAllNoCase(std::wstring& text, std::wstring_view needle, std::wstring_view with) {
    if (needle.empty()) return;
    std::wstring output;
    output.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (IEqualsAt(text, pos, needle)) {
            output.append(with);
            pos += needle.size();
            continue;
        }
        output.push_back(text[pos++]);
    }
    text = std::move(output);
}

bool IsHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// '=' намеренно НЕ входит в набор: иначе "digest=<sha256>" склеивается в один
// токен и безобидная пара ключ-значение целиком попадает под редактирование.
// Дополнение base64 обрабатывается отдельно, только в хвосте прогона.
bool IsTokenChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           c == L'+' || c == L'/' || c == L'_' || c == L'-';
}

// Один компонент IPv4 (0..255) без ведущих нулей длиной больше одного символа.
bool ReadIpv4Octet(std::wstring_view text, std::size_t& at) {
    const std::size_t start = at;
    unsigned value = 0;
    while (at < text.size() && text[at] >= L'0' && text[at] <= L'9' && at - start < 3) {
        value = value * 10 + static_cast<unsigned>(text[at] - L'0');
        ++at;
    }
    return at != start && value <= 255;
}

// Заменяет адреса IPv4 и достаточно длинные литералы IPv6.
std::wstring RedactAddresses(std::wstring_view text) {
    std::wstring output;
    output.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        // IPv4
        std::size_t probe = pos;
        bool matched = true;
        for (int part = 0; part < 4 && matched; ++part) {
            if (part != 0) {
                if (probe >= text.size() || text[probe] != L'.') { matched = false; break; }
                ++probe;
            }
            if (!ReadIpv4Octet(text, probe)) matched = false;
        }
        const bool leftBoundary = pos == 0 || !((text[pos - 1] >= L'0' && text[pos - 1] <= L'9') ||
                                                text[pos - 1] == L'.');
        const bool rightBoundary = probe >= text.size() ||
                                   !((text[probe] >= L'0' && text[probe] <= L'9') ||
                                     text[probe] == L'.');
        if (matched && leftBoundary && rightBoundary) {
            output.append(kRedactedAddress);
            pos = probe;
            continue;
        }
        // IPv6: не меньше трёх групп шестнадцатеричных цифр через двоеточие.
        probe = pos;
        int groups = 0;
        while (probe < text.size()) {
            const std::size_t groupStart = probe;
            while (probe < text.size() && IsHexDigit(text[probe]) && probe - groupStart < 4) ++probe;
            if (probe == groupStart) break;
            ++groups;
            if (probe < text.size() && text[probe] == L':') ++probe; else break;
        }
        if (groups >= 3 && (pos == 0 || !IsHexDigit(text[pos - 1]))) {
            output.append(kRedactedAddress);
            pos = probe;
            continue;
        }
        output.push_back(text[pos++]);
    }
    return output;
}

// Длинные непрерывные строки из алфавита base64/hex трактуются как возможные
// секреты: токены, подписи, ключи. Известные безопасные значения (SHA-256
// ровно 64 hex-символа) в диагностике нужны, поэтому они сохраняются.
std::wstring RedactSecrets(std::wstring_view text) {
    constexpr std::size_t kSuspiciousLength = 40;
    std::wstring output;
    output.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (!IsTokenChar(text[pos])) {
            output.push_back(text[pos++]);
            continue;
        }
        const std::size_t start = pos;
        bool allHex = true;
        while (pos < text.size() && IsTokenChar(text[pos])) {
            if (!IsHexDigit(text[pos])) allHex = false;
            ++pos;
        }
        // Хвостовое дополнение base64 считается частью прогона, но не делает
        // его шестнадцатеричным.
        std::size_t padding = 0;
        while (pos < text.size() && text[pos] == L'=' && padding < 2) {
            ++pos;
            ++padding;
            allHex = false;
        }
        const std::size_t length = pos - start;
        const bool knownDigest = allHex && (length == 64 || length == 40);
        if (length >= kSuspiciousLength && !knownDigest) {
            output.append(kRedactedSecret);
        } else {
            output.append(text.substr(start, length));
        }
    }
    return output;
}

std::string Quote(std::wstring_view value) { return json::Quote(str::ToUtf8(value)); }
std::string Quote(std::string_view value) { return json::Quote(value); }

std::wstring LocalTimestamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring UtcTimestamp() {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::wstring WindowsBuild() {
    // RtlGetVersion минует манифестную совместимость и возвращает настоящую
    // версию. Собираются только версия, сборка и разрядность.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        const auto entry = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        if (entry && entry(&info) == 0) {
            return std::to_wstring(info.dwMajorVersion) + L"." +
                   std::to_wstring(info.dwMinorVersion) + L"." +
                   std::to_wstring(info.dwBuildNumber);
        }
    }
    return L"неизвестно";
}

std::wstring ProcessArchitecture() {
    SYSTEM_INFO info{};
    ::GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        default: return L"неизвестно";
    }
}

std::wstring LauncherPath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

// Состояние подписи Authenticode самого лончера. Берётся только вердикт, без
// каких-либо сведений о владельце сертификата пользователя.
std::wstring LauncherSignatureState(const std::wstring& path) {
    if (path.empty()) return L"не определено";
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG status = ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status == 0) return L"действительна";
    if (status == TRUST_E_NOSIGNATURE) return L"не подписан";
    if (status == TRUST_E_BAD_DIGEST) return L"нарушена (файл изменён)";
    if (status == CERT_E_UNTRUSTEDROOT || status == CERT_E_CHAINING) return L"цепочка не доверена";
    if (status == CERT_E_EXPIRED) return L"сертификат истёк";
    return L"отклонена";
}

std::string ReadLogTail(const std::wstring& path, std::size_t maxBytes) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        ::CloseHandle(file);
        return {};
    }
    const std::uint64_t total = static_cast<std::uint64_t>(size.QuadPart);
    const std::uint64_t want = total > maxBytes ? maxBytes : total;
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(total - want);
    if (!::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        ::CloseHandle(file);
        return {};
    }
    std::string bytes(static_cast<std::size_t>(want), '\0');
    std::size_t read = 0;
    while (read < bytes.size()) {
        DWORD chunk = 0;
        if (!::ReadFile(file, bytes.data() + read, static_cast<DWORD>(bytes.size() - read),
                        &chunk, nullptr) || chunk == 0) {
            break;
        }
        read += chunk;
    }
    ::CloseHandle(file);
    bytes.resize(read);
    // Обрезанный хвост начинается с середины строки — отбрасываем её.
    const std::size_t newline = bytes.find('\n');
    if (total > want && newline != std::string::npos) bytes.erase(0, newline + 1);
    return bytes;
}

void AppendField(std::string& out, std::string_view key, std::wstring_view value, bool last) {
    out += "    " + Quote(key) + ": " + Quote(value);
    out += last ? "\n" : ",\n";
}

} // namespace

RedactionContext CurrentRedactionContext() {
    RedactionContext context;
    wchar_t user[UNLEN + 1]{};
    DWORD userSize = UNLEN + 1;
    if (::GetUserNameW(user, &userSize) && userSize > 1) context.userName.assign(user, userSize - 1);
    wchar_t machine[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD machineSize = MAX_COMPUTERNAME_LENGTH + 1;
    if (::GetComputerNameW(machine, &machineSize)) context.machineName.assign(machine, machineSize);
    wchar_t profile[MAX_PATH]{};
    DWORD profileSize = MAX_PATH;
    if (::GetEnvironmentVariableW(L"USERPROFILE", profile, profileSize) > 0) {
        context.userProfile = profile;
    }
    return context;
}

std::wstring Redact(std::wstring_view text, const RedactionContext& context) {
    std::wstring output(text);
    // Порядок важен: сначала самые длинные и конкретные значения.
    if (context.userProfile.size() > 3) ReplaceAllNoCase(output, context.userProfile, kRedactedPath);
    if (context.userName.size() >= 2) ReplaceAllNoCase(output, context.userName, kRedactedUser);
    if (context.machineName.size() >= 2) {
        ReplaceAllNoCase(output, context.machineName, kRedactedMachine);
    }
    // Любой иной путь внутри каталога пользователей тоже скрывается: имя может
    // отличаться от текущего (другой профиль, перенесённый журнал).
    {
        std::wstring result;
        result.reserve(output.size());
        std::size_t pos = 0;
        const std::wstring_view marker = L"\\users\\";
        while (pos < output.size()) {
            if (IEqualsAt(output, pos, marker)) {
                result.append(L"\\Users\\");
                result.append(kRedactedUser);
                pos += marker.size();
                while (pos < output.size() && output[pos] != L'\\' && output[pos] != L'/' &&
                       output[pos] != L'"' && output[pos] != L' ') {
                    ++pos;
                }
                continue;
            }
            result.push_back(output[pos++]);
        }
        output = std::move(result);
    }
    output = RedactAddresses(output);
    output = RedactSecrets(output);
    return output;
}

std::wstring SuggestedFileName() {
    return L"CHEBURNET-diagnostics-" + LocalTimestamp() + L".zip";
}

Bundle BuildBundle(const RuntimePaths& paths, const Config& config,
                   const BundleOptions& options) {
    const RedactionContext context = CurrentRedactionContext();
    const std::wstring launcher = LauncherPath();

    update::StateResult state = update::LoadRuntimeState(paths.ActiveRuntimePath());
    const std::wstring currentRuntime = state.ok ? str::ToUtf16(state.state.current)
                                                 : paths.RuntimeVersion();
    const std::wstring previousRuntime = state.ok ? str::ToUtf16(state.state.previousKnownGood)
                                                  : std::wstring();
    const std::wstring pendingRuntime = state.ok ? str::ToUtf16(state.state.pending)
                                                 : std::wstring();
    const std::wstring lastResult = state.ok ? str::ToUtf16(state.state.lastResult)
                                             : std::wstring(L"состояние недоступно");

    std::vector<RuntimeStrategy> catalog;
    ExtractionResult integrity;
    {
        ResourceExtractor verifier(paths);
        integrity = verifier.VerifyInstalledRuntime(catalog);
    }

    std::string summary;
    summary += "{\n  \"schema\": 1,\n";
    summary += "  \"generated_at\": " + Quote(UtcTimestamp()) + ",\n";
    summary += "  \"launcher\": {\n";
    AppendField(summary, "semantic_version", str::ToUtf16(CHEBURNET_VERSION_STR), false);
    AppendField(summary, "pe_version", str::ToUtf16(CHEBURNET_VERSION_PE_STR), false);
    AppendField(summary, "release_channel", str::ToUtf16(CHEBURNET_RELEASE_CHANNEL), false);
    AppendField(summary, "authenticode", LauncherSignatureState(launcher), true);
    summary += "  },\n";
    summary += "  \"payload\": {\n";
    AppendField(summary, "provider", upstream::kProvider, false);
    AppendField(summary, "embedded_version", upstream::kVersion, false);
    AppendField(summary, "release_url", upstream::kReleaseUrl, false);
    AppendField(summary, "imported_at_utc", upstream::kImportedAt, false);
    AppendField(summary, "archive_sha256", str::ToUtf16(upstream::kArchiveSha256), false);
    AppendField(summary, "upstream_commit", str::ToUtf16(upstream::kCommit), true);
    summary += "  },\n";
    summary += "  \"runtime\": {\n";
    AppendField(summary, "current", currentRuntime, false);
    AppendField(summary, "previous_known_good", previousRuntime, false);
    AppendField(summary, "pending", pendingRuntime, false);
    AppendField(summary, "last_result", lastResult, false);
    AppendField(summary, "integrity",
                integrity.ok ? std::wstring(L"проверена")
                             : (L"нарушена: " + Redact(integrity.error, context)), false);
    AppendField(summary, "strategies_available", std::to_wstring(catalog.size()), true);
    summary += "  },\n";
    summary += "  \"settings\": {\n";
    AppendField(summary, "strategy_id", str::ToUtf16(config.strategyId), false);
    AppendField(summary, "game_filter", str::ToUtf16(GameFilterModeName(config.gameFilter)), false);
    AppendField(summary, "update_mode", str::ToUtf16(UpdateModeName(config.update.mode)), false);
    AppendField(summary, "update_channel", str::ToUtf16(config.update.channel), false);
    AppendField(summary, "check_on_start", config.update.checkOnStart ? L"true" : L"false", true);
    summary += "  },\n";
    summary += "  \"system\": {\n";
    AppendField(summary, "windows_build", WindowsBuild(), false);
    AppendField(summary, "architecture", ProcessArchitecture(), true);
    summary += "  },\n";
    summary += "  \"binaries\": [\n";
    // Хеши только тех файлов, которыми владеет CHEBURNET. Пути внутри архива
    // относительные, чтобы не раскрывать раскладку машины.
    bool firstBinary = true;
    for (int i = 0; i < kEmbeddedResourceCount; ++i) {
        const auto& resource = kEmbeddedResources[i];
        if (!firstBinary) summary += ",\n";
        firstBinary = false;
        summary += "    {" + Quote(std::string_view("name")) + ": " +
                   Quote(std::string_view(resource.logicalName)) + ", " +
                   Quote(std::string_view("expected_sha256")) + ": " +
                   Quote(std::string_view(resource.sha256)) + ", " +
                   Quote(std::string_view("bytes")) + ": " +
                   std::to_string(resource.expectedSize) + "}";
    }
    summary += "\n  ]\n}\n";

    const std::string logTail = ReadLogTail(paths.LogsDir() + L"\\cheburnet.log",
                                            options.maxLogBytes);
    const std::wstring redactedLog = Redact(str::ToUtf16(logTail), context);

    std::string manifest;
    manifest += "{\n  \"schema\": 1,\n";
    manifest += "  \"product\": " + Quote(std::string_view("CHEBURNET")) + ",\n";
    manifest += "  \"purpose\": " +
                Quote(std::string_view("local support bundle; never uploaded automatically")) + ",\n";
    manifest += "  \"contents\": [\n";
    manifest += "    {\"file\": \"summary.json\", \"describes\": \"launcher/payload/runtime/"
                "settings/system state and expected hashes of CHEBURNET-owned files\"},\n";
    manifest += "    {\"file\": \"application.log\", \"describes\": \"redacted tail of "
                "cheburnet.log, bounded to " + std::to_string(options.maxLogBytes) + " bytes\"},\n";
    manifest += "    {\"file\": \"manifest.json\", \"describes\": \"this description\"}\n";
    manifest += "  ],\n";
    manifest += "  \"redacted\": [\"user name\", \"computer name\", \"user profile paths\", "
                "\"any path under \\\\Users\\\\\", \"IPv4 and IPv6 literals\", "
                "\"token-like strings\"],\n";
    manifest += "  \"never_collected\": [\"environment variables\", \"proxy credentials\", "
                "\"browser data\", \"cookies\", \"packet captures\", \"user list files\", "
                "\"machine identifiers\", \"certificate subject of the signer\"]\n";
    manifest += "}\n";

    Bundle bundle;
    bundle.entries.push_back({"manifest.json", manifest});
    bundle.entries.push_back({"summary.json", summary});
    bundle.entries.push_back({"application.log", str::ToUtf8(redactedLog)});

    std::wstring preview;
    preview += L"В архив войдут только эти файлы:\n";
    preview += L"  manifest.json     — описание состава архива\n";
    preview += L"  summary.json      — версия CHEBURNET и канал, версия и происхождение движка,\n";
    preview += L"                      версии рабочей среды (текущая/предыдущая/ожидающая),\n";
    preview += L"                      состояние целостности, выбранная стратегия, игровой фильтр,\n";
    preview += L"                      режим обновлений, сборка Windows, разрядность,\n";
    preview += L"                      состояние подписи CHEBURNET.exe, ожидаемые SHA-256\n";
    preview += L"                      файлов, принадлежащих CHEBURNET\n";
    preview += L"  application.log   — хвост журнала, не более " +
               std::to_wstring(options.maxLogBytes / 1024) + L" КиБ, после редактирования\n\n";
    preview += L"Скрывается: имя пользователя, имя компьютера, пути профиля, любые пути\n";
    preview += L"внутри \\Users\\, адреса IPv4/IPv6, строки, похожие на токены.\n\n";
    preview += L"Не собирается: переменные окружения, учётные данные прокси, данные браузера,\n";
    preview += L"файлы cookie, дампы трафика, пользовательские списки, идентификаторы машины.\n\n";
    preview += L"Архив создаётся локально. CHEBURNET никуда его не отправляет.";
    bundle.preview = std::move(preview);
    return bundle;
}

std::uint32_t Crc32(const void* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> generated{};
        // Перебор по ссылкам, а не по индексу: индексирования нет вообще,
        // поэтому нечего доказывать ни читателю, ни статическому анализу
        // (C28020 на std::array::operator[] с беззнаковым счётчиком).
        std::uint32_t index = 0;
        for (std::uint32_t& slot : generated) {
            std::uint32_t value = index++;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            slot = value;
        }
        return generated;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace {

void PutU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void PutU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

} // namespace

bool WriteZipArchive(const std::wstring& path, const std::vector<BundleEntry>& entries,
                     std::wstring& error) {
    if (entries.empty()) {
        error = L"Нечего экспортировать.";
        return false;
    }
    std::string archive;
    std::string directory;
    std::uint32_t offset = 0;
    std::uint16_t count = 0;
    for (const BundleEntry& entry : entries) {
        if (entry.name.empty() || entry.name.size() > 250 ||
            entry.name.find('\\') != std::string::npos ||
            entry.name.find("..") != std::string::npos) {
            error = L"Недопустимое имя записи архива.";
            return false;
        }
        if (entry.content.size() > 0xFFFFFFFFull) {
            error = L"Запись архива слишком велика.";
            return false;
        }
        const std::uint32_t crc = Crc32(entry.content.data(), entry.content.size());
        const auto size = static_cast<std::uint32_t>(entry.content.size());
        const auto nameLength = static_cast<std::uint16_t>(entry.name.size());

        // Local file header. Метод 0 (store): архив детерминирован и не зависит
        // от библиотеки сжатия.
        PutU32(archive, 0x04034B50u);
        PutU16(archive, 20);        // version needed
        PutU16(archive, 1u << 11);  // UTF-8 names
        PutU16(archive, 0);         // method: store
        PutU16(archive, 0);         // mod time
        PutU16(archive, 0x21);      // mod date: 1980-01-01
        PutU32(archive, crc);
        PutU32(archive, size);
        PutU32(archive, size);
        PutU16(archive, nameLength);
        PutU16(archive, 0);
        archive += entry.name;
        archive += entry.content;

        PutU32(directory, 0x02014B50u);
        PutU16(directory, 20);       // version made by
        PutU16(directory, 20);       // version needed
        PutU16(directory, 1u << 11);
        PutU16(directory, 0);
        PutU16(directory, 0);
        PutU16(directory, 0x21);
        PutU32(directory, crc);
        PutU32(directory, size);
        PutU32(directory, size);
        PutU16(directory, nameLength);
        PutU16(directory, 0);        // extra
        PutU16(directory, 0);        // comment
        PutU16(directory, 0);        // disk
        PutU16(directory, 0);        // internal attributes
        PutU32(directory, 0);        // external attributes
        PutU32(directory, offset);
        directory += entry.name;

        offset = static_cast<std::uint32_t>(archive.size());
        ++count;
    }
    const auto directoryOffset = static_cast<std::uint32_t>(archive.size());
    archive += directory;
    PutU32(archive, 0x06054B50u);
    PutU16(archive, 0);
    PutU16(archive, 0);
    PutU16(archive, count);
    PutU16(archive, count);
    PutU32(archive, static_cast<std::uint32_t>(directory.size()));
    PutU32(archive, directoryOffset);
    PutU16(archive, 0);

    // CREATE_NEW: экспорт никогда не перезаписывает существующий файл молча.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        error = code == ERROR_FILE_EXISTS
                    ? L"Файл с таким именем уже существует."
                    : L"Не удалось создать файл архива (код " + std::to_wstring(code) + L").";
        return false;
    }
    std::size_t written = 0;
    bool ok = true;
    while (written < archive.size()) {
        DWORD chunk = 0;
        if (!::WriteFile(file, archive.data() + written,
                         static_cast<DWORD>(archive.size() - written), &chunk, nullptr) ||
            chunk == 0) {
            ok = false;
            break;
        }
        written += chunk;
    }
    if (ok) ok = ::FlushFileBuffers(file) != 0;
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        error = L"Не удалось записать архив диагностики.";
        return false;
    }
    return true;
}

} // namespace cheburnet::diag
