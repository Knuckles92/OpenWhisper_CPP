// Sweep H2: open items of the record tabs (focus order of the API fields, the profile card's
// lifetime, the Upload tab receiving a transcript before it was ever shown).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QWidget>

#include "appkit/registries.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "ui_record/cleanup_profile_card.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/speech_server_controls.h"

namespace {

// Where each widget sits in the window's focus chain, starting from `from`.
int chainIndex(QWidget* from, const QWidget* target) {
    int index = 0;
    for (QWidget* w = from;; w = w->nextInFocusChain()) {
        if (w == target) {
            return index;
        }
        ++index;
        if (w->nextInFocusChain() == from) {
            return -1;
        }
    }
}

} // namespace

// open item: "ui_record/transcription_tab_base.cpp setApiServer: the API fields join the end of
// the tab's focus chain". OpenWhisper builds the API fields between the local engine's fields and
// the footer, so Tab goes Backend, the local fields, Server, Model, then "AI cleanup".
TEST_CASE("the API fields follow the local fields in the focus chain, before the footer",
          "[sweep_record][widget]") {
    fv::SpeechServerControls::resetCatalogs();
    fv::MemorySettingsStore settings;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    fv::FakeClock clock;
    auto tab = std::make_unique<fv::UploadFileTab>(settings, fv::speechEngineCatalog());
    auto* controls = new fv::SpeechServerControls(settings, client, io);
    controls->setClock(clock);
    tab->setApiServer(controls);

    QWidget* start = tab->modelCombo();
    const int localModel = chainIndex(start, tab->localEngine()->modelCombo());
    const int server = chainIndex(start, controls->serverCombo());
    const int model = chainIndex(start, controls->modelCombo());
    const int cleanup = chainIndex(start, tab->cleanupCheck());
    REQUIRE(localModel > 0);
    REQUIRE(server > 0);
    REQUIRE(model > 0);
    REQUIRE(cleanup > 0);
    CHECK(localModel < server);
    CHECK(server < model);
    CHECK(model < cleanup);
    // No tab stop of the API fields is left behind after the footer.
    for (QWidget* w = tab->cleanupCheck(); w != start; w = w->nextInFocusChain()) {
        CHECK_FALSE((controls->isAncestorOf(w) && (w->focusPolicy() & Qt::TabFocus) != 0));
    }
    fv::SpeechServerControls::resetCatalogs();
}

// open item: "ui_record/upload_presenter.cpp: applyStatus defers the engine message ...
// displayTranscript and displayTranscriptionError return false when the Upload tab was never
// built, where Python would build it". main_window.py's upload_file_tab property builds the
// screen on access, so an upload job's result, error, stats and status build a missing tab.
TEST_CASE("an upload job's result builds the Upload tab when it does not exist",
          "[sweep_record][upload_presenter][widget]") {
    fv::MemorySettingsStore settings;
    fv::ScreenRegistry screens;
    QWidget host;
    screens.define(fv::screen_key::upload, 200, QStringLiteral("Upload File"),
                   [&settings](QWidget* parent) -> QWidget* {
                       return new fv::UploadFileTab(settings, fv::speechEngineCatalog(), parent);
                   });
    std::vector<QString> windowStatuses;
    fv::UploadPresenter::Ports ports;
    ports.windowStatus = [&](const QString& status) {
        windowStatuses.push_back(status);
    };
    ports.tabParent = [&host]() -> QWidget* {
        return &host;
    };
    fv::UploadPresenter presenter(screens, std::move(ports));

    // The job was the tab's (main_window.py's relay); the tab is gone before its result.
    presenter.requestUpload(QStringLiteral("talk.wav"), 3.0);
    REQUIRE(presenter.tab() == nullptr);

    presenter.applyStatus(QStringLiteral("Transcribing chunk 1/2..."));
    fv::UploadFileTab* tab = presenter.tab();
    REQUIRE(tab != nullptr);
    CHECK(tab->parentWidget() == &host);
    CHECK(windowStatuses.empty());

    delete tab;
    REQUIRE(presenter.tab() == nullptr);
    CHECK(presenter.displayTranscript(QStringLiteral("Hello there"), std::nullopt));
    tab = presenter.tab();
    REQUIRE(tab != nullptr);
    CHECK(tab->shownTranscript() == QStringLiteral("Hello there"));
    CHECK_FALSE(tab->isTranscriptionCollapsed());

    delete tab;
    CHECK(presenter.displayTranscriptionError(QStringLiteral("Error: something failed.")));
    REQUIRE(presenter.tab() != nullptr);

    // Another job's results stay Quick Record's, and build nothing.
    delete presenter.tab();
    presenter.recordingStarted();
    CHECK_FALSE(presenter.displayTranscript(QStringLiteral("dictated"), std::nullopt));
    CHECK(presenter.tab() == nullptr);
}

// Without a parent to build under, the result goes to Quick Record as before.
TEST_CASE("an upload job's result with no tab parent leaves the tab unbuilt",
          "[sweep_record][upload_presenter][widget]") {
    fv::MemorySettingsStore settings;
    fv::ScreenRegistry screens;
    screens.define(fv::screen_key::upload, 200, QStringLiteral("Upload File"),
                   [&settings](QWidget* parent) -> QWidget* {
                       return new fv::UploadFileTab(settings, fv::speechEngineCatalog(), parent);
                   });
    fv::UploadPresenter presenter(screens, {});
    presenter.requestUpload(QStringLiteral("talk.wav"), 3.0);
    CHECK_FALSE(presenter.displayTranscript(QStringLiteral("Hello"), std::nullopt));
    CHECK(presenter.tab() == nullptr);
}

// open item: "ui_record/quick_record_tab.h: profileCard_ is a raw pointer". The reference
// screenshots take the card out of the tree, as the builder's _prune does; the tab must outlive
// that through refreshes, recording state changes and a cleanup toggle.
TEST_CASE("Quick Record survives its cleanup profile card being deleted",
          "[sweep_record][widget]") {
    fv::MemorySettingsStore settings;
    fv::QuickRecordTab tab(settings, fv::speechEngineCatalog());
    REQUIRE(tab.profileCard() != nullptr);
    delete tab.profileCard();
    CHECK(tab.profileCard() == nullptr);
    CHECK(tab.profileCombo() == nullptr);
    CHECK(tab.profileHint() == nullptr);
    CHECK(tab.selectedCleanupProfileId().isEmpty());

    tab.refreshCleanupProfiles();
    tab.setRecordingActive(true);
    CHECK(tab.recordButton()->text() == QStringLiteral("Recording..."));
    tab.setRecordingActive(false);
    CHECK(tab.recordButton()->text() == QStringLiteral("Start Recording"));
    tab.cleanupCheck()->toggle();
    tab.cleanupCheck()->toggle();
    tab.loadCleanupSetting();
}
