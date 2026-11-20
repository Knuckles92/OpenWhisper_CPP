// The a32 checklist's claims on the controller (MW-07, MW-15, QR-09, OV-05) that no other test
// pins: the update notice after Later and after "Don't notify me again", the quit question's
// words, that a failed Quick Record transcription never lands in the transcript box, and that a
// remote engine's reconnect attempts reach the overlay.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QTextEdit>

#include "../ui_overlay/overlay_test_support.h"
#include "appcore/activity.h"
#include "appkit/registries.h"
#include "controller_support.h"
#include "ui_controller/quit_confirm.h"
#include "ui_controller/update_presenter.h"
#include "ui_record/quick_record_tab.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/update_notice.h"
#include "update/check.h"

using fvtest::ControllerRig;

namespace update = fv::update;

namespace {

update::UpdateCheckResult found() {
    update::UpdateCheckResult made;
    made.status = update::UpdateStatus::UpdateAvailable;
    made.current_version = "2.1.1";
    made.channel = update::InstallChannel::Installer;
    update::ReleaseInfo release;
    release.version = "2.2.0";
    release.tag_name = "v2.2.0";
    release.html_url = "https://github.com/example/app/releases/tag/v2.2.0";
    made.release = release;
    made.can_apply = false;
    made.apply_mode = update::ApplyMode::NotifyOnly;
    return made;
}

QString notice(ControllerRig& rig) {
    fv::UpdateNoticeButton* button = rig.window().updateNotice();
    return button->isHidden() ? QString() : button->text();
}

QString tray(ControllerRig& rig) {
    QAction* action = rig.context().menus().action(fv::tray_id::update);
    return action->isVisible() ? action->text() : QString();
}

} // namespace

// MW-07: Later closes the dialog and the title-bar and tray notices stay; Later with "Don't
// notify me again" ticked withdraws them.
TEST_CASE("a32 MW-07: Later keeps the update notice and Don't notify me again withdraws it",
          "[widget]") {
    ControllerRig rig;
    fv::UpdatePresenter* presenter = rig.feature().updatePresenter();
    REQUIRE(presenter != nullptr);

    presenter->onCheckFinished(found(), "", true);
    REQUIRE(presenter->dialog() != nullptr);
    presenter->dialog()->laterButton()->click();
    QApplication::processEvents();
    CHECK(presenter->dialog() == nullptr);
    CHECK(notice(rig) == QStringLiteral("Update to 2.2.0"));
    CHECK(tray(rig) == QStringLiteral("Update to 2.2.0…"));

    presenter->showFoundUpdate();
    REQUIRE(presenter->dialog() != nullptr);
    presenter->dialog()->dontNotifyCheck()->setChecked(true);
    presenter->dialog()->laterButton()->click();
    QApplication::processEvents();
    CHECK(notice(rig).isEmpty());
    CHECK(tray(rig).isEmpty());
}

// MW-15: _confirm_quit_while_busy's title and text, naming the first busy reason; nothing asked
// when idle.
TEST_CASE("a32 MW-15: the quit question names the work it would stop", "[unit]") {
    fv::ActivitySnapshot idle;
    CHECK_FALSE(fv::quitQuestion(idle).has_value());

    fv::ActivitySnapshot busy;
    busy.meeting_active = true;
    busy.component_installing = true;
    const std::optional<fv::QuitQuestion> question = fv::quitQuestion(busy);
    REQUIRE(question.has_value());
    CHECK(question->title == QStringLiteral("Quit OpenWhisper_CPP?"));
    CHECK(question->text ==
          QStringLiteral("OpenWhisper_CPP is busy with a meeting. Quitting now stops "
                         "it before it finishes."));
    busy.meeting_active = false;
    CHECK(fv::quitQuestion(busy)->text ==
          QStringLiteral("OpenWhisper_CPP is busy with a component install. Quitting now stops it "
                         "before it finishes."));
}

// QR-09: a failed transcription ends the job with the transcript box cleared; the error is the
// status line's, never the box's (show_transcription_error).
TEST_CASE("a32 QR-09: a failed transcription clears Quick Record's transcript", "[widget]") {
    ControllerRig rig;
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    rig.runtime().transcription_completed.emit("Earlier words.", std::nullopt, std::nullopt);
    REQUIRE(fvtest::waitUntil(
        [tab] { return tab->shownTranscript() == QStringLiteral("Earlier words."); }));

    rig.runtime().transcription_failed.emit("Transcription failed: the engine stopped.");
    REQUIRE(fvtest::waitUntil([tab] { return tab->shownTranscript().isEmpty(); }));
    CHECK_FALSE(tab->transcriptText()->toPlainText().contains(QStringLiteral("engine stopped")));
}

// OV-05: transcription_retry_progress -> show_transcription_retry: each reconnect attempt to a
// paired host is the overlay's processing label.
TEST_CASE("a32 OV-05: a remote reconnect attempt shows on the overlay", "[widget]") {
    ControllerRig rig;
    rig.runtime().transcription_retry_progress.emit(std::string("Retrying connection (2/3)..."));
    REQUIRE(fvtest::waitUntil(
        [&rig] { return rig.overlay().currentState() == fv::WaveformOverlay::kStateProcessing; }));
    fv::WaveformOverlayTestAccess access(rig.overlay());
    CHECK(access.workLabel() == QStringLiteral("Retrying connection (2/3)..."));
    rig.overlay().hide();
}
