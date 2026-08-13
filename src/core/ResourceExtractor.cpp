#include "ResourceExtractor.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"
#include "../util/Win32Error.h"
#include "IntegrityVerifier.h"
#include "SecureFs.h"
#include "GeneratedManifest.h"
#include "GeneratedProvenance.h"
#include "GeneratedStrategies.h"
#include "../update/RuntimeStateStore.h"
#include "../util/Json.h"

#pragma comment(lib, "advapi32.lib")

namespace cheburnet {
namespace {

struct EmbeddedBuffer {
    const void* data = nullptr;
    size_t      size = 0;
    bool        ok = false;
};

EmbeddedBuffer LoadEmbedded(unsigned int rcId) {
    EmbeddedBuffer b;
    HRSRC hres = ::FindResourceW(nullptr, MAKEINTRESOURCEW(rcId), RT_RCDATA);
    if (!hres) return b;
    HGLOBAL hg = ::LoadResource(nullptr, hres);
    if (!hg) return b;
    b.data = ::LockResource(hg);
    b.size = ::SizeofResource(nullptr, hres);
    b.ok = (b.data != nullptr && b.size > 0);
    return b;
}

bool FileIsValid(const std::wstring& path, unsigned long long expectedSize, const char* expectedSha) {
    auto size = IntegrityVerifier::FileSize(path);
    if (!size || *size != expectedSize) return false;
    auto sha = IntegrityVerifier::Sha256File(path);
    if (!sha) return false;
    return IntegrityVerifier::HexEquals(*sha, expectedSha);
}

bool ReadSmallProtectedFile(const std::wstring& path, std::size_t limit, std::string& output) {
    if (!securefs::ValidateProtectedObject(path, securefs::ObjectKind::File, true).ok) return false;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                                   FILE_FLAG_OPEN_REPARSE_POINT |
                                                   FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > limit) {
        ::CloseHandle(file);
        return false;
    }
    output.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < output.size()) {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(output.size() - offset, 64u * 1024u));
        if (!::ReadFile(file, output.data() + offset, request, &read, nullptr) || read == 0) {
            ::CloseHandle(file);
            return false;
        }
        offset += read;
    }
    ::CloseHandle(file);
    return true;
}

const std::string* StringMember(const json::Value& object, std::string_view key) {
    const json::Value* value = object.Find(key);
    return value ? value->AsString() : nullptr;
}

std::optional<std::uint64_t> UIntMember(const json::Value& object, std::string_view key) {
    const json::Value* value = object.Find(key);
    const json::Number* number = value ? value->AsNumber() : nullptr;
    return number ? number->AsUInt64() : std::nullopt;
}

