// IncrementalSession and IncrementalDictation (services/incremental_dictation.py): early window
// decodes must never change the transcript.
//
// The engine is a real ParakeetBackend behind a real SpeechService, with its decoder replaced by
// one that gives each distinct window its own label, so equal text means the same audio reached
// the engine window for window. Capture goes onto a PcmTape, the saved WAV holds the tape plus
// the recorder's 500 ms of padding, and the 16 kHz stream goes through the real Resampler16k.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio/resampler.h"
#include "base/hash.h"
#include "base/testing/manual_executor.h"
#include "dsp/window_splitter.h"
#include "engine/error.h"
#include "engine/platform.h"
#include "golden/golden.h"
#include "speech/detail/crc32.h"
#include "speech/detail/incremental_checks.h"
#include "speech/detail/wav_pcm.h"
#include "speech/file_windows.h"
#include "speech/incremental_session.h"
#include "speech/parakeet_backend.h"
#include "speech/window_decode.h"
#include "speech_test_support.h"
#include "testkit/testkit.h"

using fv::RecognitionOptions;
using fv::speech::AudioClip;
using fv::speech::BackendConfig;
using fv::speech::IncrementalDictation;
using fv::speech::IncrementalSession;
using fv::speech::Lane;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::test::parakeet_config;
using fv::speech::test::pump_until;
using namespace std::chrono_literals;

namespace {

constexpr int kRate = 44100;
constexpr int kBlock = 1024;
constexpr int kBlocksPerPoll = 43; // about one second of 1024-frame callbacks
constexpr auto kFastPoll = 10ms;

using Pcm = std::vector<std::int16_t>;

// A deterministic generator (the numpy Generator of the Python tests, by other means).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ULL + 1) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform(double low = 0.0, double high = 1.0) {
        return low + (high - low) * static_cast<double>(next() >> 11) * 0x1.0p-53;
    }
    int integer(int low, int high) { // inclusive
        return low + static_cast<int>(next() % static_cast<std::uint64_t>(high - low + 1));
    }
    double normal() {
        double sum = 0.0;
        for (int i = 0; i < 12; ++i) {
            sum += uniform();
        }
        return sum - 6.0;
    }

private:
    std::uint64_t state_;
};

// Bursts of tones and noise between quiet gaps, so split points vary (speech_like).
Pcm speech_like(double seconds, std::uint64_t seed) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    Pcm out;
    out.reserve(total + 2 * kRate);
    while (out.size() < total) {
        const auto n = static_cast<std::size_t>(rng.uniform(0.2, 1.6) * kRate);
        if (rng.uniform() < 0.3) {
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(static_cast<std::int16_t>(std::lround(rng.normal() * 30)));
            }
        } else {
            const double frequency = rng.uniform(120, 900);
            const double amplitude = rng.uniform(1500, 9000);
            for (std::size_t i = 0; i < n; ++i) {
                const double value =
                    std::sin(2 * 3.141592653589793 * frequency * static_cast<double>(i) / kRate) *
                        amplitude +
                    rng.normal() * 800;
                out.push_back(
                    static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L)));
            }
        }
    }
    out.resize(total);
    return out;
}

std::string digest_of(std::span<const float> samples) {
    fv::Sha256 hash;
    hash.update(std::as_bytes(samples));
    return hash.hexdigest();
}

// The fake recognizer: one label per distinct window, some of them empty (Engine).
struct LabelEngine {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> label_cycle{"alpha", "", "gamma", "delta", "", "zeta", "eta", "theta"};
    std::map<std::string, std::string> labels;
    std::vector<std::size_t> samples; // each decode's length
    std::vector<std::string> languages;
    std::optional<std::string> fail;
    bool hold = false;
    bool entered = false;

    std::size_t calls() {
        std::lock_guard lock(mutex);
        return samples.size();
    }
    std::size_t label_count() {
        std::lock_guard lock(mutex);
        return labels.size();
    }
    bool wait_entered(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [this] { return entered; });
    }
    void open() {
        {
            std::lock_guard lock(mutex);
            hold = false;
        }
        changed.notify_all();
    }
};

class LabelDecoder final : public fv::speech::Decoder {
public:
    explicit LabelDecoder(std::shared_ptr<LabelEngine> engine) : engine_(std::move(engine)) {}
    fv::Transcript transcribe(std::span<const float> audio,
                              const RecognitionOptions& options) override {
        std::unique_lock lock(engine_->mutex);
        engine_->entered = true;
        engine_->changed.notify_all();
        engine_->changed.wait_for(lock, 10s, [this] { return !engine_->hold; });
        if (engine_->fail) {
            throw fv::EngineError(*engine_->fail);
        }
        const auto [it, added] = engine_->labels.try_emplace(
            digest_of(audio),
            engine_->label_cycle[engine_->labels.size() % engine_->label_cycle.size()]);
        engine_->samples.push_back(audio.size());
        engine_->languages.push_back(options.language);
        fv::Transcript transcript;
        transcript.text = it->second;
        return transcript;
    }

private:
    std::shared_ptr<LabelEngine> engine_;
};

// What reached the backend, by lane, and a hook that runs before each decode.
struct LaneLog {
    std::mutex mutex;
    std::vector<Lane> lanes;
    std::vector<std::vector<std::string>> phrases;
    std::function<void(const fv::speech::DecodeJob&)> before;

    std::vector<Lane> all() {
        std::lock_guard lock(mutex);
        return lanes;
    }
};

