// WavWriter: the atomic save of services/recorder.py save_recording (temporary file beside the
// target, Python's wave header, tags, fsync, os.replace).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "audio/wav_metadata.h"
#include "audio/wav_writer.h"
#include "base/identity.h"
#include "testkit/testkit.h"

namespace {

using namespace std::chrono;

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t files_in(const std::filesystem::path& folder) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(folder),
                                                  std::filesystem::directory_iterator()));
}

const local_seconds kWhen = local_days{year{2026} / 11 / 14} + hours{9} + minutes{30};

} // namespace

TEST_CASE("a committed wav has the wave header, the frames and the tags") {
    const fv::testkit::TempDataRoot root;
    const auto target = root.path() / "recorded_audio.wav";
    fv::audio::WavWriter writer(target, {});
    const std::vector<std::int16_t> samples{1, -2, 3, -4, 32767, -32768};
    writer.write(std::span<const std::int16_t>(samples));
    CHECK(writer.data_bytes() == 12);
    const auto temporary = writer.temporary_path();
    CHECK(temporary.parent_path() == target.parent_path());
    CHECK(temporary.filename().string().starts_with("tmp"));
    CHECK(temporary.extension() == ".wav");
    writer.commit(kWhen);

    CHECK_FALSE(std::filesystem::exists(temporary));
    const auto bytes = read_bytes(target);
    const std::vector<std::uint8_t> header{
        'R',  'I', 'F', 'F', 0,  0, 0,   0,   'W', 'A',  'V',  'E', 'f', 'm',  't',
        ' ',  16,  0,   0,   0,  1, 0,   1,   0,   0x44, 0xAC, 0,   0,   0x88, 0x58,
        0x01, 0,   2,   0,   16, 0, 'd', 'a', 't', 'a',  12,   0,   0,   0};
    // Everything but the RIFF size, which the tags grew.
    CHECK(std::equal(header.begin() + 8, header.end(), bytes.begin() + 8));
    CHECK(std::equal(bytes.begin() + 44, bytes.begin() + 56,
                     reinterpret_cast<const std::uint8_t*>(samples.data())));
    CHECK(fv::audio::is_mono_pcm16(target));
    CHECK(fv::audio::read_info_icrd(target) == "2026-11-14");
    CHECK(fv::audio::read_info_isft(target) == std::string(fv::identity().product));
    CHECK(fv::audio::read_bext_origination(target) ==
          std::pair<std::string, std::string>{"2026-11-14", "09:30:00"});
}

TEST_CASE("without an origination the wav is the bare wave output") {
    const fv::testkit::TempDataRoot root;
    const auto target = root.path() / "plain.wav";
    fv::audio::WavWriter writer(target, {.rate = 16000, .channels = 2, .sample_width = 2});
    writer.write(std::vector<std::int16_t>{7, 8, 9, 10});
    writer.commit(std::nullopt);
    const auto bytes = read_bytes(target);
    CHECK(bytes.size() == 52);
    CHECK(bytes[4] == 44); // 36 + 8 data bytes
    CHECK(bytes[22] == 2);
    CHECK_FALSE(fv::audio::read_info_icrd(target).has_value());
}

TEST_CASE("the previous output is kept until a new save succeeds") {
    const fv::testkit::TempDataRoot root;
    const auto target = root.path() / "recorded_audio.wav";
    {
        fv::audio::WavWriter first(target, {});
        first.write(std::vector<std::int16_t>{1, 2, 3});
        first.commit(std::nullopt);
    }
    const auto saved = read_bytes(target);
    {
        fv::audio::WavWriter abandoned(target, {});
        abandoned.write(std::vector<std::int16_t>{4, 5, 6, 7});
        CHECK(read_bytes(target) == saved);
    } // destroyed without commit
    CHECK(read_bytes(target) == saved);
    {
        fv::audio::WavWriter discarded(target, {});
        discarded.discard();
        CHECK_THROWS_AS(discarded.write(std::vector<std::int16_t>{1}), std::logic_error);
    }
    CHECK(read_bytes(target) == saved);
    CHECK(files_in(root.path()) == 1);
}
