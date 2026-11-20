// The microphone check over audio::testing::FakeBackend: a live level bar and a plain verdict.
// The real-device variant is tagged [interactive] and is not run by the default presets.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include <QApplication>
#include <QProgressBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "audio/backend.h"
#include "microphone_support.h"
#include "recorder/journal.h"
#include "ui_record/microphone_check.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/wrapped_label.h"
#include "upload_widgets_support.h"

namespace {

using fv::MicrophoneCheck;

QString toneOf(const MicrophoneCheck& check) {
    return check.status()->property(fv::ui::prop::tone).toString();
}

// A block of constant samples: its RMS is value / 32768, and the recorder's meter smooths
// 0.3 * previous + 0.7 * RMS per block.
std::vector<std::int16_t> constantBlock(std::int16_t value) {
    return std::vector<std::int16_t>(fv::RECORDER_CHUNK, value);
}

// Hands `blocks` loud blocks to the open fake microphone and waits until the recorder has
// metered them; the levels then wait in the GUI thread's queue.
void speak(fvtest::MicrophoneRig& rig, int blocks, std::int16_t value = 12000) {
    REQUIRE(rig.recorder != nullptr);
    auto streams = rig.backend->streams();
    REQUIRE_FALSE(streams.empty());
    for (int i = 0; i < blocks; ++i) {
        streams.back()->deliver(constantBlock(value));
    }
    REQUIRE(rig.recorder->sync_capture());
}

// Lets the queued level deliveries run.
void drain() {
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(5);
        QApplication::processEvents();
    }
}

} // namespace

TEST_CASE("the microphone check maps a level to the bar by the square root of its span",
          "[widget]") {
    // Spec: sqrt((level - 0.004) / (0.2 - 0.004)) * 100, rounded half to even.
    CHECK(MicrophoneCheck::barValue(0.0) == 0);
    CHECK(MicrophoneCheck::barValue(0.004) == 0);
    CHECK(MicrophoneCheck::barValue(0.003) == 0);
    CHECK(MicrophoneCheck::barValue(0.02) == 29);
    CHECK(MicrophoneCheck::barValue(0.004 + 0.36 * 0.196) == 60);
    CHECK(MicrophoneCheck::barValue(0.2) == 100);
    CHECK(MicrophoneCheck::barValue(1.0) == 100);
    CHECK(MicrophoneCheck::kListenMs == 15000);
    CHECK(MicrophoneCheck::kQuietMs == 4000);
    CHECK(MicrophoneCheck::kHeard == 0.02);
}

TEST_CASE("the microphone check starts closed and opens only on start", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    CHECK_FALSE(check.isRunning());
    CHECK(rig.backend->open_streams() == 0);
    CHECK(rig.recorder == nullptr);
    CHECK(check.bar()->objectName() == QStringLiteral("microphoneCheckBar"));
    CHECK(check.bar()->minimum() == 0);
    CHECK(check.bar()->maximum() == 100);
    CHECK_FALSE(check.bar()->isTextVisible());
    CHECK(check.bar()->height() == 8);
    CHECK(check.bar()->accessibleName() == QStringLiteral("Microphone level"));
    CHECK(check.status()->objectName() == QStringLiteral("guidedDetail"));
    CHECK(check.status()->textFormat() == Qt::PlainText);
    CHECK(check.stopTimer()->interval() == 15000);
    CHECK(check.quietTimer()->interval() == 4000);

    QSignalSpy running(&check, &MicrophoneCheck::runningChanged);
    REQUIRE(check.start());
    CHECK(check.isRunning());
    CHECK(rig.backend->open_streams() == 1);
    CHECK(check.status()->text() ==
          QStringLiteral("Listening. Say something and watch the bar. Nothing is recorded."));
    CHECK(toneOf(check).isEmpty());
    CHECK(check.stopTimer()->isActive());
    CHECK(check.quietTimer()->isActive());
    REQUIRE(running.count() == 1);
    CHECK(running.at(0).at(0).toBool());
}

TEST_CASE("levels reach the bar only through the GUI thread's queue", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    REQUIRE(check.start());

    speak(rig, 6);
    // The recorder metered the blocks on its own thread and posted them; nothing has been
    // applied while this thread runs no events.
    CHECK(check.bar()->value() == 0);
    CHECK(check.quietTimer()->isActive());

    drain();
    CHECK(check.bar()->value() > 0);
    CHECK(check.status()->text() == QStringLiteral("Hearing you. This microphone works."));
    CHECK(toneOf(check) == QStringLiteral("ready"));
    // "Heard" cancels the quiet warning.
    CHECK_FALSE(check.quietTimer()->isActive());
}

