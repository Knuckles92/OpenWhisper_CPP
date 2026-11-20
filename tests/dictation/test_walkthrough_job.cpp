// The walkthrough job of the transcription runtime: tests/test_walkthrough_runtime.py, recorded
// like a dictation, transcribed section by section, and delivered as a document. Where Python
// calls transcribe_audio_file after setting the pending audio by hand, the C++ test runs the
// stop's own worker, whose save hands the runtime the recording the test wrote; where it patches
// build_documents, the real one writes with the FakeRenderer and the test reads the manifest back;
// where it records the backend's clip names, the engine's decode calls (one per clip) stand in.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "appcore/activity.h"
#include "base/utf8.h"
#include "cleanup_support.h"
#include "dictation/events.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "dictation/walkthrough_recording.h"
#include "pyport/text.h"
#include "speech/incremental_session.h"
#include "walkthrough/files.h"
#include "walkthrough/finish.h"
#include "walkthrough/storage.h"
#include "walkthrough/write_up.h"
#include "walkthrough_support.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using namespace std::chrono_literals;
using fv::Json;
using fv::walkthrough::FakeShotImage;
using fv::walkthrough::Note;
using fv::walkthrough::WalkthroughResult;

namespace {

double samples_s(const fv::speech::FakeCall& call) {
    return static_cast<double>(call.samples) / kWalkthroughRate;
}

// The recording of the three-section tests: words, a pause before each screenshot, words.
void write_three_parts(const std::filesystem::path& audio) {
    write_recording(audio, {tone(1.9), silence(0.3), tone(1.9), silence(0.3), tone(1.6)});
}

std::string name_of(const std::filesystem::path& path) {
    return fv::path_to_utf8(path.filename());
}

// _write_up_on: the AI write-up on with the user's own words, and one screenshot at 2 s of a
// 4 s recording, recorded and stopped.
void write_up_on(WalkthroughRig& w) {
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);
}

WalkthroughRig::Options write_up_options(Json settings = Json::object()) {
    WalkthroughRig::Options options;
    options.settings = std::move(settings);
    options.settings["walkthrough_write_up"] = true;
    options.settings["walkthrough_write_up_instructions"] = "  Make it a checklist. ";
    return options;
}

} // namespace

TEST_CASE("recovered walkthrough claims a walkthrough job and keeps sections",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_recovered_walkthrough_claims_a_walkthrough_job_and_keeps_sections
    WalkthroughRig w;
    w.session->checkpoint_recording(std::string(32, 'a'));
    REQUIRE(w.session->add_shot(shot_image(), 1.0));
    w.session->saved_shots();
    write_recording(w.audio, {tone(3.0)});
    w.session->preserve_audio(w.audio);
    const auto folder = w.folder();
    w.session->end();

    w.runtime().recover_walkthrough(folder);

    const auto job = w.runtime().current_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Walkthrough);
    CHECK(w.runtime().pending().audio_duration == 3.0);
    CHECK(w.runtime().pending().source_name == "Walkthrough");
    CHECK(w.rig.last_status() == "Recovering walkthrough...");
    // The decode waits on the jobs strand, as Python's submit does.
    CHECK(w.rig.jobs.queued() == 1);

    w.rig.run_workers();

    // Sections from the draft's screenshot at 1.0 s of folder/audio.wav.
    CHECK(w.decodes().size() == 2);
    CHECK(w.built_texts() == std::vector<std::string>{"first part", "second part"});
    REQUIRE(w.built().sections[0].kept_at_s);
    CHECK(*w.built().sections[0].kept_at_s == 1.0);
    CHECK(w.rig.entries().size() == 1);
    CHECK(w.delivery.ready().size() == 1);
}

TEST_CASE("recovery does not replace a busy job", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_recovery_does_not_replace_a_busy_job
    WalkthroughRig w;
    w.session->checkpoint_recording(std::string(32, 'a'));
    write_recording(w.audio, {tone(3.0)});
    w.session->preserve_audio(w.audio);
    w.session->end();
    REQUIRE(w.runtime().claim_job());

    w.runtime().recover_walkthrough(w.folder());

    CHECK(w.rig.jobs.queued() == 0);
    CHECK_FALSE(w.runtime().current_job());
    CHECK(w.rig.last_status() ==
          "A transcription is already in progress — wait before recovering a walkthrough");
    CHECK(std::filesystem::exists(w.folder() / "audio.wav"));
    w.runtime().finish_job();
    w.rig.run_workers();
    CHECK(w.decodes().empty());
}

TEST_CASE("a walkthrough records like a dictation without early decoding",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_walkthrough_records_like_a_dictation_without_early_decoding
    WalkthroughRig w;
    REQUIRE(w.runtime().start_walkthrough(w.session));

    const auto job = w.runtime().current_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Walkthrough);
    CHECK(w.runtime().recording_walkthrough() == w.session);
    CHECK(w.destination_labels() == std::vector<std::string>{"Walkthrough"});
    CHECK(w.rig.last_status() == "Walkthrough \xC2\xB7 Recording...");
    CHECK(w.rig.trace.with_prefix("overlay:").back() == "recording");
    CHECK_FALSE(w.runtime().incremental_dictation().session());
    // Nothing is pasted, so the user's clipboard isn't snapshotted for it.
    CHECK(w.rig.trace.count("prefetch") == 0);
}

