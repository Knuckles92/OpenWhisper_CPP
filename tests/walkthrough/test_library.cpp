// from tests/test_walkthrough_library.py, five of tests/test_walkthrough_storyboard.py and one of
// tests/test_walkthrough_document.py
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <thread>

#include "base/cancel.h"
#include "library_support.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "recorder/journal.h"
#include "settings/keys.h"
#include "walkthrough/files.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"
#include "walkthrough/write_up.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using fv::Json;

namespace fv::walkthrough_test {
extern std::filesystem::path g_self;
} // namespace fv::walkthrough_test

namespace {

std::string value_error(const std::function<void()>& call) {
    try {
        call();
    } catch (const fv::py::ValueError& error) {
        return error.what();
    }
    return "<no ValueError>";
}

std::vector<std::string> section_texts(const Walkthrough& walkthrough) {
    std::vector<std::string> texts;
    for (const auto& section : walkthrough.sections) {
        texts.push_back(section.text);
    }
    return texts;
}

std::vector<std::string> shot_names(const Walkthrough& walkthrough) {
    std::vector<std::string> names;
    for (const auto& section : walkthrough.sections) {
        names.push_back(section.shot ? fv::path_to_utf8(section.shot->filename()) : "None");
    }
    return names;
}

std::vector<std::vector<std::string>> section_notes(const Walkthrough& walkthrough) {
    std::vector<std::vector<std::string>> notes;
    for (const auto& section : walkthrough.sections) {
        notes.push_back(section.notes);
    }
    return notes;
}

std::string forward_slashes(std::string text) {
    std::ranges::replace(text, '\\', '/');
    return text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// A stand-in agent: answers every task with `reply` and records the requests.
struct Agent {
    std::string reply;
    std::vector<write_up::AgentTaskRequest> calls;

    write_up::AgentTaskRunner runner() {
        return [this](const write_up::AgentTaskRequest& request) {
            calls.push_back(request);
            fv::agents::PassOutcome outcome;
            outcome.ok = true;
            outcome.text = reply;
            return outcome;
        };
    }
};

WalkthroughResult agent_says(const Capture& capture, const std::string& reply,
                             std::vector<std::filesystem::path> shots = {}) {
    Agent agent{reply, {}};
    return write_up::write_up(capture.result, shots, "claude_code", "", false, {}, Json::object(),
                              fv::CancelToken{}, agent.runner());
}

std::filesystem::path realpath(const std::filesystem::path& path) {
    return std::filesystem::weakly_canonical(path).make_preferred();
}

const std::vector<bool> kAll{true, true, true};

} // namespace

TEST_CASE("result round trip rebuilds paths for the current machine",
          "[test_walkthrough_library]") {
    Capture capture;
    const auto moved = capture.tmp / "other-machine" / "2026-10-21_14-32-05";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::copy(capture.folder, moved, std::filesystem::copy_options::recursive);

    const auto result = load_result(moved);

    CHECK(result.folder == moved);
    CHECK_FALSE(contains(result.clipboard_text, fv::path_to_utf8(capture.folder)));
    CHECK(
        contains(forward_slashes(result.clipboard_text), forward_slashes(fv::path_to_utf8(moved))));
    CHECK(result.history_text == "first\n\n[Screenshot 1]\n\nsecond\n\n[Screenshot 2]\n\nlast");
    CHECK(result.shots == 2);
}

TEST_CASE("review edits words and omits screenshot but keeps the original",
          "[test_walkthrough_library]") {
    Capture capture;
    const std::vector<std::string> texts{"fixed", "next", "tail"};

    const auto result =
        save_review(capture.folder, texts, {false, true, false}, {}, Json::object());

    const auto saved = load_document(capture.folder);
    CHECK(section_texts(saved) == texts);
    CHECK(shot_names(saved) == std::vector<std::string>{"None", "shot-2.png", "None"});
    CHECK(result.shots == 1);
    CHECK(result.history_text == "fixed\n\nnext\n\n[Screenshot 1]\n\ntail");
    CHECK(std::filesystem::is_regular_file(capture.folder / "shot-1.png"));
    const auto original = read_json(capture.folder / "walkthrough.original.json");
    CHECK(original["sections"][0]["text"] == "first");
    CHECK(contains(result.clipboard_text, "fixed"));
    CHECK_FALSE(contains(result.clipboard_text, "shot-1.png"));
    // The FakeRenderer's PDF stands for Python's "%PDF" check.
    CHECK(read_file(result.pdf).starts_with("fake pdf"));
    save_review(capture.folder, std::vector<std::string>{"again", "next", "tail"},
                {false, true, false}, {}, Json::object());
    CHECK(read_json(capture.folder / "walkthrough.original.json") == original);
}

TEST_CASE("review can include an omitted screenshot again", "[test_walkthrough_library]") {
    Capture capture;
    save_review(capture.folder, std::vector<std::string>{"first", "", "last"}, {true, false, true},
                {}, Json::object());

    auto saved = load_document(capture.folder);
    // The emptied section survives so its screenshot can come back.
    REQUIRE(saved.sections.size() == 3);
    CHECK_FALSE(saved.sections[1].shot);
    REQUIRE(saved.sections[1].omitted);
    CHECK(saved.sections[1].omitted->filename() == "shot-2.png");
    CHECK(saved.sections[1].kept_at_s == 2.0);
    const auto manifest = read_json(capture.folder / MANIFEST_NAME);
    CHECK(manifest["sections"][1] ==
          fv::py::loads(R"({"text": "", "shot": null, "kept_at_s": 2.0, )"
                        R"("omitted_shot": "shot-2.png"})"));
    CHECK_FALSE(contains(read_file(capture.folder / MARKDOWN_NAME), "shot-2.png"));

    const auto result =
        save_review(capture.folder, std::vector<std::string>{"first", "back", "last"}, kAll, {},
                    Json::object());

    saved = load_document(capture.folder);
    CHECK(shot_names(saved) == std::vector<std::string>{"shot-1.png", "shot-2.png", "None"});
    CHECK_FALSE(saved.sections[1].omitted);
    CHECK(result.shots == 2);
    CHECK_FALSE(contains(fv::py::dumps(read_json(capture.folder / MANIFEST_NAME)), "omitted_shot"));
}

TEST_CASE("a deleted omitted screenshot does not break the document",
          "[test_walkthrough_library]") {
    Capture capture;
    save_review(capture.folder, std::vector<std::string>{"first", "second", "last"},
                {false, true, true}, {}, Json::object());
    std::filesystem::remove(capture.folder / "shot-1.png");

    const auto saved = load_document(capture.folder);

    CHECK_FALSE(saved.sections[0].shot);
    CHECK_FALSE(saved.sections[0].omitted);
}

TEST_CASE("review failure keeps existing documents and write up", "[test_walkthrough_library]") {
    // The PDF fails, as in Python; the Word document, rendered after it, fails the same way.
    const auto failing = GENERATE(FakeRenderer::Kind::Pdf, FakeRenderer::Kind::Docx);
    Capture capture;
    write_file(capture.folder / "write-up.md", "old Markdown");
    write_file(capture.folder / "write-up.pdf", "old PDF");
    write_file(capture.folder / "write-up.docx", "old Word document");
    const auto before = snapshot(capture.folder);
    capture.renderer->fail(failing, "disk full");

    try {
        save_review(capture.folder, std::vector<std::string>{"new", "next", "last"}, kAll, {},
                    Json::object());
        FAIL("save_review did not raise");
    } catch (const DocumentError& error) {
        CHECK(std::string(error.what()) == "disk full");
    }

    CHECK(snapshot(capture.folder) == before);
}

namespace {

// Writes everything as the FakeRenderer does, but makes replacing the saved walkthrough.pdf
// fail, so publishing fails after walkthrough.md was published (Python's test fails os.replace
// for that file with "PDF is locked"). On Windows the saved PDF is held open without delete
// sharing, as a PDF viewer holds it: it can still be copied aside, not replaced. Elsewhere the
// PDF is staged as a folder, which rename() cannot put over a file.
class UnpublishablePdf final : public DocumentRenderer {
public:
    explicit UnpublishablePdf(std::shared_ptr<FakeRenderer> inner) : inner_(std::move(inner)) {}
    ~UnpublishablePdf() override {
#ifdef _WIN32
        if (lock_ != INVALID_HANDLE_VALUE) {
            CloseHandle(lock_);
        }
#endif
    }
    UnpublishablePdf(const UnpublishablePdf&) = delete;
    UnpublishablePdf& operator=(const UnpublishablePdf&) = delete;

