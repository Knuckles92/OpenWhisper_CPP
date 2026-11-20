// from tests/test_walkthrough_session.py
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "golden/golden.h"
#include "pyport/json.h"
#include "support.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_shot_image.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using namespace std::chrono_literals;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

std::chrono::local_seconds started() {
    return std::chrono::local_days{std::chrono::year{2026} / 10 / 21} + 14h + 32min + 5s;
}

ShotImagePtr image(std::uint32_t rgb = 0xff4f3a) {
    return std::make_shared<FakeShotImage>(40, 30, rgb);
}

ShotImagePtr failing_image() {
    return std::make_shared<FakeShotImage>(40, 30, 0xff4f3a, FakeShotImage::Save::FailsHalfWritten);
}

std::unique_ptr<WalkthroughSession> session_in(const TempFolder& tmp) {
    WalkthroughSession::Options options;
    options.started_at = started();
    options.folder = tmp / "capture";
    return std::make_unique<WalkthroughSession>(std::move(options));
}

std::unique_ptr<WalkthroughSession> three_shots(const TempFolder& tmp) {
    auto session = session_in(tmp);
    for (const double kept_at : {2.0, 4.0, 6.0}) {
        session->add_shot(image(), kept_at);
    }
    return session;
}

std::vector<std::string> names(const std::vector<Shot>& shots) {
    std::vector<std::string> out;
    for (const auto& shot : shots) {
        out.push_back(fv::path_to_utf8(shot.path.filename()));
    }
    return out;
}

using Sections = std::vector<std::vector<int>>;

} // namespace

TEST_CASE("screenshots are written in the background and numbered", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);

    const auto first = session->add_shot(image(), 12.5);
    const auto second = session->add_shot(image(0x2f7cf6), 31.0);
    const auto saved = session->saved_shots();

    REQUIRE(saved.size() == 2);
    CHECK(saved[0] == Shot{1, tmp / "capture" / "shot-1.png", 12.5});
    CHECK(saved[1] == Shot{2, tmp / "capture" / "shot-2.png", 31.0});
    CHECK(std::filesystem::exists(first->path));
    CHECK(std::filesystem::exists(second->path));
    const std::string png = read_file(second->path);
    CHECK(FakeShotImage::pixel(std::vector<std::uint8_t>(png.begin(), png.end()), 5, 5) ==
          0x2f7cf6U);
    session->end();
}

TEST_CASE("the folder is made only when a screenshot needs it", "[test_walkthrough_session]") {
    const TempFolder tmp;
    WalkthroughSession::Options options;
    options.started_at = started();
    options.root = tmp / "captures";
    WalkthroughSession session(std::move(options));
    CHECK_FALSE(std::filesystem::exists(tmp / "captures"));

    session.add_shot(image(), 1.0);
    session.add_shot(image(), 2.0);

    std::vector<std::string> made;
    for (const auto& entry : std::filesystem::directory_iterator(tmp / "captures")) {
        made.push_back(fv::path_to_utf8(entry.path().filename()));
    }
    CHECK(made == std::vector<std::string>{"2026-10-21_14-32-05"});
    CHECK(session.folder() == tmp / "captures" / "2026-10-21_14-32-05");
    session.end();
}

TEST_CASE("a stopped walkthrough takes no more screenshots", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->add_shot(image(), 1.0);
    session->close();

    CHECK_FALSE(session->open());
    CHECK_FALSE(session->add_shot(image(), 2.0).has_value());
    CHECK(session->shot_count() == 1);
    session->end();
}

TEST_CASE("a screenshot that could not be written is left out", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->add_shot(failing_image(), 1.0);
    session->add_shot(image(), 2.0);

    const auto saved = session->saved_shots();

    REQUIRE(saved.size() == 1);
    CHECK(saved[0] == Shot{1, tmp / "capture" / "shot-2.png", 2.0});
    session->end();
}

