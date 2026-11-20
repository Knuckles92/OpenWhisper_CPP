// RIFF/WAVE origination tags used by File Explorer (tests/test_wav_metadata.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "audio/wav_metadata.h"
#include "audio/wav_writer.h"
#include "base/identity.h"
#include "golden/golden.h"
#include "testkit/testkit.h"

namespace {

using Bytes = std::vector<std::uint8_t>;
using namespace std::chrono;

const local_seconds kWhen = local_days{year{2026} / 9 / 19} + hours{13} + minutes{5} + seconds{7};

void put_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_tag(Bytes& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

// The bytes Python's wave writer alone produces for mono `frames` at 16 kHz.
Bytes untagged(const Bytes& frames, int width = 2) {
    Bytes out;
    put_tag(out, "RIFF");
    put_u32(out, 36 + static_cast<std::uint32_t>(frames.size()));
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put_u32(out, 16);
    put_u16(out, 1);
    put_u16(out, 1);
    put_u32(out, 16000);
    put_u32(out, 16000 * static_cast<std::uint32_t>(width));
    put_u16(out, static_cast<std::uint16_t>(width));
    put_u16(out, static_cast<std::uint16_t>(8 * width));
    put_tag(out, "data");
    put_u32(out, static_cast<std::uint32_t>(frames.size()));
    out.insert(out.end(), frames.begin(), frames.end());
    return out;
}

Bytes int16_frames(std::int16_t first, std::size_t count, std::int16_t step = 1) {
    Bytes out;
    for (std::size_t i = 0; i < count; ++i) {
        put_u16(out, static_cast<std::uint16_t>(first + static_cast<std::int16_t>(i) * step));
    }
    return out;
}

Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t u32_at(const Bytes& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

// Writes `frames` as mono PCM and tags the file, as the recorder does.
void write_tagged(const std::filesystem::path& path, const Bytes& frames, int width = 2,
                  local_seconds when = kWhen) {
    write_bytes(path, untagged(frames, width));
    const auto file = fv::audio::open_riff_file(path);
    fv::audio::append_origination(*file, when);
}

void assert_even_aligned_info_items(const Bytes& data) {
    std::size_t offset = 12;
    bool found_info = false;
    while (offset + 8 <= data.size()) {
        const std::uint32_t size = u32_at(data, offset + 4);
        if (std::memcmp(data.data() + offset, "LIST", 4) == 0 &&
            std::memcmp(data.data() + offset + 8, "INFO", 4) == 0) {
            found_info = true;
            const std::size_t payload = offset + 8;
            std::size_t pos = 4;
            while (pos + 8 <= size) {
                const std::uint32_t item_size = u32_at(data, payload + pos + 4);
                const std::size_t item_end = pos + 8 + item_size;
                CHECK(item_end <= size);
                if (std::memcmp(data.data() + payload + pos, "ICRD", 4) == 0) {
                    CHECK(item_size % 2 == 1);
                }
                pos = item_end + (item_size & 1u);
            }
            CHECK(pos == size);
        }
        offset = offset + 8 + size + (size & 1u);
    }
    CHECK(found_info);
}

// A RiffFile whose n-th write fails with "No space left on device" (the first one after
// writing 100 bytes of it).
class FlakyFile final : public fv::audio::RiffFile {
public:
    FlakyFile(fv::audio::RiffFile& inner, int failing_write)
        : inner_(inner), failing_(failing_write) {}

    std::uint64_t seek_end() override { return inner_.seek_end(); }
    void seek(std::uint64_t offset) override { inner_.seek(offset); }
    void truncate(std::uint64_t size) override { inner_.truncate(size); }
    std::size_t write(std::span<const std::byte> data) override {
        if (++writes_ == failing_) {
            if (failing_ == 1) {
                // Part of the tags lands before the disk fills.
                inner_.write(data.first(std::min<std::size_t>(100, data.size())));
            }
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                    "No space left on device");
        }
        return inner_.write(data);
    }

private:
    fv::audio::RiffFile& inner_;
    int failing_;
    int writes_ = 0;
};

Bytes riff(const Bytes& format_fields, const Bytes& before_format = {},
           const Bytes& data = Bytes(16, 0)) {
    Bytes body;
    put_tag(body, "WAVE");
    body.insert(body.end(), before_format.begin(), before_format.end());
    put_tag(body, "fmt ");
    put_u32(body, static_cast<std::uint32_t>(format_fields.size()));
    body.insert(body.end(), format_fields.begin(), format_fields.end());
    put_tag(body, "data");
    put_u32(body, static_cast<std::uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    Bytes out;
    put_tag(out, "RIFF");
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes format(std::uint16_t tag, std::uint16_t channels, std::uint16_t bits) {
    const auto block = static_cast<std::uint16_t>(channels * bits / 8);
    Bytes out;
    put_u16(out, tag);
    put_u16(out, channels);
    put_u32(out, 44100);
    put_u32(out, 44100u * block);
    put_u16(out, block);
    put_u16(out, bits);
    return out;
}

Bytes concat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes from_hex(const std::string& hex) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string to_hex(const Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

local_seconds parse_when(const std::string& iso) {
    // "YYYY-MM-DDTHH:MM:SS"
    const int y = std::stoi(iso.substr(0, 4));
    const unsigned mo = static_cast<unsigned>(std::stoi(iso.substr(5, 2)));
    const unsigned d = static_cast<unsigned>(std::stoi(iso.substr(8, 2)));
    return local_days{year{y} / month{mo} / day{d}} + hours{std::stoi(iso.substr(11, 2))} +
           minutes{std::stoi(iso.substr(14, 2))} + seconds{std::stoi(iso.substr(17, 2))};
}

} // namespace

// from tests/test_wav_metadata.py::test_tags_round_trip
TEST_CASE("tags round trip", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "clip.wav";
    write_tagged(path, int16_frames(1000, 8000, 0));

    CHECK(fv::audio::read_info_icrd(path) == "2026-09-19");
    CHECK(fv::audio::read_info_isft(path) == std::string(fv::identity().product));
    CHECK(fv::audio::read_bext_origination(path) ==
          std::pair<std::string, std::string>{"2026-09-19", "13:05:07"});
}

// from tests/test_wav_metadata.py::test_tagging_preserves_pcm_and_even_alignment
TEST_CASE("tagging preserves pcm and even alignment", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "clip.wav";
    const Bytes frames = int16_frames(0, 64);
    write_tagged(path, frames);

    const Bytes data = read_bytes(path);
    const Bytes expected = untagged(frames);
    REQUIRE(data.size() > expected.size());
    CHECK(Bytes(data.begin() + 8, data.begin() + static_cast<std::ptrdiff_t>(expected.size())) ==
          Bytes(expected.begin() + 8, expected.end()));
    CHECK(u32_at(data, 40) == 128); // the data chunk still holds the 64 frames
    CHECK(u32_at(data, 4) == data.size() - 8);
    assert_even_aligned_info_items(data);
}

// from tests/test_wav_metadata.py::test_odd_length_pcm_gets_padding_before_the_tags
TEST_CASE("odd length pcm gets padding before the tags", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "odd.wav";
    write_tagged(path, Bytes{0x80, 0x81, 0x7f}, 1);

    const Bytes data = read_bytes(path);
    CHECK(Bytes(data.begin() + 44, data.begin() + 47) == Bytes{0x80, 0x81, 0x7f});
    CHECK(Bytes(data.begin() + 47, data.begin() + 52) == Bytes{0x00, 'L', 'I', 'S', 'T'});
    CHECK(u32_at(data, 4) == data.size() - 8);
    CHECK(fv::audio::read_info_icrd(path) == "2026-09-19");
    CHECK(fv::audio::read_bext_origination(path) ==
          std::pair<std::string, std::string>{"2026-09-19", "13:05:07"});
}

// from tests/test_wav_metadata.py::test_tags_follow_audio_still_in_the_writer_buffer
// The C++ writer is unbuffered, so the frames are on disk before the tags by construction; what
// stays to check is that tags written by WavWriter::commit follow every frame.
TEST_CASE("tags follow audio still in the writer buffer", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "buffered.wav";
    const Bytes frames = int16_frames(0, 2000);
    fv::audio::WavWriter writer(path, {.rate = 16000, .channels = 1, .sample_width = 2});
    writer.write(std::as_bytes(std::span(frames)));
    writer.commit(kWhen);

    const Bytes data = read_bytes(path);
    CHECK(Bytes(data.begin() + 44, data.begin() + 44 + 4000) == frames);
    CHECK(fv::audio::read_info_icrd(path) == "2026-09-19");
}

// from tests/test_wav_metadata.py::test_a_failed_tag_leaves_the_untagged_wav
TEST_CASE("a failed tag leaves the untagged wav", "[test_wav_metadata]") {
    const int failing_write = GENERATE(1, 2); // the tags' own write, or the header update after it
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "clip.wav";
    const Bytes frames = int16_frames(0, 64);
    write_bytes(path, untagged(frames));
    {
        const auto file = fv::audio::open_riff_file(path);
        FlakyFile flaky(*file, failing_write);
        CHECK_THROWS_AS(fv::audio::append_origination(flaky, kWhen), std::system_error);
    }
    CHECK(read_bytes(path) == untagged(frames));
    CHECK_FALSE(fv::audio::read_info_icrd(path).has_value());
}

// from tests/test_wav_metadata.py::test_a_recording_too_large_for_riff_is_left_untagged
TEST_CASE("a recording too large for riff is left untagged", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "clip.wav";
    const Bytes frames = int16_frames(0, 64);
    write_bytes(path, untagged(frames));
    {
        const auto file = fv::audio::open_riff_file(path);
        try {
            fv::audio::append_origination(*file, kWhen, untagged(frames).size());
            FAIL("no WavMetadataError");
        } catch (const fv::audio::WavMetadataError& error) {
            CHECK(std::string(error.what()).find("too large") != std::string::npos);
        }
    }
    CHECK(read_bytes(path) == untagged(frames));
}

// from tests/test_wav_metadata.py::test_only_mono_16_bit_pcm_is_recognized
TEST_CASE("only mono 16 bit pcm is recognized", "[test_wav_metadata]") {
    Bytes junk;
    put_tag(junk, "JUNK");
    put_u32(junk, 3);
    junk.insert(junk.end(), {'a', 'b', 'c', 0});
    Bytes bare_riff;
    put_tag(bare_riff, "RIFF");
    put_u32(bare_riff, 4);
    put_tag(bare_riff, "WAVE");
    Bytes ogg;
    put_tag(ogg, "OggS");
    ogg.resize(64, 0);
    const Bytes whole = riff(format(1, 1, 16));
    const Bytes truncated(whole.begin(), whole.begin() + 30);

    const auto [content, expected] = GENERATE_COPY(table<Bytes, bool>({
        {riff(format(1, 1, 16)), true},
        // An odd-sized chunk ahead of fmt, with its pad byte.
        {riff(format(1, 1, 16), junk), true},
        {riff(format(1, 2, 16)), false},
        {riff(format(1, 1, 8)), false},
        {riff(format(1, 1, 24)), false},
        {riff(format(3, 1, 32)), false},
        // WAVE_FORMAT_EXTENSIBLE and a compressed codec (MP3) in a WAV.
        {riff(concat(format(0xFFFE, 1, 16), Bytes(24, 0))), false},
        {riff(format(0x55, 1, 16)), false},
        {truncated, false},
        {bare_riff, false},
        {ogg, false},
        {Bytes{}, false},
    }));
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "audio.bin";
    write_bytes(path, content);
    CHECK(fv::audio::is_mono_pcm16(path) == expected);
}

// from tests/test_wav_metadata.py::test_a_recording_is_recognized_and_a_missing_file_is_not
TEST_CASE("a recording is recognized and a missing file is not", "[test_wav_metadata]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "clip.wav";
    write_tagged(path, int16_frames(0, 64));
    CHECK(fv::audio::is_mono_pcm16(path));
    CHECK_FALSE(fv::audio::is_mono_pcm16(root.path() / "missing.wav"));
    CHECK_FALSE(fv::audio::is_mono_pcm16(root.path()));
}

TEST_CASE("golden append origination", "[golden]") {
    fv::golden::for_each(
        "services.wav_metadata", "append_origination", [](const fv::golden::Case& c) {
            const fv::testkit::TempDataRoot root;
            const auto path = root.path() / "golden.wav";
            write_bytes(path, untagged(from_hex(c.arg<std::string>(0)), c.arg<int>(1)));
            {
                const auto file = fv::audio::open_riff_file(path);
                fv::audio::append_origination(*file, parse_when(c.arg<std::string>(2)));
            }
            // The vectors were made with FioriVoce as originator and software.
            REQUIRE(c.raw()["result"].is_string());
            CHECK(to_hex(read_bytes(path)) == c.result<std::string>());
        });
}

TEST_CASE("golden is mono pcm16 and the tag readers", "[golden]") {
    const auto on_file = [](const fv::golden::Case& c, const auto& read) {
        const fv::testkit::TempDataRoot root;
        const auto path = root.path() / "golden.wav";
        write_bytes(path, from_hex(c.arg<std::string>(0)));
        return read(path);
    };
    fv::golden::for_each("services.wav_metadata", "is_mono_pcm16", [&](const fv::golden::Case& c) {
        CHECK(on_file(c, [](const auto& p) { return fv::audio::is_mono_pcm16(p); }) ==
              c.result<bool>());
    });
    fv::golden::for_each("services.wav_metadata", "read_info_icrd", [&](const fv::golden::Case& c) {
        CHECK(on_file(c, [](const auto& p) { return fv::audio::read_info_icrd(p); }) ==
              c.result<std::optional<std::string>>());
    });
    fv::golden::for_each("services.wav_metadata", "read_info_isft", [&](const fv::golden::Case& c) {
        CHECK(on_file(c, [](const auto& p) { return fv::audio::read_info_isft(p); }) ==
              c.result<std::optional<std::string>>());
    });
    fv::golden::for_each(
        "services.wav_metadata", "read_bext_origination", [&](const fv::golden::Case& c) {
            const auto actual =
                on_file(c, [](const auto& p) { return fv::audio::read_bext_origination(p); });
            const auto expected = c.result<std::optional<std::vector<std::string>>>();
            REQUIRE(actual.has_value() == expected.has_value());
            if (actual) {
                CHECK(std::vector<std::string>{actual->first, actual->second} == *expected);
            }
        });
}