TEST_CASE("a walkthrough reads nothing on screen", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_a_walkthrough_reads_nothing_on_screen
    FrontAppService service;
    const Json settings{{"app_context_enabled", true}};
    JobOptions options;
    options.source = "hotkey";

    const DictationJob job =
        begin_job(JobMode::Walkthrough, settings, RecognitionContext{}, options, &service);

    CHECK_FALSE(job.focus);
    CHECK_FALSE(job.target);
    CHECK(service.requests() == 0);
}

TEST_CASE("stopping closes the walkthrough to new screenshots", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_stopping_closes_the_walkthrough_to_new_screenshots
    WalkthroughRig w;
    w.record({1.0});

    CHECK_FALSE(w.session->open());
    CHECK_FALSE(w.runtime().recording_walkthrough());
    const auto job = w.runtime().current_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Walkthrough);
    CHECK_FALSE(w.session->add_shot(shot_image(), 5.0));
}

TEST_CASE("each section is transcribed and the document copied", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_each_section_is_transcribed_and_the_document_copied
    WalkthroughRig w;
    write_three_parts(w.audio);
    w.record({2.0, 4.2}, 6.0);

    w.rig.run_workers();

    // One clip per section, in the order spoken, the whole recording between them.
    const auto decodes = w.decodes();
    REQUIRE(decodes.size() == 3);
    CHECK(samples_s(decodes[0]) + samples_s(decodes[1]) + samples_s(decodes[2]) ==
          Catch::Approx(6.0).margin(0.01));
    const auto built = w.built();
    CHECK(w.built_texts() == std::vector<std::string>{"first part", "second part", "last part"});
    REQUIRE(built.sections[0].kept_at_s);
    REQUIRE(built.sections[1].kept_at_s);
    CHECK(*built.sections[0].kept_at_s == 2.0);
    CHECK(*built.sections[1].kept_at_s == 4.2);
    CHECK(built.duration_s == 6.0);
    const auto folder = w.folder();
    // The default copy is the PDF alone.
    CHECK(w.delivery.copied() ==
          std::vector<FakeWalkthroughDelivery::Copy>{{{folder / "walkthrough.pdf"}, ""}});
    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].second);
    CHECK(ready[0].first.shots == 2);
    CHECK(w.status_seen("Walkthrough copied (2 screenshots) — paste it where you need it"));
    const auto entries = w.rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text ==
          "first part\n\n[Screenshot 1]\n\nsecond part\n\n[Screenshot 2]\n\nlast part");
    CHECK(entries[0].source_name == "Walkthrough");
    CHECK(entries[0].entry_kind == "walkthrough");
    // The job is over: the session is let go, its files kept.
    CHECK_FALSE(w.runtime().has_active_job());
    CHECK(std::filesystem::exists(folder / "shot-1.png"));
}

TEST_CASE("a scripted walkthrough says each step, writes its documents and counts its shots",
          "[test_walkthrough_runtime]") {
    // The M6.11 acceptance: the statuses in order, the four documents through the renderer, and
    // {"shots": 2} in Stats.
    WalkthroughRig w;
    write_three_parts(w.audio);
    w.record({2.0, 4.2}, 6.0);
    w.rig.run_workers();

    const auto statuses = w.rig.statuses();
    const auto at = [&](const std::string& status) {
        for (std::size_t i = 0; i < statuses.size(); ++i) {
            if (statuses[i] == status) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return static_cast<std::ptrdiff_t>(-1);
    };
    const auto recording = at("Walkthrough \xC2\xB7 Recording...");
    const auto first = at("Transcribing section 1 of 3...");
    const auto third = at("Transcribing section 3 of 3...");
    const auto writing = at("Writing the walkthrough...");
    const auto copied = at("Walkthrough copied (2 screenshots) — paste it where you need it");
    CHECK(recording >= 0);
    CHECK(recording < first);
    CHECK(first < third);
    CHECK(third < writing);
    CHECK(writing < copied);
    const auto folder = w.folder();
    for (const char* name :
         {"walkthrough.md", "walkthrough.pdf", "walkthrough.docx", "walkthrough.json"}) {
        CHECK(std::filesystem::is_regular_file(folder / name));
    }
    const auto calls = w.renderer->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].kind == fv::walkthrough::FakeRenderer::Kind::Pdf);
    CHECK(calls[1].kind == fv::walkthrough::FakeRenderer::Kind::Docx);
    CHECK(calls[0].sections == 3);
    CHECK_FALSE(std::filesystem::exists(folder / "draft.json"));
    const auto rows = w.rig.stats->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entry_kind == "walkthrough");
    CHECK(rows[0].shots == 2);
}

TEST_CASE("a held note goes to the write-up not the document", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_held_note_goes_to_the_write_up_not_the_document
    auto options = write_up_options();
    options.script = words({"first part", "second part", "last part", "make it purple"});
    WalkthroughRig w(options);
    write_three_parts(w.audio);

    w.record({2.0, 4.2}, 6.0, [](fv::walkthrough::WalkthroughSession& session) {
        session.hold_note(2.6);
        session.release_note(3.4);
    });
    w.rig.run_workers();

    // Each section, then the note on its own.
    const auto decodes = w.decodes();
    REQUIRE(decodes.size() == 4);
    CHECK(samples_s(decodes[3]) == Catch::Approx(0.8).margin(0.01));
    CHECK(w.built_texts() == std::vector<std::string>{"first part", "second part", "last part"});
    const auto entries = w.rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text.find("purple") == std::string::npos);
    const auto calls = w.write_up_calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].notes == std::vector<Note>{{"make it purple", 1}});
    // Saved with the walkthrough, for a later write-up.
    CHECK(fv::walkthrough::held_notes(w.built()) == calls[0].notes);
}

