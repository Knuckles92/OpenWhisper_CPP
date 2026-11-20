#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dictation/transcription_runtime.h"
#include "runtime_support.h"

// tests/test_paste_latency.py: clipboard snapshotting and history persistence stay off the paste
// path. And tests/test_strict_switch_reads.py::test_auto_paste_reads_its_switch_strictly.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::StartsWith;

namespace {

// _harness, with the slot claimed (the `h` fixture): a recording's result is pending.
struct Harness : RuntimeRig {
    explicit Harness(bool claim = true, RigOptions options = {}) : RuntimeRig(std::move(options)) {
        PendingAudio pending;
        pending.audio_path = wav("recording.wav");
        pending.audio_duration = 10.0;
        pending.file_size = 4096;
        pending.source_name = "Quick Record";
        pending.transcription_time = 0.1;
        runtime->set_pending(pending);
        if (claim) {
            REQUIRE(runtime->claim_job());
        }
    }
};

// What TemporaryClipboard does with snapshots, over an in-memory clipboard with a change
// counter (SequencedClipboard): a prefetch snapshots the clipboard; a stage with restore reuses
// that snapshot while the clipboard is unchanged since, else snapshots again; a restore puts the
// snapshot back while the staged text is still there. `captures` counts snapshots.
// Affinity: one thread (the test's).
class SnapshotClipboard final : public fv::focus::ClipboardPort {
public:
    explicit SnapshotClipboard(std::string text) : text_(std::move(text)) {}

    fv::focus::StageResult stage_text(std::string text, std::optional<std::string>,
                                      bool restore = true) override {
        if (!restore) {
            prefetched_.reset();
            set(std::move(text));
            return {true, std::nullopt, false};
        }
        std::optional<Snapshot> snapshot = std::exchange(prefetched_, std::nullopt);
        if (!snapshot || snapshot->sequence != sequence_) {
            snapshot = capture();
        }
        set(std::move(text));
        if (snapshot->text.empty()) {
            return {true, std::nullopt, false};
        }
        lease_ = fv::focus::ClipboardLease{++next_id_, {}};
        staged_ = Staged{*snapshot, sequence_};
        return {true, lease_, false};
    }
    bool schedule_restore(const fv::focus::ClipboardLease& lease,
                          std::chrono::milliseconds) override {
        restore_scheduled_ = staged_ && lease_ && lease == *lease_;
        return restore_scheduled_;
    }
    bool commit_text(const fv::focus::ClipboardLease&, std::string text) override {
        staged_.reset();
        set(std::move(text));
        return true;
    }
    fv::focus::RestoreOutcome restore_now(const fv::focus::ClipboardLease&) override {
        return fire_restore() ? fv::focus::RestoreOutcome::Restored
                              : fv::focus::RestoreOutcome::Skipped;
    }
    void cleanup() override {}
    void request_prefetch(std::chrono::milliseconds) override { prefetched_ = capture(); }
    void discard_prefetch() override { prefetched_.reset(); }
    void capture_selection(std::function<void()>, std::function<void(std::optional<std::string>)>,
                           std::chrono::milliseconds) override {}
    bool write_text(std::string text, std::string = {}) override {
        set(std::move(text));
        return true;
    }
    bool write_files(fv::focus::ClipboardFiles) override { return false; }

    // Another app copies.
    void copy(std::string text) { set(std::move(text)); }
    // The restore timer fires.
    bool fire_restore() {
        if (!restore_scheduled_ || !staged_ || staged_->sequence != sequence_) {
            return false;
        }
        set(staged_->snapshot.text);
        staged_.reset();
        restore_scheduled_ = false;
        return true;
    }
    const std::string& text() const { return text_; }
    int captures = 0;

private:
    struct Snapshot {
        std::string text;
        std::uint64_t sequence = 0;
    };
    struct Staged {
        Snapshot snapshot;
        std::uint64_t sequence = 0;
    };
    Snapshot capture() {
        ++captures;
        return Snapshot{text_, sequence_};
    }
    void set(std::string text) {
        text_ = std::move(text);
        ++sequence_;
    }

