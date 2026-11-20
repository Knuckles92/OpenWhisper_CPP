// What the UI controller refreshes when the app changes under it (ui_controller.py
// set_engine_busy, _on_api_keys_saved and streaming's audio level callback): the model views
// when the engine goes idle and when a key changes, and the overlay's bars from the recorder.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "../ui_overlay/overlay_test_support.h"
#include "controller_support.h"
#include "personalize_support.h"
#include "recorder/recorder.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

// The Downloads page's inventory scans, counted, with Settings open.
struct ScanCount {
    std::shared_ptr<int> scans = std::make_shared<int>(0);

    explicit ScanCount(ControllerRig& rig) {
        auto& presenter = rig.context().service<fv::DownloadsPresenter>();
        const fv::DownloadsPageOptions base = presenter.pageOptions();
        presenter.setPageOptions([base, scans = scans] {
            fv::DownloadsPageOptions options = base;
            options.backgroundScan = false;
            options.scanInventory = [scans] {
                ++*scans;
                return fv::models::CachedModelsSnapshot::Inventory{};
            };
            return options;
        });
        fv::SettingsDialog& dialog = rig.context().service<fv::SettingsWindow>().ensureDialog();
        dialog.show();
        REQUIRE(waitUntil([&] { return dialog.isVisible(); }));
    }
};

} // namespace

TEST_CASE("the model views follow the engine once it is idle again", "[widget]") {
    // The Downloads page needs the network client the app has.
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    ScanCount count(rig);
    const int before = *count.scans;
    rig.ui().setEngineBusy(true);
    CHECK(*count.scans == before);
    rig.ui().setEngineBusy(false);
    CHECK(*count.scans == before + 1);
}

TEST_CASE("a key saved in Settings refreshes the model views", "[widget]") {
    // The Downloads page needs the network client the app has.
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    ScanCount count(rig);
    const int before = *count.scans;
    Q_EMIT rig.context().service<fv::SettingsPagesSignals>().apiKeysChanged();
    CHECK(waitUntil([&] { return *count.scans == before + 1; }));
}

TEST_CASE("the recorder's level reaches every bar of the overlay", "[widget]") {
    ControllerRig rig;
    rig.context().service<fv::Recorder>().level.emit(0.5f);
    fv::WaveformOverlayTestAccess access(rig.overlay());
    REQUIRE(waitUntil([&] { return access.audioLevels().front() > 0.0; }));
    for (const double level : access.audioLevels()) {
        CHECK(level == 0.5);
    }
}
