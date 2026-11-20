// test_agent_recording_indicator.py: a recording for an agent action names its agent on the
// overlay ("Agent · Codex") from before the live preview starts until the job's work is done;
// the badge goes when the capture is abandoned or replaced. The overlay halves run the real
// WaveformOverlay offscreen; the recording halves the controller's graph (the runtime's
// recording_destination_changed through the UI controller's overlay router).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <memory>
#include <string>

#include "../ui_overlay/overlay_test_support.h"
#include "automation/action_model.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "ui_controller/action_wiring.h"
#include "uimath/overlay_look.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

const QString kCodex = QStringLiteral("Agent · Codex");

// What a paint shows of the destination: the badge row is drawn, and it reads the destination
// (badgeText() is the destination whenever one is set outside Command Mode).
bool paintsDestination(Overlay& overlay, const QString& destination) {
    fvtest::paint(overlay);
    return !overlay.access.badgeRect().isNull() && overlay->recordingDestination() == destination &&
           overlay->currentState() != WaveformOverlay::kStateCommandListening;
}

} // namespace

TEST_CASE("test_agent_destination_survives_preview_captions_and_work", "[widget]") {
    const auto look = GENERATE(from_range(fv::kLooks));
    Overlay overlay;
    overlay->setRecordingLook(QString::fromUtf8(look.data(), static_cast<int>(look.size())));
    overlay->setLabelParts(false, false, false);
    overlay->setRecordingDestination(kCodex);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setLanguage(QStringLiteral("es"), {QStringLiteral("en"), QStringLiteral("es")});
    overlay->setHandsFree(true);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    for (const QString& state :
         {WaveformOverlay::kStateRecording, WaveformOverlay::kStateStreaming,
          WaveformOverlay::kStateProcessing, WaveformOverlay::kStateTranscribing,
          WaveformOverlay::kStateCleaning}) {
        INFO(state.toStdString());
        overlay->setState(state);
        if (state == WaveformOverlay::kStateStreaming) {
            overlay->updateStreamingText(
                QStringLiteral("Add a visible indicator for the agent recording ").repeated(3));
        }
        CHECK(paintsDestination(overlay, kCodex));
    }
}

TEST_CASE("test_destination_clears_when_capture_is_abandoned_or_replaced", "[widget]") {
    const QString end = GENERATE(WaveformOverlay::kStateCanceling, WaveformOverlay::kStateIdle,
                                 WaveformOverlay::kStateCommandListening,
                                 WaveformOverlay::kStateCopied, WaveformOverlay::kStateNotice);
    INFO(end.toStdString());
    Overlay overlay;
    overlay->setRecordingDestination(kCodex);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setState(end);
    CHECK_FALSE(paintsDestination(overlay, kCodex));
    CHECK(overlay->recordingDestination().isEmpty());
}

TEST_CASE("test_finished_capture_keeps_the_destination_through_fade_then_clears_it", "[widget]") {
    Overlay overlay;
    overlay->setRecordingDestination(kCodex);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    overlay->hide();
    CHECK(paintsDestination(overlay, kCodex));
    REQUIRE(overlay.access.fadingSince().has_value());
    // The fade runs its course.
    overlay.clock->advance(std::chrono::seconds(2));
    overlay.access.updateAnimation();
    CHECK_FALSE(overlay->isVisible());
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK_FALSE(paintsDestination(overlay, kCodex));
}

namespace {

// The controller's graph with the actions wired (they need the network client and the
// credential store) and a clipboard of its own.
struct DestinationRig {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    std::unique_ptr<fvtest::ControllerRig> rig;

    explicit DestinationRig(fv::Json actions) {
        fvtest::RigOptions options;
        options.http = &http;
        options.credentials = &credentials;
        options.settings["actions"] = std::move(actions);
        options.platform = [](fv::ControllerPlatform& platform) {
            platform.clipboardBackend = [] {
                return std::make_unique<fv::clipboard::FakeClipboardBackend>();
            };
        };
        rig = std::make_unique<fvtest::ControllerRig>(options);
    }
};

fv::Json agentAction(std::string agent, std::string mode) {
    return fv::Json{{"id", "agent"},    {"name", "My custom action name"},
                    {"kind", "agent"},  {"enabled", true},
                    {"shaping", "raw"}, {"params", {{"agent", agent}, {"mode", mode}}}};
}

} // namespace

TEST_CASE("test_recording_shows_its_selected_agent_before_streaming_starts", "[widget]") {
    const auto agent = GENERATE(from_range(fv::automation::AGENTS));
    const auto mode = GENERATE(from_range(fv::automation::AgentMode::ALL));
    INFO(std::string(agent) + " " + std::string(mode));
    DestinationRig made(fv::Json::array({agentAction(std::string(agent), std::string(mode))}));
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    const QString expected =
        QStringLiteral("Agent · %1")
            .arg(fv::qt::toQString(*fv::automation::agent_display_name(agent)));
    // Recorded as the overlay hears it: the destination arrives before the overlay first shows
    // the recording (the router shows it as the preview's set-up begins).
    QStringList destinationsAtRecording;
    QObject::connect(&rig.overlay(), &WaveformOverlay::stateChanged, &rig.overlay(),
                     [&](const QString& state) {
                         if (state == WaveformOverlay::kStateRecording) {
                             destinationsAtRecording << rig.overlay().recordingDestination();
                         }
                     });
    REQUIRE(rig.app().toggle_action_recording("agent"));
    REQUIRE(fvtest::waitUntil([&] { return !destinationsAtRecording.isEmpty(); }));
    CHECK(destinationsAtRecording.front() == expected);
    CHECK(rig.overlay().recordingDestination() == expected);
    // The capture keeps the agent it started with, whatever the settings say later.
    rig.settings().save_setting("actions", fv::Json::array());
    rig.overlay().setState(WaveformOverlay::kStateTranscribing);
    CHECK(rig.overlay().recordingDestination() == expected);
    rig.app().cancel();
    REQUIRE(fvtest::waitUntil([&] { return !rig.runtime().is_recording(); }));
}

TEST_CASE("test_plain_dictation_clears_the_previous_destination_even_during_a_fade", "[widget]") {
    DestinationRig made(fv::Json::array());
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    auto& overlay = rig.overlay();
    overlay.setRecordingDestination(kCodex);
    overlay.showAtCursor(WaveformOverlay::kStateTranscribing);
    overlay.hide();
    REQUIRE(rig.app().start_recording());
    REQUIRE(fvtest::waitUntil(
        [&] { return overlay.currentState() == WaveformOverlay::kStateRecording; }));
    CHECK(overlay.recordingDestination().isEmpty());
    rig.app().cancel();
    REQUIRE(fvtest::waitUntil([&] { return !rig.runtime().is_recording(); }));
}

TEST_CASE("test_refused_start_does_not_publish_an_agent_destination", "[widget]") {
    DestinationRig made(fv::Json::array({agentAction("codex", "handoff")}));
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    std::vector<std::string> published;
    auto link = rig.runtime().recording_destination_changed.connect(
        rig.uiExecutor(),
        [&](std::string destination) { published.push_back(std::move(destination)); });
    rig.audio().fail_open(-1, "Device unavailable [PaErrorCode -9985]");
    rig.audio().fail_open(0, "Device unavailable [PaErrorCode -9985]");
    CHECK_FALSE(rig.app().toggle_action_recording("agent"));
    fvtest::waitUntil([] { return false; }, std::chrono::milliseconds(200));
    CHECK(published.empty());
    CHECK(rig.overlay().recordingDestination().isEmpty());
}
