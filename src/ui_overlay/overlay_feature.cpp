#include "ui_overlay/overlay_feature.h"

#include <memory>

#include "appkit/app_context.h"
#include "ui_overlay/waveform_overlay.h"

namespace fv {

void OverlayFeature::provide(AppContext& context) {
    context.provide<WaveformOverlay>(
        std::make_shared<WaveformOverlay>(&context.settings(), &context.clock()));
}

void OverlayFeature::install(AppContext& /*context*/) {
    // The overlay has no menu, screen or page of its own to register.
}

} // namespace fv
