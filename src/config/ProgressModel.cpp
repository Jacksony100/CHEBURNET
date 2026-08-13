#include "ProgressModel.h"

#include <algorithm>

namespace cheburnet {
namespace {
constexpr int kPercent[] = {5, 10, 20, 35, 50, 60, 70, 80, 90, 100};
constexpr int kStageCount = static_cast<int>(sizeof(kPercent) / sizeof(kPercent[0]));
} // namespace

int ProgressModel::PercentForStage(Stage s) {
    int idx = static_cast<int>(s);
    if (idx < 0) idx = 0;
    if (idx >= kStageCount) idx = kStageCount - 1;
    return kPercent[idx];
}

void ProgressModel::Reach(Stage s) {
    const int idx = static_cast<int>(s);
    int prev = reached_.load();
    while (idx > prev && !reached_.compare_exchange_weak(prev, idx)) {
        // retry with updated prev
    }
    if (s == Stage::Confirmed) {
        // Confirmed is the ONLY way to reach success / 100%.
        state_.store(RunState::Success);
        anim_.store(100);
    }
}

void ProgressModel::MarkError(std::wstring message) {
    {
        std::scoped_lock lock(msgMutex_);
        error_ = std::move(message);
    }
    // Never overwrite a Success state.
    RunState expected = RunState::Running;
    state_.compare_exchange_strong(expected, RunState::Error);
}

std::wstring ProgressModel::ErrorMessage() const {
    std::scoped_lock lock(msgMutex_);
    return error_;
}

int ProgressModel::Floor() const {
    const int r = reached_.load();
    if (r < 0) return 0;
    return kPercent[std::min(r, kStageCount - 1)];
}

int ProgressModel::Ceiling() const {
    const RunState st = state_.load();
    if (st == RunState::Success) return 100;
    if (st == RunState::Error) return Floor(); // frozen where it stalled
    const int r = reached_.load();
    const int next = r + 1;
    if (next >= kStageCount) return 99; // cannot reach 100 while still Running
    return std::min(kPercent[next] - 1, 99);
}

int ProgressModel::Tick(int step) {
    if (step < 0) step = 0;
    const int f = Floor();
    const int c = Ceiling();
    int v = anim_.load();
    if (v < f) v = f;
    if (v < c) v = std::min(c, v + step);
    if (v > c) v = c;
    anim_.store(v);
    return v;
}

int ProgressModel::Display() const {
    const int f = Floor();
    const int c = Ceiling();
    int v = anim_.load();
    if (v < f) v = f;
    if (v > c) v = c;
    return v;
}

} // namespace cheburnet
