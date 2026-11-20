// The update dialog against the references OpenWhisper rendered for it
// (tools/parity/builders/a16.py, tests/reference/app_update_dialog/<state>/): the installer
// channel's copy 0.1.0 and a release 0.2.0 with a 52,428,800 byte setup, in the seven states the
// builder names. The widget tree first, then the pixels, with the application styled as
// OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>

#include <QCoreApplication>
#include <QScrollBar>
#include <QTimer>

#include "base/json.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "setup_support.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_setup/notes_view.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "update/check.h"

namespace {

using fv::AppUpdateDialog;
using fv::MemorySettingsStore;
namespace update = fv::update;

// The builder's UPDATE_NOTES: the Checksums section is dropped by the renderer.
const std::string kNotes =
    "## What's new\n\n"
    "- Dictation starts faster, with Parakeet running in the app\n"
    "- The `Settings` window opens on the page you left it on\n"
    "- Read the [full history](https://example.invalid/history) of changes\n\n"
    "## Fixes\n\n"
    "- Pasting no longer drops the first character\n\n"
    "## Checksums\n\n"
    "SHA-256: " +
    std::string(64, 'a') + "\n";

constexpr std::int64_t kSetupSize = 52'428'800;

std::optional<update::UpdateCheckResult> resultFor(const std::string& state) {
    if (state == "check-failed") {
        return std::nullopt;
    }
    std::string version = "0.2.0";
    update::UpdateStatus status = update::UpdateStatus::UpdateAvailable;
    if (state == "up-to-date") {
        version = "0.1.0";
        status = update::UpdateStatus::UpToDate;
    } else if (state == "development") {
        version = "0.0.9";
        status = update::UpdateStatus::Development;
    }
    const bool applies = state == "available" || state == "downloading" || state == "error";
    update::ReleaseAsset setup;
    setup.url = "https://example.invalid/releases/download/v" + version + "/FioriVoce-Setup-" +
                version + ".exe";
    setup.name = "FioriVoce-Setup-" + version + ".exe";
    setup.size_bytes = kSetupSize;
    // available-notify: no digest, so the release cannot be applied here.
    if (state != "available-notify") {
        setup.sha256 = std::string(64, 'a');
    }
    update::ReleaseInfo release;
    release.version = version;
    release.tag_name = "v" + version;
    release.html_url = "https://example.invalid/releases/tag/v" + version;
    release.notes = kNotes;
    release.setup_asset = setup;
    update::UpdateCheckResult result;
    result.status = status;
    result.current_version = "0.1.0";
    result.channel = update::InstallChannel::Installer;
    result.release = release;
    result.can_apply = applies;
    result.apply_mode = applies ? update::ApplyMode::Setup : update::ApplyMode::NotifyOnly;
    return result;
}

std::unique_ptr<AppUpdateDialog> build(const std::string& state, MemorySettingsStore& store) {
    const bool error = state == "check-failed";
    auto dialog = std::make_unique<AppUpdateDialog>(
        resultFor(state),
        error ? QStringLiteral("The update server could not be reached.") : QString(), store, true);
    if (state == "downloading" || state == "error") {
        // The builder shows the window at its idle size first, which is where it is centred;
        // the download state then resizes it from there.
        dialog->show();
        dialog->primaryButton()->click();
        if (state == "downloading") {
            dialog->setProgress(QStringLiteral("downloading"), 20'971'520, kSetupSize);
            // The builder drives the bar to its end value and holds it: its timer, which no
            // event has run yet, would otherwise ease the fill and move the sheen.
            fv::AnimatedProgressBar* bar = dialog->progressBar();
            bar->findChild<QTimer*>()->stop();
            bar->setFraction(20'971'520.0 / kSetupSize, false);
        } else {
            dialog->setError(QStringLiteral("The download failed."));
        }
    }
    return dialog;
}

void renameClass(fv::Json& node, const std::string& from, const std::string& to) {
    if (node.contains("class") && node.at("class") == from) {
        node["class"] = to;
    }
    if (node.contains("children")) {
        for (auto& child : node.at("children")) {
            renameClass(child, from, to);
        }
    }
}

// What the builder's _fit_natural_size does: the window the dialog's own sizeHint asks for once
// its notes are fitted to the width it ends up at.
QSize naturalSize(AppUpdateDialog& dialog) {
    dialog.show();
    QCoreApplication::processEvents();
    for (int round = 0; round < 6; ++round) {
        dialog.layout()->activate();
        const QSize hint = dialog.sizeHint();
        if (hint == dialog.size()) {
            break;
        }
        dialog.resize(hint);
        QCoreApplication::processEvents();
    }
    return dialog.size();
}

void shot(const std::string& state, const std::string& theme, int scale) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    MemorySettingsStore store;
    auto dialog = build(state, store);
    // The builder sizes the window this way before the harness shows it at that size, and the
    // layout passes decide where its hidden widgets sit in the dumped tree.
    naturalSize(*dialog);
    const std::string name = "app_update_dialog/" + state;
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const fv::Json pins = fv::screenshot::load_reference(name, variant);
    // render_reference.py dumps the tree after its grab, which also lays out the hidden
    // widgets, and OpenWhisper's notes view is the private class _NotesView. The tree is
    // compared first without geometry and class, then whole after the grab, with that one
    // class name mapped.
    fv::screenshot::Options options;
    options.ignore_keys = {"class", "geometry"};
    fv::screenshot::expect_matches(*dialog, name, variant, options);
    fv::Json expected = pins.at("tree");
    renameClass(expected, "_NotesView", "NotesView");
    for (const auto& difference :
         fv::screenshot::compare_trees(expected, fv::screenshot::dump_tree(*dialog))) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
}