TEST_CASE("canceling deletes the screenshots but nothing else", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    const auto shot = session->add_shot(image(), 1.0);
    session->saved_shots();
    const auto keep = tmp / "capture" / "notes.txt";
    write_file(keep, "mine");

    session->discard();

    CHECK_FALSE(std::filesystem::exists(shot->path));
    CHECK(std::filesystem::exists(keep));
}

TEST_CASE("canceling after the documents are written deletes them too",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->add_shot(image(), 1.0);
    session->saved_shots();
    const auto folder = tmp / "capture";
    for (const auto* name : {"walkthrough.md", "walkthrough.pdf", "walkthrough.json", "write-up.md",
                             "walkthrough.docx", "write-up.pdf", "write-up.docx", "draft.json",
                             "audio.wav", "walkthrough.original.json"}) {
        write_file(folder / name, "written");
    }

    session->discard();

    CHECK_FALSE(std::filesystem::exists(folder));
}

TEST_CASE("canceling removes the folder it made", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->add_shot(image(), 1.0);

    session->discard();

    CHECK_FALSE(std::filesystem::exists(tmp / "capture"));
}

TEST_CASE("ending keeps the files", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    const auto shot = session->add_shot(image(), 1.0);
    session->saved_shots();

    session->end();

    CHECK(std::filesystem::exists(shot->path));
    CHECK_FALSE(session->open());
}

TEST_CASE("a folder that cannot be made refuses the screenshot not the walkthrough",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    // The captures folder is a file, so no walkthrough folder can be made in it.
    write_file(tmp / "captures", "not a folder");
    WalkthroughSession::Options options;
    options.started_at = started();
    options.root = tmp / "captures";
    WalkthroughSession session(std::move(options));

    try {
        session.add_shot(image(), 1.0);
        FAIL("the screenshot was kept");
    } catch (const ScreenshotNotSaved& error) {
        CHECK(error.error_code != 0);
        CHECK_FALSE(std::string(error.what()).empty());
        CHECK(std::string(error.what()).back() != '.');
    }

    CHECK(session.open());
    CHECK(session.shot_count() == 0);
    CHECK(session.saved_shots().empty());
    session.end();
}

TEST_CASE("a screenshot that fails to write is reported and leaves no partial file",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    fv::ManualExecutor ui;
    std::vector<int> failed;
    auto session = session_in(tmp);
    const auto subscription =
        session->save_failed.connect(ui, [&](int number) { failed.push_back(number); });

    const auto shot = session->add_shot(failing_image(), 1.0);
    const auto saved = session->saved_shots();
    ui.run_all();

    CHECK(saved.empty());
    CHECK(failed == std::vector<int>{1});
    CHECK(session->lost_shots() == 1);
    CHECK_FALSE(std::filesystem::exists(shot->path));
    session->end();
}

// --- the board: moving and removing screenshots while it records ------------------------------

TEST_CASE("untouched the board reads each piece in order", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    const auto board = session->storyboard();

    CHECK(names(board.shots) == std::vector<std::string>{"shot-1.png", "shot-2.png", "shot-3.png"});
    CHECK(board.cuts_s == std::vector<double>{2.0, 4.0, 6.0});
    CHECK(board.sections == Sections{{0}, {1}, {2}, {3}});
    session->end();
}

TEST_CASE("a moved screenshot takes its words with it", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    CHECK(session->move_shot(3, -1));
    CHECK(session->move_shot(3, -1));
    const auto board = session->storyboard();

    REQUIRE(board.shots.size() == 3);
    CHECK(board.shots[0].number == 1);
    CHECK(board.shots[1].number == 2);
    CHECK(board.shots[2].number == 3);
    CHECK(names(board.shots) == std::vector<std::string>{"shot-3.png", "shot-1.png", "shot-2.png"});
    CHECK(board.sections == Sections{{2}, {0}, {1}, {3}});
    session->end();
}

TEST_CASE("the words still being spoken stay last", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    CHECK_FALSE(session->move_shot(3, 1));
    CHECK_FALSE(session->move_shot(1, -1));
    CHECK_FALSE(session->move_shot(9, 1));
    CHECK(session->storyboard().sections == Sections{{0}, {1}, {2}, {3}});
    session->end();
}

