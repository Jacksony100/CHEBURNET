#include "App.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "../console/Mascot.h"
#include "../core/IntegrityVerifier.h"
#include "../core/PrivilegeManager.h"
#include "../core/ResourceExtractor.h"
#include "../core/StatusProbe.h"
#include "../core/SecureFs.h"
#include "../ui/AnimationClock.h"
#include "../ui/Effects.h"
#include "../ui/UiEvent.h"
#include "../ui/Watchdog.h"
#include "../ui/Widgets.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Version.h"
#include "../util/Win32Error.h"
#include "../update/UpdateManager.h"
#include "../update/RuntimeStateStore.h"
#include "GeneratedManifest.h"
#include "GeneratedProvenance.h"

#pragma comment(lib, "shell32.lib")

namespace cheburnet {
namespace {

using ui::UiColor;

UiColor SevToColor(ui::Severity s) {
    switch (s) {
        case ui::Severity::Ok:    return UiColor::Primary;
        case ui::Severity::Warn:  return UiColor::Warning;
        case ui::Severity::Error: return UiColor::Error;
        case ui::Severity::Info:
        default:                  return UiColor::PrimaryDim;
    }
}

const wchar_t* PhaseName(int phase) {
    switch (phase) {
        case 1:  return L"ФАЗА 01 — СИСТЕМА";
        case 2:  return L"ФАЗА 02 — КОМПОНЕНТЫ";
        case 3:  return L"ФАЗА 03 — КОНФИГУРАЦИЯ";
        case 4:  return L"ФАЗА 04 — ДВИЖОК";
        case 5:  return L"ФАЗА 05 — СОЕДИНЕНИЕ";
        default: return L"ИНИЦИАЛИЗАЦИЯ";
    }
}

std::wstring DotRow(const std::wstring& label, int width) {
    std::wstring r = label;
    r.push_back(L' ');
    while (static_cast<int>(r.size()) < width) r.push_back(L'.');
    return r;
}

std::wstring ToUpperAsciiW(std::wstring_view s) {
    std::wstring o(s);
    for (wchar_t& c : o)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    return o;
}

const wchar_t* GameFilterDisplayName(GameFilterMode mode) {
    switch (mode) {
        case GameFilterMode::Off: return L"выключен";
        case GameFilterMode::All: return L"весь трафик";
        case GameFilterMode::Tcp: return L"только TCP";
        case GameFilterMode::Udp: return L"только UDP";
    }
    return L"выключен";
}

const wchar_t* UpdateModeDisplayName(UpdateMode mode) {
    switch (mode) {
        case UpdateMode::Notify: return L"только уведомлять";
        case UpdateMode::Download: return L"скачивать после подтверждения";
        case UpdateMode::Automatic: return L"автоматический";
        case UpdateMode::Disabled: return L"отключён";
    }
    return L"только уведомлять";
}

std::wstring RuntimeResultDisplayName(std::string_view value) {
    if (value.empty()) return L"нет данных";
    if (value == "embedded-runtime-ready") return L"встроенная среда готова";
    if (value == "interrupted-pending-rolled-back")
        return L"прерванное обновление отменено";
    if (value == "startup-integrity-rollback")
        return L"при запуске восстановлена рабочая версия";
    if (value == "candidate-preflight-ok")
        return L"кандидат прошёл предварительную проверку";
    if (value == "previous-runtime-preflight-failed")
        return L"проверка предыдущей версии не пройдена";
    if (value == "payload-update-committed") return L"обновление движка зафиксировано";
    if (value == "payload-rollback-failed") return L"не удалось подтвердить откат движка";
    if (value == "payload-update-rolled-back") return L"обновление движка отменено";
    return L"неизвестный результат";
}

} // namespace

bool App::DownloadWithProgress(update::UpdateManager& manager,
                               const update::Artifact& artifact,
                               const std::wstring& finalName,
                               const std::wstring& label,
                               std::wstring& stagedPath,
                               std::wstring& error) {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> total{artifact.size};
    bool ok = false;
    std::jthread worker([&] {
        ok = manager.DownloadVerified(
            artifact, finalName, stagedPath, error, &cancel,
            [&](std::uint64_t current, std::uint64_t declared) {
                received.store(current, std::memory_order_relaxed);
                if (declared != 0) total.store(declared, std::memory_order_relaxed);
            });
        done.store(true, std::memory_order_release);
    });
    ui::AnimationClock clock;
    while (!done.load(std::memory_order_acquire)) {
        BeginFrame();
        if (EnsureUsableSize()) {
            DrawHeader(L"// проверенное скачивание");
            const std::uint64_t current = received.load(std::memory_order_relaxed);
            const std::uint64_t expected = total.load(std::memory_order_relaxed);
            const int percent = expected == 0
                                    ? 0
                                    : static_cast<int>(std::min<std::uint64_t>(
                                          99, (current * 100u) / expected));
            ui_->FB().PutText(4, 5, L"Скачивание: " + label, ui_->Attr(UiColor::Primary));
            DrawProgressBar(ui_->FB(), ui_->Th(), 4, 7,
                            std::min(54, ui_->Width() - 10), percent,
                            clock.ElapsedMs(), true);
            const double currentMb = static_cast<double>(current) / (1024.0 * 1024.0);
            const double expectedMb = static_cast<double>(expected) / (1024.0 * 1024.0);
            wchar_t bytes[96];
            ::swprintf(bytes, std::size(bytes), L"%d%%  %.2f / %.2f MiB", percent,
                       currentMb, expectedMb);
            ui_->FB().PutText(4, 9, bytes, ui_->Attr(UiColor::Accent));
            DrawIdleMascot(11, ui_->Height() - 4);
            DrawFooter(L"Esc — отменить загрузку");
        }
        ui_->Present();
        const ui::KeyEvent event = ui_->In().Poll(60);
        if (event.key == ui::Key::Esc) cancel.store(true, std::memory_order_relaxed);
    }
    worker.join();
    return ok;
}

// ========================================================== SPLASH SCREEN ====
void App::ScreenSplash() {
    if (!ui_->Options().animations) return; // instant mode: no splash
    ui::AnimationClock clock;
    const unsigned long long dur = static_cast<unsigned long long>(ui_->Options().scaleMs(950));
    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            const unsigned long long t = clock.ElapsedMs();
            const int cy = ui_->Height() / 2;
            ui_->PutCentered(cy - 3, L"C H E B U R N E T", UiColor::Primary);
            const int barW = std::min(ui_->Width() - 12, 44);
            DrawLoadingBar(fb, th, ui_->CenterX(barW + 2), cy, barW, t);
            std::wstring load = L"ЗАГРУЗКА ";
            load.push_back(ui::Spinner::Frame(th.G(), t, ui_->Options().scaleMs(80)));
            ui_->PutCentered(cy + 2, load, UiColor::Muted);
            DrawFooter(L"Пробел / Ввод — пропустить");
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(30);
        if (ev.key == ui::Key::Space || ev.key == ui::Key::Enter) return;
        if (clock.ElapsedMs() >= dur) return;
    }
}