class LaneBackend final : public fv::speech::Backend {
public:
    LaneBackend(std::unique_ptr<fv::speech::ParakeetBackend> inner, std::shared_ptr<LaneLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    std::string_view id() const override { return inner_->id(); }
    fv::speech::Capabilities capabilities() const override { return inner_->capabilities(); }
    fv::speech::Readiness load(const fv::CancelToken& cancel) override {
        return inner_->load(cancel);
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob& job) override {
        std::function<void(const fv::speech::DecodeJob&)> before;
        {
            std::lock_guard lock(log_->mutex);
            log_->lanes.push_back(job.lane);
            log_->phrases.push_back(job.phrases);
            before = log_->before;
        }
        if (before) {
            before(job);
        }
        return inner_->decode(job);
    }
    void release() override { inner_->release(); }

private:
    std::unique_ptr<fv::speech::ParakeetBackend> inner_;
    std::shared_ptr<LaneLog> log_;
};

struct Harness {
    Harness()
        : engine(std::make_shared<LabelEngine>()), lanes(std::make_shared<LaneLog>()),
          service(
              [e = engine, l = lanes](const BackendConfig& config) {
                  auto inner = std::make_unique<fv::speech::ParakeetBackend>(
                      config, [e](const fv::RecognizerConfig&) {
                          return std::make_unique<LabelDecoder>(e);
                      });
                  return std::make_unique<LaneBackend>(std::move(inner), l);
              },
              notify) {
        select(parakeet_config("cpu"));
        tape = std::make_shared<fv::audio::PcmTape>(kRate);
        wav = data.path() / "recorded_audio.wav";
    }

    void select(BackendConfig config) {
        service.select(Slot::Dictation, std::move(config));
        REQUIRE(pump_until(notify, [this] {
            return service.readiness(Slot::Dictation).state == ReadyState::Ready;
        }));
    }

    std::unique_ptr<IncrementalSession> session(RecognitionOptions options = {},
                                                std::vector<std::string> phrases = {},
                                                std::chrono::milliseconds poll = 1000ms) {
        auto made = IncrementalSession::create(service, tape, options, "parakeet",
                                               std::move(phrases), poll);
        REQUIRE(made);
        return made;
    }

    // Feeds PCM onto the tape in recorder blocks, polling the session as its thread would.
    void capture(std::span<const std::int16_t> pcm, IncrementalSession* polled = nullptr,
                 int polls = kBlocksPerPoll, Rng* rng = nullptr) {
        int since = 0;
        int due = polls;
        std::size_t start = 0;
        while (start < pcm.size()) {
            const std::size_t size =
                rng == nullptr ? kBlock : static_cast<std::size_t>(rng->integer(1, 4 * kBlock));
            const auto chunk = pcm.subspan(start, std::min(size, pcm.size() - start));
            tape->append(chunk);
            recorded.insert(recorded.end(), chunk.begin(), chunk.end());
            start += chunk.size();
            if (++since >= due) {
                if (polled != nullptr) {
                    polled->poll();
                }
                since = 0;
                due = rng == nullptr ? polls : rng->integer(5, 3 * polls);
            }
        }
    }

    // Ends capture and saves the WAV as the recorder does: the PCM, then 500 ms of silence.
    std::filesystem::path stop(IncrementalSession* polled = nullptr) {
        if (polled != nullptr) {
            polled->poll();
        }
        Pcm saved = recorded;
        saved.resize(saved.size() + kRate / 2, 0);
        fv::speech::test::write_wav(wav, kRate, 1, saved);
        return wav;
    }

    // LocalSpeechBackend.transcribe(path): the file decoded window by window.
    std::string file_text(const std::filesystem::path& path, RecognitionOptions options = {}) {
        return fv::speech::decode_file(service, path, options).transcript.text;
    }

    fv::testkit::TempDataRoot data;
    std::shared_ptr<LabelEngine> engine;
    std::shared_ptr<LaneLog> lanes;
    fv::ManualExecutor notify;
    fv::speech::SpeechService service;
    std::shared_ptr<fv::audio::PcmTape> tape;
    Pcm recorded;
    std::filesystem::path wav;
};

// The slot's thread polls once at start and then waits this long, so the test follows the
// session by hand, as Python's tests call poll().
constexpr auto kByHand = std::chrono::hours(24);

std::shared_ptr<IncrementalSession> hand_polled(IncrementalDictation& slot, Harness& h,
                                                RecognitionOptions options = {},
                                                std::vector<std::string> phrases = {}) {
    slot.start(h.tape, options, "parakeet", h.wav, std::move(phrases), kByHand);
    auto session = slot.session();
    REQUIRE(session);
    return session;
}

struct LongCapture {
    std::filesystem::path path;
    std::size_t calls_at_change = 0;
};

// Records 66 s into `session`, decoding early; `change` runs at `at` seconds (long_session).
LongCapture long_capture(Harness& h, IncrementalSession& session,
                         const std::function<void(IncrementalSession&)>& change = {}, int at = 35) {
    LongCapture out;
    const Pcm pcm = speech_like(66, 11);
    h.capture(std::span(pcm).first(static_cast<std::size_t>(at) * kRate), &session);
    REQUIRE(session.early_windows() == 1);
    if (change) {
        change(session);
    }
    out.calls_at_change = h.engine->calls();
    h.capture(std::span(pcm).subspan(static_cast<std::size_t>(at) * kRate), &session);
    out.path = h.stop(&session);
    return out;
}

bool wait_for(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

bool identical(const AudioClip& a, const AudioClip& b) {
    return a.offset_s == b.offset_s && a.end - a.begin == b.end - b.begin &&
           std::equal(a.samples->begin() + static_cast<std::ptrdiff_t>(a.begin),
                      a.samples->begin() + static_cast<std::ptrdiff_t>(a.end),
                      b.samples->begin() + static_cast<std::ptrdiff_t>(b.begin),
                      [](float x, float y) {
                          return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
                      });
}

RecognitionOptions language(std::string code) {
    RecognitionOptions options;
    options.language = std::move(code);
    return options;
}

} // namespace

// --- the shared splitter and resampler ---------------------------------------------------------

TEST_CASE("test_windows_refactor_is_bit_identical_to_the_original",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_windows_refactor_is_bit_identical_to_the_original
    Harness h;
    h.capture(speech_like(71, 1));
    const auto path = h.stop();
    const auto windows = fv::speech::file_windows(path);

    // windows() as it was before WindowSplitter: the whole stream, cut while 30 s are pending.
    const auto saved = fv::speech::detail::read_wav_pcm(path);
    Pcm pcm(saved.pcm.size() / 2);
    std::memcpy(pcm.data(), saved.pcm.data(), pcm.size() * 2);
    fv::audio::Resampler16k resampler(kRate);
    std::vector<std::int16_t> resampled;
    resampler.push(pcm, resampled);
    resampler.finish(resampled);
    std::vector<float> pending(resampled.size());
    std::ranges::transform(resampled, pending.begin(),
                           [](std::int16_t s) { return static_cast<float>(s) / 32768.0f; });
    std::vector<AudioClip> original;
    std::size_t offset = 0;
    while (pending.size() - offset >= static_cast<std::size_t>(fv::dsp::MAX_SAMPLES)) {
        const std::size_t end = fv::dsp::split_point(std::span(pending).subspan(offset));
        original.push_back(
            fv::speech::test::clip({pending.begin() + static_cast<std::ptrdiff_t>(offset),
                                    pending.begin() + static_cast<std::ptrdiff_t>(offset + end)},
                                   static_cast<double>(offset) / 16000));
        offset += end;
    }
    if (offset < pending.size()) {
        original.push_back(fv::speech::test::clip(
            {pending.begin() + static_cast<std::ptrdiff_t>(offset), pending.end()},
            static_cast<double>(offset) / 16000));
    }
    REQUIRE(windows.size() == 3);
    REQUIRE(original.size() == 3);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        CHECK(identical(windows[i], original[i]));
    }
}

