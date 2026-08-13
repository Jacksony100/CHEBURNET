#pragma once
#include <string>
#include <string_view>

namespace cheburnet {

// UI preferences (config.json "ui" block). Strings are validated on load.
struct UiSettings {
    bool        animations = true;
    bool        reducedMotion = false;
    bool        glitch = true;
    bool        mascotIdle = true;
    std::string speed = "normal";       // slow | normal | fast
    std::string unicodeMode = "auto";   // auto | unicode | ascii
};

enum class GameFilterMode { Off, All, Tcp, Udp };

enum class UpdateMode { Notify, Download, Automatic, Disabled };

struct UpdateSettings {
    std::string channel = "stable";
    UpdateMode  mode = UpdateMode::Notify;
    bool        checkOnStart = true;
    std::string skippedPayloadVersion;
};

// Persisted user configuration (%ProgramData%\CHEBURNET\user\config.json).
struct Config {
    std::string strategyId = "general"; // default == general.bat equivalent
    GameFilterMode gameFilter = GameFilterMode::Off;
    UpdateSettings update;
    UiSettings  ui;
};

std::string_view GameFilterModeName(GameFilterMode mode);
std::string_view UpdateModeName(UpdateMode mode);

struct ConfigParseResult {
    bool        ok = false;
    Config      config;
    std::string error;
};

// Pure parse of a UTF-8 JSON string. Validates structure and that strategyId is
// a syntactically valid identifier ([A-Za-z0-9_-], non-empty). Does NOT check
// that the strategy exists in the registry - callers do that separately.
ConfigParseResult ParseConfig(std::string_view json);

// Pure serialise to compact, human-readable UTF-8 JSON.
std::string SerializeConfig(const Config& cfg);

// Disk helpers. LoadConfig always returns a usable Config: on any parse/IO error
// it returns defaults. If `knownStrategy` is provided and the parsed strategy is
// unknown, it is reset to "general".
Config LoadConfig(const std::wstring& path);
bool   SaveConfig(const std::wstring& path, const Config& cfg);

// One-time validated import from the legacy root config.json into user\.
// The source is never deleted automatically.
bool MigrateLegacyConfig(const std::wstring& legacyPath, const std::wstring& newPath,
                         std::wstring& detail);

// True if id is a syntactically valid strategy identifier.
bool IsValidStrategyIdSyntax(std::string_view id);

} // namespace cheburnet
