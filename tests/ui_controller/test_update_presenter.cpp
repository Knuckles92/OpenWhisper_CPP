// The title-bar and tray update notice, the update dialog the checks and the notice open, and the
// About box's update line. Ported from tests/test_update_notice.py (7 of 8: the compact button is
// M0.10's tests/ui_widgets/test_update_notice.cpp) and test_application_controller.py's
// test_auto_update_check_failure_is_background_status, over the controller's real presenter (the
// main window and tray of the rig) or one built beside it with recording ports.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <QAction>
#include <QApplication>

#include "controller_support.h"
#include "settings/keys.h"
#include "ui_controller/update_presenter.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/update_notice.h"
#include "ui_widgets/wrapped_label.h"
#include "update/check.h"
#include "update/trust.h"

using fvtest::ControllerRig;

namespace update = fv::update;

namespace {

update::UpdateCheckResult
result(update::UpdateStatus status = update::UpdateStatus::UpdateAvailable,
       const std::string& version = "2.2.0") {
    update::UpdateCheckResult made;
    made.status = status;
    made.current_version = "2.1.1";
    made.channel = update::InstallChannel::Installer;
    update::ReleaseInfo release;
    release.version = version;
    release.tag_name = "v" + version;
    release.html_url = "https://github.com/example/app/releases/tag/v" + version;
    release.notes = "Bug fixes";
    made.release = release;
    made.can_apply = false;
    made.apply_mode = update::ApplyMode::NotifyOnly;
    return made;
}

fv::UpdatePresenter& presenter(ControllerRig& rig) {
    fv::UpdatePresenter* found = rig.feature().updatePresenter();
    REQUIRE(found != nullptr);
    return *found;
}

QString notice(ControllerRig& rig) {
    fv::UpdateNoticeButton* button = rig.window().updateNotice();
    return button->isHidden() ? QString() : button->text();
}

QString tray(ControllerRig& rig) {
    QAction* action = rig.context().menus().action(fv::tray_id::update);
    return action->isVisible() ? action->text() : QString();
}

// Closes the dialog the presenter has open, as Later does.
void closeDialog(fv::UpdatePresenter& presenter) {
    if (fv::AppUpdateDialog* dialog = presenter.dialog()) {
        dialog->reject();
        QApplication::processEvents();
    }
}

} // namespace

// from tests/test_update_notice.py::test_automatic_check_lights_the_notice_without_a_dialog
TEST_CASE("automatic check lights the notice without a dialog", "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", false);
    CHECK(notice(rig) == QStringLiteral("Update to 2.2.0"));
    CHECK(tray(rig) == QStringLiteral("Update to 2.2.0…"));
    CHECK(rig.tray().toolTip().contains(QStringLiteral("2.2.0")));
    CHECK(presenter(rig).dialog() == nullptr);
}

// from tests/test_update_notice.py::test_manual_check_opens_the_dialog_and_lights_the_notice
TEST_CASE("manual check opens the dialog and lights the notice", "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", true);
    fv::AppUpdateDialog* dialog = presenter(rig).dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
    CHECK(dialog->titleLabel()->text() == QStringLiteral("OpenWhisper_CPP 2.2.0 is available"));
    CHECK(notice(rig) == QStringLiteral("Update to 2.2.0"));
    closeDialog(presenter(rig));
    CHECK(presenter(rig).dialog() == nullptr);
}

// from tests/test_update_notice.py::test_failed_check_keeps_a_release_found_earlier
TEST_CASE("failed check keeps a release found earlier", "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", false);
    presenter(rig).onCheckFinished(std::nullopt, "Could not reach the update server.", false);
    CHECK(notice(rig) == QStringLiteral("Update to 2.2.0"));
    REQUIRE(presenter(rig).lastResult().has_value());
    CHECK(presenter(rig).lastResult()->release->version == "2.2.0");
}

// from tests/test_update_notice.py::test_up_to_date_result_withdraws_the_notice
TEST_CASE("up to date result withdraws the notice", "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", false);
    presenter(rig).onCheckFinished(result(update::UpdateStatus::UpToDate, "2.1.1"), "", false);
    CHECK(notice(rig).isEmpty());
    CHECK(tray(rig).isEmpty());
    CHECK(rig.tray().toolTip() == QStringLiteral("OpenWhisper_CPP"));
}

// from tests/test_update_notice.py::test_notices_follow_the_notify_setting
TEST_CASE("notices follow the notify setting", "[test_update_notice][widget]") {
    ControllerRig rig;
    rig.settings().save_setting(fv::keys::update_notify_enabled.key(), fv::Json(false));
    presenter(rig).onCheckFinished(result(), "", false);
    CHECK(notice(rig).isEmpty());

    rig.settings().save_setting(fv::keys::update_notify_enabled.key(), fv::Json(true));
    // Settings > General reports its Updates tiles as "updates" (notify_changed).
    rig.ui().settingsPageChanged(QStringLiteral("updates"));
    CHECK(notice(rig) == QStringLiteral("Update to 2.2.0"));

    // Turning checks off withdraws the notice too.
    rig.settings().save_setting(fv::keys::update_check_enabled.key(), fv::Json(false));
    presenter(rig).refreshNotice();
    CHECK(notice(rig).isEmpty());
}

