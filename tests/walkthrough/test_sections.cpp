// from tests/test_walkthrough_sections.py
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>

#include "golden/golden.h"
#include "support.h"
#include "walkthrough/sections.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using Catch::Approx;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

Pcm16 mono(std::vector<std::int16_t> samples) {
    return {std::move(samples), RATE, 1};
}

std::vector<double> marks(std::initializer_list<double> values) {
    return values;
}

} // namespace

TEST_CASE("a cut moves into the pause near its mark", "[test_walkthrough_sections]") {
    const auto audio = mono(concat({tone(2.0), quiet(0.3), tone(2.0)}));

    const auto cuts = cut_frames(audio, marks({1.8}));

    REQUIRE(cuts.size() == 1);
    CHECK(2.0 * RATE <= cuts[0]);
    CHECK(cuts[0] <= 2.3 * RATE);
}

TEST_CASE("without a pause the cut stays at the mark", "[test_walkthrough_sections]") {
    const auto audio = mono(tone(4.0));

    CHECK(cut_frames(audio, marks({1.5})) == std::vector<std::int64_t>{24000});
}

TEST_CASE("a softer stretch that is not a pause does not move the cut",
          "[test_walkthrough_sections]") {
    const auto audio = mono(concat({tone(1.4), tone(0.2, 0.8), tone(1.4)}));

    CHECK(cut_frames(audio, marks({1.2})) == std::vector<std::int64_t>{19200});
}

TEST_CASE("a pause out of reach is ignored", "[test_walkthrough_sections]") {
    const auto audio = mono(concat({tone(3.0), quiet(0.3), tone(1.0)}));

    const auto cuts = cut_frames(audio, marks({3.0 - REACH_S - 0.5}));

    CHECK(cuts.at(0) < 3.0 * RATE - 0.4 * RATE);
}

TEST_CASE("cuts never pass the next mark or go back", "[test_walkthrough_sections]") {
    // The only pause is after both marks: neither cut may reorder them.
    const auto audio = mono(concat({tone(1.4), quiet(0.3), tone(2.0)}));

    const auto cuts = cut_frames(audio, marks({1.0, 1.1}));

    REQUIRE(cuts.size() == 2);
    CHECK(cuts[0] <= 1.1 * RATE);
    CHECK(cuts[0] <= cuts[1]);
}

TEST_CASE("marks past the end cut at the end", "[test_walkthrough_sections]") {
    const auto audio = mono(tone(1.0));

    CHECK(cut_frames(audio, marks({5.0, 6.0})) == std::vector<std::int64_t>{RATE, RATE});
}

TEST_CASE("split writes one clip per section", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto audio = concat({tone(1.5), quiet(0.2), tone(1.5), quiet(0.2), tone(1.5)});
    const auto source = tmp / "recording.wav";
    write_wav(source, audio);

    const auto clips = split_recording(source, marks({1.6, 3.3}), tmp.path());

    REQUIRE(clips.size() == 3);
    double total = 0;
    for (const auto& clip : clips) {
        REQUIRE(clip.path.has_value());
        CHECK(std::filesystem::exists(*clip.path));
        total += clip.duration_s;
    }
    CHECK(total == Approx(static_cast<double>(audio.size()) / RATE));
    const auto middle = read_wav_info(*clips[1].path);
    CHECK(middle.rate == RATE);
    CHECK(middle.channels == 1);
    CHECK(middle.width == 2);
    CHECK(static_cast<double>(middle.frames) / RATE == Approx(clips[1].duration_s));
}

TEST_CASE("a section too short to hold words is not written", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "recording.wav";
    write_wav(source, tone(2.0));

    const auto clips = split_recording(source, marks({2.0 - MIN_CLIP_S / 2}), tmp.path());

    REQUIRE(clips.size() == 2);
    CHECK(clips[0].path.has_value());
    CHECK_FALSE(clips[1].path.has_value());
    CHECK(clips[1].duration_s < MIN_CLIP_S);
}

