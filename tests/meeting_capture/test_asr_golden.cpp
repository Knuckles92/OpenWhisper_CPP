// Golden vectors of meeting.asr.offline, meeting.asr.engine and meeting.audio_playback
// (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asr_test_support.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "capture_test_support.h"
#include "golden/golden.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/offline.h"
#include "meeting_capture/playback.h"
#include "meeting_capture/segment_ids.h"
#include "pyport/errors.h"

namespace golden = fv::golden;
using fv::Json;
using fv::meeting::test::make_signal;
using fv::meeting::test::TempDir;
using fv::meeting_state::TranscriptSegment;

namespace {

// The Python exception type a call raised, "" when it returned.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    } catch (const fv::meeting::detail::WaveError&) {
        return "Error"; // wave.Error
    }
    return "";
}

template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised([&] { (void)fn(); }) == c.raises_type());
        return;
    }
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::matches(c.result<Json>(), actual, c.float_tolerance()));
}

std::vector<TranscriptSegment> rows(const Json& list) {
    std::vector<TranscriptSegment> out;
    for (const Json& row : list) {
        TranscriptSegment made;
        made.segment_id = row.at(0).get<std::string>();
        made.meeting_id = "m";
        made.channel = row.at(1).get<std::string>();
        made.start_s = row.at(2).get<double>();
        made.end_s = row.at(3).get<double>();
        made.text = row.at(4).get<std::string>();
        out.push_back(std::move(made));
    }
    return out;
}

Json ids(const std::vector<TranscriptSegment>& segments) {
    Json out = Json::array();
    for (const auto& segment : segments) {
        out.push_back(segment.segment_id);
    }
    return out;
}

std::vector<fv::Segment> scripted(const Json& list) {
    std::vector<fv::Segment> out;
    for (const Json& item : list) {
        out.push_back(
            {item.at(2).get<std::string>(), item.at(0).get<double>(), item.at(1).get<double>()});
    }
    return out;
}

// A 16-bit WAV whose every channel carries `mono`, as wave writes it.
void write_channels(const std::filesystem::path& path, const std::vector<std::int16_t>& mono,
                    int rate, int channels) {
    std::vector<std::int16_t> frames;
    for (const std::int16_t sample : mono) {
        for (int c = 0; c < channels; ++c) {
            frames.push_back(sample);
        }
    }
    fv::meeting::test::write_wav(path, 0, 0.0, rate, channels);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(0, std::ios::end);
    file.write(reinterpret_cast<const char*>(frames.data()),
               static_cast<std::streamsize>(frames.size() * 2));
    const auto data = static_cast<std::uint32_t>(frames.size() * 2);
    const auto patch = [&](std::streamoff at, std::uint32_t v) {
        const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                               static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        file.seekp(at);
        file.write(bytes, 4);
    };
    patch(4, 36 + data);
    patch(40, data);
}
} // namespace

TEST_CASE("meeting.asr.offline offline_cut_ranges replays", "[golden]") {
    golden::for_each("meeting.asr.offline", "offline_cut_ranges_signal", [](const golden::Case& c) {
        replay(c, [&] {
            std::optional<std::vector<std::int16_t>> audio;
            if (!c.args().at(2).is_null()) {
                audio = make_signal(c.args().at(2));
            }
            fv::meeting::OfflineCutOptions options;
            options.target_sec = c.arg<double>(3);
            options.max_sec = c.arg<double>(4);
            options.quiet_window_s = c.arg<double>(5);
            options.overlap_s = c.arg<double>(6);
            options.quiet_rms = c.arg<double>(7);
            std::optional<std::span<const std::int16_t>> view;
            if (audio) {
                view = std::span<const std::int16_t>(*audio);
            }
            Json out = Json::array();
            for (const auto& [start, end] : fv::meeting::offline_cut_ranges(
                     c.arg<std::int64_t>(0), c.arg<int>(1), view, options)) {
                out.push_back(Json::array({start, end}));
            }
            return out;
        });
    });
}

TEST_CASE("meeting.asr.offline drop_overlapped_prefix replays", "[golden]") {
    golden::for_each("meeting.asr.offline", "drop_overlapped_prefix_rows",
                     [](const golden::Case& c) {
                         replay(c, [&] {
                             return ids(fv::meeting::drop_overlapped_prefix(rows(c.args().at(0)),
                                                                            c.arg<double>(1)));
                         });
                     });
}

TEST_CASE("meeting.asr.offline drop_mic_echo replays", "[golden]") {
    golden::for_each("meeting.asr.offline", "drop_mic_echo_rows", [](const golden::Case& c) {
        replay(c, [&] { return ids(fv::meeting::drop_mic_echo(rows(c.args().at(0)))); });
    });
}

TEST_CASE("meeting.asr.offline offline_segment_id replays", "[golden]") {
    golden::for_each("meeting.asr.offline", "offline_segment_id", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(fv::meeting::offline_segment_id(c.arg<std::string>(0),
                                                        c.arg<std::string>(1), c.arg<double>(2),
                                                        c.arg<std::int64_t>(3)));
        });
    });
}

