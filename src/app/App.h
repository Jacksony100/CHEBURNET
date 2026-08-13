#pragma once
#include <memory>
#include <string>
#include <string_view>

#include "../config/Config.h"
#include "../config/Strategies.h"
#include "../core/Launcher.h"
#include "../core/ProcessManager.h"
#include "../core/RuntimePaths.h"
#include "../ui/AnimationClock.h"
#include "../ui/Theme.h"
#include "../ui/UiContext.h"
#include "../update/UpdateManager.h"
#include "OperationState.h"

namespace cheburnet {

// Command-line overrides (highest priority over config.json ui block).
struct CliFlags {
    bool noAnimation = false;
    bool reducedMotion = false;
    bool asciiOnly = false;
    bool debugUi = false;
};

// Top-level controller. Owns runtime paths, config, the winws ProcessManager and
// the UI context; drives the boot -> connect -> connected -> menu screen flow on
// a single UI thread. Business logic stays in core/; this only composes it with
// the ui/ presentation layer.
class App {
public:
    App();
    explicit App(const CliFlags& flags);
    ~App();

    int Run();

private:
    const RuntimeStrategy& CurrentStrategy() const;
    ui::UiOptions          BuildUiOptions() const;

    // ---- chrome / helpers (App.cpp) ----
    void BeginFrame();
    void DrawHeader(std::wstring_view subtitle);
    void DrawFooter(std::wstring_view hints);
    // Persistent mascot for the main screens: centred in the free space between
    // topY and bottomLimit, with idle blink/glint + star flicker. Draws nothing
    // if it does not fit. Not used on settings/data screens (strategy, logs).
    void DrawIdleMascot(int topY, int bottomLimit);
    bool EnsureUsableSize(); // true if usable; draws the "enlarge window" note otherwise
    bool Confirm(std::wstring_view prompt);
    void ShowMessage(std::wstring_view title, std::wstring_view body, ui::UiColor titleColor,
                     std::wstring_view hint = L"Enter — назад");

    // ---- screens (UiScreens.cpp) ----
    enum class Post { Menu, Quit, Reconnect };
    void          ScreenSplash();
    void          ScreenBoot();
    ConnectResult ScreenConnect();
    Post          ScreenConnected(const ConnectResult& r);
    void          ScreenMainMenu();
    void          ScreenStrategy();
    void          ScreenDiagnostics();
    void          ScreenLogs();
    void          ScreenAbout();
    void          ScreenUpdates();
    void          CheckUpdatesOnStart();
    bool          DownloadWithProgress(update::UpdateManager& manager,
                                       const update::Artifact& artifact,
                                       const std::wstring& finalName,
                                       const std::wstring& label,
                                       std::wstring& stagedPath,
                                       std::wstring& error);
    void          ScreenSettings();
    Post          DoConnectFlow(); // connect animation + connected screen
    void          HandleExit();
    StopResult    StopManagedConnection();

    RuntimePaths                    paths_;
    Config                          config_;
    CliFlags                        cli_;
    std::unique_ptr<ProcessManager> pm_;
    std::unique_ptr<ui::UiContext>  ui_;
    OperationState                  operationState_;

    // Shared idle-mascot animation state (continuous across screens).
    ui::AnimationClock              mascotClock_;
    bool                            mascotBlink_ = false;
    unsigned long long              mascotNextBlink_ = 3000;
    unsigned long long              mascotBlinkEnd_ = 0;
};

} // namespace cheburnet