TEST_CASE("a removed screenshot leaves its words to the next section",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    CHECK(session->remove_shot(2));
    const auto board = session->storyboard();

    CHECK(names(board.shots) == std::vector<std::string>{"shot-1.png", "shot-3.png"});
    // Its words and the next section's are one stretch: no cut between them.
    CHECK(board.cuts_s == std::vector<double>{2.0, 6.0});
    CHECK(board.sections == Sections{{0}, {1}, {2}});
    CHECK_FALSE(std::filesystem::exists(tmp / "capture" / "shot-2.png"));
    CHECK(session->shot_count() == 2);
    CHECK_FALSE(session->remove_shot(2));
    session->end();
}

TEST_CASE("words that moved away from their neighbours are cut apart",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->move_shot(1, 1); // 2, 1, 3

    session->remove_shot(2); // its words open section 1's
    const auto board = session->storyboard();

    CHECK(names(board.shots) == std::vector<std::string>{"shot-1.png", "shot-3.png"});
    CHECK(board.cuts_s == std::vector<double>{2.0, 4.0, 6.0});
    CHECK(board.sections == Sections{{1, 0}, {2}, {3}});
    session->end();
}

TEST_CASE("undo takes the newest screenshot wherever it moved", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->move_shot(3, -1);

    CHECK(session->undo_last() == 3);
    CHECK(session->undo_last() == 2);
    const auto board = session->storyboard();

    CHECK(names(board.shots) == std::vector<std::string>{"shot-1.png"});
    // Their words stay in the order the board showed them: shot 3's had moved above shot 2's.
    CHECK(board.cuts_s == std::vector<double>{2.0, 4.0, 6.0});
    CHECK(board.sections == Sections{{0}, {2, 1, 3}});
    session->end();
}

TEST_CASE("undoing in the order kept rejoins the recording", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    session->undo_last();
    session->undo_last();
    const auto board = session->storyboard();

    CHECK(board.cuts_s == std::vector<double>{2.0});
    CHECK(board.sections == Sections{{0}, {1}});
    session->end();
}

TEST_CASE("a screenshot kept after an edit closes the section being spoken",
          "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->undo_last();

    session->add_shot(image(), 8.0);
    const auto board = session->storyboard();

    CHECK(names(board.shots) == std::vector<std::string>{"shot-1.png", "shot-2.png", "shot-4.png"});
    CHECK(board.cuts_s == std::vector<double>{2.0, 4.0, 8.0});
    CHECK(board.sections == Sections{{0}, {1}, {2}, {3}});
    session->end();
}

TEST_CASE("a stopped walkthrough keeps its board", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->close();

    CHECK_FALSE(session->remove_shot(1));
    CHECK_FALSE(session->move_shot(2, -1));
    CHECK_FALSE(session->undo_last().has_value());
    CHECK(session->shot_count() == 3);
    session->end();
}

TEST_CASE("recovery sees only the screenshots left on the board", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->checkpoint_recording(std::string(32, '0'));
    session->remove_shot(2);
    session->storyboard(); // waits for the writer

    const auto draft = fv::py::loads(read_file(tmp / "capture" / "draft.json"));
    std::vector<std::string> shots;
    for (const auto& item : draft.at("shots")) {
        shots.push_back(item.at("shot").get<std::string>());
    }
    CHECK(shots == std::vector<std::string>{"shot-1.png", "shot-3.png"});
    session->end();
}

TEST_CASE("the section a moment belongs to follows the board", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);
    session->move_shot(2, -1);

    const auto board = session->storyboard();

    CHECK(board.section_at(1.0) == 1); // said before shot 1, which is now second
    CHECK(board.section_at(3.0) == 0);
    CHECK(board.section_at(7.0) == 3);
    session->end();
}

// --- notes held for the agent ------------------------------------------------------------------