TEST_CASE("a screenshot moved up takes its words into the document", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_screenshot_moved_up_takes_its_words_into_the_document
    WalkthroughRig w;
    write_three_parts(w.audio);
    w.record({2.0, 4.2}, 6.0,
             [](fv::walkthrough::WalkthroughSession& session) { session.move_shot(2, -1); });
    w.rig.run_workers();

    // The recording is still decoded in the order it was spoken.
    CHECK(w.decodes().size() == 3);
    CHECK(w.built_texts() == std::vector<std::string>{"second part", "first part", "last part"});
    const auto built = w.built();
    REQUIRE(built.sections[0].shot);
    REQUIRE(built.sections[1].shot);
    CHECK(name_of(*built.sections[0].shot) == "shot-2.png");
    CHECK(*built.sections[0].kept_at_s == 4.2);
    CHECK(name_of(*built.sections[1].shot) == "shot-1.png");
    CHECK(*built.sections[1].kept_at_s == 2.0);
}

TEST_CASE("an undone screenshot leaves its words to the section after it",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_an_undone_screenshot_leaves_its_words_to_the_section_after_it
    WalkthroughRig w;
    write_three_parts(w.audio);
    w.record({2.0, 4.2}, 6.0,
             [](fv::walkthrough::WalkthroughSession& session) { session.undo_last(); });
    w.rig.run_workers();

    // Shot 2's words and the closing words are one stretch, decoded whole.
    CHECK(w.decodes().size() == 2);
    CHECK(w.built_texts() == std::vector<std::string>{"first part", "second part"});
    const auto built = w.built();
    REQUIRE(built.sections[0].shot);
    CHECK(name_of(*built.sections[0].shot) == "shot-1.png");
    CHECK_FALSE(built.sections[1].shot);
    CHECK_FALSE(std::filesystem::exists(w.folder() / "shot-2.png"));
}

TEST_CASE("finished walkthrough uses preferred copy", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_finished_walkthrough_uses_preferred_copy
    struct Row {
        std::string format;
        bool pdf;
        bool markdown;
    };
    const Row row = GENERATE(Row{"both", true, true}, Row{"agent", false, true},
                             Row{"pdf", true, false}, Row{"unknown", true, false});
    CAPTURE(row.format);
    WalkthroughRig::Options options;
    options.settings["walkthrough_copy_format"] = row.format;
    WalkthroughRig w(options);
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);

    w.rig.run_workers();

    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    const auto& result = ready[0].first;
    CHECK_FALSE(result.clipboard_text.empty());
    std::vector<std::filesystem::path> files;
    if (row.pdf) {
        files.push_back(w.folder() / "walkthrough.pdf");
    }
    CHECK(w.delivery.copied() == std::vector<FakeWalkthroughDelivery::Copy>{
                                     {files, row.markdown ? result.clipboard_text : ""}});
    CHECK(ready[0].second);
    if (!row.markdown) {
        CHECK(w.status_containing("paste it where you need it"));
    }
}

TEST_CASE("finished walkthrough agent copy has pdf and action prompt",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_finished_walkthrough_agent_copy_has_pdf_and_action_prompt
    WalkthroughRig::Options options;
    options.settings = Json{{"walkthrough_copy_format", "agent_task"}, {"auto_paste", false}};
    WalkthroughRig w(options);
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);

    w.rig.run_workers();

    const auto copied = w.delivery.copied();
    REQUIRE(copied.size() == 1);
    CHECK(copied[0].first.empty());
    auto pdf = std::filesystem::weakly_canonical(w.folder() / "walkthrough.pdf");
    CHECK(copied[0].second.find(fv::path_to_utf8(pdf.make_preferred())) != std::string::npos);
    CHECK(copied[0].second.find("carry out the tasks and changes") != std::string::npos);
    REQUIRE(w.delivery.ready().size() == 1);
    CHECK(w.delivery.ready()[0].second);
    CHECK(w.status_containing("paste the instructions, then drag the PDF"));
    CHECK(w.delivery.pasted().empty());
}

TEST_CASE("only agent auto paste uses two part delivery", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_only_agent_auto_paste_uses_two_part_delivery
    const std::string format = GENERATE("agent_task", "pdf", "both", "agent", "word");
    const bool auto_paste = GENERATE(true, false);
    CAPTURE(format, auto_paste);
    WalkthroughRig::Options options;
    options.settings = Json{{"walkthrough_copy_format", format}, {"auto_paste", auto_paste}};
    options.front = chat_window();
    WalkthroughRig w(options);
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);

    w.rig.run_workers();

    const auto pasted = w.delivery.pasted();
    if (format == "agent_task" && auto_paste) {
        REQUIRE(pasted.size() == 1);
        CHECK(pasted[0].first == w.delivery.ready().back().first);
        CHECK(pasted[0].second == chat_window());
        CHECK(w.delivery.pasted_after_its_card());
    } else {
        CHECK(pasted.empty());
        CHECK(w.front.identities() == 0);
    }
}