// ============================================================ BOOT SCREEN ====
void App::ScreenBoot() {
    struct BootLine { std::wstring label; std::wstring result; ui::Severity sev; };

    std::vector<BootLine> lines;
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        const bool mem = ::GlobalMemoryStatusEx(&ms) != 0;
        lines.push_back({L"КАРТА ПАМЯТИ", mem ? L"ГОТОВО" : L"ОШИБКА",
                         mem ? ui::Severity::Ok : ui::Severity::Error});
        lines.push_back({L"ТАБЛИЦА РЕСУРСОВ", kEmbeddedResourceCount > 0 ? L"ПРОВЕРЕНА" : L"ПУСТА",
                         kEmbeddedResourceCount > 0 ? ui::Severity::Ok : ui::Severity::Error});
        SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        lines.push_back({L"КАНАЛ УПРАВЛЕНИЯ", scm ? L"ГОТОВ" : L"ОГРАНИЧЕН",
                         scm ? ui::Severity::Ok : ui::Severity::Warn});
        if (scm) ::CloseServiceHandle(scm);
        lines.push_back({L"КОНСОЛЬ", L"ПОДКЛЮЧЕНА", ui::Severity::Ok});
        lines.push_back({L"КАНАЛ ОБНОВЛЕНИЙ", L"стабильный", ui::Severity::Ok});
        lines.push_back({L"ДВИЖОК", paths_.RuntimeVersion(), ui::Severity::Ok});
    }

    const bool instant = !ui_->Options().animations;
    ui::AnimationClock clock;
    const int stepMs = ui_->Options().scaleMs(180);
    const int wmMs = ui_->Options().scaleMs(30);
    const unsigned long long total =
        static_cast<unsigned long long>(lines.size()) * stepMs + ui_->Options().scaleMs(600);

    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            const unsigned long long t = instant ? total + 1 : clock.ElapsedMs();
            int y = std::max(2, ui_->Height() / 2 - 6);
            ui_->PutCentered(y, L"ЗАПУСК CHEBURNET v" CHEBURNET_VERSION_WSTR, UiColor::Primary);
            y += 2;
            const int baseX = ui_->CenterX(46);
            for (size_t i = 0; i < lines.size(); ++i) {
                const unsigned long long lineT = static_cast<unsigned long long>(i + 1) * stepMs;
                fb.PutText(baseX, y, DotRow(lines[i].label, 40), th.Attr(UiColor::PrimaryDim));
                if (t >= lineT) {
                    fb.PutText(baseX + 41, y, lines[i].result, th.Attr(SevToColor(lines[i].sev)));
                } else if (t + stepMs >= lineT) {
                    fb.PutCh(baseX + 41, y, ui::Spinner::Frame(th.G(), t, ui_->Options().scaleMs(90)),
                             th.Attr(UiColor::Warning));
                }
                ++y;
            }
            const unsigned long long wmStart = static_cast<unsigned long long>(lines.size()) * stepMs;
            if (t >= wmStart) {
                const std::wstring wm = L"C H E B U R N E T";
                const int reveal =
                    ui::Scanline::Revealed(static_cast<int>(wm.size()), t - wmStart, wmMs, instant);
                ui_->PutCentered(y + 1, wm.substr(0, static_cast<size_t>(reveal)), UiColor::Accent);
            }
            DrawFooter(L"Пробел / Ввод — пропустить");
        }
        ui_->Present();
        if (instant) return;
        const ui::KeyEvent ev = ui_->In().Poll(30);
        if (ev.key == ui::Key::Space || ev.key == ui::Key::Enter) return;
        if (clock.ElapsedMs() >= total) return;
    }
}

// ========================================================= CONNECT SCREEN ====
ConnectResult App::ScreenConnect() {
    ui::UiEventQueue q;
    ConnectResult result;
    std::atomic<bool> done{false};
    const bool alreadyConnected = pm_->IsConnectedByUs();
    if (alreadyConnected) {
        operationState_.Complete(AppOperationState::Connected);
    } else if (!operationState_.TryTransition(AppOperationState::Disconnected,
                                               AppOperationState::Connecting)) {
        result.kind = ConnectKind::Error;
        result.message = L"Другая операция CHEBURNET уже выполняется.";
        result.detail = L"Подключение отклонено автоматом состояний (ERROR_BUSY).";
        return result;
    }

    std::jthread worker([&] {
        try {
            Launcher launcher(paths_, *pm_);
            result = launcher.Connect(CurrentStrategy(), config_.gameFilter, q);
        } catch (...) {
            // An exception must never escape the thread entry (that calls
            // std::terminate). Degrade to a clean error outcome.
            result.kind = ConnectKind::Error;
            result.message = L"Внутренняя ошибка при подключении.";
            result.detail = L"Не хватило ресурсов или неожиданный сбой. См. лог.";
            ui::UiEvent e;
            e.type = ui::UiEventType::StageFailed;
            e.severity = ui::Severity::Error;
            e.label = L"Исключение в конвейере подключения";
            q.Push(std::move(e));
            Logger::Error(L"исключение в рабочем потоке подключения");
        }
        operationState_.Complete(result.success() ? AppOperationState::Connected
                                                  : AppOperationState::Error);
        done.store(true);
    });

    int status[21] = {0}; // 0 pending,1 active,2 ok,3 warn,4 failed
    int barPercent = 0;
    int highest = 0;
    int activePhase = 1;
    bool failed = false;
    ui::AnimationClock clock;

    for (;;) {
        for (const ui::UiEvent& e : q.Drain()) {
            if (e.checkpoint >= 1 && e.checkpoint <= 20) {
                if (e.type == ui::UiEventType::StageStarted) status[e.checkpoint] = 1;
                else if (e.type == ui::UiEventType::StageOk)
                    status[e.checkpoint] = (e.severity == ui::Severity::Warn) ? 3 : 2;
                else if (e.type == ui::UiEventType::StageFailed) {
                    status[e.checkpoint] = 4;
                    failed = true;
                }
                highest = std::max(highest, e.checkpoint);
                for (const auto& cpd : ConnectCheckpoints())
                    if (cpd.index == e.checkpoint) activePhase = cpd.phase;
            }
            if (e.percent > barPercent) barPercent = e.percent;
        }

        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            DrawHeader(L"// подключение");

            const MascotArt& art = MascotForWidth(ui_->Width());
            int y = 3;
            const bool showMascot = ui_->Layout() != ui::LayoutMode::Minimal &&
                                    ui_->Height() >= 3 + static_cast<int>(art.lines.size()) + 12;
            if (showMascot) {
                const bool starBright = ui::Blink::On(clock.ElapsedMs(), 500);
                ui::DrawMascot(fb, th, ui_->CenterX(art.width), y, art, false, starBright);
                y += static_cast<int>(art.lines.size()) + 1;
            }

            // Phase label (typewriter for a short phrase).
            const std::wstring phase = PhaseName(activePhase);
            ui::Typewriter tw(phase, 40);
            ui_->PutCentered(y, tw.Visible(clock.ElapsedMs(), !ui_->Options().effTypewriter()),
                             UiColor::Accent);
            y += 2;

            // Scrolling window of the last ~8 checkpoints.
            const auto& CP = ConnectCheckpoints();
            const int feedX = ui_->CenterX(52);
            int first = std::max(1, highest - 7);
            for (int i = first; i <= std::max(highest, 1); ++i) {
                const ConnectCheckpoint& c = CP[static_cast<size_t>(i - 1)];
                wchar_t idx[8];
                ::swprintf(idx, 8, L"[%02d] ", i);
                fb.PutText(feedX, y, idx, th.Attr(UiColor::Muted));
                fb.PutText(feedX + 5, y, DotRow(c.label, 42), th.Attr(UiColor::PrimaryDim));
                const int s = status[i];
                if (s == 2)
                    fb.PutText(feedX + 44, y, L"ДА", th.Attr(UiColor::Primary));
                else if (s == 3)
                    fb.PutText(feedX + 44, y, L"ВНИМ", th.Attr(UiColor::Warning));
                else if (s == 4)
                    fb.PutText(feedX + 44, y, L"СБОЙ", th.Attr(UiColor::Error));
                else if (s == 1)
                    fb.PutCh(feedX + 44, y,
                             ui::Spinner::Frame(th.G(), clock.ElapsedMs(), ui_->Options().scaleMs(90)),
                             th.Attr(UiColor::Warning));
                ++y;
            }
            ++y;

            DrawProgressBar(fb, th, feedX, y, 40, barPercent, clock.ElapsedMs(),
                            !failed && !done.load());
            DrawFooter(failed ? L"подождите..." : L"установка соединения, не закрывайте окно");
        }
        ui_->Present();

        if (done.load() && q.Empty()) break;
        ui_->In().Poll(ui_->Options().scaleMs(55));
    }
    worker.join();

    // ---- Outcome panel ----
    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            DrawHeader(L"// результат");
            int y = 4;
            switch (result.kind) {
                case ConnectKind::Connected:
                case ConnectKind::AlreadyRunningOurs:
                fb.PutText(4, y, L"[ ПОДКЛЮЧЕНО ] " + result.message, ui_->Attr(UiColor::Primary));
                    break;
                case ConnectKind::AlreadyRunningService:
                case ConnectKind::ForeignWinws:
                    fb.PutText(4, y, L"[ ВНИМАНИЕ ] " + result.message, ui_->Attr(UiColor::Warning));
                    break;
                default:
                    fb.PutText(4, y, L"[ ОШИБКА ] " + result.message, ui_->Attr(UiColor::Error));
                    break;
            }
            y += 2;
            size_t pos = 0;
            while (pos <= result.detail.size() && !result.detail.empty()) {
                size_t nl = result.detail.find(L'\n', pos);
                fb.PutText(4, y++,
                           result.detail.substr(pos, nl == std::wstring::npos ? std::wstring::npos
                                                                               : nl - pos),
                           ui_->Attr(UiColor::PrimaryDim));
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
            int mascotTop = y + 1;
            if (!result.success()) {
                fb.PutText(4, y + 1, L"Лог: " + Logger::LogFilePath(), ui_->Attr(UiColor::Muted));
                mascotTop = y + 3;
            }
            DrawIdleMascot(mascotTop, ui_->Height() - 4);
            DrawFooter(result.success() ? L"Ввод — продолжить"
                                        : L"Ввод — меню     D — диагностика");
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(200);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Enter || ev.key == ui::Key::Space) break;
        if (!result.success() && ev.key == ui::Key::Char && (ev.ch == L'd' || ev.ch == L'D')) {
            ScreenDiagnostics();
        }
    }
    if (!result.success()) operationState_.Complete(AppOperationState::Disconnected);
    return result;
}

