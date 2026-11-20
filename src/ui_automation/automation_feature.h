#pragma once

#include <string_view>

#include "appkit/feature.h"

namespace fv {

// The Automate chapter of Settings: the Actions, Hooks and API & MCP pages and their widgets.
// provide() gives the context the session's ActionRelay (when it has an io executor and a
// credential store), and manual-only action and hook test controls when their dependencies exist.
// Saved hooks are not subscribed to events by this feature. install() registers the Actions,
// Hooks and API & MCP pages and puts their shortcuts on every Settings window's Hotkeys page.
// Affinity: UI actor.
class AutomationFeature final : public Feature {
public:
    static constexpr std::string_view kId = "automation";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