TEST_CASE("agent auto paste waits for write up and uses delivered pdf",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_agent_auto_paste_waits_for_write_up_and_uses_delivered_pdf
    const bool failed = GENERATE(false, true);
    CAPTURE(failed);
    auto options = write_up_options(Json{{"walkthrough_copy_format", "agent_task"}});
    options.queue_write_ups = true;
    options.front = chat_window();
    WalkthroughRig w(options);
    w.write_up = [failed](const WalkthroughWriteUpRequest& request) {
        if (failed) {
            throw fv::walkthrough::write_up::WriteUpFailed("Agent unavailable");
        }
        WalkthroughResult result = request.result;
        result.pdf = result.folder / "write-up.pdf";
        result.written_up = true;
        return result;
    };
    write_up_on(w);

    w.rig.run_workers();
    CHECK(w.delivery.pasted().empty());
    REQUIRE(w.queued_strand.run_all() == 1);

    const auto pasted = w.delivery.pasted();
    REQUIRE(pasted.size() == 1);
    CHECK(pasted[0].second == chat_window());
    CHECK(pasted[0].first == w.delivery.ready().back().first);
    CHECK(name_of(pasted[0].first.pdf) == (failed ? "walkthrough.pdf" : "write-up.pdf"));
    CHECK(pasted[0].first.write_up_error.empty() == !failed);
}

TEST_CASE("each section is cleaned up on its own", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_each_section_is_cleaned_up_on_its_own
    WalkthroughRig::Options options;
    options.settings["transcript_cleanup_enabled"] = true;
    WalkthroughRig w(options);
    FakeCleaner cleaner([](const std::string& text) { return fv::py::upper(text); });
    w.runtime().set_cleaner(&cleaner);
    write_recording(w.audio, {tone(5.0)});
    w.record({2.5}, 5.0);

    w.rig.run_workers();
    w.runtime().set_cleaner(nullptr);

    std::vector<std::string> seen;
    for (const auto& call : cleaner.calls()) {
        seen.push_back(call.text);
    }
    CHECK(seen == std::vector<std::string>{"first part", "second part"});
    CHECK(w.built_texts() == std::vector<std::string>{"FIRST PART", "SECOND PART"});
}

TEST_CASE("screenshots without words still make a walkthrough", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_screenshots_without_words_still_make_a_walkthrough
    WalkthroughRig::Options options;
    options.script = words({"", ""});
    WalkthroughRig w(options);
    write_recording(w.audio, {silence(4.0)});
    w.record({2.0}, 4.0);

    w.rig.run_workers();

    const auto entries = w.rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "[Screenshot 1]");
    CHECK(w.delivery.ready().size() == 1);
}

TEST_CASE("no screenshots and no words is no speech", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_no_screenshots_and_no_words_is_no_speech
    WalkthroughRig::Options options;
    options.script = words({""});
    WalkthroughRig w(options);
    write_recording(w.audio, {silence(2.0)});
    w.record({}, 2.0);

    w.rig.run_workers();

    CHECK(w.rig.entries().empty());
    CHECK(w.delivery.ready().empty());
    CHECK_FALSE(w.runtime().has_active_job());
    // No empty document is written, and the column stops saying "Writing it up…".
    CHECK(w.renderer->calls().empty());
    CHECK_FALSE(std::filesystem::exists(w.folder() / "walkthrough.json"));
    CHECK(w.delivery.dismissed() == 1);
    CHECK(w.rig.last_status() == std::string(EMPTY_ASR_MESSAGE));
}

TEST_CASE("a document that cannot be written keeps the words", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_document_that_cannot_be_written_keeps_the_words
    WalkthroughRig w;
    write_recording(w.audio, {tone(1.9), silence(0.3), tone(1.8)});
    w.record({2.0}, 4.0);
    w.renderer->fail(std::nullopt, "No space left on device");

    w.rig.run_workers();

    const auto entries = w.rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "first part\n\n[Screenshot 1]\n\nsecond part");
    CHECK(w.delivery.copied() == std::vector<FakeWalkthroughDelivery::Copy>{{{}, entries[0].text}});
    CHECK(w.delivery.ready().empty());
    CHECK(w.delivery.dismissed() == 1);
    const std::string status = "Couldn't save the walkthrough document (No space left on device)"
                               " — its words are copied and kept in History";
    CHECK(w.status_seen(status));
    CHECK(w.rig.notices() == std::vector<std::string>{status});
}

TEST_CASE("a document the disk refuses says the system's reason as Python's strerror",
          "[walkthrough_job]") {
    // _complete_walkthrough's OSError: document_error is exc.strerror, which Python takes from
    // FormatMessage without its trailing full stop and line break.
    class DiskFullRenderer final : public fv::walkthrough::DocumentRenderer {
    public:
        static std::error_code disk_full() {
#ifdef _WIN32
            return {112, std::system_category()}; // ERROR_DISK_FULL
#else
            return {ENOSPC, std::system_category()};
#endif
        }
        std::filesystem::path write_pdf(const fv::walkthrough::Walkthrough&,
                                        const std::filesystem::path& folder,
                                        const fv::walkthrough::DocumentFormat&,
                                        fv::walkthrough::PaperSize) override {
            throw std::filesystem::filesystem_error("Could not write the PDF",
                                                    folder / "walkthrough.pdf", disk_full());
        }
        std::filesystem::path write_docx(const fv::walkthrough::Walkthrough&,
                                         const std::filesystem::path& folder,
                                         const fv::walkthrough::DocumentFormat&,
                                         fv::walkthrough::PaperSize) override {
            return folder / "walkthrough.docx";
        }
        void write_markdown_pdf(std::string_view, const std::filesystem::path&,
                                const std::filesystem::path&, std::string_view,
                                const fv::walkthrough::Look&, fv::walkthrough::PaperSize) override {
        }
        void write_markdown_docx(std::string_view, const std::filesystem::path&,
                                 const std::filesystem::path&, std::string_view,
                                 const fv::walkthrough::Look&,
                                 fv::walkthrough::PaperSize) override {}
    };
    WalkthroughRig w;
    fv::walkthrough::set_document_renderer(std::make_shared<DiskFullRenderer>());
    write_recording(w.audio, {tone(1.9), silence(0.3), tone(1.8)});
    w.record({2.0}, 4.0);

    w.rig.run_workers();

    std::string reason = DiskFullRenderer::disk_full().message();
    while (!reason.empty() &&
           (static_cast<unsigned char>(reason.back()) <= ' ' || reason.back() == '.')) {
        reason.pop_back();
    }
    REQUIRE_FALSE(reason.empty());
    const std::string status = "Couldn't save the walkthrough document (" + reason +
                               ") — its words are copied and kept in History";
    CHECK(w.status_seen(status));
    CHECK(w.rig.notices() == std::vector<std::string>{status});
    CHECK(w.delivery.ready().empty());
    REQUIRE(w.rig.entries().size() == 1);
    CHECK(w.rig.entries()[0].text == "first part\n\n[Screenshot 1]\n\nsecond part");
}

