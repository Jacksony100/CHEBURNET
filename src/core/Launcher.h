#pragma once
#include <string>
#include <vector>

#include "../config/Strategies.h"
#include "../ui/UiEvent.h"
#include "ProcessManager.h"
#include "RuntimePaths.h"

namespace cheburnet {

enum class ConnectKind {
    Connected,               // we started winws and confirmed it is alive
    AlreadyRunningOurs,      // a winws we previously started is already active
    AlreadyRunningService,   // the zapret service is active - do not duplicate
    ForeignWinws,            // a winws not started by CHEBURNET is running
    Error
};

struct ConnectResult {
    ConnectKind  kind = ConnectKind::Error;
    std::wstring message; // short user-facing summary
    std::wstring detail;  // additional context (exit code, stderr tail, log path)
    bool success() const {
        return kind == ConnectKind::Connected || kind == ConnectKind::AlreadyRunningOurs;
    }
};

// One entry in the 5-phase, 20-checkpoint connect model.
struct ConnectCheckpoint {
    int            index;   // 1..20
    int            phase;   // 1..5
    int            percent; // monotonic; only the last is 100
    const wchar_t* label;
};

// The full checkpoint table (pure/const - unit tested for the "100% only at the
// final confirmed checkpoint" invariant).
const std::vector<ConnectCheckpoint>& ConnectCheckpoints();

// Drives the real connect pipeline, posting a UiEvent per checkpoint. 100% is
// only posted after winws.exe is confirmed alive.
class Launcher {
public:
    Launcher(const RuntimePaths& paths, ProcessManager& pm) : paths_(paths), pm_(pm) {}

    ConnectResult Connect(const RuntimeStrategy& strategy, GameFilterMode gameFilter,
                          ui::UiEventQueue& events);

    std::wstring StdoutLogPath() const;
    std::wstring StderrLogPath() const;

private:
    const RuntimePaths& paths_;
    ProcessManager&     pm_;
};

} // namespace cheburnet