// ======================================================= CONNECTED SCREEN ====
App::Post App::ScreenConnected(const ConnectResult& r) {
    ui::UiEventQueue wq;
    const int interval = ui_->Options().reducedMotion ? 3000 : 1500;
    ui::Watchdog watchdog(wq, [this] { return pm_->IsConnectedByUs(); }, interval);
    watchdog.Start();

    ui::AnimationClock frame;
    unsigned long long lastCheck = 0;
    int heartbeat = 0;
    bool connectionLost = false;

    // Idle-blink scheduling.
    bool blinkActive = false;
    unsigned long long nextBlink = 3000 + static_cast<unsigned long long>(ui_->Rand().Range(0, 5000));
    unsigned long long blinkEnd = 0;

    const unsigned long pid = r.kind == ConnectKind::AlreadyRunningOurs || pm_->Record().valid()
                                  ? pm_->Record().pid
                                  : 0;

    for (;;) {
        for (const ui::UiEvent& e : wq.Drain()) {
            if (e.type == ui::UiEventType::WatchdogHeartbeat) {
                lastCheck = frame.ElapsedMs();
                heartbeat = 6;
            } else if (e.type == ui::UiEventType::ProcessStopped) {
                connectionLost = true;
                operationState_.Complete(AppOperationState::Error);
            }
        }

        if (connectionLost) {
            watchdog.Stop();
            // Red glitch + CONNECTION LOST + options.
            ui::AnimationClock lostClock;
            for (;;) {
                BeginFrame();
                if (EnsureUsableSize()) {
                    DrawHeader(L"// соединение потеряно");
            std::wstring title = L"СОЕДИНЕНИЕ ПОТЕРЯНО";
                    if (ui_->Options().effGlitch())
                        title = ui::Glitch::Apply(title, ui_->Rand(),
                                                  static_cast<int>(lostClock.ElapsedMs() / 60), 3, 4);
                    ui_->PutCentered(ui_->Height() / 2 - 2, title, UiColor::Error);
                    ui_->PutCentered(ui_->Height() / 2, L"winws.exe завершился неожиданно",
                                     UiColor::Muted);
                    DrawFooter(L"[ R ] Перезапустить   [ D ] Диагностика   [ M ] Меню");
                }
                ui_->Present();
                const ui::KeyEvent ev = ui_->In().Poll(60);
                if (ev.resized) continue;
                if (ev.key == ui::Key::Char) {
                    if (ev.ch == L'r' || ev.ch == L'R') {
                        operationState_.Complete(AppOperationState::Disconnected);
                        return Post::Reconnect;
                    }
                    if (ev.ch == L'm' || ev.ch == L'M') {
                        operationState_.Complete(AppOperationState::Disconnected);
                        return Post::Menu;
                    }
                    if (ev.ch == L'd' || ev.ch == L'D') ScreenDiagnostics();
                } else if (ev.key == ui::Key::Enter || ev.key == ui::Key::Esc) {
                    operationState_.Complete(AppOperationState::Disconnected);
                    return Post::Menu;
                }
            }
        }

        const unsigned long long t = frame.ElapsedMs();
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            DrawHeader(L"// активное соединение");

            wchar_t up[9];
            ui::FormatUptime(t, up);
            const bool hb = heartbeat > 0;
            std::vector<ui::StatusCell> left = {
                {L"СОСТОЯНИЕ  ", L"ПОДКЛЮЧЕНО", UiColor::Primary},
                {L"ДВИЖОК     ", paths_.RuntimeVersion(), UiColor::Accent},
                {L"СТРАТЕГИЯ  ", CurrentStrategy().displayName, UiColor::Accent},
                {L"ИГРОВОЙ    ", GameFilterDisplayName(config_.gameFilter), UiColor::Accent},
                {L"ЦЕЛОСТНОСТЬ", L"ПРОВЕРЕНА", UiColor::Primary},
            };
            std::vector<ui::StatusCell> right = {
                {L"СЕАНС    ", up, UiColor::Accent},
                {L"ПРОЦЕСС  ", L"PID " + std::to_wstring(pid), UiColor::Accent},
                {L"КОНТРОЛЬ ", std::wstring(L"АКТИВЕН ") + th.G().heartbeat,
                 hb ? UiColor::Primary : UiColor::Muted},
                {L"ВЕРСИЯ   ", CHEBURNET_VERSION_WSTR, UiColor::Accent},
            };
            const int panelW = std::min(ui_->Width() - 4, 72);
            int y = 3;
            y += DrawStatusPanel(fb, th, ui_->CenterX(panelW), y, panelW, L" СОСТОЯНИЕ CHEBURNET ",
                                 left, right);
            y += 1;

            const MascotArt& art = MascotForWidth(ui_->Width());
            if (ui_->Layout() != ui::LayoutMode::Minimal &&
                ui_->Height() >= y + static_cast<int>(art.lines.size()) + 6) {
                const bool starBright = ui::Blink::On(t, 1200) && ui_->Options().effMascotIdle();
                ui::DrawMascot(fb, th, ui_->CenterX(art.width), y, art, blinkActive, starBright);
                y += static_cast<int>(art.lines.size()) + 1;
            }

            // Live status ticker (rebuilt each frame so uptime updates).
            ui::Ticker ticker;
            ticker.SetLines({
                L"СЕТЕВОЙ ДВИЖОК ............. РАБОТАЕТ",
                std::wstring(L"ПРОФИЛЬ СТРАТЕГИИ .......... ") + ToUpperAsciiW(CurrentStrategy().displayName),
                L"КОНТРОЛЬ ПРОЦЕССА .......... АКТИВЕН",
                L"ЦЕЛОСТНОСТЬ РЕСУРСОВ ....... ПРОВЕРЕНА",
                std::wstring(L"ВРЕМЯ СЕАНСА ............... ") + up,
            });
            ui_->PutCentered(std::min(y, ui_->Height() - 5), ticker.Current(t, 2500),
                             UiColor::PrimaryDim);
            const std::wstring lc =
                lastCheck ? L"последняя проверка: " + std::to_wstring((t - lastCheck) / 1000) + L" сек назад"
                          : L"ожидание первой проверки...";
            ui_->PutCentered(std::min(y + 1, ui_->Height() - 4), lc, UiColor::Muted);

            DrawFooter(L"[ Ввод ] Меню   [ S ] Статус   [ D ] Диагностика   [ Q ] Выход");
        }
        ui_->Present();

        if (ui_->Options().effMascotIdle()) {
            if (!blinkActive && t >= nextBlink) {
                blinkActive = true;
                blinkEnd = t + 160;
            }
            if (blinkActive && t >= blinkEnd) {
                blinkActive = false;
                nextBlink = t + 3000 + static_cast<unsigned long long>(ui_->Rand().Range(0, 5000));
            }
        }
        if (heartbeat > 0) --heartbeat;

        const ui::KeyEvent ev = ui_->In().Poll(ui_->Options().reducedMotion ? 400 : 120);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Enter || ev.key == ui::Key::Esc) {
            watchdog.Stop();
            return Post::Menu;
        }
        if (ev.key == ui::Key::Char) {
            if (ev.ch == L'q' || ev.ch == L'Q') {
                watchdog.Stop();
                return Post::Quit;
            }
            if (ev.ch == L'd' || ev.ch == L'D') ScreenDiagnostics();
            if (ev.ch == L's' || ev.ch == L'S') {
                std::wstring info = L"Стратегия: " + CurrentStrategy().displayName + L"\n";
                info += L"Игровой фильтр: " + std::wstring(GameFilterDisplayName(config_.gameFilter)) + L"\n";
                info += L"PID winws.exe: " + std::to_wstring(pid) + L"\n";
                info += L"Рабочая среда: " + paths_.RuntimeVersionDir();
                ShowMessage(L"СОСТОЯНИЕ СОЕДИНЕНИЯ", info, UiColor::Primary);
            }
        }
    }
}

