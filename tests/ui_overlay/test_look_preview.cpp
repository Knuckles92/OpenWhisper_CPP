// The live recording overlay inside Settings (tests/test_recording_look.py, the preview's
// tests). The microphone is a real Recorder on audio::testing::FakeBackend, so no device opens;
// its level reaches the preview as the recorder's level signal.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QTemporaryDir>
#include <QTimer>

#include "audio/testing/fake_backend.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "overlay_test_support.h"
#include "pyport/text.h"
#include "recorder/recorder.h"
#include "settings/testing/memory_store.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/loudness.h"
#include "uimath/overlay_look.h"
#include "uimath/sample_voice.h"

using Catch::Approx;
using fv::RecordingLookPreview;
using fv::WaveformOverlay;

namespace fv {

struct RecordingLookPreviewTestAccess {
    explicit RecordingLookPreviewTestAccess(RecordingLookPreview& preview) : p(preview) {}

    void tick() { p.tick(); }
    Recorder* recorder() const { return p.recorder_.get(); }
    void setFactory(std::function<std::unique_ptr<Recorder>()> factory) {
        p.recorderFactory_ = std::move(factory);
    }

    RecordingLookPreview& p;
};

} // namespace fv

namespace {

// A word on a desk microphone, as raw RMS.
constexpr float kSpeech = 0.06f;

// Paths whose data root is the test's own folder: resolving them creates the data root, which
// must never be the owner's per-user folder.
fv::Paths pathsIn(const QTemporaryDir& folder) {
    const std::filesystem::path root(folder.path().toStdWString());
    fv::MapEnvReader env;
    env.set(std::string(fv::identity().env_prefix) + "DATA_DIR", fv::path_to_utf8(root / "data"));
    // The per-user base too, so nothing is looked up outside the folder.
    for (const char* name : {"LOCALAPPDATA", "HOME", "USERPROFILE", "XDG_DATA_HOME"}) {
        env.set(name, fv::path_to_utf8(root / "home"));
    }
    return fv::Paths::resolve(fv::identity(), env);
}

// The fixture's preview: recorders on a fake microphone named "USB mic", a pinned clock.
struct Preview {
    Preview()
        : clock(fvtest::pinnedClock()),
          backend(std::make_shared<fv::audio::testing::FakeBackend>()), paths(pathsIn(folder)) {
        backend->set_layout({fv::audio::testing::FakeHostApi{"MME", 0, -1}},
                            {fv::audio::testing::FakeDevice{"USB mic", 0, 1, 0, 44100.0}}, 0);
        REQUIRE(folder.isValid());
        widget = std::make_unique<RecordingLookPreview>([this] { return makeRecorder(); },
                                                        [] { return QStringLiteral("USB mic"); },
                                                        nullptr, nullptr, clock.get());
    }
    ~Preview() {
        widget->hide();
        widget->close();
    }
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    std::unique_ptr<fv::Recorder> makeRecorder() {
        fv::RecorderOptions options;
        options.output_file = take();
        auto recorder = std::make_unique<fv::Recorder>(*backend, paths, options);
        recorders.push_back(recorder.get());
        return recorder;
    }
    std::filesystem::path take() const {
        return std::filesystem::path(folder.path().toStdWString()) / "preview.wav";
    }

    RecordingLookPreview* operator->() { return widget.get(); }
    fv::WaveformOverlayTestAccess overlayAccess() {
        return fv::WaveformOverlayTestAccess(widget->overlay());
    }
    fv::RecordingLookPreviewTestAccess access() {
        return fv::RecordingLookPreviewTestAccess(*widget);
    }

    std::unique_ptr<fv::FakeClock> clock;
    std::shared_ptr<fv::audio::testing::FakeBackend> backend;
    // Before paths, which resolves inside it.
    QTemporaryDir folder;
    fv::Paths paths;
    std::vector<fv::Recorder*> recorders;
    std::unique_ptr<RecordingLookPreview> widget;
};

std::vector<QWidget*> otherVisibleWindows(const QWidget* except) {
    std::vector<QWidget*> found;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->isVisible() && widget != except) {
            found.push_back(widget);
        }
    }
    return found;
}

} // namespace