TEST_CASE("held stretches reach the storyboard", "[test_walkthrough_session]") {
    const TempFolder tmp;
    auto session = three_shots(tmp);

    session->hold_note(2.5);
    session->release_note(3.5);
    session->release_note(3.9); // nothing held: ignored
    session->hold_note(7.0);
    session->close();        // still held: it runs to the end
    session->hold_note(8.0); // the recording stopped

    const auto board = session->storyboard();
    CHECK(board.notes_s == std::vector<std::pair<double, double>>{{2.5, 3.5}, {7.0, kInf}});
    CHECK(board.section_at(2.5) == 1);
    session->end();
}

// --- beyond OpenWhisper's tests ------------------------------------------------------------------

TEST_CASE("four screenshots are written on the writer thread, the draft after each", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->checkpoint_recording(std::string(32, 'a'));
    for (const double kept_at : {1.0, 2.0, 3.0, 4.0}) {
        session->add_shot(image(), kept_at);
    }
    CHECK(session->move_shot(3, -1));
    CHECK(session->move_shot(3, -1));

    const auto board = session->storyboard();

    CHECK(names(board.shots) ==
          std::vector<std::string>{"shot-3.png", "shot-1.png", "shot-2.png", "shot-4.png"});
    CHECK(board.sections == Sections{{2}, {0}, {1}, {3}, {4}});
    for (int number = 1; number <= 4; ++number) {
        CHECK(
            std::filesystem::exists(tmp / "capture" / ("shot-" + std::to_string(number) + ".png")));
    }
    session.reset(); // waits for the writer
    CHECK(read_file(tmp / "capture" / "draft.json") ==
          "{\n  \"version\": 1,\n  \"started_at\": \"2026-10-21T14:32:05\",\n"
          "  \"journal_id\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n  \"shots\": [\n"
          "    {\n      \"shot\": \"shot-1.png\",\n      \"kept_at_s\": 1.0\n    },\n"
          "    {\n      \"shot\": \"shot-2.png\",\n      \"kept_at_s\": 2.0\n    },\n"
          "    {\n      \"shot\": \"shot-3.png\",\n      \"kept_at_s\": 3.0\n    },\n"
          "    {\n      \"shot\": \"shot-4.png\",\n      \"kept_at_s\": 4.0\n    }\n  ],\n"
          "  \"notes_s\": []\n}");
}

namespace {

// Saves as FakeShotImage does, noting the thread it saved on and how many screenshots draft.json
// listed just before (kNoDraft when there was none).
class WatchedImage final : public ShotImage {
public:
    static constexpr std::size_t kNoDraft = static_cast<std::size_t>(-1);

    struct Log {
        std::mutex mutex;
        std::vector<std::thread::id> threads;
        std::vector<std::size_t> draft_shots;
    };

    WatchedImage(std::filesystem::path draft, std::shared_ptr<Log> log)
        : draft_(std::move(draft)), log_(std::move(log)) {}

    int width() const override { return inner_.width(); }
    int height() const override { return inner_.height(); }
    double device_pixel_ratio() const override { return inner_.device_pixel_ratio(); }
    std::vector<std::uint8_t> png_bytes() const override { return inner_.png_bytes(); }
    void save_png(const std::filesystem::path& path) const override {
        const auto shots = std::filesystem::exists(draft_)
                               ? fv::py::loads(read_file(draft_)).at("shots").size()
                               : kNoDraft;
        {
            std::lock_guard lock(log_->mutex);
            log_->threads.push_back(std::this_thread::get_id());
            log_->draft_shots.push_back(shots);
        }
        inner_.save_png(path);
    }

private:
    FakeShotImage inner_{8, 8, 0x336699};
    std::filesystem::path draft_;
    std::shared_ptr<Log> log_;
};

} // namespace