TEST_CASE("test_splitter_windows_do_not_depend_on_how_the_stream_was_cut",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_splitter_windows_do_not_depend_on_how_the_stream_was_cut
    Pcm stream(static_cast<std::size_t>(95 * fv::dsp::SAMPLE_RATE));
    Rng noise(2);
    for (auto& sample : stream) {
        sample = static_cast<std::int16_t>(
            std::clamp(std::lround(noise.normal() * 3000), -32768L, 32767L));
    }
    fv::dsp::WindowSplitter whole;
    auto expected = whole.push(stream);
    for (auto& window : whole.finish()) {
        expected.push_back(std::move(window));
    }
    Rng rng(3);
    fv::dsp::WindowSplitter pieces;
    std::vector<fv::dsp::Window> got;
    for (std::size_t start = 0; start < stream.size();) {
        const auto size = std::min<std::size_t>(static_cast<std::size_t>(rng.integer(1, 50'000)),
                                                stream.size() - start);
        for (auto& window : pieces.push(std::span(stream).subspan(start, size))) {
            got.push_back(std::move(window));
        }
        start += size;
    }
    for (auto& window : pieces.finish()) {
        got.push_back(std::move(window));
    }
    REQUIRE(got.size() == 4);
    REQUIRE(expected.size() == 4);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].start_s == expected[i].start_s);
        CHECK(got[i].samples == expected[i].samples);
    }
    CHECK(pieces.total() == whole.total());
    CHECK(pieces.total() == static_cast<std::int64_t>(stream.size()));
}

TEST_CASE("test_incremental_stream_is_bit_identical_to_the_saved_files_windows",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_incremental_stream_is_bit_identical_to_the_saved_files_windows
    // Followed from the first sample in random blocks and polls, the 16 kHz windows and their
    // boundaries equal file_windows() of the saved file: finish() checks each one's SHA-256.
    Harness h;
    auto session = h.session(language("en"));
    Rng rng(5);
    h.capture(speech_like(67, 4), session.get(), kBlocksPerPoll, &rng);
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() == 2);
    const auto transcript = session->finish(path);
    INFO(session->invalid_reason().value_or(""));
    REQUIRE(transcript.has_value());
    const auto want = fv::speech::file_windows(path);
    CHECK(want.size() == 3);
    // Every window the engine saw is one of the file's: no label was made for another.
    CHECK(h.engine->label_count() <= want.size());
}

// --- scheduling and join -----------------------------------------------------------------------

TEST_CASE("test_long_dictation_decodes_windows_while_recording_and_matches_the_file_path",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_long_dictation_decodes_windows_while_recording_and_matches_the_file_path
    Harness h;
    auto session = h.session();
    const Pcm pcm = speech_like(80, 6);
    const auto part = [&](int from, int to) {
        return std::span(pcm).subspan(static_cast<std::size_t>(from) * kRate,
                                      static_cast<std::size_t>(to - from) * kRate);
    };

    h.capture(part(0, 29), session.get());
    CHECK(session->early_windows() == 0);
    CHECK(h.engine->calls() == 0);
    h.capture(part(29, 31), session.get());
    CHECK(session->early_windows() == 1);
    h.capture(std::span(pcm).subspan(31 * kRate), session.get());
    const auto path = h.stop(session.get());
    const std::size_t early = session->early_windows();
    CHECK(early >= 2);

    const auto background = h.lanes->all();
    CHECK(background == std::vector<Lane>(early, Lane::Incremental));
    const auto transcript = session->finish(path);
    REQUIRE(transcript.has_value());
    const auto all = h.lanes->all();
    const std::vector<Lane> at_stop(all.begin() + static_cast<std::ptrdiff_t>(early), all.end());
    // Only what was left at stop is decoded then, on the Final lane.
    CHECK(at_stop.size() == fv::speech::file_windows(path).size() - early);
    CHECK_FALSE(at_stop.empty());
    CHECK(at_stop == std::vector<Lane>(at_stop.size(), Lane::Final));

    const std::size_t labels = h.engine->label_count();
    CHECK(transcript->text == h.file_text(path));
    // No new labels: the file path decoded the same windows.
    CHECK(h.engine->label_count() == labels);
    CHECK(labels == early + at_stop.size());
    CHECK(transcript->text.find("  ") == std::string::npos);
}

