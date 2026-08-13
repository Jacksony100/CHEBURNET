#include "Config.h"

#include <windows.h>

#include "../util/Json.h"
#include "../util/Version.h"
#include "../core/SecureFs.h"

namespace cheburnet {
namespace {

const json::Value* Member(const json::Value* object, std::string_view key) {
    return object ? object->Find(key) : nullptr;
}

const std::string* StringMember(const json::Value* object, std::string_view key) {
    const json::Value* value = Member(object, key);
    return value ? value->AsString() : nullptr;
}

const bool* BoolMember(const json::Value* object, std::string_view key) {
    const json::Value* value = Member(object, key);
    return value ? value->AsBool() : nullptr;
}

std::optional<GameFilterMode> ParseGameFilter(std::string_view value) {
    if (value == "off") return GameFilterMode::Off;
    if (value == "all") return GameFilterMode::All;
    if (value == "tcp") return GameFilterMode::Tcp;
    if (value == "udp") return GameFilterMode::Udp;
    return std::nullopt;
}

std::optional<UpdateMode> ParseUpdateMode(std::string_view value) {
    if (value == "notify") return UpdateMode::Notify;
    if (value == "download") return UpdateMode::Download;
    if (value == "automatic") return UpdateMode::Automatic;
    if (value == "disabled") return UpdateMode::Disabled;
    return std::nullopt;
}

} // namespace

std::string_view GameFilterModeName(GameFilterMode mode) {
    switch (mode) {
        case GameFilterMode::Off: return "off";
        case GameFilterMode::All: return "all";
        case GameFilterMode::Tcp: return "tcp";
        case GameFilterMode::Udp: return "udp";
    }
    return "off";
}

std::string_view UpdateModeName(UpdateMode mode) {
    switch (mode) {
        case UpdateMode::Notify: return "notify";
        case UpdateMode::Download: return "download";
        case UpdateMode::Automatic: return "automatic";
        case UpdateMode::Disabled: return "disabled";
    }
    return "notify";
}

bool IsValidStrategyIdSyntax(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

ConfigParseResult ParseConfig(std::string_view input) {
    ConfigParseResult result;
    json::ParseOptions options;
    options.maxBytes = 1024 * 1024;
    options.maxDepth = 16;
    options.maxValues = 512;
    json::ParseResult parsed = json::Parse(input, options);
    if (!parsed.ok || !parsed.value.AsObject()) {
        result.error = parsed.ok ? "config is not a JSON object" : parsed.error;
        return result;
    }

    const std::string* strategy = StringMember(&parsed.value, "strategy");
    if (!strategy || !IsValidStrategyIdSyntax(*strategy)) {
        result.error = "missing or invalid top-level 'strategy'";
        return result;
    }
    result.config.strategyId = *strategy;

    // Current schema: a four-state string. Legacy bool migrates false->off and
    // true->all. A nested decoy key can never affect this scoped lookup.
    if (const json::Value* game = Member(&parsed.value, "gameFilter")) {
        if (const std::string* mode = game->AsString()) {
            const auto parsedMode = ParseGameFilter(*mode);
            if (!parsedMode) {
                result.error = "invalid top-level 'gameFilter' value";
                return result;
            }
            result.config.gameFilter = *parsedMode;
        } else if (const bool* legacy = game->AsBool()) {
            result.config.gameFilter = *legacy ? GameFilterMode::All : GameFilterMode::Off;
        } else {
            result.error = "invalid top-level 'gameFilter' type";
            return result;
        }
    }

    if (const json::Value* ui = Member(&parsed.value, "ui"); ui) {
        if (!ui->AsObject()) {
            result.error = "invalid top-level 'ui' type";
            return result;
        }
        if (const bool* value = BoolMember(ui, "animations")) result.config.ui.animations = *value;
        if (const bool* value = BoolMember(ui, "reduced_motion"))
            result.config.ui.reducedMotion = *value;
        if (const bool* value = BoolMember(ui, "glitch_effects")) result.config.ui.glitch = *value;
        if (const bool* value = BoolMember(ui, "mascot_idle")) result.config.ui.mascotIdle = *value;
        if (const std::string* value = StringMember(ui, "animation_speed"); value &&
            (*value == "slow" || *value == "normal" || *value == "fast"))
            result.config.ui.speed = *value;
        if (const std::string* value = StringMember(ui, "unicode_mode"); value &&
            (*value == "auto" || *value == "unicode" || *value == "ascii"))
            result.config.ui.unicodeMode = *value;
    }

    if (const json::Value* update = Member(&parsed.value, "update"); update) {
        if (!update->AsObject()) {
            result.error = "invalid top-level 'update' type";
            return result;
        }
        if (const std::string* channel = StringMember(update, "channel"); channel &&
            *channel == "stable")
            result.config.update.channel = *channel;
        if (const std::string* mode = StringMember(update, "mode")) {
            // Unknown mode fails safely to documented notify-only default.
            if (auto parsedMode = ParseUpdateMode(*mode)) result.config.update.mode = *parsedMode;
        }
        if (const bool* check = BoolMember(update, "check_on_start"))
            result.config.update.checkOnStart = *check;
        if (const json::Value* skipped = Member(update, "skipped_payload_version")) {
            if (const std::string* value = skipped->AsString())
                result.config.update.skippedPayloadVersion = *value;
            else if (!skipped->IsNull())
                result.config.update.skippedPayloadVersion.clear();
        }
    }

    result.ok = true;
    return result;
}

std::string SerializeConfig(const Config& config) {
    std::string out;
    out += "{\n";
    out += "  \"version\": \"" CHEBURNET_VERSION_STR "\",\n";
    out += "  \"strategy\": " + json::Quote(config.strategyId) + ",\n";
    out += "  \"gameFilter\": " + json::Quote(GameFilterModeName(config.gameFilter)) + ",\n";
    out += "  \"update\": {\n";
    out += "    \"channel\": \"stable\",\n";
    out += "    \"mode\": " + json::Quote(UpdateModeName(config.update.mode)) + ",\n";
    out += std::string("    \"check_on_start\": ") +
           (config.update.checkOnStart ? "true" : "false") + ",\n";
    out += "    \"skipped_payload_version\": ";
    out += config.update.skippedPayloadVersion.empty()
               ? "null\n"
               : json::Quote(config.update.skippedPayloadVersion) + "\n";
    out += "  },\n";
    out += "  \"ui\": {\n";
    out += std::string("    \"animations\": ") + (config.ui.animations ? "true" : "false") + ",\n";
    out += "    \"animation_speed\": " + json::Quote(config.ui.speed) + ",\n";
    out += std::string("    \"reduced_motion\": ") +
           (config.ui.reducedMotion ? "true" : "false") + ",\n";
    out += std::string("    \"glitch_effects\": ") + (config.ui.glitch ? "true" : "false") + ",\n";
    out += std::string("    \"mascot_idle\": ") + (config.ui.mascotIdle ? "true" : "false") + ",\n";
    out += "    \"unicode_mode\": " + json::Quote(config.ui.unicodeMode) + "\n";
    out += "  }\n";
    out += "}\n";
    return out;
}

Config LoadConfig(const std::wstring& path) {
    Config defaults;
    if (!securefs::ValidateProtectedObject(path, securefs::ObjectKind::File, true).ok)
        return defaults;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return defaults;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        ::CloseHandle(file);
        return defaults;
    }
    std::string content;
    char buffer[4096];
    DWORD read = 0;
    bool ioOk = true;
    for (;;) {
        if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            ioOk = false;
            break;
        }
        if (read == 0) break;
        content.append(buffer, read);
        if (content.size() > 1024u * 1024u) {
            ioOk = false;
            break;
        }
    }
    ::CloseHandle(file);
    if (!ioOk) return defaults;
    const ConfigParseResult parsed = ParseConfig(content);
    return parsed.ok ? parsed.config : defaults;
}