TEST_CASE("screenshots are written on the session's own thread and the draft after each",
          "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    const auto log = std::make_shared<WatchedImage::Log>();
    const auto draft = tmp / "capture" / "draft.json";

    for (const double kept_at : {1.0, 2.0, 3.0, 4.0}) {
        session->add_shot(std::make_shared<WatchedImage>(draft, log), kept_at);
        session->storyboard(); // this one is written, and its checkpoint with it
    }

    // No draft before the first write; after each write one that lists it.
    CHECK(log->draft_shots == std::vector<std::size_t>{WatchedImage::kNoDraft, 1, 2, 3});
    REQUIRE(log->threads.size() == 4);
    CHECK(log->threads[0] != std::this_thread::get_id());
    CHECK(std::ranges::all_of(log->threads,
                              [&](const std::thread::id id) { return id == log->threads[0]; }));
    CHECK(fv::py::loads(read_file(draft)).at("shots").size() == 4);
    session->end();
}

TEST_CASE("a session needs its folder or the captures folder", "[session]") {
    WalkthroughSession::Options options;
    options.started_at = started();
    CHECK_THROWS_AS(WalkthroughSession(std::move(options)), std::invalid_argument);
}

TEST_CASE("a recovered draft reads as the walkthrough it was", "[session]") {
    const TempFolder tmp;
    const auto folder = tmp / "capture";
    std::filesystem::create_directory(folder);
    write_file(folder / "shot-1.png", "png");
    write_file(folder / "shot-3.png", "png");

    const auto session =
        WalkthroughSession::recovered({started(),
                                       folder,
                                       {{folder / "shot-1.png", 2.0}, {folder / "shot-3.png", 6.0}},
                                       {{3.0, 4.0}, {7.0, kInf}}});

    CHECK_FALSE(session->open());
    CHECK(session->started_at() == started());
    CHECK(session->folder() == folder);
    CHECK(session->shot_count() == 2);
    CHECK(session->lost_shots() == 0);
    CHECK_FALSE(session->add_shot(image(), 9.0).has_value());
    const auto board = session->storyboard(0ms);
    CHECK(board.shots ==
          std::vector<Shot>{{1, folder / "shot-1.png", 2.0}, {2, folder / "shot-3.png", 6.0}});
    CHECK(board.cuts_s == std::vector<double>{2.0, 6.0});
    CHECK(board.sections == Sections{{0}, {1}, {2}});
    CHECK(board.notes_s == std::vector<std::pair<double, double>>{{3.0, 4.0}, {7.0, kInf}});
    CHECK(board.section_at(3.0) == 1);

    session->discard();
    CHECK_FALSE(std::filesystem::exists(folder));
}

TEST_CASE("a recovered draft never creates its folder", "[session]") {
    const TempFolder tmp;
    const auto session = WalkthroughSession::recovered({started(), tmp / "gone", {}, {}});

    CHECK(session->folder() == tmp / "gone");
    CHECK(session->storyboard(0ms).shots.empty());
    CHECK_FALSE(std::filesystem::exists(tmp / "gone"));
}

TEST_CASE("canceling keeps a folder that holds an earlier write-up", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    const auto shot = session->add_shot(image(), 1.0);
    session->saved_shots();
    write_file(tmp / "capture" / "write-up-1.md", "earlier");
    write_file(tmp / "capture" / "walkthrough.md", "written");

    session->discard();

    CHECK_FALSE(std::filesystem::exists(shot->path));
    CHECK_FALSE(std::filesystem::exists(tmp / "capture" / "walkthrough.md"));
    CHECK(std::filesystem::exists(tmp / "capture" / "write-up-1.md"));
}

TEST_CASE("canceling before any screenshot touches nothing", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    session->hold_note(1.0);

    session->discard();

    CHECK_FALSE(std::filesystem::exists(tmp / "capture"));
    CHECK_FALSE(session->open());
}

TEST_CASE("kept times and note times never go below zero", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);

    const auto shot = session->add_shot(image(), -3.0);
    session->hold_note(-1.0);
    session->release_note(0.5);
    session->hold_note(2.0);
    session->release_note(2.0); // no length: dropped

    REQUIRE(shot.has_value());
    CHECK(shot->kept_at_s == 0.0);
    CHECK(session->storyboard().notes_s == std::vector<std::pair<double, double>>{{0.0, 0.5}});
    session->end();
}