TEST_CASE("a level of 0.02 is heard and a quieter one is not", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    REQUIRE(check.start());

    check.applyLevel(0.019f);
    CHECK(check.bar()->value() == MicrophoneCheck::barValue(0.019f));
    CHECK(check.status()->text() ==
          QStringLiteral("Listening. Say something and watch the bar. Nothing is recorded."));
    check.applyLevel(0.02001f);
    CHECK(check.status()->text() == QStringLiteral("Hearing you. This microphone works."));
    CHECK(toneOf(check) == QStringLiteral("ready"));
    // A later quiet level moves the bar but keeps the verdict.
    check.applyLevel(0.0f);
    CHECK(check.bar()->value() == 0);
    CHECK(check.status()->text() == QStringLiteral("Hearing you. This microphone works."));
}

TEST_CASE("four quiet seconds warn that nothing is heard yet", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    REQUIRE(check.start());

    fvtest::expire(check.quietTimer());
    CHECK(check.status()->text() ==
          QStringLiteral("Not hearing anything yet. Check that the microphone isn't muted, or "
                         "pick another one."));
    CHECK(toneOf(check) == QStringLiteral("attention"));
    CHECK(check.isRunning());

    // Once heard, a stale quiet timeout says nothing.
    check.applyLevel(0.05f);
    fvtest::expire(check.quietTimer());
    CHECK(check.status()->text() == QStringLiteral("Hearing you. This microphone works."));
}

TEST_CASE("stopping without having heard anything says so and closes the microphone", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    QSignalSpy running(&check, &MicrophoneCheck::runningChanged);
    REQUIRE(check.start());
    check.applyLevel(0.01f);

    check.stop();
    CHECK_FALSE(check.isRunning());
    CHECK(rig.backend->open_streams() == 0);
    CHECK(check.bar()->value() == 0);
    CHECK(check.status()->text() ==
          QStringLiteral("Didn't hear anything. Check that the microphone isn't muted, or pick "
                         "another one."));
    CHECK(toneOf(check) == QStringLiteral("attention"));
    CHECK_FALSE(check.stopTimer()->isActive());
    CHECK_FALSE(check.quietTimer()->isActive());
    REQUIRE(running.count() == 2);
    CHECK_FALSE(running.at(1).at(0).toBool());

    // A second stop is a no-op.
    check.stop();
    CHECK(running.count() == 2);
}

TEST_CASE("stopping after hearing keeps the good verdict on screen", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    REQUIRE(check.start());
    speak(rig, 6);
    drain();

    check.stop();
    CHECK(check.bar()->value() == 0);
    CHECK(check.status()->text() == QStringLiteral("Hearing you. This microphone works."));
    CHECK(toneOf(check) == QStringLiteral("ready"));
    CHECK(rig.backend->open_streams() == 0);
}

TEST_CASE("fifteen seconds end the test by themselves", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    REQUIRE(check.start());
    fvtest::expire(check.stopTimer());
    CHECK_FALSE(check.isRunning());
    CHECK(rig.backend->open_streams() == 0);
}

TEST_CASE("the microphone check never records anything", "[widget]") {
    fvtest::MicrophoneRig rig;
    {
        MicrophoneCheck check(rig.factory());
        REQUIRE(check.start());
        speak(rig, 4);
        check.stop();
        CHECK_FALSE(std::filesystem::exists(rig.outputFile()));
    }
    // Neither the take nor a recording journal outlives the check.
    CHECK_FALSE(std::filesystem::exists(rig.outputFile()));
    CHECK(rig.backend->open_streams() == 0);
}

TEST_CASE("hiding the check stops it", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    check.show();
    REQUIRE(check.start());
    CHECK(check.isRunning());
    check.hide();
    CHECK_FALSE(check.isRunning());
    CHECK(rig.backend->open_streams() == 0);
}

TEST_CASE("destroying the check closes its microphone", "[widget]") {
    fvtest::MicrophoneRig rig;
    {
        MicrophoneCheck check(rig.factory());
        REQUIRE(check.start());
        CHECK(rig.backend->open_streams() == 1);
    }
    CHECK(rig.backend->open_streams() == 0);
}

