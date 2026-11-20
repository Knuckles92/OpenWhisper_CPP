#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "base/identity.h"
#include "support.h"
#include "walkthrough/finish.h"
#include "walkthrough/sections.h"
#include "walkthrough/testing/fake_shot_image.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

namespace {

// Decodes a clip to "<file name> <frames>", padded with spaces the caller must strip.
struct FakeTranscriber {
    std::vector<std::filesystem::path> clips;

    ClipTranscriber function() {
        return [this](const std::filesystem::path& wav) {
            clips.push_back(wav);
            return "  " + fv::path_to_utf8(wav.filename()) + " " +
                   std::to_string(read_wav_info(wav).frames) + "\n";
        };
    }
};

Storyboard board(std::vector<double> cuts, std::vector<std::vector<int>> sections,
                 std::vector<std::pair<double, double>> notes = {}) {
    Storyboard value;
    for (std::size_t index = 0; index < cuts.size(); ++index) {
        value.shots.push_back({static_cast<int>(index) + 1, {}, cuts[index]});
    }
    value.cuts_s = std::move(cuts);
    value.sections = std::move(sections);
    value.notes_s = std::move(notes);
    return value;
}

} // namespace

TEST_CASE("each section is decoded from its own clip and the notes from theirs", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    write_wav(audio, tone(6.0));
    FakeTranscriber transcriber;
    std::vector<std::pair<int, int>> progress;

    const auto [texts, notes] = transcribe_sections(
        audio, board({2.0, 4.0}, {{0}, {1}, {2}}, {{1.0, 1.5}}), transcriber.function(),
        [&](int index, int total) { progress.emplace_back(index, total); });

    CHECK(texts == std::vector<std::string>{"section-1.wav 24000", "section-2.wav 32000",
                                            "section-3.wav 32000"});
    CHECK(notes == std::vector<Note>{{"note-1.wav 8000", 0}});
    CHECK(progress == std::vector<std::pair<int, int>>{{1, 4}, {2, 4}, {3, 4}, {4, 4}});
    // The clips were written in a temporary folder of their own, which is gone.
    REQUIRE(transcriber.clips.size() == 4);
    const auto folder = transcriber.clips.front().parent_path();
    std::string prefix(fv::identity().product);
    for (char& c : prefix) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(fv::path_to_utf8(folder.filename()).starts_with(prefix + "-walkthrough-"));
    CHECK(fv::path_to_utf8(folder.filename()).size() == prefix.size() + 13 + 8);
    CHECK_FALSE(std::filesystem::exists(folder));
}

TEST_CASE("pieces a section reads are joined in board order, empty words left out", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    write_wav(audio, tone(6.0));
    int calls = 0;
    const ClipTranscriber transcribe = [&](const std::filesystem::path& wav) {
        ++calls;
        return wav.filename() == "section-2.wav" ? std::string(" \t")
                                                 : fv::path_to_utf8(wav.stem());
    };

    const auto [texts, notes] =
        transcribe_sections(audio, board({2.0, 4.0}, {{2, 0}, {1}, {}}), transcribe);

    CHECK(texts == std::vector<std::string>{"section-3 section-1", "", ""});
    CHECK(notes.empty());
    CHECK(calls == 3);
}

TEST_CASE("a clip too short to hold words is not decoded", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    write_wav(audio, tone(2.0));
    FakeTranscriber transcriber;
    std::vector<std::pair<int, int>> progress;

    const auto [texts, notes] =
        transcribe_sections(audio, board({0.0}, {{0}, {1}}, {{1.0, 1.1}}), transcriber.function(),
                            [&](int index, int total) { progress.emplace_back(index, total); });

    CHECK(texts == std::vector<std::string>{"", "section-2.wav 30400"});
    // A note that holds no words is no note.
    CHECK(notes.empty());
    CHECK(transcriber.clips.size() == 1);
    CHECK(progress == std::vector<std::pair<int, int>>{{1, 3}, {2, 3}, {3, 3}});
}

TEST_CASE("audio that cannot be cut is decoded whole into the first section", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    // 8-bit (ValueError), not a WAV (wave.Error), missing (OSError).
    const auto make = GENERATE(0, 1, 2);
    if (make == 0) {
        write_file(audio, wav_bytes(std::string(16000, '\x80'), RATE, 1, 1));
    } else if (make == 1) {
        write_file(audio, std::string("ID3\x04 an mp3 file", 16));
    }
    std::vector<std::filesystem::path> decoded;
    std::vector<std::pair<int, int>> progress;

    const auto [texts, notes] = transcribe_sections(
        audio, board({1.0, 2.0}, {{0}, {1}, {2}}, {{0.5, 0.75}}),
        [&](const std::filesystem::path& wav) {
            decoded.push_back(wav);
            return std::string(" everything ");
        },
        [&](int index, int total) { progress.emplace_back(index, total); });

    CHECK(texts == std::vector<std::string>{" everything ", "", ""});
    CHECK(notes.empty());
    CHECK(decoded == std::vector<std::filesystem::path>{audio});
    CHECK(progress == std::vector<std::pair<int, int>>{{1, 1}});
}

TEST_CASE("a WAV that ends inside its header is not taken for a whole-file decode", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    write_file(audio, "RIF");

    CHECK_THROWS_AS(transcribe_sections(audio, board({}, {{0}}),
                                        [](const std::filesystem::path&) { return std::string(); }),
                    WaveTruncated);
}

TEST_CASE("a cancel before a clip stops the decoding and cleans up", "[finish]") {
    const TempFolder tmp;
    const auto audio = tmp / "audio.wav";
    write_wav(audio, tone(4.0));
    FakeTranscriber transcriber;

    CHECK_THROWS_AS(transcribe_sections(audio, board({2.0}, {{0}, {1}}), transcriber.function(),
                                        [](int index, int) {
                                            if (index == 2) {
                                                throw std::runtime_error("canceled");
                                            }
                                        }),
                    std::runtime_error);

    REQUIRE(transcriber.clips.size() == 1);
    CHECK_FALSE(std::filesystem::exists(transcriber.clips.front().parent_path()));
}

TEST_CASE("the history text marks where each screenshot sits", "[finish]") {
    const std::vector<std::string> texts{"  First words. ", "", "\nLast words."};

    CHECK(marked_text(texts, 2) ==
          "First words.\n\n[Screenshot 1]\n\n[Screenshot 2]\n\nLast words.");
    CHECK(marked_text(texts, 0) == "First words.\n\nLast words.");
    CHECK(marked_text(std::vector<std::string>{""}, 0).empty());
}

TEST_CASE("a failed walkthrough's status says where its screenshots are", "[finish]") {
    const TempFolder tmp;
    CHECK(kept_note(nullptr).empty());

    WalkthroughSession::Options options;
    options.folder = tmp / "capture";
    WalkthroughSession session(std::move(options));
    CHECK(kept_note(&session).empty());

    session.add_shot(std::make_shared<FakeShotImage>(4, 4, 0x112233), 1.0);
    CHECK(kept_note(&session) == " — screenshots kept in " + fv::path_to_utf8(tmp / "capture"));
    session.end();
}
