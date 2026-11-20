// The setup dialog against the references OpenWhisper rendered for it
// (tools/parity/builders/a31.py, tests/reference/meeting_setup_dialog/<state>/): opened on the
// way to a meeting, with the builder's pinned model inspection and audio check, each step waited
// for before the next.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QApplication>
#include <QElapsedTimer>

#include "audio/testing/fake_backend.h"
#include "meeting/setup_check.h"
#include "screenshot/screenshot.h"
#include "tab_support.h"
#include "ui_meeting/meeting_setup_dialog.h"
#include "ui_widgets/buttons.h"

using fv::MeetingSetupDialog;
using fv::meeting::AudioCheck;
using fv::meeting::ModelSetup;

namespace {

void waitUntilIdle(MeetingSetupDialog& dialog) {
    QElapsedTimer timer;
    timer.start();
    while (dialog.isBusy() && timer.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    REQUIRE_FALSE(dialog.isBusy());
    QApplication::processEvents();
}

// SETUP_STATES of the builder: the inspection's result, and the audio check's when one runs.
void setupShot(const std::string& theme, int scale, const char* state, ModelSetup models,
               std::optional<AudioCheck> audio) {
    const fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    auto fake = std::make_shared<fv::audio::testing::FakeBackend>();
    MeetingSetupDialog::Hooks hooks;
    hooks.inspect = [models](const fv::meeting::SetupDeps&) {
        return models;
    };
    if (audio) {
        hooks.checkAudio = [answer = *audio](fv::audio::Backend&, const fv::meeting::SetupDeps&,
                                             const fv::CancelToken&, bool) {
            return answer;
        };
    }
    MeetingSetupDialog dialog({}, *fake, true, nullptr, hooks);
    waitUntilIdle(dialog);
    if (audio) {
        dialog.audioButton()->click();
        waitUntilIdle(dialog);
    }
    const std::string name = std::string("meeting_setup_dialog/") + state;
    fv::screenshot::expect_matches(dialog, name,
                                   theme + "-windows11-dpr1-s" + std::to_string(scale));
}

const ModelSetup kSpeakerMissing{"Parakeet TDT 0.6B v3", "", "", true};
const ModelSetup kReady{"Parakeet TDT 0.6B v3", "", "", false};
const AudioCheck kHeard{"Signal detected", "Signal detected"};
const AudioCheck kSilent{"Signal detected", "No signal detected"};

} // namespace

TEST_CASE("reference screenshot meeting_setup_dialog initial dark", "[screenshot]") {
    setupShot("dark", 100, "initial", kSpeakerMissing, std::nullopt);
}

TEST_CASE("reference screenshot meeting_setup_dialog initial light", "[screenshot]") {
    setupShot("light", 100, "initial", kSpeakerMissing, std::nullopt);
}

TEST_CASE("reference screenshot meeting_setup_dialog initial dark 130", "[screenshot]") {
    setupShot("dark", 130, "initial", kSpeakerMissing, std::nullopt);
}

TEST_CASE("reference screenshot meeting_setup_dialog ready-check dark", "[screenshot]") {
    setupShot("dark", 100, "ready-check", kReady, kHeard);
}

TEST_CASE("reference screenshot meeting_setup_dialog ready-check light", "[screenshot]") {
    setupShot("light", 100, "ready-check", kReady, kHeard);
}

TEST_CASE("reference screenshot meeting_setup_dialog ready-check dark 130", "[screenshot]") {
    setupShot("dark", 130, "ready-check", kReady, kHeard);
}

TEST_CASE("reference screenshot meeting_setup_dialog unready-check dark", "[screenshot]") {
    setupShot("dark", 100, "unready-check", kReady, kSilent);
}

TEST_CASE("reference screenshot meeting_setup_dialog unready-check light", "[screenshot]") {
    setupShot("light", 100, "unready-check", kReady, kSilent);
}

TEST_CASE("reference screenshot meeting_setup_dialog unready-check dark 130", "[screenshot]") {
    setupShot("dark", 130, "unready-check", kReady, kSilent);
}