void naturalSizeMatches(const std::string& state, const std::string& theme, int scale) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    MemorySettingsStore store;
    auto dialog = build(state, store);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const fv::Json pins = fv::screenshot::load_reference("app_update_dialog/" + state, variant);
    const QSize size = naturalSize(*dialog);
    CHECK(size.width() == pins.at("logical_size").at(0).get<int>());
    CHECK(size.height() == pins.at("logical_size").at(1).get<int>());
}

} // namespace

#define UPDATE_DIALOG_SHOT(NAME, STATE, THEME, SCALE, TITLE)                                       \
    TEST_CASE("update dialog " NAME " matches OpenWhisper, " TITLE, "[screenshot]") {              \
        shot(STATE, THEME, SCALE);                                                                 \
    }

UPDATE_DIALOG_SHOT("available", "available", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("available", "available", "light", 100, "light")
UPDATE_DIALOG_SHOT("available", "available", "dark", 130, "dark at 130 %")
UPDATE_DIALOG_SHOT("available-notify", "available-notify", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("available-notify", "available-notify", "light", 100, "light")
UPDATE_DIALOG_SHOT("up-to-date", "up-to-date", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("up-to-date", "up-to-date", "light", 100, "light")
UPDATE_DIALOG_SHOT("development", "development", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("development", "development", "light", 100, "light")
UPDATE_DIALOG_SHOT("check-failed", "check-failed", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("check-failed", "check-failed", "light", 100, "light")
UPDATE_DIALOG_SHOT("downloading", "downloading", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("downloading", "downloading", "light", 100, "light")
UPDATE_DIALOG_SHOT("downloading", "downloading", "dark", 130, "dark at 130 %")
UPDATE_DIALOG_SHOT("error", "error", "dark", 100, "dark")
UPDATE_DIALOG_SHOT("error", "error", "light", 100, "light")

TEST_CASE("the update dialog's natural size is the references' size", "[screenshot]") {
    for (const char* state : {"available", "available-notify", "up-to-date", "development",
                              "check-failed", "downloading", "error"}) {
        CAPTURE(state);
        naturalSizeMatches(state, "dark", 100);
    }
    naturalSizeMatches("available", "dark", 130);
    naturalSizeMatches("downloading", "dark", 130);
    naturalSizeMatches("available", "light", 100);
}
