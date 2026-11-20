// The recording overlay's level meter (A04-64; streaming.py setup_audio_level_callback): each
// level of the recorder fills all 20 bars the overlay draws, and the next one replaces it.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "../ui_overlay/overlay_test_support.h"
#include "controller_support.h"
#include "ui_controller/overlay_router.h"

using fvtest::ControllerRig;

TEST_CASE("a level fills the overlay's twenty bars, and the next replaces it", "[widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();
    fv::WaveformOverlayTestAccess access(rig.overlay());

    router.setLevel(0.4f);
    CHECK(access.audioLevels() == std::vector<double>(20, static_cast<double>(0.4f)));

    router.setLevel(0.1f);
    CHECK(access.audioLevels() == std::vector<double>(20, static_cast<double>(0.1f)));
}
