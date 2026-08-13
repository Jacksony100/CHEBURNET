#include "UpdatePackage.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <windows.h>

#include "Version.h"
#include "../core/IntegrityVerifier.h"
#include "../core/RuntimePaths.h"
#include "../core/SecureFs.h"
#include "../util/Json.h"
#include "../util/StringUtil.h"

namespace cheburnet::update {

bool NormalizePackagePath(std::string_view input, std::string& normalized) {
    normalized.clear();
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
        for (unsigned char c : component) {
            if (c < 0x20u || c == 0x7fu || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|') return false;
        }
        std::string device(component.substr(0, component.find('.')));
        std::transform(device.begin(), device.end(), device.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool reserved = device == "con" || device == "prn" || device == "aux" ||
                              device == "nul" ||
                              (device.size() == 4 &&
                               (device.rfind("com", 0) == 0 || device.rfind("lpt", 0) == 0) &&
                               device[3] >= '1' && device[3] <= '9');
        if (reserved) return false;
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(component);
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    if (normalized.rfind("bin/", 0) != 0 && normalized.rfind("lists/", 0) != 0 &&
        normalized != "strategies/catalog.json" && normalized != "provenance.json" &&
        normalized != "runtime-manifest.json") return false;
    return true;
}

PackageValidation ValidatePackageEntries(const std::vector<PackageEntry>& entries,
                                         const PackageLimits& limits) {
    PackageValidation result;
    if (entries.empty() || entries.size() > limits.maxFiles) {
        result.error = "package file-count limit";
        return result;
    }
    std::set<std::string> seen;
    std::uint64_t total = 0;
    bool winws = false, provenance = false, strategies = false, runtimeManifest = false;
    for (const PackageEntry& entry : entries) {
        if (entry.type != PackageEntryType::Regular) {
            result.error = "non-regular package entry rejected";
            return result;
        }
        std::string path;
        if (!NormalizePackagePath(entry.path, path)) {
            result.error = "unsafe or non-allowlisted package path";
            return result;
        }
        std::string folded = path;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!seen.insert(folded).second) {
            result.error = "duplicate normalized package path";
            return result;
        }
        if (entry.size == 0 || entry.size > limits.maxFileBytes ||
            total > limits.maxTotalBytes - entry.size) {
            result.error = "package size limit";
            return result;
        }
        total += entry.size;
        if (entry.sha256.size() != 64 ||
            std::any_of(entry.sha256.begin(), entry.sha256.end(), [](char c) {
                return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            })) {
            result.error = "invalid entry SHA-256";
            return result;
        }
        winws = winws || folded == "bin/winws.exe";
        provenance = provenance || folded == "provenance.json";
        strategies = strategies || folded.rfind("strategies/", 0) == 0;
        runtimeManifest = runtimeManifest || folded == "runtime-manifest.json";
    }
    if (!winws || !provenance || !strategies || !runtimeManifest) {
        result.error = "mandatory package content missing";
        return result;
    }
    result.ok = true;
    return result;
}

namespace {

constexpr unsigned char kMagic[8] = {'C','B','P','K','G','1','\r','\n'};

bool ReadExact(HANDLE file, void* buffer, std::size_t size) {
    auto* current = static_cast<unsigned char*>(buffer);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(size > (1u << 20) ? (1u << 20) : size);
        DWORD read = 0;
        if (!::ReadFile(file, current, chunk, &read, nullptr) || read != chunk) return false;
        current += read;
        size -= read;
    }
    return true;
}

const std::string* StringAt(const json::Value& value, std::string_view key) {
    const json::Value* member = value.Find(key);
    return member ? member->AsString() : nullptr;
}

std::optional<std::uint64_t> UIntAt(const json::Value& value, std::string_view key) {
    const json::Value* member = value.Find(key);
    const json::Number* number = member ? member->AsNumber() : nullptr;
    return number ? number->AsUInt64() : std::nullopt;
}

std::wstring ToWindowsPath(std::string_view path) {
    std::wstring output;
    output.reserve(path.size());
    for (unsigned char c : path) output.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
    return output;
}

} // namespace

PackageResult ParsePackageFile(const std::wstring& path, const PackageLimits& limits) {
    PackageResult result;
    const auto object = securefs::ValidateObject(path, securefs::ObjectKind::File, true);
    if (!object.ok) { result.error = "package is not a safe regular file"; return result; }
    const auto fileSize = IntegrityVerifier::FileSize(path);
    if (!fileSize || *fileSize < 12 || *fileSize > limits.maxTotalBytes + 1024ull * 1024ull) {
        result.error = "package size is outside limits";
        return result;
    }
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.error = "cannot open package"; return result; }
    unsigned char prefix[12]{};
    if (!ReadExact(file, prefix, sizeof(prefix)) || !std::equal(std::begin(kMagic), std::end(kMagic), prefix)) {
        ::CloseHandle(file); result.error = "package magic mismatch"; return result;
    }
    const std::uint32_t headerSize = static_cast<std::uint32_t>(prefix[8]) |
                                     (static_cast<std::uint32_t>(prefix[9]) << 8u) |
                                     (static_cast<std::uint32_t>(prefix[10]) << 16u) |
                                     (static_cast<std::uint32_t>(prefix[11]) << 24u);
    if (headerSize == 0 || headerSize > 1024u * 1024u || 12ull + headerSize > *fileSize) {
        ::CloseHandle(file); result.error = "package header size rejected"; return result;
    }
    std::string header(headerSize, '\0');
    if (!ReadExact(file, header.data(), header.size())) {
        ::CloseHandle(file); result.error = "truncated package header"; return result;
    }
    ::CloseHandle(file);
    json::ParseOptions parseOptions;
    parseOptions.maxBytes = 1024u * 1024u;
    parseOptions.maxDepth = 12;
    parseOptions.maxValues = limits.maxFiles * 8 + 32;
    const auto parsed = json::Parse(header, parseOptions);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    if (!root || root->size() != 6) { result.error = "malformed package header"; return result; }
    const auto schema = UIntAt(parsed.value, "schema");
    const auto payloadSchema = UIntAt(parsed.value, "payload_schema");
    const auto strategySchema = UIntAt(parsed.value, "strategy_schema");
    const std::string* version = StringAt(parsed.value, "payload_version");
    const std::string* provider = StringAt(parsed.value, "provider");
    const json::Value* files = parsed.value.Find("files");
    if (!schema || *schema != 1 || !payloadSchema || !strategySchema || !version || !provider ||
        !ParseVersion(*version) || !files || !files->AsArray() ||
        *payloadSchema == 0 || *strategySchema == 0 ||
        *payloadSchema > 1000 || *strategySchema > 1000) {
        result.error = "unsupported package header schema";
        return result;
    }
    std::uint64_t expectedOffset = 0;
    std::size_t entryIndex = 0;
    for (const json::Value& item : *files->AsArray()) {
        const std::string* entryPath = StringAt(item, "path");
        const std::string* sha = StringAt(item, "sha256");
        const std::string* type = StringAt(item, "type");
        const auto size = UIntAt(item, "size");
        const auto offset = UIntAt(item, "offset");
        const json::Value::Object* entryObject = item.AsObject();
        if (!entryObject || entryObject->size() != 5 || !entryPath || !sha || !type || !size || !offset ||
            *offset != expectedOffset) {
            result.error = "invalid or non-contiguous package entry";
            return result;
        }
        PackageEntry entry{*entryPath,
                           *type == "file" ? PackageEntryType::Regular : PackageEntryType::Other,
                           *size, *sha};
        result.package.entries.push_back(std::move(entry));
        std::string normalizedPath;
        if (NormalizePackagePath(*entryPath, normalizedPath) &&
            str::IEqualsAscii(normalizedPath, "runtime-manifest.json")) {
            result.package.runtimeManifestIndex = entryIndex;
        }
        if (expectedOffset > limits.maxTotalBytes - *size) {
            result.error = "package offset overflow";
            return result;
        }
        expectedOffset += *size;
        ++entryIndex;
    }
    const PackageValidation validated = ValidatePackageEntries(result.package.entries, limits);
    if (!validated.ok) { result.error = validated.error; return result; }
    result.package.schema = 1;
    result.package.payloadVersion = *version;
    result.package.provider = *provider;
    result.package.payloadSchema = static_cast<int>(*payloadSchema);
    result.package.strategySchema = static_cast<int>(*strategySchema);
    result.package.dataOffset = 12ull + headerSize;
    if (result.package.runtimeManifestIndex >= result.package.entries.size()) {
        result.error = "runtime manifest index missing";
        return result;
    }
    if (result.package.dataOffset + expectedOffset != *fileSize) {
        result.error = "truncated or trailing package data";
        return result;
    }
    result.ok = true;
    return result;
}