TEST_CASE("test_join_keeps_empty_windows_exactly_like_transcribe", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_join_keeps_empty_windows_exactly_like_transcribe
    Harness h;
    h.engine->label_cycle = {"", "one", "", ""};
    auto session = h.session();
    h.capture(speech_like(95, 7), session.get());
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() == 3);
    auto text = session->finish(path);
    REQUIRE(text.has_value());
    CHECK(text->text == h.file_text(path));
    const std::vector<std::string> texts{"", "one", "", ""};
    CHECK(fv::speech::join_texts(texts) == "one");
}

TEST_CASE("test_silent_windows_are_skipped_the_same_way", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_silent_windows_are_skipped_the_same_way
    Harness h;
    Pcm pcm = speech_like(90, 8);
    std::fill(pcm.begin() + 5 * kRate, pcm.begin() + 70 * kRate, std::int16_t{0});
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h);
    h.capture(pcm, session.get());
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() == 3);
    // The digital-silence window never reached the engine.
    CHECK(h.engine->calls() == 2);
    CHECK(slot.transcribe(path, {}).transcript.text == h.file_text(path));
    CHECK_FALSE(session->invalid_reason().has_value());
}

TEST_CASE("test_short_dictation_costs_nothing_and_falls_through", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_short_dictation_costs_nothing_and_falls_through
    Harness h;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto session = slot.session();
    REQUIRE(session);
    h.capture(speech_like(25, 9));
    const auto path = h.stop();
    std::this_thread::sleep_for(50ms);
    CHECK(h.engine->calls() == 0);
    CHECK(session->early_windows() == 0);
    const auto text = slot.transcribe(path, {}).transcript.text;
    // The whole file went through the plain path once.
    CHECK(h.lanes->all() == std::vector<Lane>(fv::speech::file_windows(path).size(), Lane::Final));
    CHECK(text == h.file_text(path));
}

TEST_CASE("test_nothing_decoded_early_means_the_plain_path", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_nothing_decoded_early_means_the_plain_path
    Harness h;
    auto session = h.session();
    h.capture(speech_like(31, 10), session.get(), 10'000); // no poll before the end
    const auto path = h.stop();
    CHECK_FALSE(session->finish(path).has_value());
}

// --- invalidation ------------------------------------------------------------------------------

TEST_CASE("test_state_changes_during_recording_fall_back_to_the_file",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_state_changes_during_recording_fall_back_to_the_file
    // A reload, an engine switch and a settings language change reach the session as a new
    // engine generation; a cancel, a microphone switch and a meeting arrive as discard() from
    // the dictation runtime.
    const std::string change =
        GENERATE("reload", "cancel", "switch", "device", "meeting", "language");
    Harness h;
    const std::map<std::string, std::string> discarded{{"cancel", "the transcription was canceled"},
                                                       {"device", "the audio device was switched"},
                                                       {"meeting", "a meeting started"}};
    auto session = h.session();
    const auto run = long_capture(h, *session, [&](IncrementalSession& followed) {
        if (const auto reason = discarded.find(change); reason != discarded.end()) {
            followed.discard(reason->second);
        } else if (change == "reload") {
            h.select(parakeet_config("cpu"));
        } else if (change == "switch") {
            auto other = parakeet_config("cpu");
            other.model_key = "parakeet-v2";
            h.select(other);
        } else {
            auto config = parakeet_config("cpu");
            config.settings =
                std::make_shared<const fv::Json>(fv::Json{{"local_asr_language", "de"}});
            h.select(config);
        }
    });
    // Window 2 completed after the change; a stale session must not decode it.
    CHECK(h.engine->calls() == run.calls_at_change);
    CHECK(session->invalid_reason().has_value());
    CHECK_FALSE(session->finish(run.path).has_value());
}

TEST_CASE("test_early_decode_error_falls_back", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_early_decode_error_falls_back
    Harness h;
    h.engine->fail = "worker hiccup";
    auto session = h.session();
    h.capture(speech_like(40, 12), session.get());
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() == 0);
    CHECK_THAT(session->invalid_reason().value_or(""),
               Catch::Matchers::ContainsSubstring("worker hiccup"));
    h.engine->fail.reset();
    CHECK_FALSE(session->finish(path).has_value());
    IncrementalDictation slot(h.service);
    CHECK(slot.transcribe(path, {}).transcript.text == h.file_text(path));
}

TEST_CASE("test_saved_file_that_differs_from_the_followed_audio_falls_back",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_saved_file_that_differs_from_the_followed_audio_falls_back
    const std::string damage = GENERATE("sample", "truncate", "format");
    Harness h;
    auto session = h.session();
    const auto run = long_capture(h, *session);
    const auto saved = fv::speech::detail::read_wav_pcm(run.path);
    Pcm pcm(saved.pcm.size() / 2);
    std::memcpy(pcm.data(), saved.pcm.data(), pcm.size() * 2);
    int rate = kRate;
    if (damage == "sample") {
        pcm[10 * kRate] ^= 1;
    } else if (damage == "truncate") {
        pcm.resize(32 * kRate);
    } else {
        rate = 48000;
    }
    fv::speech::test::write_wav(run.path, rate, 1, pcm);
    const auto calls = h.engine->calls();
    CHECK_FALSE(session->finish(run.path).has_value());
    // Nothing is decoded on a mismatch.
    CHECK(h.engine->calls() == calls);
    // The early windows are compared first, as in Python. A flipped low bit may not survive the
    // resampler, and then the CRC of the followed PCM catches it.
    const auto reason = session->invalid_reason().value_or("valid");
    INFO(damage << ": " << reason);
    if (damage == "sample") {
        CHECK((reason == "the saved file's early window differs" ||
               reason == "the saved WAV does not start with the audio followed"));
    } else {
        CHECK(reason == "the saved file's early window differs");
    }
}

