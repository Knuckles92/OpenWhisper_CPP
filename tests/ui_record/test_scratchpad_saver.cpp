// The Scratchpad's saver: the newest text is written off the GUI thread, an older text never
// lands over a newer one, and a failed write changes nothing (ui_qt/widgets/scratchpad.py
// _Saver and _write_atomically).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "base/executor.h"
#include "base/testing/manual_executor.h"
#include "testkit/testkit.h"
#include "ui_record/scratchpad_saver.h"

namespace fs = std::filesystem;

namespace {

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int temporaries(const fs::path& folder) {
    int count = 0;
    for (const auto& entry : fs::directory_iterator(folder)) {
        count += entry.path().extension() == ".tmp" ? 1 : 0;
    }
    return count;
}

} // namespace

// from
// tests/test_personalize_s8_scratchpad.py::TestSaving::test_an_older_save_never_lands_over_a_newer_one
TEST_CASE("an older text never lands over a newer one") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path target = root.path() / "notes.txt";
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    saver.writeNow("newer");
    saver.write(1, "older");
    CHECK(read(target) == "newer");
    // The text that was written holds its number too: it is not written twice.
    saver.write(1, "same number");
    CHECK(read(target) == "newer");
    saver.write(2, "newest");
    CHECK(read(target) == "newest");
}

TEST_CASE("a burst of edits costs one write of the last text") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path target = root.path() / "notes.txt";
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    CHECK(saver.idle());
    saver.submit("a");
    saver.submit("ab");
    saver.submit("abc");
    CHECK_FALSE(saver.idle());
    CHECK(strand.queued() == 1);
    CHECK_FALSE(fs::exists(target));
    strand.run_all();
    CHECK(read(target) == "abc");
    CHECK(saver.idle());
}

TEST_CASE("writing now drops the text still waiting for the strand") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path target = root.path() / "notes.txt";
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    saver.submit("old");
    saver.writeNow("new");
    strand.run_all();
    CHECK(read(target) == "new");
    CHECK(saver.idle());
}

TEST_CASE("a submit after a write goes out as the next text") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path target = root.path() / "notes.txt";
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    saver.writeNow("one");
    saver.submit("two");
    strand.run_all();
    CHECK(read(target) == "two");
    saver.submit("three");
    CHECK_FALSE(saver.idle());
    strand.run_all();
    CHECK(read(target) == "three");
}

TEST_CASE("the folder is made, the bytes are the text's and no temporary stays") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path folder = root.path() / "not" / "yet";
    const fs::path target = folder / "notes.txt";
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    saver.writeNow("Line one \xE2\x9C\x93\nLine two");
    CHECK(read(target) == "Line one \xE2\x9C\x93\nLine two");
    CHECK(temporaries(folder) == 0);
}

TEST_CASE("a failed write is dropped and the next one lands") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    // A file where the folder should be: the write cannot happen.
    const fs::path blocker = root.path() / "blocker";
    std::ofstream(blocker) << "not a folder";
    fs::path target = blocker / "notes.txt";
    fv::ScratchpadSaver saver(strand, [&target] { return target; });
    saver.writeNow("lost");
    saver.submit("lost too");
    strand.run_all();
    CHECK(saver.idle());
    CHECK(read(blocker) == "not a folder");

    target = root.path() / "notes.txt";
    saver.writeNow("kept");
    CHECK(read(target) == "kept");
}

TEST_CASE("a text queued when the saver goes is still written") {
    fv::testkit::TempDataRoot root;
    fv::ManualExecutor strand;
    const fs::path target = root.path() / "notes.txt";
    {
        fv::ScratchpadSaver saver(strand, [target] { return target; });
        saver.submit("queued");
    }
    strand.run_all();
    CHECK(read(target) == "queued");
}

TEST_CASE("many edits on a real strand end with the last text") {
    fv::testkit::TempDataRoot root;
    const fs::path target = root.path() / "notes.txt";
    fv::ThreadPool pool("scratchpad-test", 2);
    fv::Strand strand(pool, "scratchpad", 1, fv::ShutdownMode::Drain);
    fv::ScratchpadSaver saver(strand, [target] { return target; });
    std::string last;
    for (int i = 0; i < 200; ++i) {
        last = "text " + std::to_string(i);
        saver.submit(last);
        if (i % 50 == 25) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    for (int wait = 0; wait < 500 && !saver.idle(); ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(saver.idle());
    CHECK(read(target) == last);
    CHECK(temporaries(root.path()) == 0);
    strand.shutdown(std::chrono::seconds(2));
}