// ========================================================== MAIN MENU ========
void App::ScreenMainMenu() {
    ui::Menu menu;
    int savedSel = 0;

    auto buildItems = [&] {
        const bool connected = pm_->IsConnectedByUs();
        std::vector<ui::MenuItem> items;
        items.push_back({connected ? L"Перезапустить соединение" : L"Подключиться",
                         connected ? L"Остановить и снова запустить winws.exe с текущей стратегией"
                                   : L"Запустить winws.exe с выбранной стратегией",
                         true, L'c'});
        items.push_back({L"Отключиться", L"Остановить активный процесс winws.exe", connected, L'x'});
        items.push_back({L"Выбрать стратегию",
                         L"Открыть матрицу стратегий с поиском и описанием", true, L't'});
        items.push_back({std::wstring(L"Настройки игрового фильтра  [") +
                             GameFilterDisplayName(config_.gameFilter) + L"]",
                         L"Режимы: выключен → весь трафик → только TCP → только UDP", true, L'g'});
        items.push_back({L"Состояние системы", L"Права, ОС, службы, конфликты, пути", true, L'v'});
        items.push_back({L"Проверить обновления", L"HTTPS + подписанный ECDSA P-256 манифест", true, L'u'});
        items.push_back({L"Диагностика", L"Проверка компонентов, процесса и служб", true, L'd'});
        items.push_back({L"Журнал событий", L"Просмотр cheburnet.log с фильтром уровней", true, L'l'});
        items.push_back({L"Настройки", L"Политика обновлений и пользовательские параметры", true, L's'});
        items.push_back({L"Обслуживание среды", L"Удалить старые версии рабочей среды", true, L'm'});
        items.push_back({L"О программе", L"Версия, движок, лицензии, авторство", true, L'a'});
        items.push_back({L"Завершить CHEBURNET", L"Выйти (можно оставить соединение активным)", true, L'q'});
        menu.SetItems(std::move(items));
        menu.SetSelected(savedSel);
    };
    buildItems();

    auto act = [&](int index) -> bool { // returns true to exit menu
        switch (index) {
            case 0: {
                if (pm_->IsConnectedByUs()) {
                    const StopResult stopped = StopManagedConnection();
                    if (stopped.status != StopStatus::Stopped &&
                        stopped.status != StopStatus::NotRunning) {
                        ShowMessage(L"ОТКАЗ", L"Текущее соединение не удалось безопасно остановить.",
                                    UiColor::Error);
                        break;
                    }
                }
                if (DoConnectFlow() == Post::Quit) return true;
                break;
            }
            case 1: {
                if (pm_->IsConnectedByUs() && Confirm(L"Остановить активное соединение?")) {
                    StopResult sr = StopManagedConnection();
                    if (sr.status == StopStatus::Stopped)
                        ShowMessage(L"ОТКЛЮЧЕНО", L"Соединение остановлено.", UiColor::Primary);
                    else if (sr.status == StopStatus::IdentityMismatch)
                        ShowMessage(L"ОТКАЗ", L"Процесс не совпал с записью CHEBURNET и НЕ был завершён.",
                                    UiColor::Warning);
                    else
                        ShowMessage(L"ОШИБКА", L"Не удалось остановить процесс.", UiColor::Error);
                }
                break;
            }
            case 2: ScreenStrategy(); break;
            case 3: {
                switch (config_.gameFilter) {
                    case GameFilterMode::Off: config_.gameFilter = GameFilterMode::All; break;
                    case GameFilterMode::All: config_.gameFilter = GameFilterMode::Tcp; break;
                    case GameFilterMode::Tcp: config_.gameFilter = GameFilterMode::Udp; break;
                    case GameFilterMode::Udp: config_.gameFilter = GameFilterMode::Off; break;
                }
                if (!SaveConfig(paths_.ConfigPath(), config_)) {
                    ShowMessage(L"ОШИБКА", L"Не удалось сохранить настройки игрового фильтра.",
                                UiColor::Error);
                    break;
                }
                Logger::Info(L"режим игрового фильтра: " +
                             std::wstring(GameFilterDisplayName(config_.gameFilter)));
                if (pm_->IsConnectedByUs() &&
                    Confirm(L"Игровой фильтр изменён. Перезапустить соединение?")) {
                    const StopResult stopped = StopManagedConnection();
                    if (stopped.status == StopStatus::Stopped ||
                        stopped.status == StopStatus::NotRunning) {
                        if (DoConnectFlow() == Post::Quit) return true;
                    } else {
                        ShowMessage(L"ОТКАЗ", L"Соединение не удалось безопасно остановить.",
                                    UiColor::Error);
                    }
                }
                break;
            }
            case 4: {
                std::wstring s;
                s += L"Права администратора: " +
                     std::wstring(PrivilegeManager::IsElevated() ? L"есть" : L"нет") + L"\n";
                s += L"Windows 10/11 x64: " +
                     std::wstring((probe::Is64BitWindows() && probe::IsWindows10OrGreater()) ? L"да"
                                                                                             : L"нет") +
                     L"\n";
                s += L"Стратегия: " + CurrentStrategy().displayName + L"\n";
                s += L"Игровой фильтр: " +
                     std::wstring(GameFilterDisplayName(config_.gameFilter)) + L"\n";
                const auto svc = probe::QueryService(L"zapret");
                s += L"Служба zapret: " +
                     std::wstring(svc == probe::ServiceState::Running ? L"работает" : L"не мешает") + L"\n";
                auto winws = probe::FindProcesses(L"winws.exe");
                const ProcessRecord statusRecord = pm_->Record();
                const unsigned long our = ProcessManager::TrustedPidForExclusion(
                    statusRecord, paths_.RuntimeRoot());
                int foreign = 0;
                for (auto& p : winws)
                    if (p.pid != our) ++foreign;
                s += L"Посторонний winws.exe: " +
                     std::wstring(foreign ? std::to_wstring(foreign) + L" процесс(ов)" : L"нет") + L"\n";
                s += L"Рабочая среда: " + paths_.RuntimeVersionDir() + L"\n";
                s += L"Логи: " + paths_.LogsDir();
                ShowMessage(L"СОСТОЯНИЕ СИСТЕМЫ", s, UiColor::Primary);
                break;
            }
            case 5: ScreenUpdates(); break;
            case 6: ScreenDiagnostics(); break;
            case 7: ScreenLogs(); break;
            case 8: ScreenSettings(); break;
            case 9: {
                if (Confirm(L"Удалить старые версии рабочей среды (текущая сохранится)?")) {
                    const AppOperationState prior = operationState_.Get();
                    if ((prior != AppOperationState::Disconnected &&
                         prior != AppOperationState::Connected) ||
                        !operationState_.TryTransition(prior, AppOperationState::Updating)) {
                        ShowMessage(L"ОТКАЗ", L"Другая операция уже выполняется.", UiColor::Error);
                        break;
                    }
                    ResourceExtractor extractor(paths_);
                    const int removed = extractor.CleanupOldVersions();
                    operationState_.Complete(prior);
                    ShowMessage(L"ОБСЛУЖИВАНИЕ",
                                L"Удалено старых версий: " + std::to_wstring(removed) + L".",
                                UiColor::Primary);
                }
                break;
            }
            case 10: ScreenAbout(); break;
            case 11: return true;
            default: break;
        }
        return false;
    };

    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            DrawHeader(L"// командный центр");

            // Status line.
            const bool connected = pm_->IsConnectedByUs();
            fb.PutText(2, 3, L"Состояние: ", th.Attr(UiColor::Muted));
            if (connected)
                fb.PutText(13, 3, L"ПОДКЛЮЧЕНО (pid " + std::to_wstring(pm_->Record().pid) + L")",
                           th.Attr(UiColor::Primary));
            else if (probe::ServiceActive(L"zapret"))
                fb.PutText(13, 3, L"активна служба zapret", th.Attr(UiColor::Warning));
            else
                fb.PutText(13, 3, L"отключено", th.Attr(UiColor::Muted));

            const int menuW = std::min(ui_->Width() - 4, 40);
            const int menuX = 4;
            const int menuY = 5;
            fb.Box(menuX, menuY - 1, menuW, static_cast<int>(13) + 2, th.Attr(UiColor::PrimaryDim),
                   th.G(), L" КОМАНДНЫЙ ЦЕНТР ", th.Attr(UiColor::Primary));
            menu.Render(fb, th, menuX + 1, menuY, menuW - 2);

            // Description of selected item to the right.
            const int descX = menuX + menuW + 3;
            if (descX < ui_->Width() - 6 && !menu.Empty()) {
                fb.PutText(descX, menuY, menu.SelectedItem().label, th.Attr(UiColor::Accent));
                fb.PutText(descX, menuY + 2, menu.SelectedItem().description, th.Attr(UiColor::Muted));
            }

            DrawIdleMascot(menuY + 14, ui_->Height() - 4);
            DrawFooter(L"↑ ↓ выбор   Ввод — подтвердить   Esc — выход   буква — быстрый выбор");
        }
        ui_->Present();

        const ui::KeyEvent ev = ui_->In().Poll(ui_->Options().effMascotIdle() ? 120 : 250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Up) { menu.MoveUp(); savedSel = menu.Selected(); }
        else if (ev.key == ui::Key::Down) { menu.MoveDown(); savedSel = menu.Selected(); }
        else if (ev.key == ui::Key::Enter) {
            if (act(menu.Selected())) return;
            buildItems();
        } else if (ev.key == ui::Key::Esc) {
            return;
        } else if (ev.key == ui::Key::Char) {
            const int idx = menu.IndexForHotkey(ev.ch);
            if (idx >= 0) {
                savedSel = idx;
                if (act(idx)) return;
                buildItems();
            }
        }
    }
}

