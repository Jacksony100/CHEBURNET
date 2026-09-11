#include "RuntimeRecovery.h"

#include "../util/StringUtil.h"

namespace cheburnet {

PendingAction DecidePendingRecovery(bool hasPending, bool processValid,
                                    std::wstring_view runningImagePath,
                                    std::wstring_view pendingWinwsPath,
                                    std::wstring_view currentWinwsPath) {
    if (!hasPending) return PendingAction::None;
    if (!processValid) return PendingAction::ClearPendingOnly;
    if (str::IEqualsAscii(runningImagePath, pendingWinwsPath)) {
        return PendingAction::StopPendingRuntimeThenClear;
    }
    if (str::IEqualsAscii(runningImagePath, currentWinwsPath)) {
        return PendingAction::ClearPendingOnly;
    }
    return PendingAction::RefuseUnknownRuntime;
}

IntegrityAction DecideIntegrityRecovery(bool runtimeIsEmbeddedVersion, bool activeVerified,
                                        bool haveTrustedState, bool havePreviousKnownGood) {
    if (runtimeIsEmbeddedVersion) return IntegrityAction::UseEmbedded;
    if (activeVerified) return IntegrityAction::UseInstalled;
    if (!haveTrustedState || !havePreviousKnownGood) return IntegrityAction::Refuse;
    return IntegrityAction::RollbackToPrevious;
}

} // namespace cheburnet
