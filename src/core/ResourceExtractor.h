#pragma once
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

#include "RuntimePaths.h"
#include "../config/Strategies.h"
#include "../update/RuntimeStateStore.h"

namespace cheburnet {

struct ExtractionResult {
    bool         ok = false;
    std::wstring error;          // user-facing message when !ok
    int          extracted = 0;  // files written this run
    int          present = 0;    // files already valid / present
    std::wstring failedResource; // logical name of the first failure
};

struct RuntimeFile {
    std::wstring path;
    std::uint64_t size = 0;
    std::string sha256;
};
struct CleanupResult {
    bool ok = false;
    int removed = 0;
    std::wstring error;
};
// Pure deletion policy. A missing/unreadable state authorizes no deletions.
bool BuildRuntimeKeepSet(const update::StateResult& state, std::wstring_view active,
                         std::wstring_view embedded, std::wstring_view runningVersion,
                         std::vector<std::wstring>& keep);

// Inventory validation does not change files/ACLs and never follows links.
// Missing allowlisted files are permitted during extraction, extras are not.
bool ValidateRuntimeInventory(const std::wstring& root,
                              const std::vector<std::wstring>& allowed,
                              std::wstring& unexpected);

// Extracts the embedded runtime payload (winws.exe, WinDivert, cygwin, .bin
// patterns, immutable vendor list files) into the embedded versioned runtime
// directory. Every resource is verified by size + SHA-256 every launch and is
// re-extracted if wrong. User lists live separately under user\lists.
//
// Verification is performed on the in-memory RCDATA buffer BEFORE writing, so a
// tampered payload is never materialised on disk (avoids TOCTOU).
class ResourceExtractor {
public:
    explicit ResourceExtractor(const RuntimePaths& paths) : paths_(paths) {}

    ExtractionResult EnsureExtracted();

    // Validate all files of an externally installed runtime against its
    // protected runtime-manifest.json and load its strict strategy catalog.
    ExtractionResult VerifyInstalledRuntime(std::vector<RuntimeStrategy>& catalog,
                                             std::vector<RuntimeFile>* files = nullptr) const;
    // Read-only inspection of either embedded or externally installed runtime.
    ExtractionResult VerifyRuntime(std::vector<RuntimeStrategy>& catalog,
                                   std::vector<RuntimeFile>* files = nullptr) const;

    // Re-hash every executable/binary resource on disk against the applicable
    // embedded or installed manifest.
    // Called immediately before launching winws.exe to close any TOCTOU window
    // between extraction and CreateProcessW. Returns false and sets `firstBad`
    // (logical name) on the first size/SHA-256 mismatch or missing file.
    bool VerifyBinaries(std::wstring& firstBad) const;

    // Verify existing directories without adopting untrusted owners or ACLs.
    // All critical runtime directories must already carry the exact policy.
    bool VerifyRuntimeDirs();

    // Remove runtime\<version> directories other than the current one. Never
    // recurses through reparse points (junction/symlink) - it unlinks them.
    // Missing/corrupt state refuses all deletions. Preserve active paths and
    // a verified running version in addition to current/previous/pending/embedded.
    // Returns an explicit error, including partial deletion/enumeration errors.
    CleanupResult CleanupOldVersions();

private:
    const RuntimePaths& paths_;
};

} // namespace cheburnet