// ======================================================= STRATEGY SCREEN =====
void App::ScreenStrategy() {
    const auto& all = strategies::All();
    std::wstring search;
    int sel = 0;
    int scroll = 0;
    bool showDetails = false;

    auto matches = [&](const RuntimeStrategy& s) {
        if (search.empty()) return true;
        const std::wstring q = str::ToLowerAsciiW(search);
        return str::ToLowerAsciiW(s.displayName).find(q) != std::wstring::npos ||
               str::ToLowerAsciiW(str::ToUtf16(s.id)).find(q) != std::wstring::npos;
    };

    for (;;) {
        std::vector<const RuntimeStrategy*> filtered;
        for (const auto& s : all)
            if (matches(s)) filtered.push_back(&s);
        if (sel >= static_cast<int>(filtered.size())) sel = static_cast<int>(filtered.size()) - 1;
        if (sel < 0) sel = 0;

        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            DrawHeader(L"// матрица стратегий");

            const int boxX = 2, boxY = 3;
            const int boxW = ui_->Width() - 4;
            const int listH = ui_->Height() - 11;
            fb.Box(boxX, boxY, boxW, listH + 4, th.Attr(UiColor::PrimaryDim), th.G(),
                       L" МАТРИЦА СТРАТЕГИЙ ", th.Attr(UiColor::Primary));
            fb.PutText(boxX + 2, boxY + 1, L"Поиск: ", th.Attr(UiColor::Muted));
            fb.PutText(boxX + 9, boxY + 1, search + th.G().cursorBlock, th.Attr(UiColor::Accent));

            const int rowY = boxY + 3;
            const int visible = listH;
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + visible) scroll = sel - visible + 1;
            for (int i = 0; i < visible && (scroll + i) < static_cast<int>(filtered.size()); ++i) {
                const RuntimeStrategy* s = filtered[static_cast<size_t>(scroll + i)];
                const bool cur = s->id == config_.strategyId;
                const bool selected = (scroll + i) == sel;
                const WORD attr = selected ? th.Attr(UiColor::Selected)
                                  : cur     ? th.Attr(UiColor::Primary)
                                            : th.Attr(UiColor::PrimaryDim);
                std::wstring row = selected ? L" > " : (cur ? L" * " : L"   ");
                row += s->displayName;
                if (static_cast<int>(row.size()) < 26) row.append(26 - row.size(), L' ');
                // badge
                const wchar_t* badge = !s->recommended ? L"НЕ РЕКОМЕНДУЕТСЯ"
                                      : s->id == "general" ? L"РЕКОМЕНДУЕТСЯ"
                                                           : L"ПРОФИЛЬ";
                row += badge;
                if (static_cast<int>(row.size()) < boxW - 4)
                    row.append(static_cast<size_t>(boxW - 4) - row.size(), L' ');
                fb.PutText(boxX + 2, rowY + i, row.substr(0, static_cast<size_t>(boxW - 4)), attr);
            }

            // Summary / details of selected.
            int infoY = boxY + listH + 4 + 1;
            if (!filtered.empty()) {
                const RuntimeStrategy* s = filtered[static_cast<size_t>(sel)];
                fb.PutText(2, infoY, s->description, th.Attr(UiColor::Muted));
                if (showDetails) {
                    auto args = strategies::BuildArguments(*s, L"BIN", L"LISTS", config_.gameFilter);
                    fb.PutText(2, infoY + 1, L"Аргументов winws: " + std::to_wstring(args.size()),
                               th.Attr(UiColor::PrimaryDim));
                    if (!args.empty())
                        fb.PutText(2, infoY + 2, args.front().substr(0, static_cast<size_t>(ui_->Width() - 4)),
                                   th.Attr(UiColor::PrimaryDim));
                }
            }
            fb.PutText(2, ui_->Height() - 4,
                        L"Текущая: " + CurrentStrategy().displayName + L"    Игровой фильтр: " +
                             GameFilterDisplayName(config_.gameFilter),
                       th.Attr(UiColor::Muted));
            DrawFooter(L"↑ ↓ выбор   Ввод — применить   Пробел — детали   ввод — поиск   Esc — назад");
        }
        ui_->Present();

        const ui::KeyEvent ev = ui_->In().Poll(250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Esc) return;
        else if (ev.key == ui::Key::Up) { if (sel > 0) --sel; }
        else if (ev.key == ui::Key::Down) { ++sel; }
        else if (ev.key == ui::Key::PageUp) { sel -= 5; if (sel < 0) sel = 0; }
        else if (ev.key == ui::Key::PageDown) { sel += 5; }
        else if (ev.key == ui::Key::Space) { showDetails = !showDetails; }
        else if (ev.key == ui::Key::Backspace) { if (!search.empty()) search.pop_back(); sel = 0; }
        else if (ev.key == ui::Key::Enter) {
            if (!filtered.empty()) {
                const RuntimeStrategy* s = filtered[static_cast<size_t>(sel)];
                if (s->id != config_.strategyId) {
                    config_.strategyId = s->id;
                    if (!SaveConfig(paths_.ConfigPath(), config_)) {
                        ShowMessage(L"ОШИБКА", L"Не удалось сохранить защищённую конфигурацию.",
                                    UiColor::Error);
                        return;
                    }
                    Logger::Info(L"выбрана стратегия: " + str::ToUtf16(s->id));
                    if (pm_->IsConnectedByUs() &&
                        Confirm(L"Стратегия изменена. Перезапустить соединение сейчас?")) {
                        const StopResult stopped = StopManagedConnection();
                        if (stopped.status == StopStatus::Stopped ||
                            stopped.status == StopStatus::NotRunning) {
                            DoConnectFlow();
                        } else {
                            ShowMessage(L"ОТКАЗ", L"Соединение не удалось безопасно остановить.",
                                        UiColor::Error);
                        }
                        return;
                    }
                }
                ShowMessage(L"СТРАТЕГИЯ ПРИМЕНЕНА", L"Активна: " + s->displayName, UiColor::Primary);
                return;
            }
        } else if (ev.key == ui::Key::Char) {
            search.push_back(ev.ch);
            sel = 0;
        }
    }
}

