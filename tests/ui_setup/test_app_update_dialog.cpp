// The update dialog (ui_qt/dialogs/app_update_dialog.py) and its notes view, ported from
// tests/test_app_update_dialog.py. The opt-outs are saved to an in-memory settings store, so
// nothing here reads or writes a settings file, opens a link or reaches the network; the text
// decisions themselves are update_copy's vectors (tests/update).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <optional>
#include <string>

#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QFrame>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QTextDocument>

#include "base/json.h"
#include "settings/testing/memory_store.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_setup/notes_view.h"
#include "ui_theme/palette.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "update/check.h"

namespace {

using fv::AppUpdateDialog;
using fv::Json;
using fv::MemorySettingsStore;
using Action = fv::AppUpdateDialog::ResultAction;
using Catch::Matchers::ContainsSubstring;
namespace update = fv::update;

// The values test_app_update_dialog.py's _result builds, with this product's names: the running
// copy 0.1.0, a release 0.2.0 whose setup asset has a digest.
struct Spec {
    update::UpdateStatus status = update::UpdateStatus::UpdateAvailable;
    update::InstallChannel channel = update::InstallChannel::Source;
    bool can_apply = false;
    std::string version = "0.2.0";
    std::optional<update::ApplyMode> apply_mode;
    std::string notes = "Bug fixes";
    bool prerelease = false;
};

update::UpdateCheckResult makeResult(const Spec& spec = {}) {
    update::UpdateCheckResult result;
    result.status = spec.status;
    result.current_version = "0.1.0";
    result.channel = spec.channel;
    update::ReleaseInfo release;
    release.version = spec.version;
    release.tag_name = "v" + spec.version;
    release.html_url = "https://example.invalid/releases/tag/v" + spec.version;
    release.notes = spec.notes;
    release.prerelease = spec.prerelease;
    update::ReleaseAsset setup;
    setup.url = "https://example.invalid/OpenWhisper_CPP-Setup-" + spec.version + ".exe";
    setup.name = "OpenWhisper_CPP-Setup-" + spec.version + ".exe";
    setup.size_bytes = 90'000'000;
    setup.sha256 = std::string(64, 'a');
    release.setup_asset = setup;
    result.release = release;
    result.can_apply = spec.can_apply;
    result.apply_mode = spec.apply_mode.value_or(spec.can_apply ? update::ApplyMode::Setup
                                                                : update::ApplyMode::NotifyOnly);
    return result;
}

Spec installer(bool canApply = true) {
    Spec spec;
    spec.channel = update::InstallChannel::Installer;
    spec.can_apply = canApply;
    return spec;
}

QString qs(const char* text) {
    return QString::fromUtf8(text);
}

// An installer-channel update the dialog may download, already downloading.
struct Downloading {
    MemorySettingsStore store;
    AppUpdateDialog dialog{makeResult(installer()), {}, store, true};
    int cancels = 0;
    int downloads = 0;

    Downloading() {
        QObject::connect(&dialog, &AppUpdateDialog::cancelRequested, &dialog,
                         [this] { ++cancels; });
        QObject::connect(&dialog, &AppUpdateDialog::downloadRequested, &dialog,
                         [this] { ++downloads; });
        dialog.primaryButton()->click();
    }
};

} // namespace

TEST_CASE("test_source_primary_is_open_release_notes", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    CHECK(dialog.primaryButton()->text() == qs("Open release notes"));
    CHECK(dialog.primaryButton()->property("buttonId").toString() == qs("updatePrimaryButton"));
    // No git checkout runs OpenWhisper_CPP, so there is no "git pull" hint to show.
    CHECK(dialog.hintLabel()->text().isEmpty());
    CHECK(dialog.hintLabel()->isHidden());
}

TEST_CASE("test_installer_primary_is_download_and_install", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(installer()), {}, store, true);
    CHECK(dialog.primaryButton()->text() == qs("Download and install"));
}

TEST_CASE("test_notify_only_build_does_not_show_windows_installer_size", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(installer(false)), {}, store, false);
    CHECK_FALSE(dialog.bodyLabel()->text().contains(qs("Download size")));
}