TEST_CASE("the recording is preserved beside the screenshots", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    write_wav(tmp / "recorded_audio.wav", tone(0.5));

    session->preserve_audio(tmp / "recorded_audio.wav");

    CHECK(read_file(tmp / "capture" / "audio.wav") == read_file(tmp / "recorded_audio.wav"));
    CHECK_FALSE(std::filesystem::exists(tmp / "capture" / ".audio.tmp"));
    CHECK(std::filesystem::exists(tmp / "capture" / "draft.json"));

    // Preserving the preserved file again copies nothing.
    session->preserve_audio(tmp / "capture" / "audio.wav");
    CHECK(read_file(tmp / "capture" / "audio.wav") == read_file(tmp / "recorded_audio.wav"));
    CHECK_THROWS_AS(session->preserve_audio(tmp / "missing.wav"),
                    std::filesystem::filesystem_error);
    CHECK_FALSE(std::filesystem::exists(tmp / "capture" / ".audio.tmp"));
    session->end();
}

TEST_CASE("the job worker's results are carried for the UI thread", "[session]") {
    const TempFolder tmp;
    auto session = session_in(tmp);
    CHECK_FALSE(session->result().has_value());
    CHECK(session->document_error().empty());
    CHECK(session->started_at() == started());

    WalkthroughResult result;
    result.folder = tmp / "capture";
    result.shots = 2;
    session->set_result(result);
    session->set_write_up_input({{tmp / "capture" / "shot-1.png"}, {{"make it purple", 1}}});
    session->set_document_error("The PDF could not be written");

    CHECK(session->result() == result);
    CHECK(session->write_up_input().notes == std::vector<Note>{{"make it purple", 1}});
    CHECK(session->write_up_input().shots ==
          std::vector<std::filesystem::path>{tmp / "capture" / "shot-1.png"});
    CHECK(session->document_error() == "The PDF could not be written");
}

TEST_CASE("a session started without a time takes the clock's local time", "[session]") {
    const TempFolder tmp;
    // 2026-07-04 19:20:30 UTC is 12:20:30 in Los Angeles (PDT).
    fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 7 / 4} + 19h + 20min + 30s +
                            450ms,
                        "America/Los_Angeles");
    WalkthroughSession::Options options;
    options.root = tmp / "captures";
    options.clock = &clock;
    WalkthroughSession session(std::move(options));

    CHECK(session.started_at() ==
          std::chrono::local_days{std::chrono::year{2026} / 7 / 4} + 12h + 20min + 30s);
    CHECK(session.folder().filename() == "2026-07-04_12-20-30");
}

TEST_CASE("what a copy puts on the clipboard follows the format", "[session]") {
    const TempFolder tmp;
    WalkthroughResult result;
    result.folder = tmp / "capture";
    result.pdf = tmp / "capture" / "walkthrough.pdf";
    result.markdown = tmp / "capture" / "walkthrough.md";
    result.clipboard_text = "# Walkthrough\n";

    CHECK(result.name() == "Walkthrough");
    result.written_up = true;
    CHECK(result.name() == "Write-up");
    CHECK(result.docx() == tmp / "capture" / "walkthrough.docx");

    using Copy = std::pair<std::vector<std::filesystem::path>, std::string>;
    CHECK(result.clipboard("pdf") == Copy{{result.pdf}, ""});
    CHECK(result.clipboard("both") == Copy{{result.pdf}, "# Walkthrough\n"});
    CHECK(result.clipboard("word") == Copy{{result.docx()}, ""});
    CHECK(result.clipboard("agent") == Copy{{}, "# Walkthrough\n"});
    CHECK(result.clipboard("something else") == Copy{{}, ""});
    const auto [files, prompt] = result.clipboard("agent_task");
    CHECK(files.empty());
    CHECK(prompt ==
          "Read the attached walkthrough PDF, including its screenshots, and carry out the tasks "
          "and changes I describe in it. Use the words and screenshots together to understand the "
          "intended result. Implement the requested work rather than only summarizing the "
          "document. Ask for clarification if a required detail is missing; otherwise proceed, "
          "verify the result, and report what you changed and anything still unresolved.\n\n"
          "PDF file (if the attachment is unavailable): " +
              fv::path_to_utf8(std::filesystem::weakly_canonical(result.pdf)));
}

