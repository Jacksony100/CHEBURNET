#pragma once
#include <atomic>
#include <mutex>
#include <string>

namespace cheburnet {

// Real launch stages, each mapped to a milestone percentage. The animation may
// smoothly interpolate BELOW the next milestone but can only display 100% after
// Reach(Confirmed), which is the single path to success.
enum class Stage {
    Start = 0,       // 5   application started
    OsCheck,         // 10  OS / architecture checked
    AdminCheck,      // 20  admin rights verified
    RuntimePrep,     // 35  runtime directory prepared
    ExtractVerify,   // 50  resources extracted + verified
    ConflictCheck,   // 60  conflicting processes / services checked
    ArgsPrep,        // 70  strategy arguments prepared
    LaunchProc,      // 80  winws.exe launched
    ProcAlive,       // 90  process liveness checked
    Confirmed        // 100 confirmed successful start
};

enum class RunState { Running, Success, Error };

// Thread-safe. The launch worker calls Reach()/MarkError(); the animation thread
// calls Tick()/Display()/State().
class ProgressModel {
public:
    // Mark a stage as reached (monotonic - never goes backwards).
    void Reach(Stage s);

    // Freeze progress and record a user-facing error.
    void MarkError(std::wstring message);

    RunState State() const { return state_.load(); }
    std::wstring ErrorMessage() const;

    // Highest stage index reached (-1 before any). Used by the animation to
    // render the live checklist.
    int ReachedIndex() const { return reached_.load(); }

    // Lowest percentage guaranteed reached.
    int Floor() const;
    // Highest percentage the animation may currently display (<100 while Running).
    int Ceiling() const;

    // Advance the animated value toward the ceiling by up to `step`, clamped to
    // [Floor, Ceiling]. Returns the new display value. 100 is only ever returned
    // after Reach(Confirmed).
    int Tick(int step);
    int Display() const;

    static int PercentForStage(Stage s);

private:
    std::atomic<int>       reached_{-1};
    std::atomic<int>       anim_{0};
    std::atomic<RunState>  state_{RunState::Running};
    mutable std::mutex     msgMutex_;
    std::wstring           error_;
};

} // namespace cheburnet