TEST_CASE("the body names the download size when the update can be applied", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(installer()), {}, store, true);
    CHECK(dialog.bodyLabel()->text() ==
          qs("You have 0.1.0 (Windows installer).  Download size: 90 MB."));
    CHECK(dialog.titleLabel()->text() == qs("OpenWhisper_CPP 0.2.0 is available"));
    CHECK(dialog.windowTitle() == qs("OpenWhisper_CPP Updates"));
    CHECK(dialog.isModal());
    CHECK(dialog.minimumWidth() == 520);
}

TEST_CASE("test_later_persists_opt_outs", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    dialog.dontNotifyCheck()->setChecked(true);
    dialog.dontCheckCheck()->setChecked(true);
    const auto writes = store.file().writes();

    dialog.laterButton()->click();

    const Json saved = store.contents();
    CHECK(saved.at("update_notify_enabled") == false);
    CHECK(saved.at("update_check_enabled") == false);
    CHECK(store.file().writes() == writes + 1);
    CHECK(dialog.resultAction() == Action::Later);
    CHECK(dialog.result() == QDialog::Rejected);
}

TEST_CASE("test_dont_notify_only", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    dialog.dontNotifyCheck()->setChecked(true);

    dialog.laterButton()->click();

    const Json saved = store.contents();
    CHECK(saved.at("update_notify_enabled") == false);
    CHECK(saved.at("update_check_enabled") == true);
}

TEST_CASE("test_primary_persists_opt_outs", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    const auto writes = store.file().writes();

    dialog.primaryButton()->click();

    const Json saved = store.contents();
    CHECK(saved.at("update_notify_enabled") == true);
    CHECK(saved.at("update_check_enabled") == true);
    CHECK(store.file().writes() == writes + 1);
    CHECK(dialog.resultAction() == Action::Primary);
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("the boxes start from the saved choices", "[ui_setup][widget]") {
    MemorySettingsStore store(Json{{"update_notify_enabled", false}});
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    CHECK(dialog.dontNotifyCheck()->isChecked());
    CHECK_FALSE(dialog.dontCheckCheck()->isChecked());
    CHECK(dialog.dontNotifyCheck()->text() == qs("Don't notify me again"));
    CHECK(dialog.dontCheckCheck()->text() == qs("Don't check automatically"));
}

TEST_CASE("closing the window while idle is Later and saves the opt-outs in one write",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    dialog.dontCheckCheck()->setChecked(true);
    const auto writes = store.file().writes();

    dialog.reject();
    dialog.reject();

    CHECK(store.contents().at("update_check_enabled") == false);
    CHECK(store.contents().at("update_notify_enabled") == true);
    CHECK(store.file().writes() == writes + 1);
    CHECK(dialog.resultAction() == Action::Later);
    CHECK(dialog.result() == QDialog::Rejected);
}

TEST_CASE("Escape closes an idle dialog as Later", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    dialog.dontNotifyCheck()->setChecked(true);
    dialog.show();

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&dialog, &press);

    CHECK_FALSE(dialog.isVisible());
    CHECK(store.contents().at("update_notify_enabled") == false);
}

TEST_CASE("a failed save of the opt-outs is logged and the dialog still closes",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(), {}, store, false);
    store.file().fail_next_replace("disk full");

    dialog.laterButton()->click();

    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(store.contents().contains("update_notify_enabled"));
}

TEST_CASE("test_failed_check_offers_releases_page", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(std::nullopt, qs("The update server rate-limited this check."), store,
                           false);
    CHECK(dialog.titleLabel()->text() == qs("Could not check for updates"));
    CHECK_THAT(dialog.bodyLabel()->text().toStdString(), ContainsSubstring("rate-limited"));
    CHECK(dialog.bodyLabel()->property("tone").toString() == qs("error"));
    CHECK(dialog.primaryButton()->text() == qs("Open releases page"));
    CHECK_FALSE(dialog.primaryButton()->isHidden());
    CHECK(dialog.laterButton()->text() == qs("Close"));
    CHECK(dialog.notesCard()->isHidden());
}