// from
// tests/test_recording_look.py::test_the_preview_animates_only_while_shown_and_opens_no_windows
TEST_CASE("the preview animates only while shown and opens no windows",
          "[widget][test_recording_look]") {
    Preview preview;
    CHECK_FALSE(preview->timer().isActive());
    CHECK_FALSE(preview->overlay().isVisible());
    preview->resize(560, 200);
    preview->show();
    CHECK(preview->timer().isActive());
    CHECK(preview->overlay().isVisible());
    CHECK(preview->overlay().currentState() == WaveformOverlay::kStateRecording);
    CHECK(otherVisibleWindows(preview.widget.get()).empty());
    CHECK(preview->overlay().isPreview());
    CHECK(preview->overlay().testAttribute(Qt::WA_TransparentForMouseEvents));
    preview->hide();
    CHECK_FALSE(preview->timer().isActive());
    CHECK(preview->overlay().currentState() == WaveformOverlay::kStateIdle);
}

// from tests/test_recording_look.py::test_the_preview_shows_the_chosen_look
TEST_CASE("the preview shows the chosen look", "[widget][test_recording_look]") {
    Preview preview;
    preview->show();
    for (const std::string_view look : fv::kLooks) {
        const QString name = QString::fromUtf8(look.data(), static_cast<qsizetype>(look.size()));
        preview->setLook(name);
        CHECK(preview->overlay().recordingLook() == name);
    }
    preview->setLabelParts(false, true, false);
    CHECK(preview->overlay().labelParts() == WaveformOverlay::LabelParts{false, true, false});
}

// from tests/test_recording_look.py::test_the_sample_voice_speaks_then_pauses
TEST_CASE("the preview's sample voice speaks then pauses", "[test_recording_look]") {
    CHECK((fv::sample_level(1.0) > 0.02 || fv::sample_level(1.1) > 0.02));
    CHECK(fv::sample_level(5.5) < 0.01);
}

// from tests/test_recording_look.py::test_the_microphone_opens_only_while_on_and_keeps_nothing
TEST_CASE("the microphone opens only while on and keeps nothing", "[widget][test_recording_look]") {
    Preview preview;
    preview->show();
    CHECK(preview.recorders.empty());
    preview->micButton().setChecked(true);
    REQUIRE(preview.recorders.size() == 1);
    fv::Recorder* recorder = preview.recorders.back();
    CHECK(recorder->is_recording());
    const QString label = preview->sourceLabel().text();
    CHECK(label.contains(QStringLiteral("USB mic")));
    CHECK(label.contains(QStringLiteral("Nothing is recorded")));
    CHECK(preview->micButton().text() == QStringLiteral("Stop microphone"));
    recorder->level.emit(kSpeech);
    QApplication::processEvents();
    CHECK(preview.overlayAccess().style().audio_levels()[0] ==
          Approx(fv::level_to_height(kSpeech)));
    preview->micButton().setChecked(false);
    // Cancelled and released: the device is closed and nothing was saved.
    CHECK(preview.access().recorder() == nullptr);
    CHECK_FALSE(preview->microphoneOpen());
    CHECK(preview.backend->open_streams() == 0);
    CHECK_FALSE(std::filesystem::exists(preview.take()));
    CHECK(preview->sourceLabel().text() == QStringLiteral("Playing a sample voice."));
    CHECK(preview->micButton().text() == QStringLiteral("Try with my microphone"));
}

// from tests/test_recording_look.py::test_leaving_the_page_closes_the_microphone
TEST_CASE("leaving the page closes the microphone", "[widget][test_recording_look]") {
    Preview preview;
    preview->show();
    preview->micButton().setChecked(true);
    REQUIRE(preview->microphoneOpen());
    preview->hide();
    CHECK_FALSE(preview->microphoneOpen());
    CHECK(preview.backend->open_streams() == 0);
    CHECK_FALSE(preview->micButton().isChecked());
    CHECK_FALSE(preview->overlay().isVisible());
}

// from tests/test_recording_look.py::test_a_microphone_that_will_not_open_says_why
TEST_CASE("a microphone that will not open says why", "[widget][test_recording_look]") {
    Preview preview;
    preview.backend->fail_open(-1, "device busy");
    preview.backend->fail_open(0, "device busy");
    preview->show();
    preview->micButton().setChecked(true);
    CHECK_FALSE(preview->micButton().isChecked());
    CHECK(preview->sourceLabel().text().startsWith(QStringLiteral("Couldn't open the microphone")));
    CHECK(preview->sourceLabel().text().contains(QStringLiteral("device busy")));
    CHECK_FALSE(preview->microphoneOpen());
}

// A recorder factory that throws is a missing device too: Settings carries on.
TEST_CASE("a recorder that cannot be made says why", "[widget]") {
    Preview preview;
    preview.access().setFactory(
        []() -> std::unique_ptr<fv::Recorder> { throw std::runtime_error("no audio system"); });
    preview->show();
    preview->micButton().setChecked(true);
    CHECK_FALSE(preview->micButton().isChecked());
    CHECK(preview->sourceLabel().text() ==
          QStringLiteral("Couldn't open the microphone: no audio system."));
}