TEST_CASE("screenshots that could not be saved are named in the status",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_screenshots_that_could_not_be_saved_are_named_in_the_status
    WalkthroughRig w;
    write_recording(w.audio, {tone(4.0)});
    REQUIRE(w.runtime().start_walkthrough(w.session));
    w.recorder->duration = 2.0;
    REQUIRE(w.session->add_shot(shot_image(FakeShotImage::Save::FailsHalfWritten), 2.0));
    w.recorder->duration = 4.0;
    w.runtime().stop_recording();

    w.rig.run_workers();

    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].first.lost_shots == 1);
    CHECK(w.status_containing("1 screenshot couldn't be saved and is left out"));
}

TEST_CASE("canceling a walkthrough deletes its screenshots", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_canceling_a_walkthrough_deletes_its_screenshots
    WalkthroughRig w;
    REQUIRE(w.runtime().start_walkthrough(w.session));
    const auto shot = w.session->add_shot(shot_image(), 1.0);
    REQUIRE(shot);
    w.session->saved_shots();
    REQUIRE(std::filesystem::exists(shot->path));
    REQUIRE(w.recorder->is_recording());

    w.runtime().cancel();

    CHECK_FALSE(std::filesystem::exists(shot->path));
    CHECK_FALSE(w.runtime().recording_walkthrough());
    CHECK(w.rig.last_status() == "Recording canceled");
}

TEST_CASE("a failed walkthrough keeps its screenshots and says where",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_failed_walkthrough_keeps_its_screenshots_and_says_where
    WalkthroughRig::Options options;
    options.script.decode_error = "engine stopped";
    WalkthroughRig w(options);
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);
    const auto folder = w.folder();

    w.rig.run_workers();

    CHECK(w.status_containing("screenshots kept in " + fv::path_to_utf8(folder)));
    CHECK(w.status_seen("Error: engine stopped — screenshots kept in " + fv::path_to_utf8(folder)));
    CHECK(std::filesystem::exists(folder / "shot-1.png"));
    CHECK(w.delivery.copied().empty());
}

TEST_CASE("a cancel between sections stops the walkthrough", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_a_cancel_between_sections_stops_the_walkthrough
    // Python sets the cancel inside the first clip's decode; here the Cancel lands while the
    // engine holds that decode, and the job stops before the second clip.
    WalkthroughRig w;
    write_recording(w.audio, {tone(4.0)});
    w.record({2.0}, 4.0);
    const auto folder = w.folder();
    w.rig.engine->hold();
    std::thread worker([&w] { w.rig.jobs.run_all(); });
    const bool entered = w.rig.engine->wait_entered(10s);
    if (entered) {
        CHECK(w.runtime().cancel());
    }
    w.rig.engine->resume();
    worker.join();
    REQUIRE(entered);
    w.rig.run_workers();

    CHECK(w.decodes().size() == 1);
    CHECK(w.renderer->calls().empty());
    CHECK_FALSE(std::filesystem::exists(folder / "walkthrough.json"));
    CHECK(w.delivery.copied().empty());
    // Canceled after the recording stopped: thrown away all the same.
    CHECK_FALSE(std::filesystem::exists(folder / "shot-1.png"));
    CHECK_FALSE(w.status_containing("screenshots kept"));
}

TEST_CASE("a walkthrough written up is what gets copied", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_a_walkthrough_written_up_is_what_gets_copied
    WalkthroughRig w(write_up_options(Json{{"walkthrough_write_up_agent", "codex"},
                                           {"walkthrough_write_up_notes", true},
                                           {"walkthrough_copy_format", "both"}}));
    w.write_up = [](const WalkthroughWriteUpRequest& request) {
        WalkthroughResult result = request.result;
        result.pdf = result.folder / "write-up.pdf";
        result.clipboard_text = "# Checklist";
        result.written_up = true;
        return result;
    };
    write_up_on(w);

    w.rig.run_workers();

    const auto folder = w.folder();
    const auto calls = w.write_up_calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].shots == std::vector<std::filesystem::path>{folder / "shot-1.png"});
    CHECK(calls[0].instructions == "Make it a checklist.");
    CHECK(calls[0].follow_notes);
    CHECK(calls[0].agent == "codex");
    // Its own cancel: the job's ended with the job.
    CHECK_FALSE(calls[0].cancel.cancelled());
    CHECK(w.status_seen("Writing it up with Codex..."));
    CHECK(w.delivery.copied() ==
          std::vector<FakeWalkthroughDelivery::Copy>{{{folder / "write-up.pdf"}, "# Checklist"}});
    CHECK(w.status_seen("Write-up copied (1 screenshot) — paste it into your agent"));
    CHECK_FALSE(w.runtime().writing_up());
    CHECK_FALSE(w.rig.activity.snapshot().write_up_running);
}

