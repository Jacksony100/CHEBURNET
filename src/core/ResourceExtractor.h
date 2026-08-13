#pragma once
#include <string>
#include <vector>

#include "RuntimePaths.h"
#include "../config/Strategies.h"

namespace cheburnet {

struct ExtractionResult {
    bool         ok = false;
    std::wstring error;          // user-facing message when !ok
    int          extracted = 0;  // files written this run
    int          present = 0;    // files already valid / present
    std::wstring failedResource; // logical name of the first failure
};

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
    ExtractionResult VerifyInstalledRuntime(std::vector<RuntimeStrategy>& catalog) const;

    // Re-hash every executable/binary resource on disk against the applicable
    // embedded or installed manifest.
    // Called immediately before launching winws.exe to close any TOCTOU window
    // between extraction and CreateProcessW. Returns false and sets `firstBad`
    // (logical name) on the first size/SHA-256 mismatch or missing file.
    bool VerifyBinaries(std::wstring& firstBad) const;

    // Harden every CHEBURNET directory: reset the OWNER to Administrators and
    // apply a PROTECTED, inheritable DACL (SYSTEM/Administrators full, Users
    // read+execute). Resetting the owner is essential: an object owner keeps
    // implicit WRITE_DAC, so a standard user who pre-creates the tree could
    // otherwise rewrite the DACL. Call BEFORE extracting so files inherit the
    // locked-down ACL. Returns false if a critical directory cannot be secured;
    // the launch pipeline treats that as fatal.
    bool HardenRuntimeDirs();

    // Remove runtime\<version> directories other than the current one. Never
    // recurses through reparse points (junction/symlink) - it unlinks them.
    // Returns the number of old version directories removed.
    int CleanupOldVersions();

private:
    const RuntimePaths& paths_;
};

} // namespace cheburnet
