// Meeting playback (meeting/audio_playback.py): the mixed WAV, its cache and its refusals. From
// tests/test_meeting_data_lifecycle.py.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr_test_support.h"
#include "base/utf8.h"
#include "capture_test_support.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/playback.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/errors.h"

using fv::Json;
using fv::meeting::build_playback;
using fv::meeting::PLAYBACK_RATE;
using fv::meeting::test::TempDir;
using fv::meeting::test::write_wav;

namespace {

// LifecycleRepository: one meeting and its chunk rows.
struct Lifecycle {
    explicit Lifecycle(const std::filesystem::path& spool) {
        Json meeting = Json::object();
        meeting["id"] = "m_audio";
        meeting["title"] = "Audio";
        meeting["status"] = "ended";
        meeting["started_at"] = "2026-01-01T00:00:00";
        meeting["host_token"] = "host";
        meeting["guest_token"] = "guest";
        meeting["spool_dir"] = fv::path_to_utf8(spool);
        repository->create_meeting(meeting);
    }

    void add_chunk(int seq, const std::filesystem::path& file, double start_s, double duration_s,
                   const std::string& channel = "mic") {
        Json fields = Json::object();
        fields["meeting_id"] = "m_audio";
        fields["channel"] = channel;
        fields["seq"] = seq;
        fields["file_path"] = fv::path_to_utf8(file);
        fields["start_s"] = start_s;
        fields["duration_s"] = duration_s;
        fields["sample_rate"] = 16000;
        repository->register_chunk(fields);
    }

    std::shared_ptr<fv::meeting_state::testing::FakeMeetingRepository> repository =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
};

template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name()) + ": " + error.what();
    }
    return "";
}

} // namespace

TEST_CASE("playback mixes channels and preserves silence", "[test_meeting_data_lifecycle]") {
    // from tests/test_meeting_data_lifecycle.py::test_playback_mixes_channels_and_preserves_silence
    TempDir root;
    const std::filesystem::path spool = root.path() / "m_audio";
    std::filesystem::create_directory(spool);
    write_wav(spool / "mic.wav", 1000);
    write_wav(spool / "loopback.wav", 3000);
    write_wav(spool / "later.wav", 4000);
    Lifecycle lifecycle(spool);
    lifecycle.add_chunk(0, spool / "mic.wav", 0.0, 0.25);
    lifecycle.add_chunk(0, spool / "loopback.wav", 0.0, 0.25, "loopback");
    lifecycle.add_chunk(1, spool / "later.wav", 1.0, 0.25);

    const std::filesystem::path output = build_playback(*lifecycle.repository, "m_audio");
    const auto [rendered, rate] = fv::meeting::load_wav_int16(output);
    CHECK(rate == PLAYBACK_RATE);
    REQUIRE(rendered.size() == static_cast<std::size_t>(PLAYBACK_RATE * 5 / 4));
    const auto all = [&](std::size_t from, std::size_t to, std::int16_t value) {
        return std::all_of(rendered.begin() + static_cast<std::ptrdiff_t>(from),
                           rendered.begin() + static_cast<std::ptrdiff_t>(to),
                           [&](std::int16_t sample) { return sample == value; });
    };
    CHECK(all(0, PLAYBACK_RATE / 4, 2000));
    CHECK(all(PLAYBACK_RATE / 4, PLAYBACK_RATE, 0));
    CHECK(all(PLAYBACK_RATE, rendered.size(), 4000));
    const auto written = std::filesystem::last_write_time(output);
    CHECK(build_playback(*lifecycle.repository, "m_audio") == output); // cache hit
    CHECK(std::filesystem::last_write_time(output) == written);
    CHECK_FALSE(std::filesystem::exists(spool / "playback.wav.tmp"));
    CHECK_FALSE(std::filesystem::exists(spool / "playback.json.tmp"));
}

TEST_CASE("playback rejects chunk outside spool", "[test_meeting_data_lifecycle]") {
    // from tests/test_meeting_data_lifecycle.py::test_playback_rejects_chunk_outside_spool
    TempDir root;
    const std::filesystem::path spool = root.path() / "m_audio";
    std::filesystem::create_directory(spool);
    write_wav(root.path() / "outside.wav", 1);
    Lifecycle lifecycle(spool);
    lifecycle.add_chunk(0, root.path() / "outside.wav", 0.0, 0.25);
    CHECK_THAT(raised([&] { build_playback(*lifecycle.repository, "m_audio"); }),
               Catch::Matchers::ContainsSubstring("ValueError: ") &&
                   Catch::Matchers::ContainsSubstring("outside"));
    // A path that only reaches outside through "..".
    Lifecycle sneaky(spool);
    sneaky.add_chunk(0, spool / ".." / "outside.wav", 0.0, 0.25);
    CHECK(raised([&] { build_playback(*sneaky.repository, "m_audio"); }) ==
          "ValueError: audio chunk path is outside its meeting spool");
}