    std::filesystem::path write_pdf(const Walkthrough& walkthrough,
                                    const std::filesystem::path& folder,
                                    const DocumentFormat& format, PaperSize paper) override {
#ifdef _WIN32
        // `folder` is the staging folder inside the walkthrough's.
        const auto saved = folder.parent_path() / "walkthrough.pdf";
        lock_ = CreateFileW(saved.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(lock_ != INVALID_HANDLE_VALUE);
        return inner_->write_pdf(walkthrough, folder, format, paper);
#else
        (void)walkthrough;
        (void)format;
        (void)paper;
        const auto target = folder / "walkthrough.pdf";
        std::filesystem::create_directories(target / "inside");
        return target;
#endif
    }
    std::filesystem::path write_docx(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder,
                                     const DocumentFormat& format, PaperSize paper) override {
        return inner_->write_docx(walkthrough, folder, format, paper);
    }
    void write_markdown_pdf(std::string_view text, const std::filesystem::path& target,
                            const std::filesystem::path& folder, std::string_view title,
                            const Look& look, PaperSize paper) override {
        inner_->write_markdown_pdf(text, target, folder, title, look, paper);
    }
    void write_markdown_docx(std::string_view text, const std::filesystem::path& target,
                             const std::filesystem::path& folder, std::string_view title,
                             const Look& look, PaperSize paper) override {
        inner_->write_markdown_docx(text, target, folder, title, look, paper);
    }

private:
    std::shared_ptr<FakeRenderer> inner_;
#ifdef _WIN32
    HANDLE lock_ = INVALID_HANDLE_VALUE;
#endif
};

} // namespace

TEST_CASE("failed publication rolls back documents and write up", "[test_walkthrough_library]") {
    Capture capture;
    write_file(capture.folder / "write-up.md", "old Markdown");
    write_file(capture.folder / "write-up.pdf", "old PDF");
    const auto before = snapshot(capture.folder);
    auto renderer = std::make_shared<UnpublishablePdf>(capture.renderer);
    set_document_renderer(renderer);

    CHECK_THROWS_AS(save_review(capture.folder, std::vector<std::string>{"new", "next", "last"},
                                kAll, {}, Json::object()),
                    std::filesystem::filesystem_error);

    // The lock goes before the files are read back.
    set_document_renderer(capture.renderer);
    renderer.reset();
    for (const auto& [name, contents] : before) {
        CHECK(read_file(capture.folder / name) == contents);
    }
}

TEST_CASE("review keeps a previous write up", "[test_walkthrough_library]") {
    Capture capture;
    write_file(capture.folder / "write-up.md", "old write-up");
    write_file(capture.folder / "write-up.pdf", "old PDF");
    write_file(capture.folder / "write-up.docx", "old Word document");

    const auto result = save_review(capture.folder, std::vector<std::string>{"new", "next", "last"},
                                    kAll, {}, Json::object());

    CHECK_FALSE(result.written_up);
    CHECK(result.docx() == capture.folder / "walkthrough.docx");
    // Python reads "new" back from the Word document; the FakeRenderer only records that it was
    // asked for the three sections, beside the Markdown that holds the words.
    const auto calls = capture.renderer->calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls.back().kind == FakeRenderer::Kind::Docx);
    CHECK(calls.back().sections == 3);
    CHECK(read_file(result.docx()).starts_with("fake docx"));
    CHECK(contains(read_file(capture.folder / MARKDOWN_NAME), "new"));
    CHECK(read_file(capture.folder / "write-up.md") == "old write-up");
    CHECK(read_file(capture.folder / "write-up.pdf") == "old PDF");
    CHECK(read_file(capture.folder / "write-up.docx") == "old Word document");
    CHECK(written_up(capture.folder));
}

TEST_CASE("review rejects stale editor shape", "[test_walkthrough_library]") {
    Capture capture;
    CHECK(value_error([&] {
              save_review(capture.folder, std::vector<std::string>{"one"}, {true}, {},
                          Json::object());
          }) == "The document changed; reopen Review");
}

TEST_CASE("manifest cannot reference other files", "[test_walkthrough_library]") {
    const std::string name =
        GENERATE("../outside.png", "C:\\outside.png", "/tmp/outside.png", "walkthrough.pdf");
    Capture capture;
    const auto path = capture.folder / MANIFEST_NAME;
    auto value = read_json(path);
    value["sections"][0]["shot"] = name;
    atomic_json(path, value);

    CHECK_THROWS_AS(load_document(capture.folder), fv::py::ValueError);
}

TEST_CASE("manifest rejects invalid times", "[test_walkthrough_library]") {
    const Json bad = GENERATE(Json(std::numeric_limits<double>::infinity()), Json(-1), Json("NaN"));
    Capture capture;
    const auto path = capture.folder / MANIFEST_NAME;
    auto value = read_json(path);
    value["duration_s"] = bad;
    write_file(path, fv::py::dumps(value));

    CHECK(value_error([&] { load_document(capture.folder); }) == "Invalid walkthrough time");
}

TEST_CASE("retry uses the spoken document and current agent settings",
          "[test_walkthrough_library]") {
    Capture capture;
    Json settings = Json::object();
    settings[std::string(fv::keys::walkthrough_write_up_agent.key())] = "codex";
    settings[std::string(fv::keys::walkthrough_write_up_instructions.key())] = "Make steps";
    settings[std::string(fv::keys::walkthrough_write_up_notes.key())] = true;
    Agent agent{"# Steps", {}};
    fv::CancelSource cancel;

    const auto result =
        retry_write_up(capture.folder, std::nullopt, settings, cancel.token(), agent.runner());

    CHECK(result.written_up);
    REQUIRE(agent.calls.size() == 1);
    const auto& call = agent.calls[0];
    CHECK(call.agent == "codex");
    CHECK_FALSE(call.reading);
    // The spoken document: walkthrough.md travels in the prompt.
    CHECK(call.user_prompt.starts_with("Make steps\n"));
    CHECK(contains(call.user_prompt, "<walkthrough>\n# Walkthrough"));
    CHECK(call.images ==
          std::vector<std::filesystem::path>{realpath(capture.folder / "shot-1.png"),
                                             realpath(capture.folder / "shot-2.png")});
    CHECK(contains(call.system_prompt, "note for the agent"));
    CHECK_FALSE(contains(call.user_prompt, "Notes for you"));
    CHECK_FALSE(call.cancel.cancelled());
    cancel.cancel();
    CHECK(call.cancel.cancelled());
}

TEST_CASE("held notes are saved with their section and never in the documents",
          "[test_walkthrough_library]") {
    const auto capture = noted_capture();
    const auto& folder = capture->folder;

    const auto saved = load_document(folder);

    CHECK(section_notes(saved) ==
          std::vector<std::vector<std::string>>{{}, {"make it purple"}, {"sign off warmly"}});
    CHECK(saved.sections[2].text.empty()); // kept for its note alone
    CHECK_FALSE(contains(read_file(folder / "walkthrough.md"), "purple"));
    CHECK_FALSE(contains(load_result(folder).history_text, "purple"));
    auto value = fv::py::loads(read_file(folder / "walkthrough.json"));
    value["sections"][1]["notes"] = Json::array({7});
    write_file(folder / "walkthrough.json", fv::py::dumps(value));
    CHECK(value_error([&] { load_document(folder); }) == "Invalid walkthrough notes");
}

TEST_CASE("retry follows the notes held while recording", "[test_walkthrough_library]") {
    const auto capture = noted_capture();
    Agent agent{"# Done", {}};

    retry_write_up(capture->folder, std::nullopt, Json::object(), fv::CancelToken{},
                   agent.runner());

    REQUIRE(agent.calls.size() == 1);
    CHECK(agent.calls[0].user_prompt.ends_with("Notes for you, not part of the walkthrough:\n"
                                               "- About screenshot 2 (shot-2.png): make it purple\n"
                                               "- After the last screenshot: sign off warmly"));
}

TEST_CASE("notes go with their section through review", "[test_walkthrough_library]") {
    const auto capture = noted_capture();
    const std::vector<int> order{1, 0, 2};
    save_review(capture->folder, std::vector<std::string>{"second", "first", ""},
                {true, false, true}, order, Json::object());

    const auto saved = load_document(capture->folder);

    CHECK(section_notes(saved) ==
          std::vector<std::vector<std::string>>{{"make it purple"}, {}, {"sign off warmly"}});
    // Screenshot 2 is now the only one shown, so it is the first.
    const auto notes = held_notes(saved);
    CHECK(notes == std::vector<Note>{{"make it purple", 0}, {"sign off warmly", 1}});
    const std::vector<std::string> names{"shot-2.png"};
    const auto prompt = write_up::notes_text(notes, names);
    CHECK(contains(prompt, "About screenshot 1 (shot-2.png): make it purple"));
    CHECK(contains(prompt, "After the last screenshot: sign off warmly"));
}

TEST_CASE("retry render failure does not replace existing write up", "[test_walkthrough_library]") {
    Capture capture;
    write_file(capture.folder / "write-up.md", "old Markdown");
    write_file(capture.folder / "write-up.pdf", "old PDF");
    capture.renderer->fail(FakeRenderer::Kind::MarkdownPdf, "disk full");

    CHECK_THROWS_AS(agent_says(capture, "# New write-up"), write_up::WriteUpFailed);

    CHECK(read_file(capture.folder / "write-up.md") == "old Markdown");
    CHECK(read_file(capture.folder / "write-up.pdf") == "old PDF");
    for (const auto& entry : std::filesystem::directory_iterator(capture.folder)) {
        CHECK_FALSE(fv::path_to_utf8(entry.path().filename()).starts_with("write-up-"));
    }
}

TEST_CASE("writing up again keeps each earlier write up", "[test_walkthrough_library]") {
    Capture capture;
    const std::vector<std::filesystem::path> shots{capture.folder / "shot-1.png"};

    agent_says(capture, "# First\n\n![a](shot-1.png)", shots);
    CHECK(earlier_write_ups(capture.folder).empty());
    agent_says(capture, "# Second", shots);
    agent_says(capture, "# Third", shots);

    CHECK(earlier_write_ups(capture.folder) == std::vector<int>{1, 2});
    CHECK(read_file(capture.folder / "write-up-1.md").starts_with("# First"));
    CHECK(read_file(capture.folder / "write-up-2.md").starts_with("# Second"));
    CHECK(std::filesystem::is_regular_file(capture.folder / "write-up-1.docx"));
    CHECK(load_result(capture.folder).clipboard_text.starts_with("# Third"));
    const auto earlier = load_result(capture.folder, false, 1);
    CHECK(earlier.written_up);
    CHECK(earlier.pdf == capture.folder / "write-up-1.pdf");
    CHECK(contains(earlier.clipboard_text, "shot-1.png"));
    CHECK(load_result(capture.folder, true, 1).pdf == capture.folder / "walkthrough.pdf");
}

TEST_CASE("an earlier write up saves in place", "[test_walkthrough_library]") {
    Capture capture;
    agent_says(capture, "# First");
    agent_says(capture, "# Second");

    save_write_up(capture.folder, "# First, fixed", 1, Json::object());

    CHECK(read_file(capture.folder / "write-up-1.md").starts_with("# First, fixed"));
    CHECK(read_file(capture.folder / "write-up.md").starts_with("# Second"));
    CHECK(earlier_write_ups(capture.folder) == std::vector<int>{1});
    CHECK(value_error([&] { save_write_up(capture.folder, "# Nowhere", 7, Json::object()); }) ==
          "This walkthrough has no write-up to save");
}

TEST_CASE("review keeps every write up and deleting takes them along",
          "[test_walkthrough_library]") {
    Capture capture;
    agent_says(capture, "# First");
    agent_says(capture, "# Second");

    save_review(capture.folder, std::vector<std::string>{"new", "next", "last"}, kAll, {},
                Json::object());

    CHECK(earlier_write_ups(capture.folder) == std::vector<int>{1});
    CHECK(read_file(capture.folder / "write-up.md").starts_with("# Second"));
    agent_says(capture, "# Again");
    CHECK(earlier_write_ups(capture.folder) == std::vector<int>{1, 2});
    delete_entry(capture.folder);
    CHECK_FALSE(std::filesystem::exists(capture.folder));
}

TEST_CASE("checkpoint survives session recreation and keeps audio", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    WalkthroughSession::Options options;
    options.folder = folder;
    WalkthroughSession session(std::move(options));
    session.checkpoint_recording(std::string(32, 'a'));
    session.add_shot(red_image(20, 20), 1.25);
    session.saved_shots();
    const auto source = tmp / "recording.wav";
    write_audio(source);
    session.preserve_audio(source);
    session.end();

    const auto recovered = recovery_session(folder);

    CHECK_FALSE(recovered->open());
    const auto shots = recovered->saved_shots();
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].path.filename() == "shot-1.png");
    CHECK(shots[0].kept_at_s == 1.25);
    CHECK(read_file(folder / AUDIO_NAME) == read_file(source));
    recovered->end();
}