TEST_CASE("a screenshot kept before any words leaves an empty first section",
          "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "recording.wav";
    write_wav(source, tone(2.0));

    const auto clips = split_recording(source, marks({0.0}), tmp.path());

    CHECK_FALSE(clips.at(0).path.has_value());
    CHECK(clips.at(1).duration_s == Approx(2.0));
}

TEST_CASE("stereo recordings are cut on whole frames", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto mono_audio = concat({tone(1.0), quiet(0.3), tone(1.0)});
    std::vector<std::int16_t> stereo;
    for (const auto sample : mono_audio) {
        stereo.push_back(sample);
        stereo.push_back(sample);
    }
    const auto source = tmp / "stereo.wav";
    write_wav(source, stereo, 2);

    const auto clips = split_recording(source, marks({0.9}), tmp.path());

    CHECK(1.0 <= clips.at(0).duration_s);
    CHECK(clips.at(0).duration_s <= 1.3);
    CHECK(read_wav_info(*clips.at(0).path).channels == 2);
}

TEST_CASE("only 16 bit audio is cut", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "eight-bit.wav";
    write_file(source, wav_bytes(std::string(RATE, '\x80'), RATE, 1, 1));

    CHECK_THROWS_AS(split_recording(source, marks({0.5}), tmp.path()), std::invalid_argument);
    try {
        read_pcm16_wav(source);
        FAIL("read an 8-bit WAV");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "expected 16-bit audio, got 8-bit");
    }
}

// --- stretches held for a note ----------------------------------------------------------------

TEST_CASE("a held stretch is left out of its section and cut on its own",
          "[test_walkthrough_sections]") {
    const TempFolder tmp;
    // Speech, a note (louder), more speech; a screenshot kept at 4.0 s.
    const auto audio = concat({tone(1.0), tone(1.0, 2.0), tone(2.0), tone(2.0)});
    const auto source = tmp / "recording.wav";
    write_wav(source, audio);
    const std::vector<std::pair<double, double>> held_s{{1.0, 2.0}};

    const auto sections = split_recording(source, marks({4.0}), tmp.path(), held_s);
    const auto held = clip_stretches(source, held_s, tmp.path());

    REQUIRE(sections.size() == 2);
    REQUIRE(held.size() == 1);
    CHECK(sections[0].duration_s == Approx(3.0).margin(0.01));
    CHECK(max_abs(read_wav_info(*sections[0].path).samples) <= 8000);
    CHECK(sections[1].duration_s == Approx(2.0).margin(0.01));
    CHECK(held[0].path->filename() == "note-1.wav");
    CHECK(read_wav_info(*held[0].path).frames == RATE);
    CHECK(max_abs(read_wav_info(*held[0].path).samples) > 8000);
}

TEST_CASE("a stretch held across a screenshot leaves both sections",
          "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "recording.wav";
    write_wav(source, tone(6.0));
    const std::vector<std::pair<double, double>> held_s{{2.0, 4.0}};

    const auto clips = split_recording(source, marks({3.0}), tmp.path(), held_s);

    REQUIRE(clips.size() == 2);
    CHECK(clips[0].duration_s == Approx(2.0).margin(0.01));
    CHECK(clips[1].duration_s == Approx(2.0).margin(0.01));
}

TEST_CASE("a stretch held to the end runs to the last frame", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "recording.wav";
    write_wav(source, tone(3.0));
    const std::vector<std::pair<double, double>> held_s{{2.0, kInf}};

    const auto held = clip_stretches(source, held_s, tmp.path());
    const auto spoken = split_recording(source, {}, tmp.path(), held_s);

    REQUIRE(held.size() == 1);
    REQUIRE(spoken.size() == 1);
    CHECK(read_wav_info(*held[0].path).frames == RATE);
    CHECK(spoken[0].duration_s == Approx(2.0).margin(0.01));
}

TEST_CASE("a moment held too short to hold words is no clip", "[test_walkthrough_sections]") {
    const TempFolder tmp;
    const auto source = tmp / "recording.wav";
    write_wav(source, tone(2.0));
    const std::vector<std::pair<double, double>> held_s{{1.0, 1.0 + MIN_CLIP_S / 2}};

    const auto held = clip_stretches(source, held_s, tmp.path());

    REQUIRE(held.size() == 1);
    CHECK_FALSE(held[0].path.has_value());
}