bool ValidateArtifactFile(const std::wstring& path, std::uint64_t expectedSize,
                          std::string_view expectedSha256) {
    if (expectedSize == 0 || expectedSha256.size() != 64) return false;
    const auto size = IntegrityVerifier::FileSize(path);
    const auto hash = IntegrityVerifier::Sha256File(path);
    return size && *size == expectedSize && hash &&
           IntegrityVerifier::HexEquals(*hash, std::string(expectedSha256));
}

PackageValidation ExtractPackage(const std::wstring& packagePath, const PackageInfo& package,
                                 const std::wstring& protectedRuntimeRoot,
                                 const std::wstring& destination) {
    PackageValidation result;
    if (!securefs::IsStrictDescendant(protectedRuntimeRoot, destination) ||
        RuntimePaths::Exists(destination)) {
        result.error = "runtime destination must be a new strict child";
        return result;
    }
    const auto rootCheck = securefs::ValidatePathComponents(protectedRuntimeRoot);
    const auto rootSecurity = securefs::ValidateProtectedObject(
        protectedRuntimeRoot, securefs::ObjectKind::Directory, false);
    if (!rootCheck.ok || !rootSecurity.ok ||
        !securefs::EnsureProtectedDirectory(destination).ok) {
        result.error = "runtime destination security rejected";
        return result;
    }
    auto fail = [&](std::string error) {
        securefs::RemoveTreeUnder(protectedRuntimeRoot, destination);
        result.error = std::move(error);
        return result;
    };
    HANDLE file = ::CreateFileW(packagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail("cannot reopen package");
    BY_HANDLE_FILE_INFORMATION packageObject{};
    if (!::GetFileInformationByHandle(file, &packageObject) ||
        (packageObject.dwFileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        packageObject.nNumberOfLinks != 1) {
        ::CloseHandle(file);
        return fail("package object changed before extraction");
    }
    std::uint64_t offset = 0;
    for (std::size_t index = 0; index < package.entries.size(); ++index) {
        const PackageEntry& entry = package.entries[index];
        const std::wstring relative = ToWindowsPath(entry.path);
        const std::wstring target = destination + L"\\" + relative;
        const std::size_t slash = target.find_last_of(L'\\');
        if (slash == std::wstring::npos ||
            !securefs::EnsureProtectedDirectory(target.substr(0, slash)).ok) {
            ::CloseHandle(file); return fail("cannot create protected package directory");
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(package.dataOffset + offset);
        if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
            ::CloseHandle(file); return fail("package seek failed");
        }
        std::vector<unsigned char> data(static_cast<std::size_t>(entry.size));
        if (!ReadExact(file, data.data(), data.size())) {
            ::CloseHandle(file); return fail("truncated package entry");
        }
        const auto hash = IntegrityVerifier::Sha256Hex(data.data(), data.size());
        if (!hash || !IntegrityVerifier::HexEquals(*hash, entry.sha256) ||
            !securefs::AtomicWrite(target, data.data(), data.size(), entry.sha256).ok) {
            ::CloseHandle(file); return fail("package entry integrity/activation failed");
        }
        offset += entry.size;
    }
    ::CloseHandle(file);
    result.ok = true;
    return result;
}

PackageValidation VerifyExtractedPackage(const PackageInfo& package,
                                         const std::wstring& destination) {
    PackageValidation result;
    if (!securefs::ValidatePathComponents(destination).ok) {
        result.error = "unsafe extracted runtime path";
        return result;
    }
    for (const PackageEntry& entry : package.entries) {
        std::string normalized;
        if (!NormalizePackagePath(entry.path, normalized)) {
            result.error = "unsafe extracted entry path";
            return result;
        }
        const std::wstring path = destination + L"\\" + ToWindowsPath(normalized);
        const auto object = securefs::ValidateObject(path, securefs::ObjectKind::File, true);
        const auto size = IntegrityVerifier::FileSize(path);
        const auto sha = IntegrityVerifier::Sha256File(path);
        if (!object.ok || !size || *size != entry.size || !sha ||
            !IntegrityVerifier::HexEquals(*sha, entry.sha256)) {
            result.error = "extracted package verification failed";
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace cheburnet::update
