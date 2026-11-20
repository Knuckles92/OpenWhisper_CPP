#pragma once

#include <string_view>
#include <vector>

#include "appkit/feature.h"

namespace fv {

// The Settings window feature: provides SettingsWindow, and opens it from the main window's
// settingsRequested (File > Settings, the footer's Settings button, the tray's Settings),
// settingsDestinationRequested (File > Downloads…, a page link), hotkeysRequested
// (File > Hotkeys) and engineHelpRequested. The pages register themselves, from their own
// features, through ctx.settingsPages(); this feature registers Recording and the App pages
// (AppPages), the Walkthroughs page and the Fast judgments page, and provides AppPages and the
// SettingsPagesSignals they emit on, to which it also forwards the window's hotkeyCaptureSuspended,
// and the JudgeKeys the Fast judgments page and the API keys page's TypeSafe row look keys up and
// test through.
class SettingsFeature final : public Feature {
public:
    std::string_view id() const override { return "settings"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
