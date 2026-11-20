// The Live preview checkbox in the engine card footer. It mirrors Settings > Recording > Live
// preview: the same key, the same legacy-key drops. The tests of the Remote engine are M9's.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QSignalSpy>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "record_fixture.h"
#include "settings/keys.h"
#include "ui_record/record_feature.h"
#include "ui_record/upload_file_tab.h"

namespace {

using fv::Json;

} // namespace

// from
// tests/test_live_preview_control.py::TestFooterPlacement::test_quick_record_shows_the_checkbox_beside_ai_cleanup
TEST_CASE("quick record shows the checkbox beside AI cleanup",
          "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab;
    CHECK(tab->livePreviewCheck()->isVisible());
    CHECK(tab->livePreviewCheck()->text() == QStringLiteral("Live preview"));
    CHECK(tab->engineCard()->isAncestorOf(tab->livePreviewCheck()));
    const int cleanupX =
        tab->cleanupCheck()->mapTo(tab.tab.get(), tab->cleanupCheck()->rect().topLeft()).x();
    const int previewX = tab->livePreviewCheck()
                             ->mapTo(tab.tab.get(), tab->livePreviewCheck()->rect().topLeft())
                             .x();
    CHECK(cleanupX < previewX);
}

// from
// tests/test_live_preview_control.py::TestFooterPlacement::test_recording_locks_the_toggle_with_the_backend_choice
TEST_CASE("recording locks the toggle with the backend choice",
          "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab;
    tab->setRecordingActive(true);
    CHECK_FALSE(tab->livePreviewCheck()->isEnabled());
    CHECK_FALSE(tab->modelCombo()->isEnabled());
    tab->setRecordingActive(false);
    CHECK(tab->livePreviewCheck()->isEnabled());
}

// from tests/test_live_preview_control.py::TestPersistence::test_loads_the_saved_streaming_setting
TEST_CASE("loads the saved streaming setting", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", true}});
    CHECK(tab->livePreviewCheck()->isChecked());
}

// from
// tests/test_live_preview_control.py::TestPersistence::test_defaults_to_config_when_nothing_is_saved
TEST_CASE("defaults to config when nothing is saved", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab;
    CHECK(tab->livePreviewCheck()->isChecked() == fv::keys::streaming_enabled.default_value());
}

// from
// tests/test_live_preview_control.py::TestPersistence::test_toggle_writes_the_settings_key_and_drops_legacy_switches
TEST_CASE("toggle writes the settings key and drops legacy switches",
          "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_overlay_enabled", true},
                              {"streaming_paste_enabled", true},
                              {"streaming_tiny_model_enabled", false},
                              {"live_typing_enabled", false},
                              {"ui_theme", "dark"}});
    QSignalSpy emitted(tab.tab.get(), &fv::QuickRecordTab::livePreviewChanged);

    tab->livePreviewCheck()->setChecked(true);

    const Json saved = tab.settings.contents();
    CHECK(saved.at("streaming_enabled") == true);
    for (const char* legacy : {"streaming_overlay_enabled", "streaming_paste_enabled",
                               "streaming_tiny_model_enabled", "live_typing_enabled"}) {
        CHECK_FALSE(saved.contains(legacy));
    }
    // Everything else it left alone.
    CHECK(saved.at("ui_theme") == "dark");
    CHECK(emitted.count() == 1);
}

// from
// tests/test_live_preview_control.py::TestPersistence::test_reload_from_settings_does_not_write_or_emit
TEST_CASE("reload from settings does not write or emit", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", true}});
    QSignalSpy emitted(tab.tab.get(), &fv::QuickRecordTab::livePreviewChanged);
    const std::size_t writes = tab.settings.file().writes();

    tab->loadLivePreviewSetting();

    CHECK(tab->livePreviewCheck()->isChecked());
    CHECK(tab.settings.file().writes() == writes);
    CHECK(emitted.count() == 0);
}

// from tests/test_live_preview_control.py::TestPersistence::test_failed_save_reverts_the_checkbox
TEST_CASE("failed save reverts the checkbox", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", false}});
    QSignalSpy emitted(tab.tab.get(), &fv::QuickRecordTab::livePreviewChanged);
    tab.settings.file().fail_next_replace("disk full");

    tab->livePreviewCheck()->setChecked(true);

    CHECK_FALSE(tab->livePreviewCheck()->isChecked());
    CHECK(emitted.count() == 0);
}