TEST_CASE("the watermark holds each chunk's st_mtime_ns", "[playback]") {
    // 2020-01-01T00:00:00Z, which Python's os.stat reports as 1577836800000000000.
    TempDir root;
    write_wav(root.path() / "a.wav", 100);
    Lifecycle lifecycle(root.path());
    lifecycle.add_chunk(0, root.path() / "a.wav", 0.0, 0.5);
#ifdef _WIN32
    // A FILETIME: 100 ns ticks since 1601.
    const std::filesystem::file_time_type written{std::filesystem::file_time_type::duration{
        1577836800LL * 10'000'000 + 116444736000000000LL}};
#else
    const std::filesystem::file_time_type written = std::chrono::file_clock::from_sys(
        std::chrono::sys_seconds{std::chrono::seconds{1577836800}});
#endif
    std::filesystem::last_write_time(root.path() / "a.wav", written);
    build_playback(*lifecycle.repository, "m_audio");
    std::ifstream in(root.path() / "playback.json", std::ios::binary);
    const std::string cache((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(cache == "{\"watermark\":[[1,16044,1577836800000000000]]}");
}

TEST_CASE("a spool folder saved with a trailing separator still holds its chunks", "[playback]") {
    // os.path.realpath drops the separator, so Python finds the chunk inside the spool.
    TempDir root;
    const std::filesystem::path spool = root.path() / "m_audio";
    std::filesystem::create_directory(spool);
    write_wav(spool / "mic.wav", 1000);
    Lifecycle lifecycle(spool / "");
    lifecycle.add_chunk(0, spool / "mic.wav", 0.0, 0.25);
    const auto [rendered, rate] =
        fv::meeting::load_wav_int16(build_playback(*lifecycle.repository, "m_audio"));
    CHECK(rate == PLAYBACK_RATE);
    CHECK(rendered.size() == static_cast<std::size_t>(PLAYBACK_RATE / 4));
}

TEST_CASE("playback refuses unknown meetings and missing audio", "[playback]") {
    TempDir root;
    fv::meeting_state::testing::FakeMeetingRepository repository;
    CHECK(raised([&] { build_playback(repository, "nope"); }) == "ValueError: unknown meeting");
    Lifecycle no_folder(root.path() / "missing");
    CHECK(raised([&] { build_playback(*no_folder.repository, "m_audio"); }) ==
          "FileNotFoundError: meeting audio is unavailable");
    Lifecycle no_chunks(root.path());
    CHECK(raised([&] { build_playback(*no_chunks.repository, "m_audio"); }) ==
          "FileNotFoundError: meeting audio is unavailable");
}

TEST_CASE("playback skips incompatible chunks and averages only overlaps", "[playback]") {
    TempDir root;
    write_wav(root.path() / "stereo.wav", 9000, 0.5, 16000, 2);
    write_wav(root.path() / "fast.wav", 9000, 0.5, 44100);
    write_wav(root.path() / "a.wav", -3);
    write_wav(root.path() / "b.wav", 2);
    Lifecycle lifecycle(root.path());
    lifecycle.add_chunk(0, root.path() / "stereo.wav", 0.0, 0.5);
    lifecycle.add_chunk(1, root.path() / "fast.wav", 0.0, 0.5);
    lifecycle.add_chunk(2, root.path() / "a.wav", 0.5, 0.5);
    lifecycle.add_chunk(0, root.path() / "b.wav", 0.75, 0.5, "loopback");
    const fv::meeting::test::LogCapture logs;
    const auto [rendered, rate] =
        fv::meeting::load_wav_int16(build_playback(*lifecycle.repository, "m_audio"));
    REQUIRE(rendered.size() == static_cast<std::size_t>(PLAYBACK_RATE * 5 / 4));
    CHECK(rendered[0] == 0);
    CHECK(rendered[PLAYBACK_RATE / 2] == -3);
    // (-3 + 2) // 2 floors to -1.
    CHECK(rendered[PLAYBACK_RATE * 3 / 4 + 10] == -1);
    CHECK(rendered[PLAYBACK_RATE] == 2);
    CHECK(logs.levels("Skipping incompatible meeting chunk 1").size() == 1);
    CHECK(logs.levels("Skipping incompatible meeting chunk 2").size() == 1);
}

TEST_CASE("playback renders again when a chunk changes", "[playback]") {
    TempDir root;
    write_wav(root.path() / "a.wav", 100);
    Lifecycle lifecycle(root.path());
    lifecycle.add_chunk(0, root.path() / "a.wav", 0.0, 0.5);
    const auto output = build_playback(*lifecycle.repository, "m_audio");
    CHECK(fv::meeting::load_wav_int16(output).first.at(0) == 100);
    // The watermark holds the size and modification time of every chunk.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_wav(root.path() / "a.wav", 200, 0.6);
    CHECK(
        fv::meeting::load_wav_int16(build_playback(*lifecycle.repository, "m_audio")).first.at(0) ==
        200);
    const std::string cache = [&] {
        std::ifstream in(root.path() / "playback.json", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    CHECK(cache.starts_with("{\"watermark\":[[1,"));
}
