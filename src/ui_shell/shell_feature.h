#pragma once

#include <string_view>

#include "appkit/feature.h"

namespace fv {

class MainWindow;
class SystemTray;

// The shell's entries in the registries: the title bar's File, View and Help items and the
// tray menu (ui_qt/main_window.py _setup_menu, ui_qt/system_tray.py _setup_menu), in
// appkit/order.h's order, and the Upload File screen (the engine bench until the real tab
// exists). MainWindow and SystemTray give the shell's own items their behaviour; the other items
// are there for the features that own them to connect.
//
// It also provides the MainWindow (ctx.service<MainWindow>()), built in provide() with its
// chrome only so that features can plug into it while they install; Bootstrap completes it
// (MainWindow::completeSetup) once every feature has registered its screens and menu items.
// Affinity: UI actor.
class ShellFeature final : public Feature {
public:
    static constexpr std::string_view kId = "shell";
    // The Upload File screen's key in the ScreenRegistry.
    static constexpr char kUploadScreen[] = "upload";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

// The connections the shell makes between its window and its tray (the UIController's, for the
// items M0 has): Exit in the tray quits through the window, so the close-to-tray rule cannot
// veto it.
void connectShell(MainWindow& window, SystemTray& tray);

} // namespace fv