// from
// tests/test_live_preview_control.py::TestFooterPlacement::test_upload_tab_has_no_dictation_preview_so_it_hides_the_checkbox
TEST_CASE("upload tab has no dictation preview so it hides the checkbox",
          "[test_live_preview_control][widget]") {
    fv::MemorySettingsStore settings;
    fv::UploadFileTab tab(settings, fv::speechEngineCatalog());
    tab.resize(680, 500);
    tab.show();
    QApplication::processEvents();
    CHECK_FALSE(tab.livePreviewCheck()->isVisible());
    CHECK(tab.cleanupCheck()->isVisible());
    tab.close();
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_api_grays_out_unchecked_and_says_why
TEST_CASE("api grays out unchecked and says why", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", true}});
    const std::size_t writes = tab.settings.file().writes();
    tab->setBackend(QStringLiteral("API"));
    QCheckBox* check = tab->livePreviewCheck();
    CHECK_FALSE(check->isEnabled());
    CHECK_FALSE(check->isChecked());
    // Lead decision: OpenWhisper_CPP names its one previewing engine.
    CHECK(check->toolTip().startsWith(QStringLiteral("Live preview needs Parakeet.")));
    CHECK(check->toolTip().contains(QStringLiteral("turns back on")));
    CHECK(tab.settings.file().writes() == writes);
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_switching_back_restores_the_saved_choice
TEST_CASE("switching back restores the saved choice", "[test_live_preview_control][widget]") {
    // Python switches to Moonshine; the engine that cannot preview here is the API.
    fvtest::ShownTab tab(Json{{"streaming_enabled", true}});
    tab->setBackend(QStringLiteral("API"));
    CHECK_FALSE(tab->livePreviewCheck()->isChecked());
    tab->setBackend(QStringLiteral("Parakeet"));
    CHECK(tab->livePreviewCheck()->isEnabled());
    CHECK(tab->livePreviewCheck()->isChecked());
    CHECK(tab->livePreviewCheck()->toolTip().startsWith(QStringLiteral("Show draft text")));
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_saved_off_says_how_to_use_it_instead
TEST_CASE("saved off says how to use it instead", "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", false}});
    tab->setBackend(QStringLiteral("API"));
    CHECK(tab->livePreviewCheck()->toolTip().contains(QStringLiteral("Switch to one to use it")));
}

// from
// tests/test_live_preview_control.py::TestEngineSupport::test_ending_a_recording_does_not_unlock_an_engine_that_cannot_preview
TEST_CASE("ending a recording does not unlock an engine that cannot preview",
          "[test_live_preview_control][widget]") {
    fvtest::ShownTab tab(Json{{"streaming_enabled", true}});
    tab->setBackend(QStringLiteral("API"));
    tab->setRecordingActive(true);
    tab->setRecordingActive(false);
    CHECK_FALSE(tab->livePreviewCheck()->isEnabled());
}

// from
// tests/test_live_preview_control.py::TestMainWindowRelay::test_toggle_syncs_the_other_tab_and_emits_once
// The window's half: refresh_live_preview_controls over the tabs the record feature built. The
// connection from a tab's livePreviewChanged to it, and the window's own signal, are the
// controller's (M2.28); the test makes that connection as the controller will.
TEST_CASE("toggle syncs the other tab and emits once", "[test_live_preview_control][widget]") {
    fv::MemorySettingsStore settings(Json{{"streaming_enabled", false}});
    QObject context;
    fv::qt::QtUiExecutor ui(&context);
    QWidget window;
    fv::RecordWidgets widgets(settings, ui);
    fv::QuickRecordTab* record = widgets.quickRecord(&window);
    fv::UploadFileTab* upload = widgets.upload(&window);
    QSignalSpy emitted(record, &fv::QuickRecordTab::livePreviewChanged);
    QObject::connect(record, &fv::QuickRecordTab::livePreviewChanged, &context,
                     [&widgets] { widgets.refreshLivePreviewControls(); });

    record->livePreviewCheck()->setChecked(true);

    CHECK(emitted.count() == 1);
    // The hidden Upload copy follows so a future tab flip agrees.
    CHECK(upload->livePreviewCheck()->isChecked());
}
