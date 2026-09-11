#include "App.h"

#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>

#include "../console/Mascot.h"
#include "../core/PrivilegeManager.h"
#include "../core/ResourceExtractor.h"
#include "../update/RuntimeStateStore.h"
#include "RuntimeRecovery.h"
#include "../update/UpdateManager.h"
#include "../ui/Widgets.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"
#include "GeneratedProvenance.h"

namespace cheburnet {
namespace {

BOOL WINAPI CtrlHandler(DWORD type) {
    // Runs on its own OS thread - must NOT touch ProcessManager (data race). The
    // winws Job Object has no kill-on-close, so winws survives console close.
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            return TRUE; // ignore: don't kill the launcher mid-operation
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            Logger::Warn(L"консоль закрывается — winws оставлен работать по политике объекта заданий");
            Logger::Shutdown();
            return FALSE;
        default:
            return FALSE;
    }
}

void SetCursorVisible(bool visible) {
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci{};
    if (::GetConsoleCursorInfo(out, &ci)) {
        ci.bVisible = visible ? TRUE : FALSE;
        ::SetConsoleCursorInfo(out, &ci);
    }
}

} // namespace

App::App() : App(CliFlags{}) {}

App::App(const CliFlags& flags) : cli_(flags) {
    if (paths_.ProgramDataRoot().empty()) {
        throw std::runtime_error("не удалось получить доверенный путь ProgramData");
    }
    const update::StateResult state = update::LoadRuntimeState(paths_.ActiveRuntimePath());
    if (!state.ok && !state.missing) {
        throw std::runtime_error("доверенное состояние рабочей среды недействительно");
    }
    if (state.ok && !state.state.pending.empty()) {
        const RuntimePaths pendingPaths(str::ToUtf16(state.state.pending));
        const RuntimePaths currentPaths(str::ToUtf16(state.state.current));
        ProcessManager recovery(paths_);
        const ProcessRecord active = recovery.Record();
        // Решение отделено от исполнения: таблица переходов проверяется тестом
        // runtimerecovery без прав администратора и без ProgramData.
        const PendingAction action = DecidePendingRecovery(
            true, active.valid(), active.imagePath, pendingPaths.WinwsExePath(),
            currentPaths.WinwsExePath());
        if (action == PendingAction::RefuseUnknownRuntime) {
            // Never normalize an unexplained running version by merely
            // clearing pending state. That would make process identity and
            // active-runtime.json disagree.
            throw std::runtime_error("работающая среда не является ни текущей, ни ожидающей");
        }
        if (action == PendingAction::StopPendingRuntimeThenClear) {
            const StopResult stopped = recovery.Stop();
            if (stopped.status != StopStatus::Stopped &&
                stopped.status != StopStatus::NotRunning) {
                throw std::runtime_error("не удалось остановить прерванную ожидающую среду");
            }
            Logger::Error(L"прерванная ожидающая среда остановлена перед восстановлением состояния");
        }
        update::StoredRuntimeState recovered = state.state;
        recovered.pending.clear();
        recovered.lastResult = "interrupted-pending-rolled-back";
        if (!update::SaveRuntimeState(paths_.ActiveRuntimePath(), recovered)) {
            throw std::runtime_error("не удалось очистить прерванное ожидающее состояние");
        }
    }
    if (paths_.RuntimeVersion() != upstream::kVersion) {
        std::vector<RuntimeStrategy> catalog;
        ResourceExtractor extractor(paths_);
        ExtractionResult verified = extractor.VerifyInstalledRuntime(catalog);
        const IntegrityAction integrity = DecideIntegrityRecovery(
            false, verified.ok, state.ok, !state.state.previousKnownGood.empty());
        if (integrity != IntegrityAction::UseInstalled) {
            if (integrity == IntegrityAction::Refuse) {
                throw std::runtime_error("активная среда не прошла проверку, версия для отката отсутствует");
            }
            RuntimePaths fallback(str::ToUtf16(state.state.previousKnownGood));
            std::vector<RuntimeStrategy> fallbackCatalog;
            ResourceExtractor fallbackExtractor(fallback);
            ExtractionResult fallbackVerified;
            if (fallback.RuntimeVersion() == upstream::kVersion) {
                strategies::RestoreEmbeddedCatalog();
                fallbackVerified = fallbackExtractor.EnsureExtracted();
            } else {
                fallbackVerified = fallbackExtractor.VerifyInstalledRuntime(fallbackCatalog);
            }
            if (!fallbackVerified.ok) {
                throw std::runtime_error("активная и предыдущая рабочая среды не прошли проверку");
            }
            update::StoredRuntimeState rolledBack = state.state;
            rolledBack.current = state.state.previousKnownGood;
            rolledBack.previousKnownGood.clear();
            rolledBack.pending.clear();
            rolledBack.lastResult = "startup-integrity-rollback";
            if (!update::SaveRuntimeState(paths_.ActiveRuntimePath(), rolledBack)) {
                throw std::runtime_error("не удалось зафиксировать состояние отката при запуске");
            }
            paths_ = std::move(fallback);
            if (!fallbackCatalog.empty()) strategies::ActivateCatalog(std::move(fallbackCatalog));
            Logger::Error(L"целостность активной среды нарушена; восстановлена предыдущая рабочая версия");
        } else {
            strategies::ActivateCatalog(std::move(catalog));
        }
    } else {
        strategies::RestoreEmbeddedCatalog();
    }
    std::wstring migrationDetail;
    if (!MigrateLegacyConfig(paths_.ProgramDataRoot() + L"\\config.json",
                             paths_.ConfigPath(), migrationDetail)) {
        Logger::Warn(L"миграция старой конфигурации отклонена: " + migrationDetail);
    } else if (migrationDetail == L"старая конфигурация импортирована; исходный файл сохранён") {
        Logger::Info(migrationDetail);
    }
    config_ = LoadConfig(paths_.ConfigPath());
    if (!strategies::Exists(config_.strategyId)) {
        Logger::Warn(L"неизвестная стратегия в конфигурации, сброс на стандартную: " +
                     str::ToUtf16(config_.strategyId));
        config_.strategyId = "general";
        if (!SaveConfig(paths_.ConfigPath(), config_))
            throw std::runtime_error("не удалось сохранить переход на стандартную стратегию");
    }
    pm_ = std::make_unique<ProcessManager>(paths_);
    operationState_.Complete(pm_->IsConnectedByUs() ? AppOperationState::Connected
                                                    : AppOperationState::Disconnected);
}