TEST_CASE("a failed check without a sentence says the server could not be reached",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(std::nullopt, QString(), store, false);
    CHECK(dialog.bodyLabel()->text() == qs("The update server could not be reached."));

    dialog.primaryButton()->click();
    CHECK(dialog.resultAction() == Action::Primary);
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("test_up_to_date_has_no_primary", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec;
    spec.status = update::UpdateStatus::UpToDate;
    spec.version = "0.1.0";
    AppUpdateDialog dialog(makeResult(spec), {}, store, false);
    CHECK(dialog.primaryButton()->isHidden());
    CHECK(dialog.laterButton()->text() == qs("Close"));
    CHECK(dialog.titleLabel()->text() == qs("OpenWhisper_CPP is up to date"));
    CHECK(dialog.bodyLabel()->text() ==
          qs("You have 0.1.0 (Source copy).  The latest release is 0.1.0."));
    CHECK(dialog.notesCard()->isHidden());
}

TEST_CASE("test_progress_copy_uses_update_not_installer", "[ui_setup][widget]") {
    Downloading run;
    CHECK(run.downloads == 1);
    CHECK(run.dialog.bodyLabel()->text() == qs("Downloading the update…"));
    CHECK(run.dialog.laterButton()->text() == qs("Cancel"));
    run.dialog.setProgress(qs("verifying"), 50, 100);
    CHECK(run.dialog.bodyLabel()->text() == qs("Verifying the download…"));
    run.dialog.setProgress(qs("extracting"), 50, 100);
    CHECK(run.dialog.bodyLabel()->text() == qs("Preparing the update…"));
    run.dialog.setProgress(qs("restarting"), 1, 1);
    CHECK(run.dialog.bodyLabel()->text() == qs("Restarting to finish the update…"));
    run.dialog.setProgress(qs("rolling_back"), 1, 2);
    CHECK(run.dialog.bodyLabel()->text() == qs("Restoring the previous version…"));
}

TEST_CASE("an unknown or opening phase keeps the body line and names the release",
          "[ui_setup][widget]") {
    Downloading run;
    run.dialog.setProgress(qs("verifying"), 1, 4);
    run.dialog.setProgress(qs("opening"), 0, 0);
    CHECK(run.dialog.bodyLabel()->text() == qs("Verifying the download…"));
    CHECK(run.dialog.progressDetail()->text() == qs("OpenWhisper_CPP 0.2.0"));
    run.dialog.setProgress(qs("no such phase"), 1, 4);
    CHECK(run.dialog.bodyLabel()->text() == qs("Verifying the download…"));
    CHECK(run.dialog.progressDetail()->text() == qs("OpenWhisper_CPP 0.2.0"));
    CHECK(run.dialog.progressPercent()->text() == qs("25%"));
}

TEST_CASE("test_cancel_leaves_a_working_button", "[ui_setup][widget]") {
    Downloading run;

    run.dialog.laterButton()->click();

    CHECK(run.cancels == 1);
    CHECK(run.dialog.bodyLabel()->text() == qs("Canceling the update…"));
    CHECK(run.dialog.laterButton()->isEnabled());
    CHECK(run.dialog.laterButton()->text() == qs("Close"));
    CHECK(run.dialog.progressBar()->isIndeterminate());
    CHECK(run.dialog.progressPercent()->text().isEmpty());
}

TEST_CASE("test_second_cancel_closes_the_dialog", "[ui_setup][widget]") {
    Downloading run;
    int rejected = 0;
    QObject::connect(&run.dialog, &QDialog::rejected, &run.dialog, [&] { ++rejected; });

    run.dialog.laterButton()->click();
    run.dialog.laterButton()->click();

    CHECK(rejected == 1);
    CHECK(run.cancels == 1);
}

TEST_CASE("closing the window during a download cancels first and closes second",
          "[ui_setup][widget]") {
    Downloading run;
    int rejected = 0;
    QObject::connect(&run.dialog, &QDialog::rejected, &run.dialog, [&] { ++rejected; });

    run.dialog.reject();
    CHECK(rejected == 0);
    CHECK(run.cancels == 1);
    CHECK(run.dialog.bodyLabel()->text() == qs("Canceling the update…"));

    run.dialog.reject();
    CHECK(rejected == 1);
    CHECK(run.cancels == 1);
    // The primary click saved the choices already, and a second close adds nothing.
    CHECK(run.dialog.resultAction() == Action::Primary);
}

TEST_CASE("test_restarting_phase_cannot_be_canceled", "[ui_setup][widget]") {
    Downloading run;
    CHECK(run.dialog.busy());

    run.dialog.setProgress(qs("restarting"), 1, 1);

    CHECK_FALSE(run.dialog.laterButton()->isEnabled());
    // A dialog that refuses to close vetoes the quit the setup waits for.
    CHECK_FALSE(run.dialog.busy());
    int rejected = 0;
    QObject::connect(&run.dialog, &QDialog::rejected, &run.dialog, [&] { ++rejected; });
    run.dialog.reject();
    CHECK(rejected == 1);
    CHECK(run.cancels == 0);
}

TEST_CASE("markHandedOff clears busy and disables Cancel", "[ui_setup][widget]") {
    Downloading run;
    run.dialog.laterButton()->click();
    CHECK(run.dialog.laterButton()->text() == qs("Close"));

    run.dialog.markHandedOff();

    CHECK_FALSE(run.dialog.busy());
    CHECK_FALSE(run.dialog.laterButton()->isEnabled());
}

TEST_CASE("test_release_notes_render_as_rich_text", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec = installer();
    spec.notes = "- A bullet\n\n### SHA-256\n\n```\n" + std::string(64, 'a') + "\n```";
    AppUpdateDialog dialog(makeResult(spec), {}, store, true);

    CHECK_FALSE(dialog.notesCard()->isHidden());
    const QString rendered = dialog.notesView()->toHtml();
    CHECK(rendered.contains(qs("A bullet")));
    CHECK_FALSE(rendered.contains(qs("- A bullet")));
    CHECK_FALSE(rendered.contains(qs("SHA-256")));
    CHECK(dialog.notesView()->objectName() == qs("updateNotesView"));
    CHECK(dialog.notesView()->openExternalLinks());
    CHECK(dialog.notesView()->document()->indentWidth() == 14);
    CHECK(dialog.notesView()->height() == 56);
}

TEST_CASE("test_notes_card_hidden_without_notes", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec;
    spec.notes = "";
    AppUpdateDialog dialog(makeResult(spec), {}, store, false);
    CHECK(dialog.notesCard()->isHidden());
}

TEST_CASE("notes that render to nothing hide the card", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec;
    spec.notes = "## Checksums\n\n" + std::string(64, 'a');
    AppUpdateDialog dialog(makeResult(spec), {}, store, false);
    CHECK(dialog.notesCard()->isHidden());
}

TEST_CASE("the notes view tracks its document between 56 and 280 px", "[ui_setup][widget]") {
    MemorySettingsStore store;
    SECTION("a short changelog stays at the floor") {
        AppUpdateDialog dialog(makeResult(), {}, store, false);
        dialog.show();
        CHECK(dialog.notesView()->height() == 56);
    }
    SECTION("a medium changelog takes the height of its text") {
        Spec spec;
        spec.notes = "## A\n\n- one\n- two\n- three\n- four\n- five";
        AppUpdateDialog dialog(makeResult(spec), {}, store, false);
        dialog.show();
        QTextDocument* document = dialog.notesView()->document();
        const int needed =
            static_cast<int>(document->size().height() + 2 * document->documentMargin());
        CHECK(dialog.notesView()->height() == needed);
        CHECK(needed > 56);
        CHECK(needed < 280);
    }
    SECTION("a long changelog stops at the ceiling and scrolls") {
        Spec spec;
        for (int line = 0; line < 60; ++line) {
            spec.notes += "- change number " + std::to_string(line) + "\n";
        }
        AppUpdateDialog dialog(makeResult(spec), {}, store, false);
        dialog.show();
        CHECK(dialog.notesView()->height() == 280);
        CHECK(dialog.notesView()->verticalScrollBar()->maximum() > 0);
    }
}

TEST_CASE("test_downloading_replaces_notes_with_progress", "[ui_setup][widget]") {
    Downloading run;
    CHECK(run.dialog.progressPanel()->isVisibleTo(&run.dialog));
    CHECK(run.dialog.notesCard()->isHidden());
    CHECK(run.dialog.optionsWidget()->isHidden());
    CHECK(run.dialog.hintLabel()->isHidden());
    CHECK(run.dialog.progressBar()->isIndeterminate());
    CHECK_FALSE(run.dialog.primaryButton()->isEnabled());
    CHECK_FALSE(run.dialog.dontNotifyCheck()->isEnabled());
    CHECK_FALSE(run.dialog.dontCheckCheck()->isEnabled());
    CHECK(run.dialog.bodyLabel()->property("tone").toString().isEmpty());
    CHECK(run.dialog.progressDetail()->text() == qs("OpenWhisper_CPP 0.2.0"));
}

TEST_CASE("test_download_progress_shows_bytes_and_percent", "[ui_setup][widget]") {
    Downloading run;

    run.dialog.setProgress(qs("downloading"), 45'000'000, 90'000'000);

    CHECK_FALSE(run.dialog.progressBar()->isIndeterminate());
    CHECK(run.dialog.progressBar()->fraction() == 0.5);
    CHECK(run.dialog.progressPercent()->text() == qs("50%"));
    CHECK(run.dialog.progressDetail()->text() == qs("45 MB of 90 MB"));
    CHECK(run.dialog.progressPanel()->isVisibleTo(&run.dialog));
}

TEST_CASE("progress past the total is clamped and an unknown total sweeps", "[ui_setup][widget]") {
    Downloading run;

    run.dialog.setProgress(qs("downloading"), 150, 100);
    CHECK(run.dialog.progressBar()->fraction() == 1.0);
    CHECK(run.dialog.progressPercent()->text() == qs("100%"));

    run.dialog.setProgress(qs("downloading"), 5, 0);
    CHECK(run.dialog.progressBar()->isIndeterminate());
    CHECK(run.dialog.progressPercent()->text().isEmpty());
}

TEST_CASE("test_member_counting_phases_do_not_report_bytes", "[ui_setup][widget]") {
    Downloading run;

    run.dialog.setProgress(qs("extracting"), 3, 10);

    CHECK(run.dialog.progressDetail()->text() == qs("OpenWhisper_CPP 0.2.0"));
    CHECK(run.dialog.progressPercent()->text() == qs("30%"));
}

TEST_CASE("test_error_restores_the_notes_and_options", "[ui_setup][widget]") {
    Downloading run;

    run.dialog.setError(qs("disk full"));

    CHECK(run.dialog.progressPanel()->isHidden());
    CHECK_FALSE(run.dialog.notesCard()->isHidden());
    CHECK_FALSE(run.dialog.optionsWidget()->isHidden());
    CHECK(run.dialog.bodyLabel()->property("tone").toString() == qs("error"));
    CHECK(run.dialog.bodyLabel()->text() == qs("disk full"));
    CHECK(run.dialog.primaryButton()->isEnabled());
    CHECK(run.dialog.laterButton()->isEnabled());
    CHECK(run.dialog.laterButton()->text() == qs("Later"));
    CHECK(run.dialog.dontNotifyCheck()->isEnabled());
    CHECK(run.dialog.dontCheckCheck()->isEnabled());
    CHECK_FALSE(run.dialog.busy());
    CHECK(run.dialog.resultAction() == Action::Later);
    CHECK(run.dialog.setupButton()->isHidden());
    CHECK_FALSE(run.dialog.progressBar()->isIndeterminate());
    CHECK(run.dialog.progressBar()->fraction() == 0.0);
}

TEST_CASE("an error without a message says the download failed", "[ui_setup][widget]") {
    Downloading run;
    run.dialog.setError(QString());
    CHECK(run.dialog.bodyLabel()->text() == qs("The download failed."));
}

TEST_CASE("an error re-arms the saving of the opt-outs", "[ui_setup][widget]") {
    Downloading run;
    CHECK(run.store.contents().at("update_notify_enabled") == true);

    run.dialog.setError(qs("disk full"));
    run.dialog.dontNotifyCheck()->setChecked(true);
    run.dialog.laterButton()->click();

    CHECK(run.store.contents().at("update_notify_enabled") == false);
    CHECK(run.dialog.resultAction() == Action::Later);
    CHECK(run.dialog.result() == QDialog::Rejected);
}

TEST_CASE("test_error_offers_setup_fallback_for_native_mode", "[ui_setup][widget]") {
    // M3 never produces a native-mode result; this one is made up to prove the fallback.
    MemorySettingsStore store;
    Spec spec = installer();
    spec.apply_mode = update::ApplyMode::Native;
    AppUpdateDialog dialog(makeResult(spec), {}, store, true);
    int setups = 0;
    QObject::connect(&dialog, &AppUpdateDialog::setupRequested, &dialog, [&] { ++setups; });

    dialog.setError(qs("disk full"), true);

    CHECK_FALSE(dialog.setupButton()->isHidden());
    CHECK(dialog.setupButton()->text() == qs("Use installer instead"));
    CHECK(dialog.laterButton()->text() == qs("Later"));

    dialog.setupButton()->click();
    CHECK(setups == 1);
    CHECK(dialog.resultAction() == Action::UseSetup);
    CHECK(dialog.setupButton()->isHidden());
    CHECK(dialog.busy());
    CHECK(dialog.bodyLabel()->text() == qs("Downloading the update…"));
}

TEST_CASE("the setup fallback needs a native result with a verified setup asset",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    SECTION("a setup-mode result does not offer it") {
        AppUpdateDialog dialog(makeResult(installer()), {}, store, true);
        dialog.setError(qs("disk full"), true);
        CHECK(dialog.setupButton()->isHidden());
    }
    SECTION("a native result whose setup asset has no digest does not offer it") {
        Spec spec = installer();
        spec.apply_mode = update::ApplyMode::Native;
        update::UpdateCheckResult result = makeResult(spec);
        result.release->setup_asset->sha256 = std::nullopt;
        AppUpdateDialog dialog(result, {}, store, true);
        dialog.setError(qs("disk full"), true);
        CHECK(dialog.setupButton()->isHidden());
    }
    SECTION("the caller must ask for it") {
        Spec spec = installer();
        spec.apply_mode = update::ApplyMode::Native;
        AppUpdateDialog dialog(makeResult(spec), {}, store, true);
        dialog.setError(qs("disk full"), false);
        CHECK(dialog.setupButton()->isHidden());
    }
    SECTION("nobody listening makes the button do nothing") {
        Spec spec = installer();
        spec.apply_mode = update::ApplyMode::Native;
        AppUpdateDialog dialog(makeResult(spec), {}, store, false);
        dialog.setError(qs("disk full"), true);
        dialog.setupButton()->click();
        CHECK(dialog.resultAction() == Action::Later);
        CHECK_FALSE(dialog.busy());
    }
}

TEST_CASE("the primary button downloads only when the release can be applied and someone listens",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    SECTION("applicable and listened to") {
        AppUpdateDialog dialog(makeResult(installer()), {}, store, true);
        int downloads = 0;
        QObject::connect(&dialog, &AppUpdateDialog::downloadRequested, &dialog,
                         [&] { ++downloads; });
        dialog.primaryButton()->click();
        CHECK(downloads == 1);
        CHECK(dialog.busy());
        CHECK(dialog.result() != QDialog::Accepted);
    }
    SECTION("applicable but nobody listens") {
        AppUpdateDialog dialog(makeResult(installer()), {}, store, false);
        dialog.primaryButton()->click();
        CHECK_FALSE(dialog.busy());
        CHECK(dialog.result() == QDialog::Accepted);
    }
    SECTION("listened to but notify-only") {
        AppUpdateDialog dialog(makeResult(installer(false)), {}, store, true);
        dialog.primaryButton()->click();
        CHECK_FALSE(dialog.busy());
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(dialog.resultAction() == Action::Primary);
    }
}

TEST_CASE("test_title_names_a_beta_release", "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog stable(makeResult(), {}, store, false);
    Spec spec;
    spec.prerelease = true;
    AppUpdateDialog beta(makeResult(spec), {}, store, false);
    CHECK(stable.titleLabel()->text() == qs("OpenWhisper_CPP 0.2.0 is available"));
    CHECK(beta.titleLabel()->text() == qs("OpenWhisper_CPP 0.2.0 beta is available"));
}

TEST_CASE("a development build says so", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec;
    spec.status = update::UpdateStatus::Development;
    spec.version = "0.0.9";
    AppUpdateDialog dialog(makeResult(spec), {}, store, false);
    CHECK(dialog.titleLabel()->text() == qs("This is a development build"));
    CHECK(dialog.bodyLabel()->text() ==
          qs("You have 0.1.0 (Source copy).  This copy is newer than the latest release (0.0.9)."));
    CHECK(dialog.primaryButton()->isHidden());
}

TEST_CASE("the footer buttons keep a 40 px minimum height and fit their labels",
          "[ui_setup][widget]") {
    MemorySettingsStore store;
    AppUpdateDialog dialog(makeResult(installer()), {}, store, true);
    for (QPushButton* button : {static_cast<QPushButton*>(dialog.setupButton()),
                                static_cast<QPushButton*>(dialog.laterButton()),
                                static_cast<QPushButton*>(dialog.primaryButton())}) {
        CAPTURE(button->text().toStdString());
        CHECK(button->minimumHeight() == 40);
        CHECK(button->minimumWidth() >=
              button->fontMetrics().horizontalAdvance(button->text()) + 40);
    }
    CHECK(dialog.primaryButton()->isDefault());
}

TEST_CASE("the dialog shrinks back when the download state hides its notes", "[ui_setup][widget]") {
    MemorySettingsStore store;
    Spec spec = installer();
    spec.notes = "## A\n\n- one\n- two\n- three\n- four\n- five";
    AppUpdateDialog dialog(makeResult(spec), {}, store, true);
    dialog.show();
    const int idleHeight = dialog.height();

    dialog.primaryButton()->click();

    CHECK(dialog.height() < idleHeight);
    CHECK(dialog.height() == dialog.sizeHint().height());
}

TEST_CASE("the notes view fades at the clipped edges only", "[ui_setup][widget]") {
    fv::NotesView view;
    QString html;
    for (int line = 0; line < 40; ++line) {
        html += QStringLiteral("<p>Line %1 of the notes</p>").arg(line);
    }
    view.setHtml(html);
    view.setFrameShape(QFrame::NoFrame);
    view.setFixedSize(320, 120);
    view.show();
    QScrollBar* bar = view.verticalScrollBar();
    REQUIRE(bar->maximum() > bar->minimum());

    const QColor card = fv::ui::tokenColor("surface-sunken");
    const QColor plain = view.viewport()->palette().color(QPalette::Base);
    REQUIRE(card.rgb() != plain.rgb());
    // The row where the fade is nearly opaque, 1 px in from the viewport's edge.
    const auto row = [&](bool bottom) {
        const QImage image = view.viewport()->grab().toImage();
        const int y = bottom ? image.height() - 1 : 0;
        return image.pixelColor(image.width() - 2, y);
    };
    const auto nearCard = [&](const QColor& color) {
        return std::abs(color.red() - card.red()) < 12 &&
               std::abs(color.green() - card.green()) < 12 &&
               std::abs(color.blue() - card.blue()) < 12;
    };

    SECTION("at the top, only the bottom edge fades") {
        bar->setValue(bar->minimum());
        CHECK_FALSE(nearCard(row(false)));
        CHECK(nearCard(row(true)));
    }
    SECTION("in the middle, both edges fade") {
        bar->setValue((bar->minimum() + bar->maximum()) / 2);
        CHECK(nearCard(row(false)));
        CHECK(nearCard(row(true)));
    }
    SECTION("at the end, only the top edge fades") {
        bar->setValue(bar->maximum());
        CHECK(nearCard(row(false)));
        CHECK_FALSE(nearCard(row(true)));
    }
}

TEST_CASE("a short document draws no fade", "[ui_setup][widget]") {
    fv::NotesView view;
    view.setHtml(QStringLiteral("<p>One line</p>"));
    view.setFrameShape(QFrame::NoFrame);
    view.setFixedSize(320, 120);
    view.show();
    REQUIRE(view.verticalScrollBar()->maximum() <= view.verticalScrollBar()->minimum());
    const QImage image = view.viewport()->grab().toImage();
    const QColor plain = view.viewport()->palette().color(QPalette::Base);
    CHECK(image.pixelColor(image.width() - 2, image.height() - 1).rgb() == plain.rgb());
}

TEST_CASE("a viewport shorter than two fade bands fades over half its height",
          "[ui_setup][widget]") {
    fv::NotesView view;
    QString html;
    for (int line = 0; line < 40; ++line) {
        html += QStringLiteral("<p>Line %1 of the notes</p>").arg(line);
    }
    view.setHtml(html);
    view.setFrameShape(QFrame::NoFrame);
    view.setFixedSize(320, 30);
    view.show();
    QScrollBar* bar = view.verticalScrollBar();
    REQUIRE(bar->maximum() > bar->minimum());
    bar->setValue(bar->minimum());
    const QImage image = view.viewport()->grab().toImage();
    REQUIRE(image.height() == 30);
    const QColor plain = view.viewport()->palette().color(QPalette::Base);
    // The band is 15 px (half of 30), not 22: it starts at y 15, so y 12 is still the plain
    // background, which a 22 px band (from y 8) would already have tinted.
    CHECK(image.pixelColor(image.width() - 2, 12).rgb() == plain.rgb());
    CHECK(image.pixelColor(image.width() - 2, image.height() - 1).rgb() != plain.rgb());
}