TEST_CASE("a dictation runs while the write up is still writing", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_dictation_runs_while_the_write_up_is_still_writing
    auto options = write_up_options();
    options.queue_write_ups = true;
    WalkthroughRig w(options);
    w.write_up = [](const WalkthroughWriteUpRequest& request) {
        WalkthroughResult result = request.result;
        result.clipboard_text = "# Checklist";
        result.written_up = true;
        return result;
    };
    write_up_on(w);

    w.rig.run_workers();

    CHECK(w.runtime().writing_up());
    CHECK(w.rig.activity.snapshot().write_up_running);
    CHECK(w.queued_strand.queued() == 1);
    CHECK(w.delivery.copied().empty());
    CHECK(w.delivery.ready().empty());
    // The walkthrough's job is over and its words are in History.
    CHECK_FALSE(w.runtime().has_active_job());
    CHECK_FALSE(w.rig.entries().empty());
    CHECK(w.runtime().claim_job());
    w.runtime().finish_job();
    w.queued_strand.run_all();
    CHECK_FALSE(w.runtime().writing_up());
    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].first.written_up);
}

TEST_CASE("a dictation started while the write-up runs completes and pastes",
          "[test_walkthrough_runtime]") {
    // The M6.11 acceptance: the write-up is off the job slot, so a whole dictation records,
    // transcribes and pastes meanwhile; the write-up then copies.
    auto options = write_up_options();
    options.queue_write_ups = true;
    options.script = words({"first part", "second part", "Dictated meanwhile."});
    WalkthroughRig w(options);
    w.write_up = [](const WalkthroughWriteUpRequest& request) {
        WalkthroughResult result = request.result;
        result.written_up = true;
        return result;
    };
    write_up_on(w);
    w.rig.run_workers();
    REQUIRE(w.runtime().writing_up());

    REQUIRE(w.runtime().start_recording());
    w.runtime().stop_recording();
    w.rig.run_workers();

    CHECK(w.rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(w.rig.entries().size() == 2);
    CHECK(w.delivery.ready().empty());
    w.queued_strand.run_all();
    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].first.written_up);
    CHECK(w.status_containing("Write-up copied (1 screenshot)"));
}

TEST_CASE("a failed write up copies the walkthrough as spoken", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_failed_write_up_copies_the_walkthrough_as_spoken
    WalkthroughRig w(write_up_options(Json{{"walkthrough_copy_format", "both"}}));
    w.write_up = [](const WalkthroughWriteUpRequest&) -> WalkthroughResult {
        throw fv::walkthrough::write_up::WriteUpFailed("Claude Code is not signed in.");
    };
    write_up_on(w);

    w.rig.run_workers();

    const auto folder = w.folder();
    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(w.delivery.copied() ==
          std::vector<FakeWalkthroughDelivery::Copy>{
              {{folder / "walkthrough.pdf"}, ready[0].first.clipboard_text}});
    // The failure leads, so a status line cut short still says it.
    CHECK(w.status_seen("Write-up failed — Walkthrough copied as spoken (1 screenshot): "
                        "Claude Code is not signed in."));
    CHECK(ready[0].first.write_up_error == "Claude Code is not signed in.");
}

TEST_CASE("a cancel during the write up keeps the walkthrough as spoken",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_a_cancel_during_the_write_up_keeps_the_walkthrough_as_spoken
    auto options = write_up_options(Json{{"walkthrough_copy_format", "both"}});
    options.queue_write_ups = true;
    WalkthroughRig w(options);
    bool canceled_when_run = false;
    w.write_up =
        [&canceled_when_run](const WalkthroughWriteUpRequest& request) -> WalkthroughResult {
        canceled_when_run = request.cancel.cancelled();
        throw fv::walkthrough::write_up::WriteUpFailed("canceled");
    };
    write_up_on(w);
    const auto folder = w.folder();

    w.rig.run_workers();
    // The user's Cancel once the job is over: nothing else is left to stop.
    CHECK_FALSE(w.runtime().cancel());
    CHECK(w.runtime().cancel_write_up());
    CHECK(w.rig.last_status() == "Stopping the write-up...");
    w.queued_strand.run_all();

    CHECK(canceled_when_run);
    // Its words are already in History, so nothing is deleted.
    CHECK_FALSE(w.rig.entries().empty());
    CHECK(std::filesystem::exists(folder / "shot-1.png"));
    const auto ready = w.delivery.ready();
    REQUIRE(ready.size() == 1);
    CHECK(w.delivery.copied() ==
          std::vector<FakeWalkthroughDelivery::Copy>{
              {{folder / "walkthrough.pdf"}, ready[0].first.clipboard_text}});
    CHECK(ready[0].first.write_up_error == "canceled");
    CHECK_FALSE(w.runtime().writing_up());
    CHECK_FALSE(w.runtime().cancel_write_up());
}