// ==================================================== DIAGNOSTICS SCREEN ======
void App::ScreenDiagnostics() {
    struct Row { std::wstring text; ui::Severity sev; };
    std::vector<Row> rows;
    auto add = [&](const std::wstring& t, ui::Severity s) { rows.push_back({t, s}); };

    const bool os = probe::Is64BitWindows() && probe::IsWindows10OrGreater();
    add(os ? L"[ДА]   Windows 10/11 x64" : L"[X]    Требуется Windows 10/11 x64",
        os ? ui::Severity::Ok : ui::Severity::Error);
    const bool elev = PrivilegeManager::IsElevated();
    add(elev ? L"[ДА]   Права администратора" : L"[X]    Нет прав администратора",
        elev ? ui::Severity::Ok : ui::Severity::Error);
    const bool dir = RuntimePaths::EnsureDir(paths_.BinDir());
    add(dir ? L"[ДА]   Рабочая директория доступна" : L"[X]    Нет доступа к директории",
        dir ? ui::Severity::Ok : ui::Severity::Error);

    ResourceExtractor extractor(paths_);
    ExtractionResult ex = extractor.EnsureExtracted();
    add(ex.ok ? L"[ДА]   Компоненты извлечены и проверены (SHA-256)"
              : L"[X]    Ошибка компонентов",
        ex.ok ? ui::Severity::Ok : ui::Severity::Error);
    std::wstring bad;
    const bool bins = extractor.VerifyBinaries(bad);
    add(bins ? L"[ДА]   Целостность бинарников подтверждена"
             : L"[X]    Бинарник изменён: " + bad,
        bins ? ui::Severity::Ok : ui::Severity::Error);
    const bool winws = RuntimePaths::Exists(paths_.WinwsExePath());
    add(winws ? L"[ДА]   winws.exe на месте" : L"[X]    winws.exe отсутствует",
        winws ? ui::Severity::Ok : ui::Severity::Error);

    if (pm_->IsConnectedByUs()) {
        add(L"[ДА]   Активный winws.exe принадлежит CHEBURNET (PID " +
                std::to_wstring(pm_->Record().pid) + L")",
            ui::Severity::Ok);
    } else {
        add(L"[--]   Активного соединения CHEBURNET нет", ui::Severity::Info);
    }
    const auto svc = probe::QueryService(L"zapret");
    if (svc == probe::ServiceState::Running || svc == probe::ServiceState::StopPending)
        add(L"[?]    Служба zapret активна (возможен конфликт)", ui::Severity::Warn);
    else
        add(L"[ДА]   Служба zapret не мешает", ui::Severity::Ok);
    auto winwsProcs = probe::FindProcesses(L"winws.exe");
    const ProcessRecord diagnosticRecord = pm_->Record();
    const unsigned long our = ProcessManager::TrustedPidForExclusion(
        diagnosticRecord, paths_.RuntimeRoot());
    int foreign = 0;
    for (auto& p : winwsProcs)
        if (p.pid != our) ++foreign;
    add(foreign ? L"[?]    Посторонний winws.exe: " + std::to_wstring(foreign)
                : L"[ДА]   Конфликтующих winws.exe нет",
        foreign ? ui::Severity::Warn : ui::Severity::Ok);
    add(L"[ПРЕДУПРЕЖДЕНИЕ] Ручная проверка сетевого маршрута не выполнялась", ui::Severity::Warn);
    add(L"[СВЕДЕНИЯ] CHEBURNET " CHEBURNET_VERSION_WSTR L" | Движок " +
            std::wstring(upstream::kProvider) + L" " + upstream::kVersion,
        ui::Severity::Info);
    add(L"[СВЕДЕНИЯ] ID исходного релиза " + std::to_wstring(upstream::kReleaseId) +
            L" | фиксация " + str::ToUtf16(upstream::kCommit) +
            L" | импортирован " + upstream::kImportedAt,
        ui::Severity::Info);
    add(L"[СВЕДЕНИЯ] SHA-256 архива " + str::ToUtf16(upstream::kArchiveSha256),
        ui::Severity::Info);
    add(L"[СВЕДЕНИЯ] Активная среда " + paths_.RuntimeVersion() +
            L" | стратегия " + CurrentStrategy().displayName + L" | игровой фильтр " +
            GameFilterDisplayName(config_.gameFilter), ui::Severity::Info);
    const update::StateResult runtimeState = update::LoadRuntimeState(paths_.ActiveRuntimePath());
    if (runtimeState.ok) {
        add(L"[СВЕДЕНИЯ] Предыдущая рабочая среда " +
                (runtimeState.state.previousKnownGood.empty()
                     ? std::wstring(L"нет")
                     : str::ToUtf16(runtimeState.state.previousKnownGood)) +
                L" | последнее обновление " +
                    RuntimeResultDisplayName(runtimeState.state.lastResult),
            ui::Severity::Info);
        add(L"[СВЕДЕНИЯ] SHA-256 манифеста пакета движка " +
                (runtimeState.state.packageSha256.empty()
                     ? str::ToUtf16(upstream::kArchiveSha256)
                     : str::ToUtf16(runtimeState.state.packageSha256)),
            ui::Severity::Info);
    } else {
        add(L"[X]    Состояние активной среды не читается", ui::Severity::Error);
    }
    add(L"[СВЕДЕНИЯ] Канал обновлений стабильный | режим " +
            std::wstring(UpdateModeDisplayName(config_.update.mode)), ui::Severity::Info);

    bool healthy = true;
    for (auto& r : rows)
        if (r.sev == ui::Severity::Error) healthy = false;

    // Save report (no sensitive data).
    const std::wstring reportPath = paths_.DiagnosticsReportPath();
    {
        std::wstring report = L"=== ДИАГНОСТИКА CHEBURNET v" CHEBURNET_VERSION_WSTR L" ===\r\n";
        report += L"CHEBURNET LABS // РАЗРАБОТАНО MARSHAL JACKSONY100\r\n\r\n";
        for (auto& r : rows) report += r.text + L"\r\n";
        report += L"\r\nРЕЗУЛЬТАТ ДИАГНОСТИКИ: ";
        report += healthy ? L"ИСПРАВНО" : L"ОБНАРУЖЕНЫ ПРОБЛЕМЫ";
        report += L"\r\n";
        const std::string utf8 = "\xEF\xBB\xBF" + str::ToUtf8(report);
        const securefs::Result savedReport = securefs::AtomicWrite(reportPath, utf8);
        if (!savedReport.ok) {
            add(L"[X]    Не удалось сохранить защищённый отчёт диагностики",
                ui::Severity::Error);
            healthy = false;
        }
    }

    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            DrawHeader(L"// диагностика");
            int y = 3;
            for (auto& r : rows) {
                if (y >= ui_->Height() - 6) break;
                fb.PutText(4, y++, r.text, ui_->Attr(SevToColor(r.sev)));
            }
            ++y;
            fb.PutText(4, y++, std::wstring(L"РЕЗУЛЬТАТ ДИАГНОСТИКИ: ") + (healthy ? L"ИСПРАВНО" : L"ЕСТЬ ПРОБЛЕМЫ"),
                       ui_->Attr(healthy ? UiColor::Primary : UiColor::Warning));
            fb.PutText(4, y, L"Отчёт: " + reportPath, ui_->Attr(UiColor::Muted));
            DrawIdleMascot(y + 2, ui_->Height() - 4);
            DrawFooter(L"Ввод — назад");
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(ui_->Options().effMascotIdle() ? 120 : 250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Enter || ev.key == ui::Key::Esc) return;
    }
}