TEST_CASE("a saved file damaged past the early windows fails the CRC or the length check",
          "[incremental_session]") {
    const auto [damage, reason] = GENERATE(table<std::string, std::string>({
        {"sample", "the saved WAV does not start with the audio followed"},
        {"truncate", "the saved WAV is shorter than the audio followed"},
    }));
    Harness h;
    auto session = h.session();
    const auto run = long_capture(h, *session);
    REQUIRE(session->early_windows() == 2);
    const auto saved = fv::speech::detail::read_wav_pcm(run.path);
    Pcm pcm(saved.pcm.size() / 2);
    std::memcpy(pcm.data(), saved.pcm.data(), pcm.size() * 2);
    // Both early windows end by 60 s.
    if (damage == "sample") {
        pcm[64 * kRate] ^= 1;
    } else {
        pcm.resize(63 * kRate);
    }
    fv::speech::test::write_wav(run.path, kRate, 1, pcm);
    const auto calls = h.engine->calls();
    CHECK_FALSE(session->finish(run.path).has_value());
    CHECK(h.engine->calls() == calls);
    CHECK(session->invalid_reason() == std::optional<std::string>(reason));
}

TEST_CASE("test_resampled_length_mismatch_falls_back", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_resampled_length_mismatch_falls_back
    // Python shortens the splitter's count by monkeypatching; here the check finish() applies is
    // driven directly.
    using fv::speech::detail::resampled_length_mismatch;
    const std::int64_t frames = 66 * kRate + kRate / 2;
    const auto exact = frames * 16000 / kRate;
    CHECK_FALSE(resampled_length_mismatch(exact, frames, kRate).has_value());
    CHECK(resampled_length_mismatch(exact - 2, frames, kRate) ==
          std::optional<std::string>("resampled " + std::to_string(exact - 2) +
                                     " samples where the file holds " + std::to_string(exact) +
                                     ".0"));
    CHECK(resampled_length_mismatch(exact + 1, frames, kRate).has_value());
    // 1000 frames at 44.1 kHz is 362.8 samples: within one sample either way.
    CHECK_FALSE(resampled_length_mismatch(363, 1000, kRate).has_value());
    CHECK_FALSE(resampled_length_mismatch(362, 1000, kRate).has_value());
}

// --- cancel ------------------------------------------------------------------------------------

TEST_CASE("test_discard_never_waits_for_a_window_in_flight", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_discard_never_waits_for_a_window_in_flight
    Harness h;
    h.engine->hold = true;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto session = slot.session();
    REQUIRE(session);
    h.capture(speech_like(33, 13));
    REQUIRE(h.engine->wait_entered()); // the thread is inside a decode
    const auto started = std::chrono::steady_clock::now();
    slot.discard();
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    h.engine->open();
    CHECK(wait_for([&] { return !session->following(); }));
    CHECK(session->early_windows() == 0);
    CHECK_FALSE(session->finish(h.stop()).has_value());
}

TEST_CASE("discard and finish wake a thread that waits out a long poll interval",
          "[incremental_session]") {
    const std::string how = GENERATE("discard", "finish", "destroy");
    Harness h;
    auto session = h.session({}, {}, 60s);
    session->start();
    REQUIRE(session->following());
    // The thread is in its first wait: nothing has reached 30 s.
    std::this_thread::sleep_for(50ms);
    const auto started = std::chrono::steady_clock::now();
    if (how == "discard") {
        session->discard();
        CHECK(wait_for([&] { return !session->following(); }, 5s));
    } else if (how == "finish") {
        CHECK_FALSE(session->finish(h.stop()).has_value());
        CHECK_FALSE(session->following());
    } else {
        session.reset();
    }
    CHECK(std::chrono::steady_clock::now() - started < 5s);
}

TEST_CASE("test_cancel_while_stop_waits_for_an_early_window_ends_the_job",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_cancel_while_stop_waits_for_an_early_window_ends_the_job
    // The job's cancel reaches the early window finish() waits for.
    Harness h;
    h.engine->hold = true;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto session = slot.session();
    h.capture(speech_like(33, 17));
    REQUIRE(h.engine->wait_entered());
    const auto path = h.stop();
    fv::CancelSource cancel;
    std::optional<std::string> error;
    std::thread job([&] {
        try {
            slot.transcribe(path, {}, cancel.token());
        } catch (const fv::EngineError& raised) {
            error = raised.what();
        }
    });
    std::this_thread::sleep_for(100ms);
    cancel.cancel(); // what the Cancel hotkey does to the job
    h.engine->open();
    job.join();
    CHECK(error == std::optional<std::string>("Transcription canceled"));
    CHECK(session->invalid_reason().has_value());
}

TEST_CASE("test_cancel_at_stop_raises_the_file_paths_cancel", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_cancel_at_stop_raises_the_file_paths_cancel
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h);
    const auto run = long_capture(h, *session);
    fv::CancelSource cancel;
    cancel.cancel();
    const auto lanes_before = h.lanes->all().size();
    // The session gives up, and the file path then raises the cancel itself.
    CHECK_THROWS_WITH(slot.transcribe(run.path, {}, cancel.token()), "Transcription canceled");
    CHECK(session->invalid_reason() == std::optional<std::string>("Transcription canceled"));
    CHECK(h.lanes->all().size() == lanes_before);
}