bool SafeRuntimeRelativePath(std::string_view input, std::wstring& output) {
    output.clear();
    if (input.empty() || input.size() > 240 || input.front() == '/' || input.front() == '\\' ||
        input.find('\\') != std::string_view::npos || input.find(':') != std::string_view::npos ||
        input.find('\0') != std::string_view::npos) return false;
    std::size_t pos = 0;
    while (pos <= input.size()) {
        const std::size_t slash = input.find('/', pos);
        const std::string_view component = input.substr(
            pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == ' ' || component.back() == '.') return false;
        for (const unsigned char c : component) {
            if (c < 0x20u || c == 0x7fu || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|') return false;
        }
        std::string device(component.substr(0, component.find('.')));
        std::transform(device.begin(), device.end(), device.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (device == "con" || device == "prn" || device == "aux" || device == "nul" ||
            (device.size() == 4 &&
             (device.rfind("com", 0) == 0 || device.rfind("lpt", 0) == 0) &&
             device[3] >= '1' && device[3] <= '9')) {
            return false;
        }
        if (!output.empty()) output.push_back(L'\\');
        output += str::ToUtf16(component);
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    const std::wstring folded = str::ToLowerAsciiW(output);
    return folded.rfind(L"bin\\", 0) == 0 || folded.rfind(L"lists\\", 0) == 0 ||
           folded == L"strategies\\catalog.json" || folded == L"provenance.json" ||
           folded == L"runtime-manifest.json";
}

bool ValidateInstalledProvenance(const std::wstring& path,
                                 const std::wstring& expectedVersion) {
    std::string bytes;
    if (!ReadSmallProtectedFile(path, 64u * 1024u, bytes)) return false;
    json::ParseOptions options;
    options.maxBytes = 64u * 1024u;
    options.maxDepth = 4;
    options.maxValues = 32;
    const json::ParseResult parsed = json::Parse(bytes, options);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    if (!root || root->size() != 10) return false;
    const std::string* provider = StringMember(parsed.value, "provider");
    const std::string* version = StringMember(parsed.value, "version");
    const std::string* tag = StringMember(parsed.value, "tag");
    const std::string* source = StringMember(parsed.value, "source_url");
    const std::string* release = StringMember(parsed.value, "release_url");
    const std::string* imported = StringMember(parsed.value, "imported_at_utc");
    const std::string* archive = StringMember(parsed.value, "archive_sha256");
    const std::string* commit = StringMember(parsed.value, "upstream_commit");
    const auto releaseId = UIntMember(parsed.value, "release_id");
    const json::Value* immutableValue = parsed.value.Find("immutable");
    const bool* immutable = immutableValue ? immutableValue->AsBool() : nullptr;
    const auto validHex = [](std::string_view value, std::size_t size) {
        if (value.size() != size) return false;
        for (const char c : value)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    };
    return provider && *provider == "Flowseal/zapret-discord-youtube" && version &&
           str::ToUtf16(*version) == expectedVersion && tag && *tag == *version &&
           releaseId && *releaseId != 0 && source && source->rfind("https://", 0) == 0 &&
           release && release->rfind("https://", 0) == 0 && imported && !imported->empty() &&
           archive && validHex(*archive, 64) && commit && validHex(*commit, 40) &&
           immutable && *immutable;
}

} // namespace

ExtractionResult ResourceExtractor::EnsureExtracted() {
    ExtractionResult result;

    if (paths_.RuntimeVersion() != upstream::kVersion) {
        std::vector<RuntimeStrategy> unused;
        return VerifyInstalledRuntime(unused);
    }

    if (!securefs::EnsureProtectedDirectory(paths_.RuntimeVersionDir()).ok ||
        !securefs::EnsureProtectedDirectory(paths_.BinDir()).ok ||
        !securefs::EnsureProtectedDirectory(paths_.ListsDir()).ok ||
        !securefs::EnsureProtectedDirectory(paths_.UserDir() + L"\\lists").ok) {
        result.error = L"Не удалось создать рабочую директорию: " + paths_.RuntimeVersionDir() +
                       L"\nПричина: " + win32::FormatLastError();
        return result;
    }

    // User overlays are version-independent and are never replaced by an
    // upstream import. Empty files preserve upstream's expected filenames.
    for (const wchar_t* name : {L"list-general-user.txt", L"list-exclude-user.txt",
                                L"ipset-exclude-user.txt"}) {
        const std::wstring path = paths_.UserDir() + L"\\lists\\" + name;
        if (!RuntimePaths::Exists(path)) {
            const std::string empty;
            if (!securefs::AtomicWrite(path, empty).ok) {
                result.error = L"Не удалось создать защищённый пользовательский список: " +
                               std::wstring(name);
                return result;
            }
        } else if (!securefs::HardenObject(path, securefs::ObjectKind::File).ok) {
            result.error = L"Не удалось защитить пользовательский список: " +
                           std::wstring(name);
            return result;
        }
    }

    for (int i = 0; i < kEmbeddedResourceCount; ++i) {
        const EmbeddedResource& r = kEmbeddedResources[i];
        const std::wstring target = paths_.RuntimeVersionDir() + L"\\" + r.relPath;
        const std::wstring logical = str::ToUtf16(r.logicalName);

        // Every embedded runtime file, including vendor lists, is immutable.
        // User additions live in the separate overlay created above.
        if (FileIsValid(target, r.expectedSize, r.sha256)) {
            const auto hardened = securefs::HardenObject(target, securefs::ObjectKind::File);
            if (!hardened.ok) {
                result.failedResource = logical;
                result.error = L"Не удалось защитить файл рабочей среды (ACL): " + logical;
                return result;
            }
            ++result.present;
            continue;
        }

        // Need to extract. Verify the embedded buffer BEFORE writing.
        EmbeddedBuffer buf = LoadEmbedded(r.rcId);
        if (!buf.ok || buf.size != r.expectedSize) {
            result.failedResource = logical;
            result.error = L"Встроенный ресурс повреждён или отсутствует: " + logical;
            Logger::Error(L"не удалось загрузить встроенный ресурс: " + logical);
            return result;
        }
        auto sha = IntegrityVerifier::Sha256Hex(buf.data, buf.size);
        if (!sha || !IntegrityVerifier::HexEquals(*sha, r.sha256)) {
            result.failedResource = logical;
            result.error = L"Контрольная сумма встроенного ресурса не совпала: " + logical;
            Logger::Error(L"SHA-256 встроенного ресурса не совпадает: " + logical);
            return result;
        }

        const auto write = securefs::AtomicWrite(target, buf.data, buf.size, r.sha256);
        if (!write.ok) {
            result.failedResource = logical;
            result.error = L"Не удалось распаковать ресурс: " + logical + L"\nПричина: " +
                           win32::FormatLastError() +
                           L"\n(файл может быть занят работающим процессом)";
            Logger::Error(L"не удалось записать ресурс: " + logical);
            return result;
        }

        // Defence in depth: re-verify every immutable file after writing.
        if (!FileIsValid(target, r.expectedSize, r.sha256)) {
            result.failedResource = logical;
            result.error = L"Распакованный файл не прошёл проверку целостности: " + logical;
            Logger::Error(L"проверка ресурса после записи не пройдена: " + logical);
            return result;
        }

        const auto hardened = securefs::HardenObject(target, securefs::ObjectKind::File);
        if (!hardened.ok) {
            result.failedResource = logical;
            result.error = L"Не удалось применить обязательный ACL: " + logical;
            return result;
        }

        ++result.extracted;
        Logger::Info(L"извлечён ресурс: " + logical);
    }

    const std::wstring strategyDir = paths_.RuntimeVersionDir() + L"\\strategies";
    if (!securefs::EnsureProtectedDirectory(strategyDir).ok) {
        result.error = L"Не удалось защитить каталог стратегий.";
        return result;
    }
    const std::string catalog = kGeneratedStrategyCatalogJson;
    if (!securefs::AtomicWrite(paths_.StrategyCatalogPath(), catalog).ok) {
        result.error = L"Не удалось записать защищённый каталог стратегий.";
        return result;
    }
    const update::StateResult state = update::LoadRuntimeState(paths_.ActiveRuntimePath());
    if (state.missing) {
        update::StoredRuntimeState initial;
        initial.current = str::ToUtf8(upstream::kVersion);
        initial.lastResult = "embedded-runtime-ready";
        if (!update::SaveRuntimeState(paths_.ActiveRuntimePath(), initial)) {
            result.error = L"Не удалось инициализировать состояние активной среды.";
            return result;
        }
    } else if (!state.ok) {
        result.error = L"Защищённое состояние рабочей среды повреждено; запуск отклонён.";
        return result;
    }

    result.ok = true;
    return result;
}

ExtractionResult ResourceExtractor::VerifyInstalledRuntime(
    std::vector<RuntimeStrategy>& catalog) const {
    ExtractionResult result;
    for (const std::wstring& directory : {
             paths_.RuntimeVersionDir(), paths_.BinDir(), paths_.ListsDir(),
             paths_.RuntimeVersionDir() + L"\\strategies"}) {
        if (!securefs::HardenObject(directory, securefs::ObjectKind::Directory).ok ||
            !securefs::ValidateProtectedObject(
                directory, securefs::ObjectKind::Directory, false).ok) {
            result.error = L"ACL каталога установленной рабочей среды не прошёл проверку.";
            return result;
        }
    }
    const std::wstring manifestPath = paths_.RuntimeVersionDir() + L"\\runtime-manifest.json";
    std::string bytes;
    if (!ReadSmallProtectedFile(manifestPath, 1024u * 1024u, bytes)) {
        result.error = L"Не удалось прочитать защищённый манифест установленной рабочей среды.";
        return result;
    }
    json::ParseOptions options;
    options.maxBytes = 1024u * 1024u;
    options.maxDepth = 8;
    options.maxValues = 4096;
    const json::ParseResult parsed = json::Parse(bytes, options);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    const auto schema = root ? UIntMember(parsed.value, "schema") : std::nullopt;
    const std::string* version = root ? StringMember(parsed.value, "payload_version") : nullptr;
    const json::Value* filesValue = root ? parsed.value.Find("files") : nullptr;
    const json::Value::Array* files = filesValue ? filesValue->AsArray() : nullptr;
    if (!root || root->size() != 3 || !schema || *schema != 1 || !version ||
        str::ToUtf16(*version) != paths_.RuntimeVersion() || !files || files->empty() ||
        files->size() > 256) {
        result.error = L"Манифест установленной рабочей среды имеет неверную схему или версию.";
        return result;
    }
    bool winws = false;
    bool strategyCatalog = false;
    bool provenance = false;
    std::set<std::wstring> seen;
    for (const json::Value& item : *files) {
        const json::Value::Object* object = item.AsObject();
        const std::string* rel = object ? StringMember(item, "path") : nullptr;
        const std::string* sha = object ? StringMember(item, "sha256") : nullptr;
        const auto size = object ? UIntMember(item, "size") : std::nullopt;
        std::wstring normalized;
        if (!object || object->size() != 3 || !rel || !sha || !size || *size == 0 ||
            sha->size() != 64 || *rel == "runtime-manifest.json" ||
            !SafeRuntimeRelativePath(*rel, normalized)) {
            result.error = L"Некорректная запись в манифесте установленной рабочей среды.";
            return result;
        }
        const std::wstring folded = str::ToLowerAsciiW(normalized);
        if (!seen.insert(folded).second) {
            result.error = L"Манифест установленной рабочей среды содержит повтор пути.";
            return result;
        }
        const std::wstring full = paths_.RuntimeVersionDir() + L"\\" + normalized;
        if (!FileIsValid(full, *size, sha->c_str()) ||
            !securefs::HardenObject(full, securefs::ObjectKind::File).ok) {
            result.failedResource = normalized;
            result.error = L"Проверка целостности установленной рабочей среды не пройдена: " + normalized;
            return result;
        }
        winws = winws || folded == L"bin\\winws.exe";
        strategyCatalog = strategyCatalog || folded == L"strategies\\catalog.json";
        provenance = provenance || folded == L"provenance.json";
        ++result.present;
    }
    if (!winws || !strategyCatalog || !provenance ||
        !ValidateInstalledProvenance(paths_.RuntimeVersionDir() + L"\\provenance.json",
                                     paths_.RuntimeVersion()) ||
        !strategies::LoadCatalogFile(paths_.StrategyCatalogPath(), catalog, result.error)) {
        if (result.error.empty()) result.error = L"Рабочая среда не содержит обязательных компонентов.";
        return result;
    }
    result.ok = true;
    return result;
}

bool ResourceExtractor::VerifyBinaries(std::wstring& firstBad) const {
    if (paths_.RuntimeVersion() != upstream::kVersion) {
        std::vector<RuntimeStrategy> unused;
        ExtractionResult verified = VerifyInstalledRuntime(unused);
        firstBad = verified.failedResource;
        return verified.ok;
    }
    for (int i = 0; i < kEmbeddedResourceCount; ++i) {
        const EmbeddedResource& r = kEmbeddedResources[i];
        if (r.category != ResourceCategory::Binary) continue;
        const std::wstring target = paths_.RuntimeVersionDir() + L"\\" + r.relPath;
        if (!FileIsValid(target, r.expectedSize, r.sha256) ||
            !securefs::ValidateProtectedObject(
                target, securefs::ObjectKind::File, true).ok) {
            firstBad = str::ToUtf16(r.logicalName);
            return false;
        }
    }
    return true;
}

bool ResourceExtractor::HardenRuntimeDirs() {
    // Every directory an attacker could pre-create must have its owner reset and
    // a protected DACL applied - setting owner on a parent does NOT change a
    // child's owner, so each level is hardened explicitly.
    struct Dir { std::wstring path; bool critical; };
    const Dir dirs[] = {
        {paths_.ProgramDataRoot(), true},
        {paths_.LogsDir(), true},
        {paths_.RuntimeRoot(), true},
        {paths_.RuntimeVersionDir(), true},
        {paths_.BinDir(), true},
        {paths_.ListsDir(), true},
        {paths_.RuntimeVersionDir() + L"\\strategies", true},
    };

    bool allCriticalOk = true;
    for (const Dir& d : dirs) {
        if (!RuntimePaths::Exists(d.path)) continue;
        if (!securefs::HardenObject(d.path, securefs::ObjectKind::Directory).ok) {
            if (d.critical) allCriticalOk = false;
        }
    }
    if (allCriticalOk) {
        Logger::Info(L"каталоги рабочей среды защищены (владелец и защищённая DACL)");
    } else {
        Logger::Error(L"не удалось защитить один или несколько критических каталогов рабочей среды");
    }
    return allCriticalOk;
}

int ResourceExtractor::CleanupOldVersions() {
    int removed = 0;
    const std::wstring pattern = paths_.RuntimeRoot() + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        bool preserve = name == upstream::kVersion;
        const update::StateResult state = update::LoadRuntimeState(paths_.ActiveRuntimePath());
        if (state.ok) {
            preserve = preserve || name == str::ToUtf16(state.state.current) ||
                       name == str::ToUtf16(state.state.previousKnownGood) ||
                       name == str::ToUtf16(state.state.pending);
        }
        if (preserve) continue;
        const std::wstring full = paths_.RuntimeRoot() + L"\\" + name;
        Logger::Info(L"удаляется старая версия рабочей среды: " + name);
        if (securefs::RemoveTreeUnder(paths_.RuntimeRoot(), full).ok) ++removed;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return removed;
}

} // namespace cheburnet
