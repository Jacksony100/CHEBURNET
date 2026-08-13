#include "Animation.h"

#include <chrono>
#include <cwchar>
#include <string>
#include <vector>

namespace cheburnet {
namespace {

struct ChecklistItem {
    int          stageIndex; // Stage value this row completes at
    const wchar_t* label;
};

// Rows shown above the progress bar, each tied to a real pipeline stage.
const ChecklistItem kChecklist[] = {
    {static_cast<int>(Stage::OsCheck),       L"Проверка ОС и архитектуры"},
    {static_cast<int>(Stage::AdminCheck),    L"Проверка прав администратора"},
    {static_cast<int>(Stage::RuntimePrep),   L"Подготовка рабочей директории"},
    {static_cast<int>(Stage::ExtractVerify), L"Извлечение и проверка компонентов"},
    {static_cast<int>(Stage::ConflictCheck), L"Проверка конфликтов"},
    {static_cast<int>(Stage::LaunchProc),    L"Запуск сетевого движка"},
    {static_cast<int>(Stage::ProcAlive),     L"Проверка процесса"},
};
constexpr int kChecklistCount = static_cast<int>(sizeof(kChecklist) / sizeof(kChecklist[0]));

std::wstring PadDots(const std::wstring& label, int targetCol) {
    std::wstring out = label;
    out.push_back(L' ');
    while (static_cast<int>(out.size()) < targetCol) out.push_back(L'.');
    out.push_back(L' ');
    return out;
}

} // namespace

void ConnectionAnimation::Start(int originX, int originY, int barWidth) {
    finished_.store(false);
    thread_ = std::jthread([this, originX, originY, barWidth](std::stop_token st) {
        Run(st, originX, originY, barWidth);
        finished_.store(true);
    });
}

void ConnectionAnimation::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void ConnectionAnimation::Run(std::stop_token st, int originX, int originY, int barWidth) {
    using namespace std::chrono_literals;
    const int checklistY = originY;
    const int labelY = originY + kChecklistCount + 1;
    const int barY = labelY + 1;
    constexpr int kDotCol = 38;

    int frame = 0;
    for (;;) {
        const RunState state = progress_.State();
        const int reached = progress_.ReachedIndex();

        // --- Checklist ---
        for (int i = 0; i < kChecklistCount; ++i) {
            const int y = checklistY + i;
            const int stageIdx = kChecklist[i].stageIndex;
            std::wstring row = L"  [*] " + PadDots(kChecklist[i].label, kDotCol);
            Color rowColor = Color::Green;
            std::wstring status;
            if (reached >= stageIdx) {
                status = L"ДА";
                rowColor = Color::BrightGreen;
            } else if (state == RunState::Error && stageIdx == reached + 1) {
                status = L"СБОЙ";
                rowColor = Color::Red;
            } else if (state == RunState::Running && stageIdx == reached + 1) {
                status = L"...";
                rowColor = Color::Yellow;
            } else {
                status = L"   ";
                rowColor = Color::DarkGray;
            }
            con_.ClearRow(y);
            con_.WriteAt(originX, y, row, Color::Green);
            con_.WriteColored(status + L"   ", rowColor);
        }

        if (state == RunState::Error) {
            break; // freeze immediately on error
        }

        const int percent = progress_.Tick(2);

        // --- "Подключение" + 1..8 dots ---
        const int dots = (frame % 8) + 1;
        std::wstring label = L"  Подключение";
        label.append(static_cast<size_t>(dots), L'.');
        label.append(static_cast<size_t>(8 - dots), L' ');
        con_.WriteAt(originX, labelY, label, Color::Green);

        // --- Segmented bar + percent ---
        int filled = (percent * barWidth) / 100;
        if (filled < 0) filled = 0;
        if (filled > barWidth) filled = barWidth;
        con_.WriteAt(originX, barY, L"  [", Color::DarkGray);
        con_.WriteColored(std::wstring(static_cast<size_t>(filled), L'█'), Color::BrightGreen);
        con_.WriteColored(std::wstring(static_cast<size_t>(barWidth - filled), L'░'),
                          Color::DarkGreen);
        con_.WriteColored(L"] ", Color::DarkGray);
        wchar_t pct[8];
        swprintf(pct, 8, L"%3d%% ", percent);
        con_.WriteColored(pct, Color::White);

        if (state == RunState::Success && percent >= 100) {
            break;
        }
        if (st.stop_requested()) break;

        ++frame;
        std::this_thread::sleep_for(45ms);
    }
}

} // namespace cheburnet