TEST_CASE("an unreadable recording is transcribed whole above the screenshots",
          "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_an_unreadable_recording_is_transcribed_whole_above_the_screenshots
    // Python's recording is no WAV at all, which its fake engine still "decodes"; the real speech
    // service decodes only audio, so this one is a WAV the cutter cannot read (8-bit) that the
    // engine can.
    WalkthroughRig::Options options;
    options.script = words({"everything I said"});
    WalkthroughRig w(options);
    write_recording(w.audio, {tone(3.0)}, 8);
    w.record({1.0, 2.0}, 3.0);

    w.rig.run_workers();

    const auto decodes = w.decodes();
    REQUIRE(decodes.size() == 1);
    CHECK(samples_s(decodes[0]) == Catch::Approx(3.0).margin(0.01));
    // The words above both screenshots, which have none of their own (the manifest keeps no
    // empty trailing section, so History's marked text shows the three texts).
    const auto entries = w.rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "everything I said\n\n[Screenshot 1]\n\n[Screenshot 2]");
    CHECK(w.built_texts().front() == "everything I said");
}

TEST_CASE("walkthrough text reaches events with its raw words", "[test_walkthrough_runtime]") {
    // from
    // tests/test_walkthrough_runtime.py::test_walkthrough_text_reaches_events_with_its_raw_words
    DictationJob job;
    job.mode = JobMode::Walkthrough;

    const Json data = events::transcription_completed(&job, "Fixed.", std::string("fixed"),
                                                      Json::object(), std::nullopt, std::nullopt);

    CHECK(data["mode"] == "walkthrough");
    CHECK(data["raw_text"] == "fixed");
}

TEST_CASE("the saved job future is never needed", "[test_walkthrough_runtime]") {
    // from tests/test_walkthrough_runtime.py::test_the_saved_job_future_is_never_needed
    // A walkthrough job carries no captures for the paste check.
    FrontAppService service(chat_window());
    JobOptions options;
    options.source = "hotkey";
    const DictationJob job =
        begin_job(JobMode::Walkthrough, Json::object(), RecognitionContext{}, options, &service);

    CHECK(paste_target(&job, &service) == PasteTarget::Unknown);
    CHECK_FALSE(job.focus);
    CHECK_FALSE(job.selection.valid());
}

// --- beyond the Python file --------------------------------------------------------------

TEST_CASE("a walkthrough's stop that fails keeps its screenshots and ends the recording",
          "[walkthrough_job]") {
    // transcription.py _stop_recording: the failed stop forgets the recording (its screenshots
    // stay) and, for a walkthrough, says the recording ended.
    WalkthroughRig w;
    REQUIRE(w.runtime().start_walkthrough(w.session));
    const auto shot = w.session->add_shot(shot_image(), 1.0);
    REQUIRE(shot);
    w.session->saved_shots();
    w.recorder->stop_ok = false;

    w.runtime().stop_recording();

    CHECK(w.rig.last_status() == "Failed to stop recording");
    CHECK(w.rig.trace.with_prefix("recording:").back() == "off");
    CHECK_FALSE(w.session->open());
    CHECK_FALSE(w.runtime().recording_walkthrough());
    CHECK(std::filesystem::exists(shot->path));
    CHECK_FALSE(w.runtime().has_active_job());
}

TEST_CASE("a walkthrough job holds the library and a write-up keeps holding it",
          "[walkthrough_job]") {
    // The flags walkthrough_library_refusal reads: the walkthrough being transcribed, then its
    // AI write-up, never a dictation.
    auto options = write_up_options();
    options.queue_write_ups = true;
    WalkthroughRig w(options);
    write_up_on(w);
    CHECK(w.rig.activity.snapshot().walkthrough_job);
    w.rig.run_workers();
    CHECK_FALSE(w.rig.activity.snapshot().walkthrough_job);
    CHECK(w.rig.activity.snapshot().write_up_running);
    w.queued_strand.run_all();
    CHECK_FALSE(w.rig.activity.snapshot().write_up_running);

    REQUIRE(w.runtime().claim_job());
    CHECK_FALSE(w.rig.activity.snapshot().walkthrough_job);
    w.runtime().finish_job();
}

TEST_CASE("recovering a walkthrough is refused while another is saved or recorded",
          "[walkthrough_job]") {
    // transcription.py recover_walkthrough's refusals, and a draft that cannot be read.
    WalkthroughRig w;
    w.session->checkpoint_recording(std::string(32, 'a'));
    write_recording(w.audio, {tone(3.0)});
    w.session->preserve_audio(w.audio);
    w.session->end();
    const auto folder = w.folder();

    w.rig.activity.set_library_change(true);
    w.runtime().recover_walkthrough(folder);
    CHECK(w.rig.last_status() ==
          "Wait for the walkthrough being saved to finish before recovering one");
    w.rig.activity.set_library_change(false);

    w.rig.activity.set_meeting_active(true);
    w.runtime().recover_walkthrough(folder);
    CHECK(w.rig.last_status() ==
          "A transcription is already in progress — wait before recovering a walkthrough");
    w.rig.activity.set_meeting_active(false);

    // The recovered audio is not a WAV.
    {
        std::ofstream broken(folder / "audio.wav", std::ios::binary | std::ios::trunc);
        broken << "not a wav file";
    }
    w.runtime().recover_walkthrough(folder);
    CHECK(w.rig.last_status() ==
          "Couldn't recover the walkthrough: file does not start with RIFF id");

    w.runtime().recover_walkthrough(w.captures() / "nowhere");
    CHECK(w.rig.last_status().starts_with("Couldn't recover the walkthrough: "));
    CHECK_FALSE(w.runtime().has_active_job());
    CHECK(w.rig.jobs.queued() == 0);
}