// --- reading WAVs as Python's wave module does --------------------------------------------------

TEST_CASE("a file that is not a RIFF WAVE is a wave error", "[sections]") {
    const TempFolder tmp;
    const auto source = tmp / "not.wav";
    write_file(source, std::string("RIFX\x10\0\0\0WAVEfmt ", 16));
    CHECK_THROWS_AS(read_pcm16_wav(source), WaveError);
    write_file(source, std::string("RIFF\x04\0\0\0WAVX", 12));
    CHECK_THROWS_AS(read_pcm16_wav(source), WaveError);
    write_file(source, "RI");
    CHECK_THROWS_AS(read_pcm16_wav(source), WaveTruncated);
}

TEST_CASE("an unknown format is named, and a missing data chunk is a wave error", "[sections]") {
    const TempFolder tmp;
    const auto source = tmp / "float.wav";
    std::string bytes = wav_bytes(std::string(8, '\0'), RATE, 1, 4);
    bytes[20] = 3; // WAVE_FORMAT_IEEE_FLOAT
    write_file(source, bytes);
    try {
        read_pcm16_wav(source);
        FAIL("read a float WAV");
    } catch (const WaveError& error) {
        CHECK(std::string(error.what()) == "unknown format: 3");
    }
    write_file(source, wav_bytes("", RATE, 1, 2).substr(0, 36));
    CHECK_THROWS_AS(read_pcm16_wav(source), WaveError);
}

TEST_CASE("24 bit audio names its width", "[sections]") {
    const TempFolder tmp;
    const auto source = tmp / "deep.wav";
    write_file(source, wav_bytes(std::string(30, '\0'), RATE, 1, 3));
    try {
        read_pcm16_wav(source);
        FAIL("read a 24-bit WAV");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "expected 16-bit audio, got 24-bit");
    }
}

TEST_CASE("a short data chunk is read as far as it goes, in whole frames", "[sections]") {
    const TempFolder tmp;
    const auto source = tmp / "short.wav";
    std::string bytes = wav_bytes(pcm_bytes({1, 2, 3, 4, 5}), RATE, 2, 2);
    // The header claims 1000 bytes of data; the file holds ten, which is two frames and a half.
    bytes[40] = static_cast<char>(0xE8);
    bytes[41] = 0x03;
    write_file(source, bytes);

    const auto audio = read_pcm16_wav(source);

    CHECK(audio.channels == 2);
    CHECK(audio.samples == std::vector<std::int16_t>{1, 2, 3, 4});
}

TEST_CASE("an extensible PCM WAV and chunks before the data are read", "[sections]") {
    const TempFolder tmp;
    const auto source = tmp / "extensible.wav";
    std::string fmt;
    const auto put = [&](std::string& out, std::uint32_t value, int bytes) {
        for (int index = 0; index < bytes; ++index) {
            out.push_back(static_cast<char>(value >> (8 * index)));
        }
    };
    put(fmt, 0xFFFE, 2);
    put(fmt, 1, 2);
    put(fmt, RATE, 4);
    put(fmt, RATE * 2, 4);
    put(fmt, 2, 2);
    put(fmt, 16, 2);
    put(fmt, 22, 2);
    put(fmt, 16, 2);
    put(fmt, 4, 4);
    fmt += std::string("\x01\x00\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71", 16);
    const std::string data = pcm_bytes({7, -7, 9});
    std::string body = "WAVE";
    body += "LIST";
    put(body, 3, 4);
    body += std::string("abc\0", 4); // an odd chunk and its pad byte
    body += "fmt ";
    put(body, static_cast<std::uint32_t>(fmt.size()), 4);
    body += fmt;
    body += "data";
    put(body, static_cast<std::uint32_t>(data.size()), 4);
    body += data;
    std::string file = "RIFF";
    put(file, static_cast<std::uint32_t>(body.size()), 4);
    write_file(source, file + body);

    const auto audio = read_pcm16_wav(source);

    CHECK(audio.rate == RATE);
    CHECK(audio.samples == std::vector<std::int16_t>{7, -7, 9});
}