TEST_CASE("test_cancel_during_the_final_decode_raises_like_the_file_path",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_cancel_during_the_final_decode_raises_like_the_file_path
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h);
    fv::CancelSource cancel;
    h.lanes->before = [&](const fv::speech::DecodeJob& job) {
        if (job.lane == Lane::Final) {
            cancel.cancel(); // the Cancel hotkey during Transcribing
        }
    };
    const Pcm pcm = speech_like(66, 11);
    h.capture(pcm, session.get());
    h.stop(session.get());
    REQUIRE(session->early_windows() == 2);
    CHECK_THROWS_WITH(slot.transcribe(h.wav, {}, cancel.token()), "Transcription canceled");
    CHECK(session->invalid_reason().has_value());
}

// --- lifecycle and eligibility -----------------------------------------------------------------

TEST_CASE("test_thread_follows_capture_and_exits_when_it_ends", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_thread_follows_capture_and_exits_when_it_ends
    // The thread follows the tape until finish() ends it.
    Harness h;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto session = slot.session();
    REQUIRE(session);
    h.capture(speech_like(64, 14));
    REQUIRE(wait_for([&] { return session->early_windows() == 2; }));
    const auto path = h.stop();
    const auto text = slot.transcribe(path, {}).transcript.text;
    CHECK_FALSE(session->following());
    CHECK_FALSE(session->invalid_reason().has_value());
    CHECK(text == h.file_text(path));
    CHECK(slot.session() == nullptr);
}

TEST_CASE("test_thread_exits_when_the_engine_is_cleaned_up", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_thread_exits_when_the_engine_is_cleaned_up
    Harness h;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto session = slot.session();
    REQUIRE(session);
    h.capture(speech_like(31, 15));
    // A meeting releasing the engine (or shutdown) moves its generation.
    h.service.release_local(Slot::Dictation);
    CHECK(wait_for([&] { return !session->following(); }, 5s));
    CHECK(session->invalid_reason().has_value());
}

TEST_CASE("test_uploads_and_retranscribes_never_use_the_session", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_uploads_and_retranscribes_never_use_the_session
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h);
    h.capture(speech_like(66, 11), session.get());
    const auto path = h.stop(session.get());
    REQUIRE(session->early_windows() == 2);
    const auto upload = h.data.path() / "upload.wav";
    std::filesystem::copy_file(path, upload);
    const auto lanes_before = h.lanes->all().size();
    const auto text = slot.transcribe(upload, {}).transcript.text;
    CHECK(session->invalid_reason() ==
          std::optional<std::string>("the job is not this recording's"));
    CHECK(slot.session() == nullptr);
    // The upload was decoded whole.
    CHECK(h.lanes->all().size() - lanes_before == fv::speech::file_windows(upload).size());
    CHECK(text == h.file_text(path));
}

TEST_CASE("test_a_new_recording_supersedes_a_stale_session", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_a_new_recording_supersedes_a_stale_session
    Harness h;
    IncrementalDictation slot(h.service);
    slot.start(h.tape, {}, "parakeet", h.wav, {}, kFastPoll);
    auto stale = slot.session();
    REQUIRE(stale);
    slot.start(h.tape, {}, "api", h.wav, {}, kFastPoll);
    CHECK(stale->invalid_reason() == std::optional<std::string>("a new recording started"));
    CHECK(slot.session() == nullptr);
}

TEST_CASE("test_ineligible_engines_and_recorders_get_no_session", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_ineligible_engines_and_recorders_get_no_session
    // Whisper, Moonshine and Qwen are dropped engines; OpenWhisper_CPP's other backends are api and
    // remote. Python's "canceled" case (a backend flag) has no counterpart: a cancel reaches a
    // session through discard().
    const std::string engine = GENERATE("api", "remote", "moonshine", "unavailable", "stereo");
    Harness h;
    std::shared_ptr<const fv::audio::PcmTape> tape = h.tape;
    std::string backend = engine;
    if (engine == "unavailable") {
        backend = "parakeet";
        h.service.release_local(Slot::Dictation);
    } else if (engine == "stereo") {
        backend = "parakeet";
        tape = std::make_shared<fv::audio::PcmTape>(kRate, 2);
    }
    CHECK(IncrementalSession::create(h.service, tape, {}, backend) == nullptr);
}

// --- recorder reads ----------------------------------------------------------------------------

TEST_CASE("test_read_recorded_bytes_restores_the_write_position", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_read_recorded_bytes_restores_the_write_position The
    // tape view a session reads from, and the WAV saved from the same samples.
    Harness h;
    const auto read = [&](std::int64_t first) {
        const auto view = h.tape->view(first, h.tape->frames());
        Pcm out(static_cast<std::size_t>(view.frames()));
        view.copy_to(out);
        return out;
    };
    CHECK(read(0).empty());
    Pcm first(3000);
    std::iota(first.begin(), first.end(), std::int16_t{0});
    Pcm second(2000);
    std::iota(second.begin(), second.end(), std::int16_t{3000});
    h.capture(first);
    CHECK(read(1000) == Pcm(first.begin() + 1000, first.end()));
    h.capture(second);
    Pcm both = first;
    both.insert(both.end(), second.begin(), second.end());
    CHECK(read(0) == both);
    CHECK(read(5000).empty());
    CHECK_THROWS_AS(h.tape->view(5001, 5001), std::out_of_range);
    const auto saved = fv::speech::detail::read_wav_pcm(h.stop());
    Pcm pcm(saved.pcm.size() / 2);
    std::memcpy(pcm.data(), saved.pcm.data(), pcm.size() * 2);
    CHECK(Pcm(pcm.begin(), pcm.begin() + 5000) == both);
}

// --- test_personalize_s4_incremental.py --------------------------------------------------------