// ========================================================== LOGS SCREEN ======
void App::ScreenLogs() {
    ui::LogViewer viewer;
    // Parse cheburnet.log tail into lines.
    {
        std::vector<ui::LogLine> lines;
        const std::wstring logPath = Logger::LogFilePath();
        HANDLE h = securefs::ValidateProtectedObject(
                       logPath, securefs::ObjectKind::File, true).ok
                       ? ::CreateFileW(logPath.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
                       : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION object{};
            if (!::GetFileInformationByHandle(h, &object) ||
                (object.dwFileAttributes &
                 (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
                object.nNumberOfLinks != 1) {
                ::CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            }
        }
        LARGE_INTEGER size{};
        if (h != INVALID_HANDLE_VALUE &&
            (!::GetFileSizeEx(h, &size) || size.QuadPart < 0)) {
            ::CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        if (h != INVALID_HANDLE_VALUE) {
            const long long kMax = 128 * 1024;
            long long toRead = size.QuadPart > kMax ? kMax : size.QuadPart;
            if (toRead > 0) {
                LARGE_INTEGER off;
                off.QuadPart = size.QuadPart - toRead;
                if (!::SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
                    ::CloseHandle(h);
                    h = INVALID_HANDLE_VALUE;
                }
            }
        }
        if (h != INVALID_HANDLE_VALUE) {
            const long long kMax = 128 * 1024;
            const long long toRead = size.QuadPart > kMax ? kMax : size.QuadPart;
            std::string content;
            content.resize(static_cast<size_t>(toRead));
            DWORD rd = 0;
            bool readOk = true;
            if (toRead > 0) {
                readOk = ::ReadFile(h, content.data(), static_cast<DWORD>(toRead), &rd, nullptr) &&
                         rd == static_cast<DWORD>(toRead);
            }
            content.resize(rd);
            ::CloseHandle(h);
            if (!readOk) content.clear();
            size_t pos = 0;
            while (pos < content.size()) {
                size_t eol = content.find('\n', pos);
                std::string raw = content.substr(pos, eol == std::string::npos ? std::string::npos
                                                                               : eol - pos);
                pos = (eol == std::string::npos) ? content.size() : eol + 1;
                std::wstring w = str::ToUtf16(str::Trim(raw));
                if (w.empty()) continue;
                ui::LogLine ln;
                // Format: [ts] [LEVEL] message
                ui::Severity sev = ui::Severity::Info;
                if (w.find(L"[ОШИБКА]") != std::wstring::npos ||
                    w.find(L"[ERROR]") != std::wstring::npos) {
                    sev = ui::Severity::Error;
                } else if (w.find(L"[ПРЕДУПРЕЖДЕНИЕ]") != std::wstring::npos ||
                           w.find(L"[WARN]") != std::wstring::npos) {
                    sev = ui::Severity::Warn;
                }
                ln.sev = sev;
                // extract time and level cheaply
                if (w.size() > 24 && w[0] == L'[') {
                    ln.time = w.substr(12, 8); // HH:MM:SS
                    size_t lb = w.find(L'[', 1);
                    size_t rb = (lb != std::wstring::npos) ? w.find(L']', lb) : std::wstring::npos;
                    if (lb != std::wstring::npos && rb != std::wstring::npos) {
                        ln.level = sev == ui::Severity::Error ? L"ОШИБКА"
                                   : sev == ui::Severity::Warn ? L"ПРЕДУПР."
                                                               : L"СВЕДЕНИЯ";
                        ln.text = (rb + 2 <= w.size()) ? w.substr(rb + 2) : L"";
                    } else {
                        ln.text = w;
                    }
                } else {
                    ln.text = w;
                }
                lines.push_back(std::move(ln));
            }
        }
        viewer.SetLines(std::move(lines));
    }

    for (;;) {
        BeginFrame();
        if (EnsureUsableSize()) {
            auto& fb = ui_->FB();
            auto& th = ui_->Th();
            DrawHeader(L"// журнал событий");
            fb.PutText(2, 3, L"Фильтр: " + viewer.FilterName(), th.Attr(UiColor::Accent));
            const int listY = 5;
            const int listH = ui_->Height() - listY - 4;
            fb.Box(2, listY - 1, ui_->Width() - 4, listH + 2, th.Attr(UiColor::PrimaryDim), th.G());
            viewer.Render(fb, th, 4, listY, ui_->Width() - 8, listH);
            DrawFooter(L"↑ ↓ / PgUp PgDn — прокрутка   F — фильтр   O — открыть файл   Esc — назад");
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(250);
        if (ev.resized) continue;
        if (ev.key == ui::Key::Esc || ev.key == ui::Key::Enter) return;
        else if (ev.key == ui::Key::Up) viewer.ScrollUp(1);
        else if (ev.key == ui::Key::Down) viewer.ScrollDown(1);
        else if (ev.key == ui::Key::PageUp) viewer.ScrollUp(10);
        else if (ev.key == ui::Key::PageDown) viewer.ScrollDown(10);
        else if (ev.key == ui::Key::Char) {
            if (ev.ch == L'f' || ev.ch == L'F') viewer.CycleFilter();
            if (ev.ch == L'o' || ev.ch == L'O') {
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.lpVerb = L"open";
                const std::wstring p = Logger::LogFilePath();
                sei.lpFile = p.c_str();
                sei.nShow = SW_SHOWNORMAL;
                if (::ShellExecuteExW(&sei) && sei.hProcess) ::CloseHandle(sei.hProcess);
            }
        }
    }
}

// ======================================================= UPDATE SCREEN ======
void App::ScreenUpdates() {
    const AppOperationState prior = operationState_.Get();
    if ((prior != AppOperationState::Disconnected && prior != AppOperationState::Connected) ||
        !operationState_.TryTransition(prior, AppOperationState::Updating)) {
        ShowMessage(L"ОБНОВЛЕНИЕ ОТКЛОНЕНО", L"Другая операция CHEBURNET уже выполняется.",
                    UiColor::Error);
        return;
    }
    ShowMessage(L"ПРОВЕРКА ОБНОВЛЕНИЙ",
                L"[СЕТЬ] HTTPS / системный прокси\n[ПОДПИСЬ] Отдельная подпись ECDSA P-256\n"
                L"Проверка выполняется. Сетевой сбой не мешает рабочей среде.",
                UiColor::Primary);
    update::UpdateManager manager(paths_);
    const update::CheckResult check = manager.CheckNow(
        config_.update.mode != UpdateMode::Disabled);
    std::wstring details = check.message + L"\n\n";
    details += L"CHEBURNET: " CHEBURNET_VERSION_WSTR;
        details += L"\nДвижок: " + paths_.RuntimeVersion();
    if (check.status == update::CheckStatus::Available) {
        const bool payloadSkipped =
            !check.manifest.payload.version.empty() &&
            check.manifest.payload.version == config_.update.skippedPayloadVersion;
        details += L"\nДоступно: CHEBURNET " + str::ToUtf16(check.manifest.launcher.version);
        details += L", движок " + str::ToUtf16(check.manifest.payload.version);
        details += L"\n\nПрименение привилегированного пакета требует явного подтверждения.";
        if (payloadSkipped && check.launcher != update::Eligibility::Upgrade) {
            details += L"\nЭта версия движка пропущена в настройках. Сбросьте пропуск повторным подтверждением.";
            if (Confirm(L"Сбросить пропуск этой версии движка?")) {
                config_.update.skippedPayloadVersion.clear();
                if (!SaveConfig(paths_.ConfigPath(), config_))
                    details += L"\nНе удалось сохранить изменение политики пропуска.";
            }
        } else if (Confirm(L"Скачать проверенные обновления в защищённую область?")) {
            std::wstring staged, error;
            bool any = false;
            if (check.launcher == update::Eligibility::Upgrade) {
                if (DownloadWithProgress(manager, check.manifest.launcher,
                                         L"CHEBURNET-new.exe", L"CHEBURNET",
                                         staged, error)) {
                    any = true;
                    // Intentionally safer fallback from release-spec §15: the
                    // running elevated EXE is never overwritten in-place.
                    SHELLEXECUTEINFOW sei{};
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = L"open";
                    sei.lpFile = paths_.UpdatesDir().c_str();
                    sei.nShow = SW_SHOWNORMAL;
                    ::ShellExecuteExW(&sei);
                } else {
                    details += L"\nСкачивание CHEBURNET: " + error;
                }
            }
            if (check.payload == update::Eligibility::Upgrade && !payloadSkipped) {
                if (DownloadWithProgress(
                        manager, check.manifest.payload,
                        L"engine-" + str::ToUtf16(check.manifest.payload.version) + L".cbpkg",
                        L"движок " + str::ToUtf16(check.manifest.payload.version),
                        staged, error)) {
                    any = true;
                    if (Confirm(L"Применить проверенный пакет движка сейчас?")) {
                        const update::PayloadApplyResult applied = manager.ApplyPayload(
                            check.manifest.payload, staged, *pm_, config_.strategyId,
                            config_.gameFilter, check.manifest.keyId);
                        details += L"\nДвижок: " + applied.message;
                        if (applied.status == update::PayloadApplyStatus::RolledBack ||
                            applied.status == update::PayloadApplyStatus::RollbackFailed) {
                            operationState_.Complete(AppOperationState::RollingBack);
                        }
                        if (applied.status == update::PayloadApplyStatus::Applied) {
                            paths_ = RuntimePaths(str::ToUtf16(applied.candidateVersion));
                            pm_ = std::make_unique<ProcessManager>(paths_);
                            if (!strategies::Exists(config_.strategyId)) {
                                config_.strategyId = "general";
                                if (!SaveConfig(paths_.ConfigPath(), config_))
                                    details += L"\nНе удалось сохранить резервную стратегию в конфигурации.";
                                details += L"\nВыбранная стратегия отсутствует; применена general.";
                            }
                        } else if (applied.status == update::PayloadApplyStatus::RolledBack) {
                            pm_ = std::make_unique<ProcessManager>(paths_);
                        }
                    } else {
                        details += L"\nПакет движка помещён в защищённую область; применение отменено пользователем.";
                    }
                } else {
                    details += L"\nСкачивание движка: " + error;
                }
            } else if (check.payload == update::Eligibility::Upgrade && payloadSkipped) {
                details += L"\nДвижок " + str::ToUtf16(check.manifest.payload.version) +
                           L" пропущен согласно сохранённой политике.";
            }
            if (any) details += L"\nПроверенные файлы: " + paths_.UpdatesDir();
        } else if (check.payload == update::Eligibility::Upgrade &&
                   Confirm(L"Пропустить только движок " +
                           str::ToUtf16(check.manifest.payload.version) + L"?")) {
            config_.update.skippedPayloadVersion = check.manifest.payload.version;
            if (SaveConfig(paths_.ConfigPath(), config_))
                details += L"\nВерсия движка помечена как пропущенная.";
            else
                details += L"\nНе удалось сохранить политику пропуска.";
        }
    }
    operationState_.Complete(pm_->IsConnectedByUs() ? AppOperationState::Connected
                                                    : AppOperationState::Disconnected);
    ShowMessage(check.status == update::CheckStatus::Rejected ? L"ОБНОВЛЕНИЕ ОТКЛОНЕНО"
                                                              : L"ОБНОВЛЕНИЯ",
                details,
                check.status == update::CheckStatus::Rejected ? UiColor::Error
                : check.status == update::CheckStatus::Available ? UiColor::Warning
                                                                  : UiColor::Primary);
}

// ====================================================== SETTINGS SCREEN =====
void App::ScreenSettings() {
    for (;;) {
        std::wstring mode = UpdateModeDisplayName(config_.update.mode);
        std::wstring body = L"Канал обновлений: стабильный\nРежим обновлений: " + mode +
                            L"\nПроверка при запуске: " +
                            std::wstring(config_.update.checkOnStart ? L"ВКЛ" : L"ВЫКЛ") +
                            L"\n\n[M] режим   [C] проверка при запуске   [Esc] назад";
        BeginFrame();
        if (EnsureUsableSize()) {
            DrawHeader(L"// настройки");
            int y = 4;
            size_t pos = 0;
            while (pos <= body.size()) {
                const size_t nl = body.find(L'\n', pos);
                ui_->FB().PutText(4, y++, body.substr(pos, nl == std::wstring::npos
                                                            ? std::wstring::npos : nl - pos),
                                  ui_->Attr(UiColor::PrimaryDim));
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
            DrawIdleMascot(y + 1, ui_->Height() - 4);
            DrawFooter(L"M / C изменить   Esc назад");
        }
        ui_->Present();
        const ui::KeyEvent ev = ui_->In().Poll(250);
        if (ev.key == ui::Key::Esc) return;
        if (ev.key == ui::Key::Char && (ev.ch == L'm' || ev.ch == L'M')) {
            switch (config_.update.mode) {
                case UpdateMode::Notify: config_.update.mode = UpdateMode::Download; break;
                case UpdateMode::Download: config_.update.mode = UpdateMode::Automatic; break;
                case UpdateMode::Automatic: config_.update.mode = UpdateMode::Disabled; break;
                case UpdateMode::Disabled: config_.update.mode = UpdateMode::Notify; break;
            }
            if (!SaveConfig(paths_.ConfigPath(), config_))
                ShowMessage(L"ОШИБКА", L"Не удалось сохранить режим обновлений.", UiColor::Error);
        } else if (ev.key == ui::Key::Char && (ev.ch == L'c' || ev.ch == L'C')) {
            config_.update.checkOnStart = !config_.update.checkOnStart;
            if (!SaveConfig(paths_.ConfigPath(), config_))
                ShowMessage(L"ОШИБКА", L"Не удалось сохранить проверку при запуске.", UiColor::Error);
        }
    }
}

// ========================================================= ABOUT SCREEN ======
void App::ScreenAbout() {
    ShowMessage(
        L"CHEBURNET v" CHEBURNET_VERSION_WSTR,
        L"Управление соединением для Windows 10/11 x64\n"
        L"\n"
        L"Движок: Flowseal/zapret-discord-youtube " + paths_.RuntimeVersion() + L"\n"
        L"Основные компоненты: bol-van/winws, WinDivert\n"
        L"Программа запуска: C++20 / Win32\n"
        L"\n"
        L"CHEBURNET LABS\n"
        L"РАЗРАБОТАНО MARSHAL JACKSONY100\n"
        L"\n"
        L"Сборка: выпуск x64\n"
        L"Программа: Jacksony100 / участники CHEBURNET\n"
        L"Авторы исходных проектов: Flowseal, bol-van, WinDivert и Cygwin\n"
        L"Лицензии: THIRD_PARTY_NOTICES.md и LICENSES/",
        UiColor::Primary, L"Ввод — назад");
}

} // namespace cheburnet