App::~App() = default;

const RuntimeStrategy& App::CurrentStrategy() const {
    if (const RuntimeStrategy* s = strategies::Find(config_.strategyId)) return *s;
    return strategies::Default();
}

ui::UiOptions App::BuildUiOptions() const {
    ui::UiOptions o;
    o.animations = config_.ui.animations;
    o.reducedMotion = config_.ui.reducedMotion;
    o.glitch = config_.ui.glitch;
    o.mascotIdle = config_.ui.mascotIdle;
    o.speed = config_.ui.speed == "slow"   ? ui::AnimSpeed::Slow
              : config_.ui.speed == "fast" ? ui::AnimSpeed::Fast
                                           : ui::AnimSpeed::Normal;
    o.asciiOnly = (config_.ui.unicodeMode == "ascii");
    // CLI overrides win.
    if (cli_.noAnimation) o.animations = false;
    if (cli_.reducedMotion) o.reducedMotion = true;
    if (cli_.asciiOnly) o.asciiOnly = true;
    o.debugUi = cli_.debugUi;
    return o;
}

// ---- chrome helpers --------------------------------------------------------

void App::BeginFrame() {
    ui_->SyncSize();
    ui_->FB().Clear(ui_->Attr(ui::UiColor::Default));
}

void App::DrawHeader(std::wstring_view subtitle) {
    auto& fb = ui_->FB();
    auto& th = ui_->Th();
    fb.PutText(2, 0, L"CHEBURNET", th.Attr(ui::UiColor::Primary));
    if (!subtitle.empty()) fb.PutText(13, 0, subtitle, th.Attr(ui::UiColor::Muted));
    const std::wstring ver = L"v" CHEBURNET_VERSION_WSTR;
    fb.PutText(ui_->Width() - static_cast<int>(ver.size()) - 2, 0, ver, th.Attr(ui::UiColor::Muted));
    fb.HLine(2, 1, ui_->Width() - 4, th.G().h, th.Attr(ui::UiColor::PrimaryDim));
}

void App::DrawFooter(std::wstring_view hints) {
    auto& fb = ui_->FB();
    auto& th = ui_->Th();
    const int h = ui_->Height();
    fb.HLine(2, h - 3, ui_->Width() - 4, th.G().h, th.Attr(ui::UiColor::PrimaryDim));
    fb.PutText(2, h - 2, hints, th.Attr(ui::UiColor::Muted));
    const std::wstring brand = ui::BrandingLine(ui_->Layout());
    fb.PutText(ui_->Width() - static_cast<int>(brand.size()) - 2, h - 1, brand,
               th.Attr(ui::UiColor::Muted));
}