namespace {

const std::vector<std::string> kKsenia{"Ksenia"};

} // namespace

TEST_CASE("test_early_windows_decode_with_the_context", "[test_personalize_s4_incremental]") {
    // from tests/test_personalize_s4_incremental.py::test_early_windows_decode_with_the_context
    // Parakeet's phrases are applied after recognition, so they never reach the model.
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h, language("fr"), kKsenia);
    h.capture(speech_like(64, 21), session.get());
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() >= 1);
    {
        std::lock_guard lock(h.engine->mutex);
        CHECK(h.engine->languages == std::vector<std::string>(h.engine->languages.size(), "fr"));
    }
    const auto text = slot.transcribe(path, language("fr"), {}, kKsenia).transcript.text;
    CHECK_FALSE(session->invalid_reason().has_value());
    {
        std::lock_guard lock(h.engine->mutex);
        CHECK(h.engine->languages == std::vector<std::string>(h.engine->languages.size(), "fr"));
    }
    {
        std::lock_guard lock(h.lanes->mutex);
        CHECK(h.lanes->phrases ==
              std::vector<std::vector<std::string>>(h.lanes->phrases.size(), kKsenia));
    }
    CHECK(text == h.file_text(path, language("fr")));
}

TEST_CASE("test_a_different_final_context_decodes_the_file", "[test_personalize_s4_incremental]") {
    // from
    // tests/test_personalize_s4_incremental.py::test_a_different_final_context_decodes_the_file
    const auto [code, phrases] = GENERATE(table<std::string, std::vector<std::string>>({
        {"de", {"Ksenia"}},
        {"fr", {"Ksenia", "Olu"}},
        {"", {}},
    }));
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h, language("fr"), kKsenia);
    h.capture(speech_like(64, 21), session.get());
    const auto path = h.stop(session.get());
    CHECK(session->early_windows() >= 1);
    const auto lanes_before = h.lanes->all().size();
    const auto text = slot.transcribe(path, language(code), {}, phrases).transcript.text;
    CHECK(session->invalid_reason() ==
          std::optional<std::string>("the dictation's language or vocabulary changed"));
    CHECK(h.lanes->all().size() - lanes_before == fv::speech::file_windows(path).size());
    CHECK(text == h.file_text(path, language(code)));
}

TEST_CASE("test_an_empty_context_matches_none", "[test_personalize_s4_incremental]") {
    // from tests/test_personalize_s4_incremental.py::test_an_empty_context_matches_none
    Harness h;
    IncrementalDictation slot(h.service);
    auto session = hand_polled(slot, h);
    h.capture(speech_like(64, 21), session.get());
    const auto path = h.stop(session.get());
    CHECK(session->matches({}, {}));
    slot.transcribe(path, {});
    CHECK_FALSE(session->invalid_reason().has_value());
    std::lock_guard lock(h.lanes->mutex);
    CHECK(h.lanes->phrases == std::vector<std::vector<std::string>>(h.lanes->phrases.size(),
                                                                    std::vector<std::string>{}));
}

TEST_CASE("test_a_settings_language_change_still_invalidates",
          "[test_personalize_s4_incremental]") {
    // from
    // tests/test_personalize_s4_incremental.py::test_a_settings_language_change_still_invalidates
    // local_asr_language reaches the engine through a new select, so the session sees a new
    // engine generation.
    Harness h;
    auto session = h.session({}, kKsenia);
    const auto run = long_capture(h, *session, [&](IncrementalSession&) {
        auto config = parakeet_config("cpu");
        config.settings = std::make_shared<const fv::Json>(fv::Json{{"local_asr_language", "fr"}});
        h.select(config);
    });
    CHECK(session->invalid_reason() ==
          std::optional<std::string>("the engine was reloaded, released, or canceled"));
    CHECK_FALSE(session->finish(run.path).has_value());
}

// --- golden vectors and private helpers ---------------------------------------------------------

TEST_CASE("_context_key replays OpenWhisper's keys", "[golden]") {
    fv::golden::for_each(
        "services.incremental_dictation", "_context_key", [](const fv::golden::Case& c) {
            const bool none = c.arg<bool>(2);
            const auto phrases = c.arg<std::vector<std::string>>(1);
            const auto key = none ? fv::speech::context_key("", {})
                                  : fv::speech::context_key(c.arg<std::string>(0), phrases);
            const auto expected = c.result<fv::Json>();
            CHECK(key.language == expected[0].get<std::string>());
            CHECK(key.phrases == expected[1].get<std::vector<std::string>>());
        });
}

TEST_CASE("crc32 is zlib's", "[incremental_session]") {
    const std::string text = "123456789";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size());
    CHECK(fv::speech::detail::crc32(bytes) == 0xCBF43926u);
    CHECK(fv::speech::detail::crc32({}) == 0u);
    // Continued, it equals the CRC of the whole.
    CHECK(fv::speech::detail::crc32(bytes.subspan(4), fv::speech::detail::crc32(bytes.first(4))) ==
          0xCBF43926u);
}