    std::string text_;
    std::uint64_t sequence_ = 1;
    std::optional<Snapshot> prefetched_;
    std::optional<Staged> staged_;
    std::optional<fv::focus::ClipboardLease> lease_;
    bool restore_scheduled_ = false;
    std::uint64_t next_id_ = 0;
};

// _clipboard_harness: the runtime's delivery goes through a SnapshotClipboard.
struct ClipboardHarness {
    explicit ClipboardHarness(RigOptions options = {}) : rig(std::move(options)) {
        TranscriptionServices services;
        services.settings = &rig.settings;
        services.recorder = &rig.recorder;
        services.speech = rig.speech.get();
        services.history = rig.history.get();
        services.stats = rig.stats.get();
        services.delivery.clipboard = &clipboard;
        services.delivery.keys = &rig.keys;
        services.activity = &rig.activity;
        services.ui = &rig.ui;
        services.jobs = &rig.jobs;
        services.persistence = &rig.persistence;
        services.events = &rig.events;
        services.capture = &rig.capture;
        services.hooks.stop_playback = [] {
        };
        runtime = std::make_unique<TranscriptionRuntime>(std::move(services));
        status_link = runtime->status_update.connect(
            rig.ui, [this](std::string status) { statuses.push_back(std::move(status)); });
    }
    ~ClipboardHarness() {
        status_link.disconnect();
        runtime.reset();
    }

    // The recording stopped and its result is in.
    void complete(const std::string& words) {
        rig.recorder.recording = false;
        REQUIRE(runtime->claim_job());
        runtime->on_transcription_complete(words);
        rig.run_workers();
    }

    RuntimeRig rig;
    SnapshotClipboard clipboard{"user's own clipboard"};
    std::unique_ptr<TranscriptionRuntime> runtime;
    std::vector<std::string> statuses;
    fv::Subscription status_link;
};

} // namespace

// --- paste before history ---------------------------------------------------------------

TEST_CASE("paste happens before history is saved", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_paste_happens_before_history_is_saved
    Harness h;
    h.runtime->on_transcription_complete("hello world");
    h.run_workers();

    const auto& trace = h.trace;
    REQUIRE(trace.has("history"));
    CHECK(trace.index("stage") < trace.index("paste"));
    CHECK(trace.index("paste") < trace.index("history"));
    CHECK(trace.index("history") < trace.index("refresh_history"));
    // History reads the one-shot metadata before it is cleared.
    const auto entries = h.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "hello world");
    CHECK(entries[0].audio_file);
    CHECK(entries[0].audio_duration == 10.0);
    CHECK(entries[0].source_name == "Quick Record");
    CHECK_FALSE(h.runtime->pending().audio_path);
    CHECK_FALSE(h.runtime->pending().source_name);
    CHECK_FALSE(h.runtime->has_active_job());
}

TEST_CASE("paste status stays the final status", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_paste_status_stays_the_final_status
    Harness h;
    h.runtime->on_transcription_complete("hello world");
    h.run_workers();

    CHECK(h.statuses() == std::vector<std::string>{"Ready (Pasted)"});
}

TEST_CASE("history failure still pastes", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_history_failure_still_pastes
    Harness h;
    RuntimeLog log;
    h.break_history();

    h.runtime->on_transcription_complete("hello world");
    h.run_workers();

    CHECK(h.keys.sent() == std::vector<std::string>{"paste"});
    const auto statuses = h.statuses();
    CHECK(std::find(statuses.begin(), statuses.end(), "Ready (Pasted)") != statuses.end());
    CHECK_THAT(h.last_status(), StartsWith("History could not be saved: "));
    CHECK(h.last_status().ends_with(". Copy the transcript before closing."));
    CHECK(log.contains("Failed to save transcription to history"));
    CHECK_FALSE(h.runtime->pending().audio_path);
    CHECK_FALSE(h.runtime->has_active_job());
}

