#pragma once

#include <string_view>

#include "appkit/feature.h"

namespace fv {

// Provides RuntimeSetup, which M1.16's startup asks to settle a missing speech runtime or model,
// and GuidedSetup, the first-run guided setup and its offer. Seeds guided_setup_state
// (guided_setup_state::initialize) when it provides. Registers no screen, menu or page: the
// File menu's "Guided setup…" is the shell's, and opens the setup window through
// MainWindow::guidedSetupRequested.
// Affinity: UI actor.
class SetupFeature final : public Feature {
public:
    static constexpr std::string_view kId = "setup";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