TEST_CASE("destroying a running check keeps no recording for recovery", "[widget]") {
    fvtest::MicrophoneRig rig;
    {
        MicrophoneCheck check(rig.factory());
        REQUIRE(check.start());
        // Audio in the journal: a take released without a cancel would stay on disk for
        // recovery, as an interrupted Quick Record does.
        speak(rig, 4);
    }
    CHECK(rig.backend->open_streams() == 0);
    CHECK_FALSE(std::filesystem::exists(rig.outputFile()));
    const std::vector<std::filesystem::path> recovered =
        fv::recover_recordings(rig.outputFile(), rig.root.path() / "recovered");
    CHECK(recovered.empty());
}

TEST_CASE("starting again closes the first microphone first", "[widget]") {
    fvtest::MicrophoneRig rig;
    MicrophoneCheck check(rig.factory());
    QSignalSpy running(&check, &MicrophoneCheck::runningChanged);
    REQUIRE(check.start());
    check.applyLevel(0.05f);
    REQUIRE(check.start());
    CHECK(rig.backend->open_streams() == 1);
    // The earlier verdict is forgotten: a new test begins with a quiet bar and the listening text.
    CHECK(check.bar()->value() == 0);
    CHECK(check.status()->text() ==
          QStringLiteral("Listening. Say something and watch the bar. Nothing is recorded."));
    CHECK(toneOf(check).isEmpty());
    CHECK(running.count() == 3); // true, false, true
}

TEST_CASE("a microphone that will not open says why", "[widget]") {
    fvtest::MicrophoneRig rig;
    // No device at all: the recorder reports "No audio device available".
    rig.backend->set_layout({}, {}, -1);
    MicrophoneCheck check(rig.factory());
    QSignalSpy running(&check, &MicrophoneCheck::runningChanged);
    CHECK_FALSE(check.start());
    CHECK_FALSE(check.isRunning());
    CHECK(check.status()->text() ==
          QStringLiteral("Couldn't open the microphone: No audio device available."));
    CHECK(toneOf(check) == QStringLiteral("attention"));
    CHECK(running.count() == 0);
    CHECK_FALSE(check.stopTimer()->isActive());
}

TEST_CASE("a factory that throws is shown as the reason and does not break setup", "[widget]") {
    MicrophoneCheck check(
        []() -> std::unique_ptr<fv::Recorder> { throw std::runtime_error("Device unavailable"); });
    CHECK_FALSE(check.start());
    CHECK(check.status()->text() ==
          QStringLiteral("Couldn't open the microphone: Device unavailable."));
    CHECK(toneOf(check) == QStringLiteral("attention"));

    MicrophoneCheck silent([]() -> std::unique_ptr<fv::Recorder> { throw std::runtime_error(""); });
    CHECK_FALSE(silent.start());
    CHECK(silent.status()->text() ==
          QStringLiteral(
              "Couldn't open the microphone. Pick another one, or check that OpenWhisper_CPP "
              "is allowed to use it."));
}

TEST_CASE("a factory that makes no recorder fails with the plain text", "[widget]") {
    MicrophoneCheck check([]() -> std::unique_ptr<fv::Recorder> { return nullptr; });
    QSignalSpy running(&check, &MicrophoneCheck::runningChanged);
    CHECK_FALSE(check.start());
    CHECK_FALSE(check.isRunning());
    CHECK(check.status()->text() ==
          QStringLiteral(
              "Couldn't open the microphone. Pick another one, or check that OpenWhisper_CPP "
              "is allowed to use it."));
    CHECK(toneOf(check) == QStringLiteral("attention"));
    CHECK(running.count() == 0);
    CHECK_FALSE(check.stopTimer()->isActive());
    // A later stop has nothing to close.
    check.stop();
    CHECK(running.count() == 0);
}

// The real-device variant: opens the system's default microphone, which takes it over for the
// test.
// It carries [interactive] and runs only in the windows-interactive preset.
TEST_CASE("the microphone check opens and closes the real default microphone", "[interactive]") {
    fv::testkit::TempDataRoot root;
    const auto backend = fv::audio::make_portaudio_backend();
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const std::filesystem::path output = root.path() / "microphone-check.wav";
    MicrophoneCheck check([&] {
        fv::RecorderOptions options;
        options.output_file = output;
        return std::make_unique<fv::Recorder>(*backend, paths, std::move(options));
    });
    REQUIRE(check.start());
    QTest::qWait(1000);
    CHECK(check.isRunning());
    check.stop();
    CHECK_FALSE(check.isRunning());
    CHECK_FALSE(std::filesystem::exists(output));
}