TEST_CASE("a draft keeps held notes so recovery leaves them out", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    const auto source = tmp / "recording.wav";
    write_audio(source);
    {
        WalkthroughSession::Options options;
        options.folder = folder;
        WalkthroughSession session(std::move(options));
        session.checkpoint_recording(std::string(32, 'a'));
        session.hold_note(0.5);
        session.release_note(1.0);
        session.hold_note(1.5);
        session.preserve_audio(source);
        session.end();
    }
    // The checkpoints queued above run on the session's writer and may still be replacing the
    // draft after preserve_audio returns (on Windows an open during the replace fails); Python
    // drains the writer before this read, and here the session's destructor does. A checkpoint
    // that runs after end() writes the held note as null too.
    const auto draft = fv::py::loads(read_file(folder / DRAFT_NAME));
    CHECK(draft["notes_s"] == fv::py::loads("[[0.5, 1.0], [1.5, null]]"));

    const auto recovered = recovery_session(folder);

    CHECK(recovered->storyboard().notes_s ==
          std::vector<std::pair<double, double>>{{0.5, 1.0},
                                                 {1.5, std::numeric_limits<double>::infinity()}});
    recovered->end();
}

TEST_CASE("a draft with notes out of order is refused", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    write_audio(folder / AUDIO_NAME);
    write_file(folder / DRAFT_NAME,
               R"({"version": 1, "started_at": "2026-10-21T14:32:05", "journal_id": ")" +
                   std::string(32, 'a') +
                   R"(", "shots": [], "notes_s": [[2.0, 3.0], [1.0, 1.5]]})");

    CHECK(value_error([&] { recovery_session(folder); }) == "Unordered note times");
}