void App::DrawIdleMascot(int topY, int bottomLimit) {
    if (ui_->Layout() == ui::LayoutMode::Minimal) return; // no room in minimal
    const int avail = bottomLimit - topY;
    if (avail < 9) return;

    const MascotArt& full = MascotFull();
    const MascotArt& compact = MascotCompact();
    const MascotArt* art = nullptr;
    if (avail >= static_cast<int>(full.lines.size()) && ui_->Width() >= full.width + 6)
        art = &full;
    else if (avail >= static_cast<int>(compact.lines.size()))
        art = &compact;
    if (!art) return;

    const int rows = static_cast<int>(art->lines.size());
    const int y = topY + (avail - rows) / 2;
    const int x = ui_->CenterX(art->width);

    bool blink = false;
    bool starBright = false;
    if (ui_->Options().effMascotIdle()) {
        const unsigned long long t = mascotClock_.ElapsedMs();
        if (!mascotBlink_ && t >= mascotNextBlink_) {
            mascotBlink_ = true;
            mascotBlinkEnd_ = t + 160;
        }
        if (mascotBlink_ && t >= mascotBlinkEnd_) {
            mascotBlink_ = false;
            mascotNextBlink_ = t + 3000 + static_cast<unsigned long long>(ui_->Rand().Range(0, 5000));
        }
        blink = mascotBlink_;
        starBright = (t % 1500) < 160; // brief red-star twinkle
    }
    ui::DrawMascot(ui_->FB(), ui_->Th(), x, y, *art, blink, starBright);
}

bool App::EnsureUsableSize() {
    if (!ui_->TooSmall()) return true;
    auto& fb = ui_->FB();
    fb.Clear(ui_->Attr(ui::UiColor::Default));
    ui_->PutCentered(ui_->Height() / 2 - 1, L"Окно слишком маленькое", ui::UiColor::Warning);
    ui_->PutCentered(ui_->Height() / 2 + 1, L"Увеличьте окно минимум до 100x30", ui::UiColor::Muted);
    return false;
}

void App::ShowMessage(std::wstring_view title, std::wstring_view body, ui::UiColor titleColor,
                      std::wstring_view hint) {
    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            DrawHeader(L"");
            int y = 4;
            ui_->FB().PutText(4, y, title, ui_->Attr(titleColor));
            y += 2;
            // Body may contain \n.
            size_t pos = 0;
            while (pos <= body.size()) {
                size_t nl = body.find(L'\n', pos);
                std::wstring_view line = body.substr(pos, nl == std::wstring_view::npos
                                                              ? std::wstring_view::npos
                                                              : nl - pos);
                ui_->FB().PutText(4, y++, line, ui_->Attr(ui::UiColor::PrimaryDim));
                if (nl == std::wstring_view::npos) break;
                pos = nl + 1;
            }
            DrawIdleMascot(y + 1, ui_->Height() - 4);
            DrawFooter(hint);
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(ui_->Options().effMascotIdle() ? 120 : 250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Enter || ev.key == ui::Key::Esc || ev.key == ui::Key::Space) return;
    }
}

bool App::Confirm(std::wstring_view prompt) {
    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            const int boxW = std::min(ui_->Width() - 8, 66);
            const int boxX = ui_->CenterX(boxW);
            const int boxY = ui_->Height() / 2 - 2;
            fb.FillRect(boxX, boxY, boxW, 5, L' ', th.Attr(ui::UiColor::Default));
            fb.Box(boxX, boxY, boxW, 5, th.Attr(ui::UiColor::Warning), th.G(), L" ПОДТВЕРЖДЕНИЕ ",
                   th.Attr(ui::UiColor::Warning));
            fb.PutText(boxX + 3, boxY + 1, prompt, th.Attr(ui::UiColor::Primary));
            fb.PutText(boxX + 3, boxY + 3, L"[ Enter ] Да     [ Esc ] Отмена",
                       th.Attr(ui::UiColor::Muted));
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Enter) return true;
        if (ev.key == ui::Key::Esc) return false;
    }
}

// ---- run flow --------------------------------------------------------------