TEST_CASE("read_wav_pcm reads chunks as the wave module does", "[incremental_session]") {
    fv::testkit::TempDataRoot data;
    const auto path = data.path() / "x.wav";
    const auto write = [&](const std::string& bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const auto le32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v), static_cast<char>(v >> 8),
                           static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    };
    const auto le16 = [](std::uint16_t v) {
        return std::string{static_cast<char>(v), static_cast<char>(v >> 8)};
    };
    const std::string fmt =
        "fmt " + le32(16) + le16(1) + le16(1) + le32(44100) + le32(88200) + le16(2) + le16(16);
    // An odd-sized chunk before the data is skipped with its pad byte.
    const std::string odd = "LIST" + le32(3) + "abc" + std::string(1, '\0');
    const std::string data_chunk = "data" + le32(6) + std::string("\x01\x00\x02\x00\x03\x00", 6);
    const std::string body = "WAVE" + fmt + odd + data_chunk + "bext" + le32(2) + "zz";
    write("RIFF" + le32(static_cast<std::uint32_t>(body.size())) + body);
    auto wav = fv::speech::detail::read_wav_pcm(path);
    CHECK(wav.channels == 1);
    CHECK(wav.sample_width == 2);
    CHECK(wav.rate == 44100);
    CHECK(wav.frames == 3);
    CHECK(wav.pcm == std::string("\x01\x00\x02\x00\x03\x00", 6));

    // A data chunk that claims more than the file holds reads short.
    const std::string cut = "WAVE" + fmt + "data" + le32(10) + std::string("\x01\x00", 2);
    write("RIFF" + le32(static_cast<std::uint32_t>(cut.size())) + cut);
    wav = fv::speech::detail::read_wav_pcm(path);
    CHECK(wav.frames == 5);
    CHECK(wav.pcm.size() == 2);

    write("RIFX" + le32(4) + "WAVE");
    CHECK_THROWS_WITH(fv::speech::detail::read_wav_pcm(path), "file does not start with RIFF id");
    write("RIFF" + le32(4) + "AVI ");
    CHECK_THROWS_WITH(fv::speech::detail::read_wav_pcm(path), "not a WAVE file");
    write("RIFF" + le32(4) + "WAVE");
    CHECK_THROWS_WITH(fv::speech::detail::read_wav_pcm(path),
                      "fmt chunk and/or data chunk missing");
    const std::string early = "WAVE" + data_chunk + fmt;
    write("RIFF" + le32(static_cast<std::uint32_t>(early.size())) + early);
    CHECK_THROWS_WITH(fv::speech::detail::read_wav_pcm(path), "data chunk before fmt chunk");
    const std::string floats = "WAVE" + std::string("fmt ") + le32(16) + le16(3) + le16(1) +
                               le32(44100) + le32(176400) + le16(4) + le16(32) + data_chunk;
    write("RIFF" + le32(static_cast<std::uint32_t>(floats.size())) + floats);
    CHECK_THROWS_WITH(fv::speech::detail::read_wav_pcm(path), "unknown format: 3");
}

// --- the real engine ---------------------------------------------------------------------------

TEST_CASE("a long dictation decoded while recording equals decode_file on the saved WAV",
          "[decode]") {
    const auto runtime = fv::env_utf8("OPENWHISPER_CPP_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("OPENWHISPER_CPP_TEST_MODEL");
    const auto wav = fv::env_utf8("OPENWHISPER_CPP_TEST_WAV");
    if (!runtime || !model || !wav) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR, OPENWHISPER_CPP_TEST_MODEL and "
             "OPENWHISPER_CPP_TEST_WAV.");
    }
    const auto source = fv::speech::detail::read_wav_pcm(fv::path_from_utf8(*wav));
    if (source.channels != 1 || source.sample_width != 2) {
        SKIP("OPENWHISPER_CPP_TEST_WAV is not 16-bit mono.");
    }
    Pcm speech(source.pcm.size() / 2);
    std::memcpy(speech.data(), source.pcm.data(), speech.size() * 2);
    REQUIRE_FALSE(speech.empty());
    // The test recording, repeated to 75 s.
    Pcm fixture;
    const auto total = static_cast<std::size_t>(75) * static_cast<std::size_t>(source.rate);
    while (fixture.size() < total) {
        fixture.insert(fixture.end(), speech.begin(),
                       speech.begin() + static_cast<std::ptrdiff_t>(
                                            std::min(speech.size(), total - fixture.size())));
    }

    auto config = parakeet_config("cpu");
    config.runtime_dir = fv::path_from_utf8(*runtime);
    config.model_path = fv::path_from_utf8(*model);
    fv::ManualExecutor notify;
    fv::speech::SpeechService service(
        [](const BackendConfig& c) { return std::make_unique<fv::speech::ParakeetBackend>(c); },
        notify);
    service.select(Slot::Dictation, config);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state != ReadyState::Loading; },
        std::chrono::minutes(2)));
    REQUIRE(service.readiness(Slot::Dictation).state == ReadyState::Ready);

    fv::testkit::TempDataRoot data;
    const auto saved = data.path() / "recorded_audio.wav";
    auto tape = std::make_shared<fv::audio::PcmTape>(source.rate);
    IncrementalDictation slot(service);
    slot.start(tape, language("en"), "parakeet", saved);
    auto session = slot.session();
    REQUIRE(session);
    for (std::size_t start = 0; start < fixture.size(); start += kBlock) {
        tape->append(std::span(fixture).subspan(
            start, std::min<std::size_t>(kBlock, fixture.size() - start)));
        if ((start / kBlock) % kBlocksPerPoll == 0) {
            session->poll();
        }
    }
    session->poll();
    // Two windows were decoded before the stop.
    CHECK(session->early_windows() == 2);
    Pcm padded = fixture;
    padded.resize(padded.size() + static_cast<std::size_t>(source.rate) / 2, 0);
    fv::speech::test::write_wav(saved, source.rate, 1, padded);

    const auto incremental = slot.transcribe(saved, language("en"));
    CHECK_FALSE(session->invalid_reason().has_value());
    const auto whole = fv::speech::decode_file(service, saved, language("en"));
    CHECK_FALSE(incremental.transcript.text.empty());
    CHECK(incremental.transcript.text == whole.transcript.text);
    // The session verified every window's samples against the file's (a mismatch would have
    // invalidated it); the engine then gave the same words at the same file times.
    CHECK(incremental.transcript.words == whole.transcript.words);
    CHECK(incremental.transcript.segments == whole.transcript.segments);
}