TEST_CASE("paste failure still records history", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_paste_failure_still_records_history
    Harness h;
    h.keys.fail = true;

    h.runtime->on_transcription_complete("hello world");
    h.run_workers();

    CHECK(h.trace.has("commit"));
    const auto entries = h.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "hello world");
    CHECK(h.last_status() == "Transcription complete (paste failed)");
    CHECK_FALSE(h.runtime->has_active_job());
}

TEST_CASE("unexpected clipboard error still records history and frees the job",
          "[test_paste_latency]") {
    // from
    // tests/test_paste_latency.py::test_unexpected_clipboard_error_still_records_history_and_frees_the_job
    Harness h;
    h.clipboard.stage_error = "clipboard exploded";

    CHECK_THROWS_WITH(h.runtime->on_transcription_complete("hello world"), "clipboard exploded");
    h.run_workers();

    const auto entries = h.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "hello world");
    CHECK_FALSE(h.runtime->pending().audio_path);
    CHECK_FALSE(h.runtime->has_active_job());
}

TEST_CASE("empty result skips history and paste and drops the prefetch", "[test_paste_latency]") {
    // from
    // tests/test_paste_latency.py::test_empty_result_skips_history_and_paste_and_drops_the_prefetch
    Harness h;
    h.runtime->on_transcription_complete("   ");
    h.run_workers();

    CHECK(h.entries().empty());
    CHECK(h.keys.sent().empty());
    CHECK(h.trace.has("discard_prefetch"));
    CHECK_FALSE(h.runtime->has_active_job());
}

TEST_CASE("copy only delivery drops the prefetch", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_copy_only_delivery_drops_the_prefetch
    Harness h;
    h.settings.update(fv::Json{{"auto_paste", false}});

    h.runtime->on_transcription_complete("hello world");
    h.run_workers();

    CHECK(h.keys.sent().empty());
    REQUIRE(h.trace.has("copy"));
    CHECK(h.trace.index("discard_prefetch") < h.trace.index("copy"));
    CHECK(h.last_status() == "Ready");
    CHECK(h.entries().size() == 1);
}

TEST_CASE("transcription error drops the prefetch", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_transcription_error_drops_the_prefetch
    Harness h;
    h.runtime->on_transcription_error("backend unavailable");
    h.run_workers();

    CHECK(h.trace.has("discard_prefetch"));
    CHECK_FALSE(h.runtime->has_active_job());
}

// --- snapshot prefetch lifecycle --------------------------------------------------------

TEST_CASE("recording start requests one prefetch after the recorder and overlay",
          "[test_paste_latency]") {
    // from
    // tests/test_paste_latency.py::test_recording_start_requests_one_prefetch_after_the_recorder_and_overlay
    Harness h(false);

    REQUIRE(h.runtime->start_recording());

    CHECK(h.trace.count("prefetch") == 1);
    CHECK(h.trace.index("recorder_start") < h.trace.index("prefetch"));
    const auto entries = h.trace.entries();
    std::ptrdiff_t overlay = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].starts_with("overlay:")) {
            overlay = static_cast<std::ptrdiff_t>(i);
            break;
        }
    }
    REQUIRE(overlay >= 0);
    CHECK(overlay < h.trace.index("prefetch"));
}

TEST_CASE("no prefetch when auto paste is off", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_no_prefetch_when_auto_paste_is_off
    Harness h(false);
    h.settings.update(fv::Json{{"auto_paste", false}});

    REQUIRE(h.runtime->start_recording());

    CHECK_FALSE(h.trace.has("prefetch"));
}

TEST_CASE("failed start requests no prefetch and drops any stale one", "[test_paste_latency]") {
    // from
    // tests/test_paste_latency.py::test_failed_start_requests_no_prefetch_and_drops_any_stale_one
    Harness h(false);
    h.recorder.start_ok = false;

    CHECK_FALSE(h.runtime->start_recording());

    CHECK_FALSE(h.trace.has("prefetch"));
    CHECK(h.trace.has("discard_prefetch"));
}

TEST_CASE("refused start touches no clipboard", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_refused_start_touches_no_clipboard
    Harness h(false);
    h.activity.set_meeting_active(true);

    CHECK_FALSE(h.runtime->start_recording());

    CHECK_FALSE(h.trace.has("prefetch"));
    CHECK_FALSE(h.trace.has("recorder_start"));
}