// from tests/test_update_notice.py::test_check_while_the_dialog_is_open_leaves_its_result_alone
TEST_CASE("check while the dialog is open leaves its result alone",
          "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", false);
    presenter(rig).showFoundUpdate();
    REQUIRE(presenter(rig).dialog() != nullptr);
    fv::AppUpdateDialog* shown = presenter(rig).dialog();

    presenter(rig).onCheckFinished(result(update::UpdateStatus::UpdateAvailable, "2.3.0"), "",
                                   false);
    CHECK(presenter(rig).lastResult()->release->version == "2.2.0");
    CHECK(presenter(rig).dialog() == shown);
    // A manual check while it is open only raises it.
    presenter(rig).onCheckFinished(result(update::UpdateStatus::UpdateAvailable, "2.3.0"), "",
                                   true);
    CHECK(presenter(rig).dialog() == shown);
    CHECK(presenter(rig).lastResult()->release->version == "2.2.0");
    closeDialog(presenter(rig));
}

// from tests/test_update_notice.py::test_notice_and_tray_open_the_found_release
TEST_CASE("notice and tray open the found release", "[test_update_notice][widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", false);

    Q_EMIT rig.window().updateNoticeRequested();
    REQUIRE(presenter(rig).dialog() != nullptr);
    CHECK(presenter(rig).dialog()->titleLabel()->text() ==
          QStringLiteral("OpenWhisper_CPP 2.2.0 is available"));
    closeDialog(presenter(rig));
    REQUIRE(presenter(rig).dialog() == nullptr);

    rig.window().hide();
    rig.context().menus().action(fv::tray_id::update)->trigger();
    REQUIRE(presenter(rig).dialog() != nullptr);
    // The tray restores the window first (_on_tray_update).
    CHECK(rig.window().isVisible());
    closeDialog(presenter(rig));
}

// from tests/test_application_controller.py::test_auto_update_check_failure_is_background_status
TEST_CASE("auto update check failure is background status",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::vector<QString> ambient;
    std::vector<std::string> kinds;
    fv::UpdatePresenterParts parts;
    parts.window = &rig.window();
    parts.settings = &rig.settings();
    parts.activity = &rig.activity();
    parts.ui = &rig.uiExecutor();
    parts.ambientStatus = [&ambient](const QString& status) {
        ambient.push_back(status);
    };
    fv::UpdatePresenter own(std::move(parts));

    own.onCheckFinished(std::nullopt, "GitHub rate-limited this update check.", false);

    CHECK(rig.statuses().empty());
    REQUIRE(ambient.size() == 1);
    CHECK(ambient[0].toLower().contains(QStringLiteral("rate limit")));
    CHECK(ambient[0] == fv::qt::toQString(update::update_check_failure_status(
                            "GitHub rate-limited this update check.")));
    // The UI half still ran: an automatic failure opens nothing and keeps no result.
    CHECK(own.dialog() == nullptr);
    CHECK_FALSE(own.lastResult().has_value());

    // A manual failure is the dialog's to show, not the status line's.
    own.onCheckFinished(std::nullopt, "The update server could not be reached.", true);
    CHECK(ambient.size() == 1);
    REQUIRE(own.dialog() != nullptr);
    closeDialog(own);
}

TEST_CASE("Help > Check for Updates without an updater opens nothing", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.feature().updater() == nullptr);
    Q_EMIT rig.window().checkForUpdatesRequested();
    CHECK(presenter(rig).dialog() == nullptr);
}

TEST_CASE("the release notes of a notify-only release open in the browser", "[widget]") {
    ControllerRig rig;
    presenter(rig).onCheckFinished(result(), "", true);
    fv::AppUpdateDialog* dialog = presenter(rig).dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->primaryButton() != nullptr);
    dialog->primaryButton()->click();
    QApplication::processEvents();
    CHECK(rig.calls().has("open_url:https://github.com/example/app/releases/tag/v2.2.0"));
    CHECK(presenter(rig).dialog() == nullptr);
}

// show_about_dialog's update line (ui_controller.py:2839-2860).
TEST_CASE("the About box shows the update status of the last check", "[widget]") {
    ControllerRig rig;
    fv::UpdatePresenter& shown = presenter(rig);
    CHECK(shown.aboutStatusLine().isEmpty());
    shown.onCheckFinished(result(), "", false);
    CHECK(shown.aboutStatusLine() == QStringLiteral("Update available: 2.2.0"));
    shown.onCheckFinished(result(update::UpdateStatus::Development, "0.0.9"), "", false);
    CHECK(shown.aboutStatusLine() == QStringLiteral("Development build, newer than 0.0.9"));
    update::UpdateCheckResult development = result(update::UpdateStatus::Development);
    development.release.reset();
    shown.onCheckFinished(development, "", false);
    CHECK(shown.aboutStatusLine() == QStringLiteral("Development build"));
    shown.onCheckFinished(result(update::UpdateStatus::UpToDate, "2.1.1"), "", false);
    CHECK(shown.aboutStatusLine() == QStringLiteral("Up to date"));

    QString title;
    QString html;
    fv::UpdatePresenterParts parts;
    parts.window = &rig.window();
    parts.settings = &rig.settings();
    parts.activity = &rig.activity();
    parts.ui = &rig.uiExecutor();
    update::InstallProbe probe;
    probe.registered = true;
    probe.install_dir = "C:/Programs/OpenWhisper_CPP";
    probe.running_dir = "C:/Programs/OpenWhisper_CPP";
    parts.probe = &probe;
    // The Windows installer this test describes, whatever system runs it.
    parts.platform = "win32";
    parts.aboutBox = [&title, &html](QWidget*, const QString& boxTitle, const QString& boxHtml) {
        title = boxTitle;
        html = boxHtml;
    };
    fv::UpdatePresenter own(std::move(parts));
    own.onCheckFinished(result(), "", false);
    own.showAbout();
    CHECK(title == QStringLiteral("About OpenWhisper_CPP"));
    CHECK(html.contains(QStringLiteral("Install: Windows installer</p>")));
    CHECK(html.contains(QStringLiteral("<p>Update status: Update available: 2.2.0</p>")));
}