// --- golden vectors ---------------------------------------------------------------------------

namespace {

std::vector<std::pair<double, double>> stretches(const fv::Json& value) {
    std::vector<std::pair<double, double>> out;
    for (const auto& item : value) {
        out.emplace_back(fv::golden::decode<double>(item.at(0)),
                         fv::golden::decode<double>(item.at(1)));
    }
    return out;
}

fv::Json clips_json(const std::vector<Clip>& clips) {
    fv::Json out = fv::Json::array();
    for (const auto& clip : clips) {
        if (!clip.path) {
            out.push_back({nullptr, clip.duration_s, nullptr, nullptr});
            continue;
        }
        const auto info = read_wav_info(*clip.path);
        out.push_back(
            {fv::path_to_utf8(clip.path->filename()), clip.duration_s, info.frames, info.channels});
    }
    return out;
}

Pcm16 pcm_of(const Signal& signal) {
    return {signal.interleaved, signal.rate, signal.channels};
}

} // namespace

TEST_CASE("golden: cut_frames", "[golden][sections]") {
    fv::golden::for_each(
        "services.walkthrough.sections", "cut_frames", [](const fv::golden::Case& c) {
            const auto audio = pcm_of(signal_from(c.args()[0]));
            const auto marks_s = c.arg<std::vector<double>>(1);
            CHECK(cut_frames(audio, marks_s) == c.result<std::vector<std::int64_t>>());
        });
}

TEST_CASE("golden: _quietest", "[golden][sections]") {
    fv::golden::for_each("services.walkthrough.sections", "_quietest",
                         [](const fv::golden::Case& c) {
                             const auto audio = pcm_of(signal_from(c.args()[0]));
                             CHECK(quietest(audio, c.arg<std::int64_t>(1), c.arg<std::int64_t>(2),
                                            c.arg<std::int64_t>(3)) == c.result<std::int64_t>());
                         });
}

TEST_CASE("golden: _without", "[golden][sections]") {
    fv::golden::for_each(
        "services.walkthrough.sections", "_without", [](const fv::golden::Case& c) {
            std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
            for (const auto& gap : c.args()[2]) {
                gaps.emplace_back(gap.at(0).get<std::int64_t>(), gap.at(1).get<std::int64_t>());
            }
            fv::Json spans = fv::Json::array();
            for (const auto& [start, end] :
                 without(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1), gaps)) {
                spans.push_back({start, end});
            }
            CHECK(fv::golden::equal(c.raw().at("result"), spans));
        });
}

TEST_CASE("golden: split_recording", "[golden][sections]") {
    fv::golden::for_each(
        "services.walkthrough.sections", "split_recording", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            const auto signal = signal_from(c.args()[0]);
            const auto source = tmp / "recording.wav";
            write_wav(source, signal.interleaved, signal.channels, signal.rate);
            std::filesystem::create_directory(tmp / "clips");
            const auto clips = split_recording(source, c.arg<std::vector<double>>(1), tmp / "clips",
                                               stretches(c.args()[2]));
            const auto actual = clips_json(clips);
            INFO(actual.dump());
            CHECK(fv::golden::equal(c.raw().at("result"), actual));
        });
}

TEST_CASE("golden: clip_stretches", "[golden][sections]") {
    fv::golden::for_each(
        "services.walkthrough.sections", "clip_stretches", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            const auto signal = signal_from(c.args()[0]);
            const auto source = tmp / "recording.wav";
            write_wav(source, signal.interleaved, signal.channels, signal.rate);
            std::filesystem::create_directory(tmp / "clips");
            const auto clips = clip_stretches(source, stretches(c.args()[1]), tmp / "clips");
            const auto actual = clips_json(clips);
            INFO(actual.dump());
            CHECK(fv::golden::equal(c.raw().at("result"), actual));
        });
}
