// A scripted meeting through the real pipeline: fake capture sources replay a microphone and a
// system-audio recording into the real spools, the real meeting ASR decodes the chunks on a real
// SpeechService over speech::FakeBackend, an OnlineDiarizer over fake embeddings splits system
// audio into two speakers, and the real dashboard serves a host WebSocket on 127.0.0.1. End then
// publishes its events in order and a transcript-only finalization.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio/backend.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "diarize/testing/fake_embedding_model.h"
#include "engine_support.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "pyport/json.h"
#include "speech/testing/fake_backend.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::test::eventually;

namespace {

using std::chrono::seconds;

constexpr int kRate = 16000;
constexpr std::size_t kBlock = 1024;

// A 440 Hz tone (speech for the spool and the decoder) or silence.
std::vector<std::int16_t> signal(double seconds_long, bool loud) {
    std::vector<std::int16_t> out(static_cast<std::size_t>(seconds_long * kRate));
    if (loud) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::int16_t>(6000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 *
                                                                 static_cast<double>(i) / kRate));
        }
    }
    return out;
}

std::vector<std::int16_t> join(std::initializer_list<std::vector<std::int16_t>> parts) {
    std::vector<std::int16_t> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

struct Scripted {
    Scripted() {
        fv::speech::FakeScript script;
        script.texts = {"spoken words"};
        speech_engine = std::make_shared<fv::speech::FakeEngine>(script);
        speech = std::make_unique<fv::speech::SpeechService>(speech_engine->factory(), notify);
        fv::speech::BackendConfig config;
        config.backend = "parakeet";
        config.model_key = "parakeet-v3";
        config.model_label = "Parakeet TDT 0.6B v3";
        speech->select(fv::speech::Slot::Dictation, config);
        REQUIRE(eventually([this] {
            return speech->readiness(fv::speech::Slot::Meeting).state ==
                   fv::speech::ReadyState::Ready;
        }));

        backend->set_layout(
            {{.name = "MME", .default_input_device = 0}},
            {{.name = "Mic 0", .hostapi = 0, .max_input_channels = 1, .default_samplerate = 16000}},
            0);
        backend->set_loopbacks({{.id = "{a}", .name = "Speakers {a}", .is_default = true}});

        fv::meeting::MeetingEngineOptions options;
        options.title = "Scripted meeting";
        options.spool_root = sql.root.path() / "spool";
        options.speaker_id_backend = "local";
        fv::meeting::EngineDeps deps;
        deps.repository = sql.repo;
        deps.speech = speech.get();
        deps.audio = backend;
        deps.reactor = &reactor;
        deps.clock = &clock;
        deps.capture_timing =
            fv::meeting::CaptureTiming{.watchdog_interval_s = 3600.0, .retry_interval_s = 3.0};
        fv::meeting::SourceFactory sources;
        sources.mic = [this](const fv::audio::InputDevice& device) {
            mic = std::make_shared<fv::meeting::testing::FakeCaptureSource>("mic", device.index);
            return mic;
        };
        sources.loopback = [this](const fv::audio::LoopbackEndpoint& endpoint) {
            loopback =
                std::make_shared<fv::meeting::testing::FakeCaptureSource>("loopback", endpoint.id);
            return loopback;
        };
        deps.capture_sources = sources;
        deps.diarizer =
            [this](const std::optional<std::filesystem::path>&,
                   const std::optional<std::filesystem::path>&, fv::diarize::DiarizerHost& host,
                   const std::string& meeting_id) -> std::unique_ptr<fv::diarize::Diarizer> {
            // Two voices: the first system-audio segment and the second are far apart.
            auto fake = fv::diarize::testing::make_fake_embedder(
                {std::vector<float>{1.0F, 0.0F, 0.0F}, std::vector<float>{0.0F, 1.0F, 0.0F}});
            return std::make_unique<fv::diarize::OnlineDiarizer>(std::move(fake.embedder), host,
                                                                 meeting_id, clock);
        };
        engine = std::make_unique<MeetingEngine>(std::move(options), std::move(deps));
        subscription =
            engine->add_listener(inline_executor, [this](const fv::meeting::MeetingEvent& event) {
                const std::lock_guard lock(mutex);
                kinds.push_back(event.kind);
            });
    }
    ~Scripted() {
        engine->shutdown();
        engine.reset();
        reactor.stop(std::chrono::milliseconds(1000));
    }

    // Both channels at once, block by block, the clock moving with the audio.
    void play(const std::vector<std::int16_t>& mic_audio,
              const std::vector<std::int16_t>& loopback_audio) {
        const std::size_t length = std::max(mic_audio.size(), loopback_audio.size());
        for (std::size_t at = 0; at < length; at += kBlock) {
            const double t_mono = engine->clock().monotonic();
            if (at < mic_audio.size()) {
                const std::size_t count = std::min(kBlock, mic_audio.size() - at);
                mic->deliver(std::span(mic_audio).subspan(at, count), kRate, t_mono);
            }
            if (at < loopback_audio.size()) {
                const std::size_t count = std::min(kBlock, loopback_audio.size() - at);
                loopback->deliver(std::span(loopback_audio).subspan(at, count), kRate, t_mono);
            }
            clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(static_cast<double>(kBlock) / kRate)));
            // The spool writer keeps up with a real device's pace.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::vector<std::string> event_kinds() {
        const std::lock_guard lock(mutex);
        return kinds;
    }

    fv::meeting::test::SqlRepository sql;
    fv::FakeClock clock;
    fv::ManualExecutor notify;
    std::shared_ptr<fv::speech::FakeEngine> speech_engine;
    std::unique_ptr<fv::speech::SpeechService> speech;
    std::shared_ptr<fv::audio::testing::FakeBackend> backend =
        std::make_shared<fv::audio::testing::FakeBackend>();
    fv::net::Reactor reactor;
    std::shared_ptr<fv::meeting::testing::FakeCaptureSource> mic;
    std::shared_ptr<fv::meeting::testing::FakeCaptureSource> loopback;
    fv::meeting::test::InlineExecutor inline_executor;
    std::unique_ptr<MeetingEngine> engine;
    fv::Subscription subscription;
    std::mutex mutex;
    std::vector<std::string> kinds;
};

