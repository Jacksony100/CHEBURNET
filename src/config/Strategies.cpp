#include "Strategies.h"

#include <algorithm>
#include <set>

#include <windows.h>

#include "GeneratedStrategies.h"
#include "../core/SecureFs.h"
#include "../util/Json.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"

namespace cheburnet::strategies {
namespace {

void ReplaceAll(std::wstring& s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<RuntimeStrategy> BuildRegistry() {
    std::vector<RuntimeStrategy> out;
    out.reserve(static_cast<size_t>(kGeneratedStrategyCount));
    for (int i = 0; i < kGeneratedStrategyCount; ++i) {
        const GeneratedStrategy& g = kGeneratedStrategies[i];
        RuntimeStrategy rs;
        rs.id = g.id;
        rs.sourceFile = g.sourceFile;
        rs.displayName = g.displayName;
        rs.description = g.description;
        rs.recommended = g.recommended;
        rs.argTemplate.reserve(static_cast<size_t>(g.argc));
        for (int j = 0; j < g.argc; ++j) {
            rs.argTemplate.emplace_back(g.argv[j]);
        }
        out.push_back(std::move(rs));
    }
    return out;
}

std::vector<RuntimeStrategy>& Registry() {
    static std::vector<RuntimeStrategy> registry = BuildRegistry();
    return registry;
}

const std::string* StringMember(const json::Value& object, std::string_view key) {
    const json::Value* value = object.Find(key);
    return value ? value->AsString() : nullptr;
}

bool ReadCatalog(const std::wstring& path, std::string& bytes, std::wstring& error) {
    if (!securefs::ValidateObject(path, securefs::ObjectKind::File, true).ok) {
        error = L"Каталог стратегий не является защищённым обычным файлом.";
        return false;
    }
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                                   FILE_FLAG_OPEN_REPARSE_POINT |
                                                   FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Не удалось открыть каталог стратегий.";
        return false;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 4ll * 1024ll * 1024ll) {
        ::CloseHandle(file);
        error = L"Размер каталога стратегий отклонён.";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 64u * 1024u));
        DWORD read = 0;
        if (!::ReadFile(file, bytes.data() + offset, request, &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        offset += read;
    }
    ::CloseHandle(file);
    if (!ok || offset != bytes.size()) {
        error = L"Каталог стратегий усечён или не читается.";
        return false;
    }
    return true;
}

bool ValidSourceName(std::string_view source) {
    if (source.empty() || source.size() > 180 || source == "." || source == ".." ||
        source.find_first_of("\\/:\r\n\0") != std::string_view::npos) return false;
    const std::string folded = str::ToLowerAscii(source);
    return folded.size() > 4 && folded.ends_with(".bat");
}

bool ValidTemplateToken(std::string token) {
    if (token.empty() || token.size() > 4096 || token.find('\0') != std::string::npos ||
        token.find_first_of("\r\n") != std::string::npos) return false;
    for (const std::string_view placeholder : {"%BIN%", "%LISTS%", "%USER_LISTS%",
                                                "%GAME_TCP%", "%GAME_UDP%", "%GAME_BOTH%"}) {
        std::size_t pos = 0;
        while ((pos = token.find(placeholder, pos)) != std::string::npos) {
            token.erase(pos, placeholder.size());
        }
    }
    // The build-time BAT importer maps every supported variable to one of the
    // placeholders above. An unknown percent expression is a schema mismatch,
    // never something to silently pass through to elevated winws.exe.
    return token.find('%') == std::string::npos;
}

} // namespace

const std::vector<RuntimeStrategy>& All() {
    return Registry();
}

const RuntimeStrategy* Find(std::string_view id) {
    for (const auto& s : All()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

bool Exists(std::string_view id) {
    return Find(id) != nullptr;
}

const RuntimeStrategy& Default() {
    if (const RuntimeStrategy* s = Find("general")) return *s;
    return All().front(); // registry is never empty
}

bool LoadCatalogFile(const std::wstring& path, std::vector<RuntimeStrategy>& output,
                     std::wstring& error) {
    output.clear();
    std::string bytes;
    if (!ReadCatalog(path, bytes, error)) return false;
    json::ParseOptions options;
    options.maxBytes = 4u * 1024u * 1024u;
    options.maxDepth = 8;
    options.maxValues = 300000;
    const json::ParseResult parsed = json::Parse(bytes, options);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    const json::Value* schemaValue = root ? parsed.value.Find("schema") : nullptr;
    const json::Number* schemaNumber = schemaValue ? schemaValue->AsNumber() : nullptr;
    const auto schema = schemaNumber ? schemaNumber->AsUInt64() : std::nullopt;
    const json::Value* strategiesValue = root ? parsed.value.Find("strategies") : nullptr;
    const json::Value::Array* entries = strategiesValue ? strategiesValue->AsArray() : nullptr;
    if (!root || root->size() != 2 || !schema || *schema != CHEBURNET_STRATEGY_SCHEMA ||
        !entries || entries->empty() || entries->size() > 256) {
        error = L"Неподдерживаемая схема каталога стратегий.";
        return false;
    }

    std::set<std::string> ids;
    std::size_t totalTokens = 0;
    std::vector<RuntimeStrategy> candidate;
    candidate.reserve(entries->size());
    for (const json::Value& entry : *entries) {
        const json::Value::Object* object = entry.AsObject();
        const std::string* id = object ? StringMember(entry, "id") : nullptr;
        const std::string* source = object ? StringMember(entry, "source") : nullptr;
        const std::string* name = object ? StringMember(entry, "name") : nullptr;
        const json::Value* recommendedValue = object ? entry.Find("recommended") : nullptr;
        const bool* recommended = recommendedValue ? recommendedValue->AsBool() : nullptr;
        const json::Value* argvValue = object ? entry.Find("argv") : nullptr;
        const json::Value::Array* argv = argvValue ? argvValue->AsArray() : nullptr;
        if (!object || object->size() != 5 || !id || !source || !name || !recommended || !argv ||
            !IsValidStrategyIdSyntax(*id) || !ValidSourceName(*source) || name->empty() ||
            name->size() > 160 || argv->empty() || argv->size() > 1024) {
            error = L"Некорректная запись в каталоге стратегий.";
            return false;
        }
        std::string folded = str::ToLowerAscii(*id);
        if (!ids.insert(folded).second) {
            error = L"Повторяющийся идентификатор стратегии.";
            return false;
        }
        if (totalTokens > 100000 - argv->size()) {
            error = L"Каталог стратегий превышает лимит аргументов.";
            return false;
        }
        totalTokens += argv->size();
        RuntimeStrategy runtime;
        runtime.id = *id;
        runtime.sourceFile = str::ToUtf16(*source);
        runtime.displayName = str::ToUtf16(*name);
        runtime.description = L"Imported from " + runtime.sourceFile;
        runtime.recommended = *recommended;
        runtime.argTemplate.reserve(argv->size());
        for (const json::Value& value : *argv) {
            const std::string* token = value.AsString();
            if (!token || !ValidTemplateToken(*token)) {
                error = L"Неподдерживаемый аргумент в каталоге стратегий.";
                return false;
            }
            runtime.argTemplate.push_back(str::ToUtf16(*token));
        }
        candidate.push_back(std::move(runtime));
    }
    if (ids.find("general") == ids.end()) {
        error = L"В каталоге нет обязательной стратегии general.";
        return false;
    }
    output = std::move(candidate);
    return true;
}

void ActivateCatalog(std::vector<RuntimeStrategy> catalog) {
    Registry() = std::move(catalog);
}

void RestoreEmbeddedCatalog() {
    Registry() = BuildRegistry();
}

std::wstring GameFilterTcpValue(GameFilterMode mode) {
    return mode == GameFilterMode::All || mode == GameFilterMode::Tcp ? L"1024-65535" : L"12";
}

std::wstring GameFilterUdpValue(GameFilterMode mode) {
    return mode == GameFilterMode::All || mode == GameFilterMode::Udp ? L"1024-65535" : L"12";
}

std::vector<std::wstring> BuildArguments(const RuntimeStrategy& s, const std::wstring& binDir,
                                         const std::wstring& listsDir, GameFilterMode gameFilter,
                                         const std::wstring& userListsDir) {
    const std::wstring binRepl = binDir + L"\\";
    const std::wstring listsRepl = listsDir + L"\\";
    const std::wstring userListsRepl = (userListsDir.empty() ? listsDir : userListsDir) + L"\\";
    const std::wstring tcpRepl = GameFilterTcpValue(gameFilter);
    const std::wstring udpRepl = GameFilterUdpValue(gameFilter);
    const std::wstring legacyRepl = gameFilter == GameFilterMode::Off ? L"12" : L"1024-65535";

    std::vector<std::wstring> out;
    out.reserve(s.argTemplate.size());
    for (std::wstring tok : s.argTemplate) {
        ReplaceAll(tok, L"%BIN%", binRepl);
        ReplaceAll(tok, L"%LISTS%", listsRepl);
        ReplaceAll(tok, L"%USER_LISTS%", userListsRepl);
        ReplaceAll(tok, L"%GAME_TCP%", tcpRepl);
        ReplaceAll(tok, L"%GAME_UDP%", udpRepl);
        ReplaceAll(tok, L"%GAME_BOTH%", legacyRepl);
        out.push_back(std::move(tok));
    }
    return out;
}

} // namespace cheburnet::strategies