TEST_CASE("startup recovery joins an interrupted walkthrough to its recovered audio",
          "[walkthrough_job]") {
    // transcription.py recover_recordings' walkthrough half: the draft gets audio.wav from the
    // WAV its journal was recovered into, and the status names walkthroughs first.
    WalkthroughRig::Options options;
    options.captures_root = true;
    WalkthroughRig w(options);
    const std::string journal = "0123456789abcdef0123456789abcdef";
    w.session->checkpoint_recording(journal);
    REQUIRE(w.session->add_shot(shot_image(), 1.0));
    w.session->saved_shots();
    w.session->end();
    const auto recordings = w.rig.history->recordings_folder();
    std::filesystem::create_directories(recordings);
    write_recording(recordings / ("recording_2026-10-21_14-40-00-recovered-" + journal + ".wav"),
                    {tone(1.0)});

    w.runtime().recover_recordings();

    CHECK(w.rig.last_status() ==
          "Recovered 1 walkthrough(s) — open Saved Walkthroughs to finish them");
    CHECK(std::filesystem::is_regular_file(w.folder() / "audio.wav"));
}

TEST_CASE("startup recovery without a captures root leaves walkthroughs alone",
          "[walkthrough_job]") {
    WalkthroughRig w;
    const std::string journal = "0123456789abcdef0123456789abcdef";
    w.session->checkpoint_recording(journal);
    REQUIRE(w.session->add_shot(shot_image(), 1.0));
    w.session->saved_shots();
    w.session->end();
    const auto recordings = w.rig.history->recordings_folder();
    std::filesystem::create_directories(recordings);
    write_recording(recordings / ("recording_2026-10-21_14-40-00-recovered-" + journal + ".wav"),
                    {tone(1.0)});

    w.runtime().recover_recordings();

    CHECK_FALSE(std::filesystem::exists(w.folder() / "audio.wav"));
}

TEST_CASE("quitting stops the write-up and copies nothing", "[walkthrough_job]") {
    // begin_shutdown sets the write-up's cancel; on_walkthrough_written_up copies nothing once
    // quitting.
    auto options = write_up_options();
    options.queue_write_ups = true;
    WalkthroughRig w(options);
    bool canceled_when_run = false;
    w.write_up = [&canceled_when_run](const WalkthroughWriteUpRequest& request) {
        canceled_when_run = request.cancel.cancelled();
        return request.result;
    };
    write_up_on(w);
    w.rig.run_workers();
    REQUIRE(w.runtime().writing_up());

    w.rig.activity.set_shutting_down(true);
    w.runtime().begin_shutdown();
    w.queued_strand.run_all();

    CHECK(canceled_when_run);
    CHECK_FALSE(w.runtime().writing_up());
    CHECK(w.delivery.ready().empty());
    CHECK(w.delivery.copied().empty());
}

TEST_CASE("the walkthrough recording port reaches the runtime and the recorder",
          "[walkthrough_job]") {
    WalkthroughRig w;
    WalkthroughRecording recording(w.runtime(), *w.recorder, w.rig.activity);

    w.rig.set_readiness("The speech engine is loading");
    CHECK(recording.busy_message() == "The speech engine is loading");
    w.rig.set_readiness(std::nullopt);
    CHECK(recording.busy_message().empty());

    CHECK_FALSE(recording.refuse_during_meeting());
    w.rig.activity.set_meeting_active(true);
    CHECK(recording.refuse_during_meeting());
    CHECK(w.rig.last_status() == "Meeting Mode is active — end the meeting to use dictation");
    w.rig.activity.set_meeting_active(false);
    w.rig.activity.set_backup_in_progress(true);
    CHECK(recording.refuse_during_meeting());
    CHECK(w.rig.last_status() == "Wait for backup or restore preparation to finish");
    w.rig.activity.set_backup_in_progress(false);

    recording.report_status("Screenshot 1 added");
    CHECK(w.rig.last_status() == "Screenshot 1 added");
    recording.set_destination_label("Walkthrough \xC2\xB7 2 shots");
    CHECK(w.destination_labels().back() == "Walkthrough \xC2\xB7 2 shots");

    // The fake recorder cannot pause.
    CHECK_FALSE(recording.pause());
    CHECK_FALSE(recording.paused());

    REQUIRE(recording.start_walkthrough(w.session, "ui"));
    CHECK(recording.is_recording());
    CHECK(recording.recording_walkthrough() == w.session);
    CHECK(w.runtime().current_job()->source == "ui");
    w.recorder->duration = 2.5;
    CHECK(recording.recording_duration() == 2.5);
    CHECK_FALSE(recording.writing_up());

    // Without a CancelRouter, the controller's first step: the recording.
    recording.cancel();
    CHECK(w.rig.last_status() == "Recording canceled");
    CHECK_FALSE(recording.recording_walkthrough());
    CHECK(recording.capture_canceled());
}

TEST_CASE("the walkthrough recording port hands Cancel to the controller", "[walkthrough_job]") {
    WalkthroughRig w;
    int canceled = 0;
    WalkthroughRecording recording(w.runtime(), *w.recorder, w.rig.activity,
                                   [&canceled] { ++canceled; });
    REQUIRE(recording.start_walkthrough(w.session, "hotkey"));

    recording.cancel();

    CHECK(canceled == 1);
    CHECK(recording.recording_walkthrough() == w.session);
}