std::uint16_t port_of(const std::string& url) {
    return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
}

} // namespace

TEST_CASE("a scripted meeting labels Me and two speakers and reaches the dashboard",
          "[meeting_engine_scripted]") {
    Scripted meeting;
    const Json started = meeting.engine->start();
    REQUIRE(meeting.engine->wait_for_processing(seconds(10)));
    REQUIRE(meeting.engine->store()->snapshot().at("diarization_available") == true);
    const std::string id = started.at("meeting_id").get<std::string>();
    const std::string host_token =
        meeting.sql.repo->get_meeting(id)->at("host_token").get<std::string>();

    fv::net::testing::WsTestClient host;
    REQUIRE(host.connect(port_of(started.at("url").get<std::string>()), "/ws?token=" + host_token));
    const auto hello = host.receive(seconds(5));
    REQUIRE(hello);
    CHECK(fv::py::loads(hello->data).at("type") == "hello");

    // Me speaks for two seconds; then the first remote voice for 5.5 s, a pause, the second.
    meeting.play(
        join({signal(2.0, true), signal(8.0, false)}),
        join({signal(5.5, true), signal(1.0, false), signal(3.0, true), signal(0.5, false)}));
    meeting.engine->end();
    REQUIRE(meeting.engine->wait_for_end(seconds(30)));

    // Every chunk transcribed and stored.
    const auto chunks = meeting.sql.repo->get_audio_chunks(id);
    std::set<std::string> channels;
    for (const Json& chunk : chunks) {
        channels.insert(chunk.at("channel").get<std::string>());
        CHECK(chunk.at("asr_status") == "done");
    }
    CHECK(channels == std::set<std::string>{"mic", "loopback"});
    CHECK(meeting.sql.repo->get_meeting(id)->at("status") == "ended");

    // Me on the microphone; Speaker 1 and Speaker 2 on system audio.
    const Json state = meeting.engine->store()->snapshot();
    std::map<std::string, std::string> names;
    for (const auto& [participant_id, participant] : state.at("participants").items()) {
        names[participant_id] = participant.at("display_name").get<std::string>();
    }
    std::set<std::string> mic_speakers;
    std::set<std::string> loopback_speakers;
    for (const Json& row : meeting.sql.repo->get_segments(id, -1.0, std::nullopt)) {
        const Json& speaker = row.at("speaker_participant_id");
        const std::string name = speaker.is_string() ? names[speaker.get<std::string>()] : "";
        (row.at("channel") == "mic" ? mic_speakers : loopback_speakers).insert(name);
    }
    CHECK(mic_speakers == std::set<std::string>{"Me"});
    CHECK(loopback_speakers == std::set<std::string>{"Speaker 1", "Speaker 2"});

    // The dashboard heard the live captions, the transcript and the end.
    std::set<std::string> types;
    while (const auto message = host.receive(std::chrono::milliseconds(500))) {
        const Json parsed = fv::py::loads(message->data);
        types.insert(parsed.at("type").get<std::string>());
        if (parsed.at("type") == "segments") {
            CHECK_FALSE(parsed.at("items").empty());
        }
    }
    CHECK(types.contains("speech_preview"));
    CHECK(types.contains("segments"));
    CHECK(types.contains("meeting_ended"));
    host.close();

    // ended, then the model is released, then the background work may start.
    const auto kinds = meeting.event_kinds();
    const auto position = [&kinds](std::string_view kind) {
        return static_cast<std::size_t>(std::find(kinds.begin(), kinds.end(), kind) -
                                        kinds.begin());
    };
    REQUIRE(position("background_ready") < kinds.size());
    CHECK(position("ended") < position("asr_released"));
    CHECK(position("asr_released") < position("background_ready"));
    CHECK(state.at("finalization").at("status") == "disabled");
    CHECK(state.at("finalization").at("message") == "AI insights are off for this meeting.");
}

