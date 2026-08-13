#pragma once

namespace cheburnet {

enum class PreflightResult {
    Ok,
    UnsupportedOs,   // not 64-bit or older than Windows 10
    NeedsElevation,  // not running elevated (UAC refused / not admin)
};

// Pure decision used by the launch pipeline. Kept free of Win32 so the UAC and
// OS gates can be unit-tested without an actual elevated/legacy environment.
// OS is checked before elevation, matching the pipeline's stage order.
inline PreflightResult EvaluatePreflight(bool elevated, bool os64bit, bool win10OrGreater) {
    if (!os64bit || !win10OrGreater) return PreflightResult::UnsupportedOs;
    if (!elevated) return PreflightResult::NeedsElevation;
    return PreflightResult::Ok;
}

} // namespace cheburnet