// A factory with no recorder to give is a microphone that will not open, not a crash.
TEST_CASE("a factory without a recorder leaves the microphone off", "[widget]") {
    Preview preview;
    preview.access().setFactory([]() -> std::unique_ptr<fv::Recorder> { return nullptr; });
    preview->show();
    preview->micButton().setChecked(true);
    CHECK_FALSE(preview->micButton().isChecked());
    CHECK_FALSE(preview->microphoneOpen());
    CHECK(preview->sourceLabel().text() == QStringLiteral("Couldn't open the microphone."));
}

// The preview's overlay reads the settings as the floating one does (Python's reads the saved
// settings): the saved look at first, and the language chip with two dictation languages.
TEST_CASE("the preview's overlay reads the saved look and languages", "[widget]") {
    fv::MemorySettingsStore settings(
        fv::Json{{"recording_overlay_look", "dots"},
                 {"selected_model", "parakeet"},
                 {"dictation_languages", fv::Json::array({"en", "es"})},
                 {"dictation_active_language", "es"}});
    auto clock = fvtest::pinnedClock();
    RecordingLookPreview preview({}, {}, nullptr, &settings, clock.get());
    CHECK(preview.overlay().recordingLook() == QStringLiteral("dots"));
    preview.show();
    CHECK(preview.overlay().language() == QStringLiteral("es"));
    CHECK(fv::WaveformOverlayTestAccess(preview.overlay()).chipVisible());
    preview.hide();
    preview.close();
}

// from tests/test_recording_look.py::test_no_microphone_means_no_toggle
TEST_CASE("no microphone means no toggle", "[widget][test_recording_look]") {
    RecordingLookPreview widget;
    CHECK(widget.micButton().isHidden());
    CHECK(widget.sourceLabel().text() == QStringLiteral("Playing a sample voice."));
    CHECK(widget.objectName() == QStringLiteral("recordingLookPreview"));
    CHECK(widget.sourceLabel().objectName() == QStringLiteral("infoLabel"));
    widget.close();
}

// from tests/test_recording_look.py::test_the_preview_loops_through_the_looks_finish
TEST_CASE("the preview loops through the looks finish", "[widget][test_recording_look]") {
    Preview preview;
    preview->show();
    preview->setLook(QStringLiteral("bars"));
    preview->timer().stop();
    // The loop started at the clock's time when setLook replayed the entry.
    std::chrono::milliseconds at{0};
    const auto moveTo = [&](std::chrono::milliseconds when) {
        preview.clock->advance(when - at);
        at = when;
    };
    std::vector<QString> seen;
    for (const fv::SampleStage& stage : fv::kStages) {
        moveTo(std::chrono::milliseconds(static_cast<int>((stage.start + 0.05) * 1000)));
        preview.access().tick();
        seen.push_back(preview->overlay().currentState());
    }
    CHECK(seen[0] == WaveformOverlay::kStateRecording);
    CHECK(seen[1] == WaveformOverlay::kStateProcessing);
    CHECK(seen[2] == WaveformOverlay::kStateTranscribing);
    // The finish fades out.
    CHECK(preview.overlayAccess().fadingSince().has_value());
    moveTo(std::chrono::milliseconds(static_cast<int>(fv::kLoopS * 1000) + 10));
    preview.access().tick();
    CHECK(preview->overlay().currentState() == WaveformOverlay::kStateRecording);
    CHECK_FALSE(preview.overlayAccess().fadingSince().has_value());
}

// The sample voice drives the overlay while recording (recording_look_preview._tick).
TEST_CASE("the sample voice feeds the overlay while recording", "[widget]") {
    Preview preview;
    preview->show();
    preview->timer().stop();
    preview.clock->advance(std::chrono::milliseconds(1000));
    preview.access().tick();
    const double expected = fv::level_to_height(static_cast<float>(fv::sample_level(1.0)));
    CHECK(preview.overlayAccess().style().audio_levels()[0] == Approx(expected));
}

// _PREVIEW_PATH: the throwaway take in the temporary folder, named after the product.
TEST_CASE("the preview's take is a throwaway temporary file", "[unit]") {
    const std::filesystem::path path = RecordingLookPreview::previewPath();
    const std::string name = fv::py::lower(fv::identity().product) + "_overlay_preview.wav";
    CHECK(name == "fiorivoce_overlay_preview.wav");
    CHECK(path == std::filesystem::temp_directory_path() / name);
}
