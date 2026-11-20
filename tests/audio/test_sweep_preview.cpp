// Sweep H2: open item "dictation/batch.cpp: source_name follows Windows ntpath ...
// audio/file_preview.cpp: ... a file locked between the header open and the decode fallback gives
// the not-audio sentence instead of "isn't allowed"". Python's preview_file catches the
// PermissionError of the whole _probe_audio_header, the decode included.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include "audio/file_decoder.h"
#include "audio/file_preview.h"
#include "audio/testing/decode_fallback.h"
#include "base/identity.h"
#include "testkit/testkit.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using fv::audio::AudioFileDecoder;
using fv::audio::preview_file;
using fv::audio::UnreadableAudio;
namespace testing = fv::audio::testing;

// A FLAC whose STREAMINFO leaves the sample count unknown: its preview decodes it.
std::filesystem::path no_duration_flac() {
    return fv::testkit::source_root() / "tests" / "fixtures" / "upload" / "no-duration.flac";
}

// Puts the default decoder back however the test ends.
struct OpenerReset {
    ~OpenerReset() { testing::set_decode_fallback_opener({}); }
};

std::string preview_error(const std::filesystem::path& path) {
    try {
        preview_file(path, false);
    } catch (const UnreadableAudio& error) {
        return error.what();
    }
    return "(previewed)";
}

std::string not_allowed() {
    return std::format("{} isn't allowed to read this file.", fv::identity().product);
}

} // namespace

TEST_CASE("a file the decode fallback may not open is not allowed to be read", "[sweep_preview]") {
    const OpenerReset reset;
    int opens = 0;
    // What AudioFileDecoder throws when FFmpeg's open fails with EACCES (a Windows sharing
    // violation).
    testing::set_decode_fallback_opener([&opens](const std::filesystem::path&) -> AudioFileDecoder {
        ++opens;
        throw fv::audio::AudioFileDenied(
            "Could not open the audio file no-duration.flac (Permission denied).");
    });
    CHECK(preview_error(no_duration_flac()) == not_allowed());
    CHECK(opens == 1);
}

TEST_CASE("another failure of the decode fallback is still not audio", "[sweep_preview]") {
    const OpenerReset reset;
    testing::set_decode_fallback_opener([](const std::filesystem::path&) -> AudioFileDecoder {
        throw fv::audio::AudioDecodeError(
            "Could not open the audio file no-duration.flac (Invalid data found).");
    });
    CHECK(preview_error(no_duration_flac()) == fv::audio::NOT_AUDIO_MESSAGE);
}

TEST_CASE("the decode fallback opens the real decoder by default", "[sweep_preview]") {
    const auto preview = preview_file(no_duration_flac(), false);
    CHECK(preview.sample_rate == 48000);
}

#ifdef _WIN32
TEST_CASE("a file locked between the header read and the decode is not allowed to be read",
          "[sweep_preview][platform]") {
    const OpenerReset reset;
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "locked.flac";
    std::filesystem::copy_file(no_duration_flac(), path);
    HANDLE handle = INVALID_HANDLE_VALUE;
    // The lock is taken after the header was read: the real decoder meets the sharing violation.
    testing::set_decode_fallback_opener([&handle](const std::filesystem::path& file) {
        handle = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        return AudioFileDecoder(file);
    });
    const std::string error = preview_error(path);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    CHECK(error == not_allowed());
}
#endif