TEST_CASE("cancel during recording drops the prefetch", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_cancel_during_recording_drops_the_prefetch
    Harness h(false);
    REQUIRE(h.runtime->start_recording());

    h.runtime->cancel();

    REQUIRE(h.trace.has("discard_prefetch"));
    CHECK(h.trace.index("prefetch") < h.trace.index("discard_prefetch"));
}

// --- end to end through a clipboard that snapshots --------------------------------------

TEST_CASE("dictation pastes without copying the clipboard again", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_dictation_pastes_without_copying_the_clipboard_again
    ClipboardHarness h;
    REQUIRE(h.runtime->start_recording());
    // Captured once, while recording.
    CHECK(h.clipboard.captures == 1);

    h.complete("dictated words");

    CHECK(h.clipboard.captures == 1);
    CHECK(h.rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(h.clipboard.text() == "dictated words");
    CHECK(h.clipboard.fire_restore());
    CHECK(h.clipboard.text() == "user's own clipboard");
}

TEST_CASE("restore off pastes and leaves the transcript without copying the clipboard",
          "[test_paste_latency]") {
    // from
    // tests/test_paste_latency.py::test_restore_off_pastes_and_leaves_the_transcript_without_copying_the_clipboard
    RigOptions options;
    options.settings["restore_clipboard"] = false;
    ClipboardHarness h(options);
    REQUIRE(h.runtime->start_recording());
    CHECK(h.clipboard.captures == 0); // nothing to put back, so nothing prefetched

    h.complete("dictated words");

    CHECK(h.clipboard.captures == 0);
    CHECK(h.rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(h.clipboard.text() == "dictated words");
    REQUIRE_FALSE(h.statuses.empty());
    CHECK(h.statuses.back() == "Ready (Pasted)");
}

TEST_CASE("restore is off until turned on", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_restore_is_off_until_turned_on
    RigOptions options;
    options.settings.erase("restore_clipboard");
    ClipboardHarness h(options);
    REQUIRE(h.runtime->start_recording());
    CHECK(h.clipboard.captures == 0);

    h.complete("dictated words");

    CHECK(h.rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(h.clipboard.text() == "dictated words");
    CHECK_FALSE(h.clipboard.fire_restore());
}

TEST_CASE("copy made during dictation is what gets restored", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_copy_made_during_dictation_is_what_gets_restored
    ClipboardHarness h;
    REQUIRE(h.runtime->start_recording());
    REQUIRE(h.clipboard.captures == 1);

    h.clipboard.copy("copied mid-dictation");
    h.complete("dictated words");
    CHECK(h.clipboard.fire_restore());

    CHECK(h.clipboard.captures == 2);
    CHECK(h.clipboard.text() == "copied mid-dictation");
}

TEST_CASE("canceled dictation leaves no snapshot behind", "[test_paste_latency]") {
    // from tests/test_paste_latency.py::test_canceled_dictation_leaves_no_snapshot_behind
    ClipboardHarness h;
    REQUIRE(h.runtime->start_recording());
    REQUIRE(h.clipboard.captures == 1);

    h.runtime->cancel();
    h.clipboard.stage_text("next transcript", std::nullopt);

    CHECK(h.clipboard.captures == 2);
}

// --- tests/test_strict_switch_reads.py ---------------------------------------------------

TEST_CASE("auto paste reads its switch strictly", "[test_strict_switch_reads]") {
    // from tests/test_strict_switch_reads.py::test_auto_paste_reads_its_switch_strictly
    const auto [stored, expected] =
        GENERATE(std::pair{fv::Json(true), true}, std::pair{fv::Json(false), false},
                 std::pair{fv::Json("false"), true}, std::pair{fv::Json(0), true},
                 std::pair{fv::Json(nullptr), true});
    RigOptions options;
    options.settings = fv::Json{{"auto_paste", stored}};
    RuntimeRig rig(options);

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.keys.sent().empty() != expected);
}