int App::Run() {
    ::SetConsoleTitleW(CHEBURNET_WINDOW_TITLE);
    ui_ = std::make_unique<ui::UiContext>(BuildUiOptions());
    SetCursorVisible(false);
    mascotClock_.Reset();
    ::SetConsoleCtrlHandler(CtrlHandler, TRUE);

    Logger::Info(L"интерфейс запущен, стратегия=" + str::ToUtf16(config_.strategyId));

    if (!PrivilegeManager::IsElevated()) {
        ShowMessage(L"ТРЕБУЮТСЯ ПРАВА АДМИНИСТРАТОРА",
                    L"Запустите CHEBURNET от имени администратора\n"
                    L"(правая кнопка мыши → «Запуск от имени администратора»).",
                    ui::UiColor::Error, L"любая клавиша — выход");
        SetCursorVisible(true);
        return 2;
    }

    ScreenSplash();
    ScreenBoot();
    // Проверка обновлений уходит в фон до подключения: недоступный DNS, прокси
    // или сеть не должны выглядеть как зависание и не должны задерживать
    // подключение ни на миллисекунду.
    StartUpdateCheck();
    const Post p = DoConnectFlow();
    if (p == Post::Menu) ScreenMainMenu();
    HandleExit();

    // Останавливаем фоновую проверку до разрушения чего-либо, на что она может
    // ссылаться. Cancel() идемпотентен, деструктор вызовет его повторно.
    if (updateCheck_) updateCheck_->Cancel();
    SetCursorVisible(true);
    Logger::Info(L"интерфейс завершён");
    return 0;
}

void App::StartUpdateCheck() {
    if (!config_.update.checkOnStart || config_.update.mode == UpdateMode::Disabled) return;
    updateCheck_ = std::make_unique<update::UpdateCheckService>(paths_);
    if (!updateCheck_->Start()) {
        Logger::Warn(L"фоновая проверка обновлений не запустилась");
        updateCheck_.reset();
    }
}

void App::PollUpdateCheck() {
    if (!updateCheck_) return;
    update::CheckResult check;
    if (!updateCheck_->TryTakeResult(check)) return;
    // Результат пришёл из фонового потока уже готовым значением: фоновый код
    // никогда не рисует сам и ничего не меняет в интерфейсе.
    if (check.status == update::CheckStatus::Available &&
        check.manifest.payload.version != config_.update.skippedPayloadVersion) {
        ShowMessage(L"ДОСТУПНО ОБНОВЛЕНИЕ",
                    L"Проверенный манифест обновления предлагает CHEBURNET " +
                        str::ToUtf16(check.manifest.launcher.version) + L" и движок " +
                        str::ToUtf16(check.manifest.payload.version) +
                        L".\nПрименение доступно в меню «Проверить обновления».",
                    ui::UiColor::Warning);
    } else if (check.status == update::CheckStatus::Rejected) {
        ShowMessage(L"ОБНОВЛЕНИЕ ОТКЛОНЕНО", check.message, ui::UiColor::Error);
    }
    // Offline/current остаются намеренно молчаливыми: запуск обязан оставаться
    // рабочим с уже установленной проверенной средой.
}

App::Post App::DoConnectFlow() {
    for (;;) {
        ConnectResult r = ScreenConnect();
        if (!r.success()) return Post::Menu;
        const Post p = ScreenConnected(r);
        if (p == Post::Reconnect) {
            const StopResult stopped = StopManagedConnection();
            if (stopped.status != StopStatus::Stopped &&
                stopped.status != StopStatus::NotRunning) return Post::Menu;
            continue;
        }
        return p; // Menu or Quit
    }
}

StopResult App::StopManagedConnection() {
    AppOperationState current = operationState_.Get();
    if (current == AppOperationState::Disconnected) {
        if (pm_->IsConnectedByUs()) return {StopStatus::Failed, ERROR_INVALID_DATA};
        return {StopStatus::NotRunning, 0};
    }
    if ((current != AppOperationState::Connected && current != AppOperationState::Error) ||
        !operationState_.TryTransition(current, AppOperationState::Disconnecting)) {
        return {StopStatus::Failed, ERROR_BUSY};
    }
    StopResult result = pm_->Stop();
    operationState_.Complete(
        result.status == StopStatus::Stopped || result.status == StopStatus::NotRunning
            ? AppOperationState::Disconnected
            : AppOperationState::Error);
    return result;
}

void App::HandleExit() {
    if (pm_->IsConnectedByUs()) {
        const bool leave =
            Confirm(L"Оставить соединение активным после выхода? (Enter — да, Esc — остановить)");
        if (leave) {
            pm_->Detach();
            Logger::Info(L"выход: winws оставлен работать по выбору пользователя");
        } else {
            StopResult sr = StopManagedConnection();
            if (sr.status == StopStatus::Stopped) Logger::Info(L"выход: winws остановлен пользователем");
        }
    }
}

} // namespace cheburnet