TEST_CASE("a twenty-second meeting over the real microphone and system audio",
          "[meeting_engine_scripted][interactive]") {
    // Opens the default microphone and records the default speaker's output: only the owner runs
    // it (ctest --preset windows-interactive). Speech is the fake backend; the audio is real.
    fv::meeting::test::SqlRepository sql;
    fv::speech::FakeScript script;
    script.texts = {"live words"};
    const auto speech_engine = std::make_shared<fv::speech::FakeEngine>(script);
    fv::ManualExecutor notify;
    fv::speech::SpeechService speech(speech_engine->factory(), notify);
    fv::speech::BackendConfig config;
    config.backend = "parakeet";
    config.model_key = "parakeet-v3";
    speech.select(fv::speech::Slot::Dictation, config);
    fv::net::Reactor reactor;

    fv::meeting::MeetingEngineOptions options;
    options.title = "Interactive meeting";
    options.spool_root = sql.root.path() / "spool";
    options.speaker_id_backend = "off";
    fv::meeting::EngineDeps deps;
    deps.repository = sql.repo;
    deps.speech = &speech;
    deps.audio = fv::audio::make_portaudio_backend();
    deps.reactor = &reactor;
    MeetingEngine engine(std::move(options), std::move(deps));
    const Json started = engine.start();
    std::this_thread::sleep_for(seconds(20));
    engine.end();
    REQUIRE(engine.wait_for_end(seconds(60)));
    CHECK_FALSE(sql.repo->get_audio_chunks(started.at("meeting_id").get<std::string>()).empty());
    CHECK(sql.repo->get_meeting(started.at("meeting_id").get<std::string>())->at("status") ==
          "ended");
    engine.shutdown();
    reactor.stop(std::chrono::milliseconds(1000));
}