namespace {

ConfigParseResult ReadAndParseConfigFile(const std::wstring& path) {
    ConfigParseResult result;
    if (!securefs::ValidateObject(path, securefs::ObjectKind::File, true).ok) {
        result.error = "unsafe config object";
        return result;
    }
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = "cannot open config";
        return result;
    }
    std::string content;
    char buffer[4096];
    DWORD read = 0;
    bool ioOk = true;
    for (;;) {
        if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            ioOk = false;
            break;
        }
        if (read == 0) break;
        content.append(buffer, read);
        if (content.size() > 1024u * 1024u) {
            ioOk = false;
            break;
        }
    }
    ::CloseHandle(file);
    if (!ioOk) {
        result.error = "config read/size rejected";
        return result;
    }
    return ParseConfig(content);
}

} // namespace

bool SaveConfig(const std::wstring& path, const Config& config) {
    const std::string content = SerializeConfig(config);
    return securefs::AtomicWrite(path, content).ok;
}

bool MigrateLegacyConfig(const std::wstring& legacyPath, const std::wstring& newPath,
                         std::wstring& detail) {
    if (::GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        detail = L"new config already exists";
        return true;
    }
    const DWORD attrs = ::GetFileAttributesW(legacyPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            detail = L"legacy config absent";
            return true;
        }
        detail = L"cannot query legacy config";
        return false;
    }
    if (!securefs::ValidateObject(legacyPath, securefs::ObjectKind::File, true).ok) {
        detail = L"legacy config is unsafe";
        return false;
    }
    const ConfigParseResult parsed = ReadAndParseConfigFile(legacyPath);
    if (!parsed.ok) {
        detail = L"legacy config parse/validation failed";
        return false;
    }
    if (!SaveConfig(newPath, parsed.config)) {
        detail = L"cannot save migrated config";
        return false;
    }
    detail = L"legacy config imported; source retained";
    return true;
}

} // namespace cheburnet
