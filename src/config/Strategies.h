#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"

namespace cheburnet {

// A launch strategy ported from a BAT file. `argTemplate` holds winws.exe
// argument tokens with placeholders %BIN%, %LISTS% and %GAME% that are
// substituted programmatically at launch time.
struct RuntimeStrategy {
    std::string               id;
    std::wstring              sourceFile;
    std::wstring              displayName;
    std::wstring              description;
    bool                      recommended = true;
    std::vector<std::wstring> argTemplate;
};

namespace strategies {

// All strategies in registry order (general first).
const std::vector<RuntimeStrategy>& All();

// Lookup by stable id. Returns nullptr if unknown.
const RuntimeStrategy* Find(std::string_view id);

// True if id exists in the registry.
bool Exists(std::string_view id);

// The default strategy (general == general.bat). Always present.
const RuntimeStrategy& Default();

// Parse a protected, generated strategy catalog without changing global
// state. The same strict parser is used for updater preflight and startup.
bool LoadCatalogFile(const std::wstring& path, std::vector<RuntimeStrategy>& output,
                     std::wstring& error);

// Switch the process-wide registry after a runtime has passed every update
// gate. RestoreEmbedded is used for the built-in last-known-good fallback.
void ActivateCatalog(std::vector<RuntimeStrategy> catalog);
void RestoreEmbeddedCatalog();

// Dynamic GameFilter value: enabled -> "1024-65535", disabled -> "12".
std::wstring GameFilterTcpValue(GameFilterMode mode);
std::wstring GameFilterUdpValue(GameFilterMode mode);

// Build the final winws.exe argument vector by substituting placeholders.
// binDir / listsDir are absolute directories WITHOUT a trailing separator.
std::vector<std::wstring> BuildArguments(const RuntimeStrategy& s, const std::wstring& binDir,
                                         const std::wstring& listsDir, GameFilterMode gameFilter,
                                         const std::wstring& userListsDir = L"");

} // namespace strategies
} // namespace cheburnet
