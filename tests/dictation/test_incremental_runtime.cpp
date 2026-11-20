#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dictation/transcription_runtime.h"
#include "engine/recognizer.h"
#include "preview_support.h"
#include "speech/file_windows.h"
#include "speech/incremental_session.h"
#include "speech/service.h"

// The transcription runtime's early decode of a long dictation (the IncrementalDictation slot of
// services/runtime/transcription.py): tests/test_incremental_dictation.py and
// tests/test_personalize_s4_incremental.py.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::speech::IncrementalSession;
using fv::speech::Lane;
using namespace std::chrono_literals;

namespace {

// An engine whose text depends only on each window's samples, and no live preview, so every
// decode is the dictation's.
PreviewRigOptions early_decode_options() {
    PreviewRigOptions options;
    options.settings["streaming_enabled"] = false;
    return options;
}

void label_windows(LaneEngine& engine) {
    std::lock_guard lock(engine.mutex);
    engine.text = [](Lane, std::size_t, std::span<const float> samples) {
        return content_label(samples);
    };
}

// Polls until `condition` holds, as wait_for does.
bool wait_for(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// The file path's transcript of `path` on the rig's engine.
std::string file_transcript(PreviewRig& p, const std::filesystem::path& path) {
    return fv::speech::decode_file(*p.rig.speech, path, fv::RecognitionOptions{}, Lane::Final)
        .transcript.text;
}

} // namespace

TEST_CASE("runtime transcribes its own recording from the session",
          "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_runtime_transcribes_its_own_recording_from_the_session
    PreviewRig p(early_decode_options());
    label_windows(*p.engine);
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);
    p.recorder->append(speech_like(64, 16));
    REQUIRE(wait_for([&] { return session->early_windows() == 2; }));
    std::filesystem::path saved;
    p.recorder->on_save = [&] {
        saved = p.recorder->output_file();
    };

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(p.rig.trace.with_prefix("error_dialog:").empty());
    REQUIRE(std::filesystem::exists(saved));
    const std::size_t early = p.engine->count(Lane::Incremental);
    CHECK(early == 2);
    const std::size_t windows = fv::speech::file_windows(saved).size();
    const std::size_t finals = p.engine->count(Lane::Final);
    if (finals == windows) {
        // The early text could not be proven; the whole file was decoded.
        REQUIRE(session->invalid_reason());
        CHECK(session->invalid_reason()->find("saved") != std::string::npos);
    } else {
        CHECK(session->invalid_reason() == std::nullopt);
        CHECK(finals == windows - early);
    }
    CHECK(p.rig.transcript_text == file_transcript(p, saved));
}

TEST_CASE("runtime cancel discards the session", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_runtime_cancel_discards_the_session
    PreviewRig p(early_decode_options());
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);

    REQUIRE(p.rig.runtime->cancel());

    REQUIRE(wait_for([&] { return !session->following(); }, 5s));
    CHECK(session->invalid_reason() == "the recording was canceled");
    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
}

TEST_CASE("uploads and retranscribes never use the session", "[test_incremental_dictation]") {
    // from
    // tests/test_incremental_dictation.py::test_uploads_and_retranscribes_never_use_the_session A
    // file other than the recording the session follows, transcribed while it holds early windows,
    // drops them and decodes the file whole.
    PreviewRig p(early_decode_options());
    label_windows(*p.engine);
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);
    p.recorder->append(speech_like(35, 7));
    REQUIRE(wait_for([&] { return session->early_windows() == 1; }));
    const auto upload = p.rig.wav("upload.wav");
    REQUIRE(p.rig.runtime->claim_job());

    p.rig.runtime->transcribe_audio_file(upload);

    CHECK(session->invalid_reason() == "the job is not this recording's");
    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
    CHECK(p.rig.transcript_text == file_transcript(p, upload));
    p.rig.run_workers();
}

TEST_CASE("cleared capture invalidates", "[test_incremental_dictation]") {
    // from tests/test_incremental_dictation.py::test_cleared_capture_invalidates
    // A tape is never cleared: when the microphone order changes and the recorder is rebuilt
    // (or a meeting starts), the runtime drops the recording's early decode itself, and the
    // stop decodes the saved file whole.
    PreviewRig p(early_decode_options());
    label_windows(*p.engine);
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);
    p.recorder->append(speech_like(35, 3));
    REQUIRE(wait_for([&] { return session->early_windows() == 1; }));
    std::filesystem::path saved;
    p.recorder->on_save = [&] {
        saved = p.recorder->output_file();
    };

    p.rig.runtime->discard_early_decode();
    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(session->invalid_reason() == "the recording was canceled");
    CHECK(p.engine->count(Lane::Final) == fv::speech::file_windows(saved).size());
    CHECK(p.rig.transcript_text == file_transcript(p, saved));
}

TEST_CASE("runtime starts the session with the jobs context", "[test_personalize_s4_incremental]") {
    // from
    // tests/test_personalize_s4_incremental.py::test_runtime_starts_the_session_with_the_jobs_context
    PreviewRigOptions options = early_decode_options();
    options.settings["dictation_languages"] = fv::Json::array({"en", "fr"});
    options.settings["dictation_active_language"] = "fr";
    PreviewRig p(options);
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);

    fv::RecognitionOptions french;
    french.language = "fr";
    CHECK(session->matches(french));
    CHECK_FALSE(session->matches(fv::RecognitionOptions{}));
    p.recorder->append(speech_like(35, 5));
    REQUIRE(wait_for([&] { return session->early_windows() == 1; }));
    CHECK(p.engine->all().front().language == "fr");
    p.rig.runtime->cancel();
}

TEST_CASE("a walkthrough recording is never decoded early", "[incremental]") {
    // transcription.py 609-611: a walkthrough is decoded section by section once it stops.
    PreviewRig p(early_decode_options());
    StartOptions walkthrough;
    walkthrough.mode = JobMode::Walkthrough;

    REQUIRE(p.rig.runtime->start_recording(walkthrough));

    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
    p.rig.runtime->cancel();
}

TEST_CASE("a recording that is not transcribed drops its early decode", "[incremental]") {
    // A tape never ends: a recording whose save fails must not leave its session following it.
    PreviewRig p(early_decode_options());
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);
    p.recorder->append(speech_like(2, 1));
    p.recorder->save_ok = false;

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK_FALSE(p.rig.trace.with_prefix("error_dialog:").empty());
    REQUIRE(wait_for([&] { return !session->following(); }, 5s));
    CHECK(session->invalid_reason());
    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
}

TEST_CASE("the api engine never decodes early", "[incremental]") {
    // Only Parakeet decodes a dictation's windows while it is recorded.
    PreviewRigOptions options = early_decode_options();
    options.settings["selected_model"] = "api";
    PreviewRig p(options);

    REQUIRE(p.rig.runtime->start_recording());

    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
    p.rig.runtime->cancel();
}
