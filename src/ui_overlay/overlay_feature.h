#pragma once

#include <string_view>

#include "appkit/feature.h"

namespace fv {

// Provides the one recording overlay (WaveformOverlay, a top-level window with no parent) as
// ctx.service<WaveformOverlay>(); the controller decides what it shows and when.
// Affinity: UI actor.
class OverlayFeature final : public Feature {
public:
    static constexpr std::string_view kId = "overlay";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