TEST_CASE("a moment no section reads is refused", "[session]") {
    Storyboard board;
    board.cuts_s = {1.0};
    board.sections = {{0}};
    CHECK(board.section_at(0.5) == 0);
    CHECK_THROWS_AS(board.section_at(2.0), std::out_of_range);
}

// --- golden vectors ---------------------------------------------------------------------------

TEST_CASE("golden: _storyboard", "[golden][session]") {
    fv::golden::for_each(
        "services.walkthrough.session", "_storyboard", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            const auto folder = tmp / "capture";
            auto session = session_in(tmp);
            fv::Json results = fv::Json::array();
            for (const auto& op : c.args()[0]) {
                const std::string name = op.at(0).get<std::string>();
                if (name == "add" || name == "add_failing") {
                    const auto shot = session->add_shot(name == "add" ? image() : failing_image(),
                                                        fv::golden::decode<double>(op.at(1)));
                    results.push_back(shot ? fv::Json(shot->number) : fv::Json(nullptr));
                } else if (name == "remove") {
                    results.push_back(session->remove_shot(op.at(1).get<int>()));
                } else if (name == "undo") {
                    const auto number = session->undo_last();
                    results.push_back(number ? fv::Json(*number) : fv::Json(nullptr));
                } else if (name == "move") {
                    results.push_back(session->move_shot(op.at(1).get<int>(), op.at(2).get<int>()));
                } else if (name == "hold") {
                    session->hold_note(fv::golden::decode<double>(op.at(1)));
                    results.push_back(nullptr);
                } else if (name == "release") {
                    session->release_note(fv::golden::decode<double>(op.at(1)));
                    results.push_back(nullptr);
                } else if (name == "close") {
                    session->close();
                    results.push_back(nullptr);
                } else if (name == "checkpoint") {
                    session->checkpoint_recording(op.at(1).get<std::string>());
                    results.push_back(nullptr);
                } else if (name == "count") {
                    results.push_back(session->shot_count());
                } else {
                    FAIL("unknown op " << name);
                }
            }
            const auto board = session->storyboard();
            fv::Json actual = fv::Json::object();
            actual["ops"] = results;
            fv::Json shots = fv::Json::array();
            for (const auto& shot : board.shots) {
                shots.push_back(
                    {shot.number, fv::path_to_utf8(shot.path.filename()), shot.kept_at_s});
            }
            actual["shots"] = shots;
            actual["cuts_s"] = board.cuts_s;
            actual["sections"] = board.sections;
            fv::Json notes = fv::Json::array();
            for (const auto& [start, end] : board.notes_s) {
                notes.push_back({start, end == kInf ? fv::Json{{"$float", "inf"}} : fv::Json(end)});
            }
            actual["notes_s"] = notes;
            fv::Json probes = fv::Json::array();
            for (const auto& probe : c.args()[1]) {
                probes.push_back(board.section_at(fv::golden::decode<double>(probe)));
            }
            actual["section_at"] = probes;
            actual["shot_count"] = session->shot_count();
            actual["lost_shots"] = session->lost_shots();
            session.reset(); // drains the writer
            std::vector<std::string> files;
            if (std::filesystem::exists(folder)) {
                for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                    files.push_back(fv::path_to_utf8(entry.path().filename()));
                }
            }
            std::ranges::sort(files);
            actual["files"] = files;
            actual["draft"] = std::filesystem::exists(folder / "draft.json")
                                  ? fv::Json(read_file(folder / "draft.json"))
                                  : fv::Json(nullptr);
            INFO(actual.dump());
            CHECK(fv::golden::equal(c.raw().at("result"), actual));
        });
}