TEST_CASE("meeting.asr.offline session pass replays", "[golden]") {
    golden::for_each("meeting.asr.offline", "session_pass", [](const golden::Case& c) {
        replay(c, [&] {
            const std::vector<std::int16_t> frames = make_signal(c.args().at(0));
            const Json windows = c.args().at(7);
            Json sizes = Json::array();
            const fv::meeting::WindowDecoder decode = [&](std::vector<float> audio) {
                const std::size_t index = std::min<std::size_t>(sizes.size(), windows.size() - 1);
                sizes.push_back(audio.size());
                return scripted(windows.at(index));
            };
            Json progress = Json::array();
            fv::meeting::SessionAudioOptions options;
            options.meeting_id = "m_golden";
            options.channel = c.arg<std::string>(6);
            options.origin_s = c.arg<double>(2);
            options.target_sec = c.arg<double>(3);
            options.max_sec = c.arg<double>(4);
            options.overlap_s = c.arg<double>(5);
            options.progress = [&](const std::string& detail, int current, int total) {
                progress.push_back(Json::array({detail, current, total}));
            };
            Json segments = Json::array();
            for (const auto& segment :
                 fv::meeting::transcribe_session_audio(decode, frames, c.arg<int>(1), options)) {
                segments.push_back(Json::array(
                    {segment.segment_id, segment.start_s, segment.end_s, segment.text}));
            }
            Json out = Json::object();
            out["segments"] = segments;
            out["windows"] = sizes;
            out["progress"] = progress;
            return out;
        });
    });
}

TEST_CASE("meeting.asr.engine _stable_segment_id replays", "[golden]") {
    golden::for_each("meeting.asr.engine", "_stable_segment_id", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(fv::meeting::stable_segment_id(
                c.arg<std::string>(0), c.arg<std::int64_t>(1), c.arg<std::int64_t>(2)));
        });
    });
}

TEST_CASE("meeting.asr.engine _is_digital_silence replays", "[golden]") {
    golden::for_each("meeting.asr.engine", "_is_digital_silence", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(fv::meeting::is_digital_silence(c.arg<std::vector<std::int16_t>>(0)));
        });
    });
}

TEST_CASE("meeting.asr.engine prepare_for_whisper replays", "[golden]") {
    golden::for_each("meeting.asr.engine", "prepare_for_whisper_signal", [](const golden::Case& c) {
        replay(c, [&] {
            const auto frames = make_signal(c.args().at(0));
            Json out = Json::array();
            for (const float sample : fv::meeting::prepare_for_decoder(frames, c.arg<int>(1))) {
                out.push_back(static_cast<double>(sample));
            }
            return out;
        });
    });
}

TEST_CASE("meeting.asr.engine chunk segments replay", "[golden]") {
    // _transcribe_chunk's steps, as the engine runs them: silence check, conversion, the
    // decoder's segments to meeting-clock rows.
    golden::for_each("meeting.asr.engine", "chunk_segments", [](const golden::Case& c) {
        replay(c, [&] {
            const auto frames = make_signal(c.args().at(4));
            fv::meeting_state::SpooledChunk chunk;
            chunk.chunk_id = c.arg<std::int64_t>(1);
            chunk.meeting_id = c.arg<std::string>(0);
            chunk.channel = c.arg<std::string>(2);
            chunk.start_s = c.arg<double>(3);
            Json out = Json::object();
            Json segments = Json::array();
            bool decoded = false;
            if (!fv::meeting::is_digital_silence(frames) &&
                !fv::meeting::prepare_for_decoder(frames, c.arg<int>(5)).empty()) {
                decoded = true;
                const auto pieces = scripted(c.args().at(6));
                for (const auto& segment :
                     fv::meeting::chunk_segments(chunk.meeting_id, chunk, pieces)) {
                    segments.push_back(
                        Json::array({segment.segment_id, *segment.chunk_id, segment.channel,
                                     segment.start_s, segment.end_s, segment.text}));
                }
            }
            out["segments"] = segments;
            out["decoded"] = decoded;
            return out;
        });
    });
}

TEST_CASE("meeting.audio_playback _render_mono replays", "[golden]") {
    golden::for_each("meeting.audio_playback", "render_mono_chunks", [](const golden::Case& c) {
        TempDir folder;
        replay(c, [&] {
            std::vector<Json> chunks;
            int index = 0;
            for (const Json& item : c.args().at(0)) {
                const auto path = folder.path() / ("chunk" + std::to_string(index++) + ".wav");
                write_channels(path, make_signal(item.at(3)), item.at(4).get<int>(),
                               item.at(5).get<int>());
                Json row = Json::object();
                row["id"] = item.at(0);
                row["file_path"] = fv::path_to_utf8(path);
                row["start_s"] = item.at(1);
                row["duration_s"] = item.at(2);
                chunks.push_back(row);
            }
            const auto output = folder.path() / "playback.wav";
            fv::meeting::render_mono(chunks, output);
            const auto wav = fv::meeting::detail::read_wav(output);
            const std::string bytes(reinterpret_cast<const char*>(wav.frames.data()),
                                    wav.frames.size());
            Json head = Json::array();
            for (std::size_t i = 0; i < std::min<std::size_t>(16, wav.frames.size() / 2); ++i) {
                std::int16_t sample = 0;
                std::memcpy(&sample, wav.frames.data() + 2 * i, 2);
                head.push_back(sample);
            }
            Json out = Json::object();
            out["frames"] = wav.frames.size() / 2;
            out["sha256"] = fv::sha256_hex(bytes);
            out["head"] = head;
            return out;
        });
    });
}
