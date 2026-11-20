// A chunk of the owner's recording through the real Parakeet engine on the Meeting lane. Skipped
// unless these are set (README.md):
//   FIORIVOCE_TEST_RUNTIME_DIR  an unpacked NeMo-Speech.cpp release
//   FIORIVOCE_TEST_MODEL        a Parakeet .gguf
//   FIORIVOCE_TEST_WAV          a recording with speech in it

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "asr_test_support.h"
#include "base/testing/manual_executor.h"
#include "capture_test_support.h"
#include "engine/platform.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/spool.h"
#include "speech/parakeet_backend.h"
#include "speech/service.h"

using fv::Json;
using fv::meeting::test::TempDir;
using fv::meeting_state::SpooledChunk;
using fv::meeting_state::TranscriptSegment;

namespace {

using namespace std::chrono_literals;

std::optional<fv::speech::BackendConfig> installed() {
    const auto runtime = fv::env_utf8("FIORIVOCE_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("FIORIVOCE_TEST_MODEL");
    if (!runtime || !model) {
        return std::nullopt;
    }
    fv::speech::BackendConfig config;
    config.backend = "parakeet";
    config.runtime_dir = fv::base_utf8::path_from_utf8(*runtime);
    config.model_path = fv::base_utf8::path_from_utf8(*model);
    config.model_key = "parakeet-v3";
    config.model_label = "Parakeet TDT 0.6B v3";
    // The CPU: a GPU may round differently between two loads.
    config.requested_device = "cpu";
    config.device = "cpu";
    return config;
}

} // namespace

TEST_CASE("a chunk of the test recording decodes on the meeting lane as the backend decodes it",
          "[decode]") {
    const auto config = installed();
    const auto wav = fv::env_utf8("FIORIVOCE_TEST_WAV");
    if (!config || !wav) {
        SKIP("Set FIORIVOCE_TEST_RUNTIME_DIR, FIORIVOCE_TEST_MODEL and FIORIVOCE_TEST_WAV.");
    }
    // The spool's chunk: up to ten seconds of the recording at 16 kHz mono.
    const auto [native, rate] = fv::meeting::load_wav_int16(fv::base_utf8::path_from_utf8(*wav));
    const std::size_t take =
        std::min<std::size_t>(native.size(), static_cast<std::size_t>(rate) * 10);
    const std::vector<std::int16_t> chunk_frames =
        fv::meeting::resample_to_16k(std::span<const std::int16_t>(native.data(), take), rate);
    REQUIRE_FALSE(fv::meeting::is_digital_silence(chunk_frames));
    TempDir folder;
    fv::meeting::test::write_wav_samples(folder.path() / "mic_00000.wav", chunk_frames);
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    Json fields = Json::object();
    fields["meeting_id"] = "m_decode";
    fields["channel"] = "mic";
    fields["seq"] = 0;
    fields["file_path"] = fv::base_utf8::path_to_utf8(folder.path() / "mic_00000.wav");
    fields["start_s"] = 12.5;
    fields["duration_s"] = static_cast<double>(chunk_frames.size()) / 16000;
    fields["sample_rate"] = 16000;
    SpooledChunk chunk;
    chunk.chunk_id = repository->register_chunk(fields);
    chunk.meeting_id = "m_decode";
    chunk.channel = "mic";
    chunk.file_path = fields["file_path"].get<std::string>();
    chunk.start_s = 12.5;
    chunk.duration_s = fields["duration_s"].get<double>();
    chunk.sample_rate = 16000;

    std::vector<TranscriptSegment> through_engine;
    {
        fv::ManualExecutor notify;
        fv::speech::SpeechService service(
            [](const fv::speech::BackendConfig& c) {
                return std::make_unique<fv::speech::ParakeetBackend>(c);
            },
            notify);
        service.select(fv::speech::Slot::Dictation, *config);
        fv::meeting::MeetingAsrEngine engine(service, repository, "m_decode", "auto",
                                             fv::system_clock());
        fv::CancelSource cancel;
        REQUIRE(engine.load_backend(cancel.token()));
        std::mutex mutex;
        engine.start([&](const SpooledChunk& done, std::vector<TranscriptSegment> segments) {
            const std::lock_guard lock(mutex);
            through_engine = std::move(segments);
            repository->set_chunk_status(done.chunk_id, "done", std::nullopt);
        });
        REQUIRE(engine.enqueue(chunk));
        REQUIRE(engine.drain(5min));
        engine.stop();
    }

    // LocalSpeechBackend's SpeechDecoder: the same samples, the Meeting lane, "auto".
    fv::speech::ParakeetBackend backend(*config);
    backend.reload_model();
    REQUIRE(backend.is_available());
    fv::speech::DecodeJob job;
    fv::speech::AudioClip clip;
    auto samples = std::make_shared<const std::vector<float>>(
        fv::meeting::prepare_for_decoder(chunk_frames, 16000));
    clip.end = samples->size();
    clip.samples = samples;
    job.input = clip;
    job.lane = fv::speech::Lane::Meeting;
    job.options.language = "auto";
    const auto expected =
        fv::meeting::chunk_segments("m_decode", chunk, backend.decode(job).transcript.segments);

    REQUIRE_FALSE(expected.empty());
    REQUIRE(through_engine.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("segment " << i);
        CHECK(through_engine[i].text == expected[i].text);
        CHECK(through_engine[i].segment_id == expected[i].segment_id);
        CHECK(through_engine[i].start_s == expected[i].start_s);
        CHECK(through_engine[i].end_s == expected[i].end_s);
        CHECK(through_engine[i].start_s >= 12.5);
    }
}