namespace fv::walkthrough_test {

// The child the hard-exit test starts (main.cpp): a journal and a walkthrough session record,
// one screenshot is written, then the process ends without any cleanup.
int hard_exit_child(const std::filesystem::path& output, const std::filesystem::path& folder) {
    fv::RecordingJournal journal(output, 16'000, 1, 2, [](const std::string&) {});
    std::string pcm;
    for (int frame = 0; frame < 16'000; ++frame) {
        pcm += '\x10';
        pcm += '\0';
    }
    journal.append(std::as_bytes(std::span(pcm.data(), pcm.size())));
    journal.wait_until_written(std::chrono::milliseconds(10'000));
    WalkthroughSession::Options options;
    options.folder = folder;
    WalkthroughSession session(std::move(options));
    session.checkpoint_recording(fv::path_to_utf8(journal.directory().filename()));
    session.add_shot(red_image(20, 20), 0.5);
    session.saved_shots();
    std::_Exit(7);
}

} // namespace fv::walkthrough_test

namespace {

// Runs this test program as the hard-exit child and returns its exit code (-1 when it did not
// end within 15 s, after killing it).
int run_child(const std::filesystem::path& output, const std::filesystem::path& folder) {
    const auto& self = fv::walkthrough_test::g_self;
#ifdef _WIN32
    std::wstring command = L"\"" + self.wstring() + L"\" --hard-exit-child \"" + output.wstring() +
                           L"\" \"" + folder.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info)) {
        return -2;
    }
    CloseHandle(info.hThread);
    int code = -1;
    if (WaitForSingleObject(info.hProcess, 15'000) == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(info.hProcess, &exit_code);
        code = static_cast<int>(exit_code);
    } else {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 5'000);
    }
    CloseHandle(info.hProcess);
    return code;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return -2;
    }
    if (pid == 0) {
        execl(self.c_str(), self.c_str(), "--hard-exit-child", output.c_str(), folder.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    for (int waited = 0; waited < 1500; ++waited) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
#endif
}

} // namespace

TEST_CASE("hard process exit recovers audio and screenshot timing",
          "[test_walkthrough_library][platform]") {
    const TempFolder tmp;
    const auto root = tmp / "captures";
    const auto folder = root / "2026-10-21_14-32-05";
    std::filesystem::create_directories(folder);
    const auto output = tmp / "recorded_audio.wav";

    REQUIRE(run_child(output, folder) == 7);

    const auto recordings = tmp / "recordings";
    std::vector<std::filesystem::path> recovered;
    for (int attempt = 0; attempt < 20 && recovered.empty(); ++attempt) {
        recovered = fv::recover_recordings(output, recordings);
        if (recovered.empty()) {
            // Windows can release a dead process's locks asynchronously.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    REQUIRE(recovered.size() == 1);
    CHECK(attach_recovered_audio(root, recordings) == 1);
    const auto session = recovery_session(folder);
    const auto shots = session->saved_shots();
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].kept_at_s == 0.5);
    CHECK(read_wav_info(folder / AUDIO_NAME).frames == 16'000);
    session->end();
}

TEST_CASE("recovery joins journal identity and is idempotent", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto root = tmp / "captures";
    const auto folder = root / "2026-10-21_14-32-05";
    std::filesystem::create_directories(folder);
    const auto recordings = tmp / "recordings";
    std::filesystem::create_directory(recordings);
    const std::string journal(32, 'a');
    Json draft = Json::object();
    draft["version"] = 1;
    draft["started_at"] = "2026-11-16T09:30:00.123456";
    draft["journal_id"] = journal;
    draft["shots"] = Json::array();
    atomic_json(folder / DRAFT_NAME, draft);
    const auto audio = recordings / ("recording_20261021_143205-recovered-" + journal + ".wav");
    write_audio(audio);

    CHECK(attach_recovered_audio(root, recordings) == 1);
    CHECK(attach_recovered_audio(root, recordings) == 0);

    CHECK(read_file(folder / AUDIO_NAME) == read_file(audio));
    CHECK(recovery_session(folder)->saved_shots().empty());
}

TEST_CASE("draft ignores a truncated png", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    write_audio(folder / AUDIO_NAME);
    write_file(folder / "shot-1.png", "\\x89PNG truncated");
    atomic_json(folder / DRAFT_NAME,
                fv::py::loads(R"({"version": 1, "started_at": "2026-11-16T09:30:00", )"
                              R"("shots": [{"shot": "shot-1.png", "kept_at_s": 1.0}]})"));

    const auto session = recovery_session(folder);

    CHECK(session->saved_shots().empty());
    session->end();
}

TEST_CASE("draft skips a screenshot whose write was interrupted", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    write_audio(folder / AUDIO_NAME);
    atomic_json(folder / DRAFT_NAME,
                fv::py::loads(R"({"version": 1, "started_at": "2026-11-16T09:30:00", )"
                              R"("shots": [{"shot": "shot-1.png", "kept_at_s": 1.0}]})"));

    const auto session = recovery_session(folder);

    CHECK(session->saved_shots().empty());
    session->end();
}

TEST_CASE("a png cut short or with a damaged chunk is not a screenshot",
          "[test_walkthrough_library]") {
    // The core reads no images, so recovery checks the PNG's structure in place of QImage.
    const TempFolder tmp;
    const auto folder = tmp / "2026-10-21_14-32-05";
    std::filesystem::create_directory(folder);
    write_audio(folder / AUDIO_NAME);
    const auto bytes = FakeShotImage(20, 20, 0xff0000).png_bytes();
    const std::string whole(bytes.begin(), bytes.end());
    write_file(folder / "shot-1.png", whole);
    write_file(folder / "shot-2.png", whole.substr(0, whole.size() - 12)); // no IEND
    std::string damaged = whole;
    damaged[40] = static_cast<char>(damaged[40] ^ 0x01); // inside IDAT: its CRC fails
    write_file(folder / "shot-3.png", damaged);
    atomic_json(folder / DRAFT_NAME,
                fv::py::loads(R"({"version": 1, "started_at": "2026-11-16T09:30:00", "shots": [)"
                              R"({"shot": "shot-1.png", "kept_at_s": 1.0},)"
                              R"({"shot": "shot-2.png", "kept_at_s": 2.0},)"
                              R"({"shot": "shot-3.png", "kept_at_s": 3.0}]})"));

    const auto session = recovery_session(folder);

    const auto shots = session->saved_shots();
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].path.filename() == "shot-1.png");
    session->end();
}

TEST_CASE("retention defaults to forever and never prunes drafts", "[test_walkthrough_library]") {
    Capture capture;
    const auto path = capture.folder / MANIFEST_NAME;
    const auto old = std::chrono::system_clock::now() - std::chrono::hours(24 * 100);
    std::filesystem::last_write_time(path, std::chrono::clock_cast<std::chrono::file_clock>(old));

    CHECK(prune(capture.root) == 0);
    set_retention(capture.root, 30);
    write_file(capture.folder / DRAFT_NAME, "{}");
    CHECK(prune(capture.root) == 0);
    std::filesystem::remove(capture.folder / DRAFT_NAME);
    CHECK(prune(capture.root) == 1);
    CHECK_FALSE(std::filesystem::exists(capture.folder));
}

TEST_CASE("retention refuses extra files", "[test_walkthrough_library]") {
    Capture capture;
    write_file(capture.folder / "my-notes.txt", "keep");

    CHECK(value_error([&] { delete_entry(capture.folder); }) ==
          "Folder contains extra files; remove it in your file manager");
    CHECK(read_file(capture.folder / "my-notes.txt") == "keep");
}

TEST_CASE("retention rejects invalid values", "[test_walkthrough_library]") {
    // Python also refuses True and "30"; an int parameter cannot hold either.
    const int days = GENERATE(-1, 3651);
    Capture capture;
    CHECK(value_error([&] { set_retention(capture.root, days); }) ==
          "Keep captures for 0\xE2\x80\x93"
          "3650 days; 0 means forever");
    CHECK(retention_days(capture.root) == 0);
}

TEST_CASE("finished document removes recovery checkpoint", "[test_walkthrough_library]") {
    Capture capture;
    CHECK_FALSE(std::filesystem::exists(capture.folder / DRAFT_NAME));
    CHECK(entries(capture.root) == std::vector<std::filesystem::path>{capture.folder});
}

TEST_CASE("deletion and document loading refuse symbolic links",
          "[test_walkthrough_library][platform]") {
    Capture capture;
    const auto image = capture.folder / "shot-1.png";
    const auto target = capture.tmp / "outside.png";
    std::filesystem::rename(image, target);
    std::error_code error;
    std::filesystem::create_symlink(target, image, error);
    if (error) {
        SKIP("This account cannot create symbolic links: " << error.message());
    }

    CHECK_THROWS_AS(load_document(capture.folder), fv::py::ValueError);
    CHECK_THROWS_AS(delete_entry(capture.folder), fv::py::ValueError);
    CHECK(std::filesystem::exists(target));
}

TEST_CASE("metadata requires an object", "[test_walkthrough_library]") {
    const TempFolder tmp;
    const auto path = tmp / "bad.json";
    write_file(path, "[]");
    CHECK(value_error([&] { read_json(path); }) == "Walkthrough metadata must be an object");
}

TEST_CASE("startup recovery does not load the qt document renderer", "[test_walkthrough_library]") {
    // Python checks that importing the library loads neither the PDF renderer nor QtGui. Here no
    // renderer is installed at all, and recovery, attaching audio and pruning still work.
    const TempFolder tmp;
    set_document_renderer(nullptr);
    CHECK_THROWS_AS(document_renderer(), std::logic_error);
    const auto root = tmp / "captures";
    const auto draft_folder = root / "2026-10-21_14-32-05";
    std::filesystem::create_directories(draft_folder);
    const auto recordings = tmp / "recordings";
    std::filesystem::create_directory(recordings);
    const std::string journal(32, 'b');
    atomic_json(draft_folder / DRAFT_NAME,
                fv::py::loads(R"({"version": 1, "started_at": "2026-10-21T14:32:05", )"
                              R"("journal_id": ")" +
                              journal + R"(", "shots": []})"));
    write_audio(recordings / ("recording_20261021_143205-recovered-" + journal + ".wav"));
    const auto finished = root / "2026-10-20_09-00-00";
    std::filesystem::create_directory(finished);
    write_file(finished / MANIFEST_NAME, "{}");
    const auto old = std::chrono::system_clock::now() - std::chrono::hours(24 * 10);
    std::filesystem::last_write_time(finished / MANIFEST_NAME,
                                     std::chrono::clock_cast<std::chrono::file_clock>(old));
    set_retention(root, 1);

    CHECK(attach_recovered_audio(root, recordings) == 1);
    CHECK(recovery_session(draft_folder)->saved_shots().empty());
    CHECK(prune(root) == 1);
    CHECK_FALSE(std::filesystem::exists(finished));
}

// ---- tests/test_walkthrough_storyboard.py, the library ----

namespace {

constexpr std::string_view kWriteUp = "# Two slow spots\n\n"
                                      "1. Billing stalls.\n\n"
                                      "   ![Billing](shot-1.png)\n\n"
                                      "2. Invoices hang.\n\n"
                                      "![Invoices](shot-2.png)\n\n"
                                      "Both happen after sign-in.";

} // namespace

TEST_CASE("saving in a new order moves each screenshot with its words",
          "[test_walkthrough_storyboard]") {
    Capture capture;
    const std::vector<int> order{1, 0, 2};
    const std::vector<std::string> texts{"second", "first", "last"};

    save_review(capture.folder, texts, kAll, order, Json::object());

    const auto saved = load_document(capture.folder);
    CHECK(section_texts(saved) == texts);
    CHECK(shot_names(saved) == std::vector<std::string>{"shot-2.png", "shot-1.png", "None"});
    CHECK(load_result(capture.folder).history_text ==
          "second\n\n[Screenshot 1]\n\nfirst\n\n[Screenshot 2]\n\nlast");
}

TEST_CASE("an order that is not every section once is refused", "[test_walkthrough_storyboard]") {
    Capture capture;
    const std::vector<int> order{0, 0, 2};
    CHECK(value_error([&] {
              save_review(capture.folder, std::vector<std::string>{"a", "b", "c"}, kAll, order,
                          Json::object());
          }) == "The document changed; reopen Review");
}

TEST_CASE("a write up cuts at its screenshots and joins back unchanged",
          "[test_walkthrough_storyboard]") {
    const auto parts = write_up_parts(kWriteUp);
    CHECK(parts == std::vector<std::pair<std::string, std::string>>{
                       {"# Two slow spots\n\n1. Billing stalls.", "   ![Billing](shot-1.png)"},
                       {"2. Invoices hang.", "![Invoices](shot-2.png)"},
                       {"Both happen after sign-in.", ""},
                   });
    CHECK(join_write_up(parts) == kWriteUp);
}

TEST_CASE("a write up line names a shot only when it is this walkthroughs",
          "[test_walkthrough_storyboard]") {
    Capture capture;
    CHECK(write_up_shot(capture.folder, "  ![x](shot-2.png)") == capture.folder / "shot-2.png");
    CHECK_FALSE(write_up_shot(capture.folder, "![x](elsewhere.png)"));
    CHECK_FALSE(write_up_shot(capture.folder, "![x](shot-9.png)"));
}

TEST_CASE("saving an edited write up rebuilds its documents", "[test_walkthrough_storyboard]") {
    Capture capture;
    write_file(capture.folder / "write-up.md", kWriteUp);
    write_file(capture.folder / "write-up.pdf", "%PDF-1.4 old");

    const auto result =
        save_write_up(capture.folder, "# Fixed\n\nNew words.\n\n![Billing](shot-1.png)",
                      std::nullopt, Json::object());

    CHECK(result.written_up);
    CHECK(read_file(capture.folder / "write-up.md").starts_with("# Fixed"));
    CHECK(read_file(capture.folder / "write-up.pdf").starts_with("fake markdown pdf"));
    CHECK(std::filesystem::is_regular_file(capture.folder / "write-up.docx"));
    CHECK(contains(load_result(capture.folder).clipboard_text, "New words."));
}

// ---- tests/test_walkthrough_document.py ----

TEST_CASE("the saved settings become the format and paper", "[test_walkthrough_document]") {
    CHECK(document_settings(Json::object()) == std::pair{DocumentFormat{}, PaperSize::Auto});
    const auto chosen = document_settings(
        fv::py::loads(R"({"walkthrough_title": "  Notes  ", "walkthrough_show_details": false, )"
                      R"("walkthrough_show_about": false, "walkthrough_show_captions": false, )"
                      R"("walkthrough_page_size": "a4"})"));
    CHECK(chosen.first == DocumentFormat{"Notes", false, false, false});
    CHECK(chosen.second == PaperSize::A4);
    CHECK(document_settings(fv::py::loads(R"({"walkthrough_page_size": "legal"})")).second ==
          PaperSize::Auto);
}
