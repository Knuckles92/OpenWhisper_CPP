// Golden vectors of meeting.clock, meeting.capture.health and meeting.capture.spool
// (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "capture_test_support.h"
#include "golden/golden.h"
#include "meeting_capture/detail/cut_scanner.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/health.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/session_audio.h"
#include "meeting_capture/sources.h"
#include "meeting_capture/spool.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting;
using fv::meeting::test::make_signal;
using fv::meeting::test::set_steady;
using fv::meeting::test::TempDir;

namespace {

template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised(fn) == c.raises_type());
        return;
    }
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::matches(c.result<Json>(), actual, c.float_tolerance()));
}

Json optional_int(const std::optional<std::int64_t>& value) {
    return value ? Json(*value) : Json();
}

std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return fv::sha256_hex(bytes);
}

Json chunk_json(const fv::meeting_state::SpooledChunk& chunk) {
    Json out = Json::object();
    out["chunk_id"] = chunk.chunk_id;
    out["channel"] = chunk.channel;
    out["seq"] = chunk.seq;
    out["file"] = fv::path_from_utf8(chunk.file_path).filename().string();
    out["start_s"] = chunk.start_s;
    out["duration_s"] = chunk.duration_s;
    out["sample_rate"] = chunk.sample_rate;
    return out;
}

// tools/parity/cases/meeting.capture.spool.py _spool_run.
Json spool_run(const Json& script, const Json& options) {
    TempDir folder;
    fv::FakeClock clock;
    set_steady(clock, 1000.0);
    MeetingClock meeting_clock(clock);
    meeting_clock.start();
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(clock);
    repository->fail_register_chunk(options.value("register_failures", 0));
    std::mutex seen_mutex;
    Json seen = Json::array();
    SpoolOptions spool_options;
    spool_options.target_sec = options.value("target_sec", DEFAULT_TARGET_SEC);
    spool_options.max_sec = options.value("max_sec", DEFAULT_MAX_SEC);
    spool_options.quiet_window_s = options.value("quiet_window_s", QUIET_WINDOW_S);
    spool_options.initial_seq = options.value("initial_seq", std::int64_t{0});
    SpoolWriter writer(
        "m_golden", "mic", folder.path(), meeting_clock, repository,
        [&](const fv::meeting_state::SpooledChunk& chunk) {
            const std::lock_guard lock(seen_mutex);
            seen.push_back(chunk_json(chunk));
        },
        spool_options);
    for (const Json& step : script) {
        const std::string name = step.at(0).get<std::string>();
        if (name == "pause" || name == "resume") {
            set_steady(clock, step.at(1).get<double>());
            name == "pause" ? meeting_clock.pause() : meeting_clock.resume();
            continue;
        }
        const int rate = step.at(1).get<int>();
        const double t = step.at(2).get<double>();
        const auto start = step.at(3).get<std::size_t>();
        const auto blocks = step.at(4).get<std::size_t>();
        const auto frames = step.at(5).get<std::size_t>();
        const std::vector<std::int16_t> data = make_signal(step.at(6));
        for (std::size_t k = 0; k < blocks; ++k) {
            const std::size_t index = start + k * frames;
            const double t_mono =
                name == "burst" ? t : t + static_cast<double>(index) / static_cast<double>(rate);
            fv::meeting::test::feed_waiting(
                writer, fv::meeting::test::block_of(data, index, frames, rate, t_mono));
        }
    }
    const std::optional<fv::meeting_state::SpooledChunk> final_chunk = writer.flush();

    Json out = Json::object();
    Json registered = Json::array();
    for (const Json& row : repository->get_audio_chunks("m_golden")) {
        Json fields = Json::object();
        for (const char* key : {"meeting_id", "channel", "seq", "file_path", "start_s",
                                "duration_s", "sample_rate", "asr_status"}) {
            fields[key] = row.at(key);
        }
        fields["file_path"] =
            fv::path_from_utf8(fields["file_path"].get<std::string>()).filename().string();
        registered.push_back(std::move(fields));
    }
    std::sort(registered.begin(), registered.end(),
              [](const Json& a, const Json& b) { return a.at("seq") < b.at("seq"); });
    out["registered"] = registered;
    out["on_chunk"] = seen;
    out["final"] = final_chunk ? chunk_json(*final_chunk) : Json();
    Json files = Json::object();
    for (const std::string& name : folder.names()) {
        const auto path = folder.path() / name;
        files[name] =
            Json{{"size", std::filesystem::file_size(path)}, {"sha256", file_sha256(path)}};
    }
    out["files"] = files;
    const auto session_json = session_meta_path(folder.path(), "mic");
    if (std::filesystem::is_regular_file(session_json)) {
        std::ifstream in(session_json, std::ios::binary);
        out["session_json"] = fv::py::loads(
            std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    } else {
        out["session_json"] = nullptr;
    }
    const auto session_wav = session_wav_path(folder.path(), "mic");
    out["session_wav_frames"] =
        std::filesystem::is_regular_file(session_wav)
            ? Json(fv::meeting::detail::read_wav(session_wav).frames.size() / 2)
            : Json();
    const auto health = writer.health_error();
    out["health_error"] = health ? Json(*health) : Json();
    out["gap_logs"] = writer.timeline_fixes();
    return out;
}

} // namespace

TEST_CASE("meeting.clock script", "[golden]") {
    golden::for_each("meeting.clock", "script", [](const golden::Case& c) {
        replay(c, [&] {
            fv::FakeClock clock;
            set_steady(clock, 10'000.0);
            MeetingClock meeting_clock(clock);
            Json out = Json::array();
            for (const Json& step : c.args().at(0)) {
                const std::string name = step.at(0).get<std::string>();
                if (name == "advance") {
                    clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(step.at(1).get<double>())));
                } else if (name == "start") {
                    meeting_clock.start();
                } else if (name == "pause") {
                    meeting_clock.pause();
                } else if (name == "resume") {
                    meeting_clock.resume();
                } else if (name == "resume_from_recovery") {
                    meeting_clock.resume_from_recovery(step.at(1).get<double>());
                } else if (name == "now_s") {
                    out.push_back(meeting_clock.now_s());
                } else if (name == "meeting_time") {
                    out.push_back(meeting_clock.meeting_time(step.at(1).get<double>()));
                } else if (name == "paused_total_s") {
                    out.push_back(meeting_clock.paused_total_s());
                } else if (name == "is_paused") {
                    out.push_back(meeting_clock.is_paused());
                } else if (name == "is_running") {
                    out.push_back(meeting_clock.is_running());
                } else {
                    FAIL("unknown step " << name);
                }
            }
            return out;
        });
    });
}

namespace {

Json health_trace(const Json& steps, double signal_rms) {
    CaptureSignalMonitor monitor(fv::system_clock(), signal_rms);
    Json out = Json::array();
    for (const Json& step : steps) {
        const std::string name = step.at(0).get<std::string>();
        if (name == "reset") {
            monitor.reset_stream(step.at(1).get<double>());
        } else if (name == "observe") {
            const std::vector<std::int16_t> frames(step.at(2).get<std::size_t>(),
                                                   step.at(1).get<std::int16_t>());
            const double now = step.at(5).get<double>();
            monitor.observe(CaptureBlock{.channel = "mic",
                                         .frames = frames.data(),
                                         .count = frames.size(),
                                         .rate = step.at(3).get<int>(),
                                         .t_mono = now},
                            step.at(4).get<bool>(), now);
        } else if (name == "snapshot") {
            out.push_back(monitor.snapshot(step.at(1).get<double>()).to_json());
        }
    }
    return out;
}

} // namespace

TEST_CASE("meeting.capture.health trace", "[golden]") {
    golden::for_each("meeting.capture.health", "trace", [](const golden::Case& c) {
        replay(c, [&] { return health_trace(c.args().at(0), SIGNAL_RMS_INT16); });
    });
    golden::for_each("meeting.capture.health", "trace_with_threshold", [](const golden::Case& c) {
        replay(c, [&] { return health_trace(c.args().at(0), c.arg<double>(1)); });
    });
}

TEST_CASE("meeting.capture.spool resample_to_16k", "[golden]") {
    golden::for_each("meeting.capture.spool", "resample_to_16k", [](const golden::Case& c) {
        replay(c, [&] {
            const auto samples = c.arg<std::vector<std::int16_t>>(0);
            return Json(resample_to_16k(samples, c.arg<int>(1)));
        });
    });
}

TEST_CASE("meeting.capture.spool find_cut_point", "[golden]") {
    golden::for_each("meeting.capture.spool", "find_cut_point_signal", [](const golden::Case& c) {
        replay(c, [&] {
            const auto buffer = make_signal(c.args().at(0));
            return optional_int(find_cut_point(buffer, c.arg<int>(1), c.arg<double>(2),
                                               c.arg<double>(3), c.arg<double>(4),
                                               c.arg<double>(5)));
        });
    });
}

TEST_CASE("meeting.capture.spool cut scanner pushes equal the one-shot cut", "[golden]") {
    golden::for_each("meeting.capture.spool", "cut_scanner_pushes", [](const golden::Case& c) {
        replay(c, [&] {
            const auto data = make_signal(c.args().at(0));
            const int rate = c.arg<int>(1);
            fv::meeting::detail::CutScanner scanner(rate, c.arg<double>(2), c.arg<double>(3),
                                                    QUIET_RMS, c.arg<double>(4));
            Json out = Json::array();
            std::size_t at = 0;
            for (const auto size : c.arg<std::vector<std::size_t>>(5)) {
                const std::size_t begin = std::min(at, data.size());
                const std::size_t end = std::min(at + size, data.size());
                scanner.push(std::span(data).subspan(begin, end - begin));
                at += size;
                const auto cut = scanner.cut_index();
                out.push_back(optional_int(cut));
                // Incremental scanning agrees with scanning everything pushed so far at once.
                const auto one_shot =
                    find_cut_point(std::span(data).first(end), rate, c.arg<double>(2),
                                   c.arg<double>(3), QUIET_RMS, c.arg<double>(4));
                if (cut) {
                    CHECK(one_shot == cut);
                }
            }
            return out;
        });
    });
}

TEST_CASE("meeting.capture.spool gap_fill_frame_count", "[golden]") {
    golden::for_each("meeting.capture.spool", "gap_fill_frame_count", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(gap_fill_frame_count(c.arg<std::optional<double>>(0), c.arg<double>(1),
                                             c.arg<int>(2), c.arg<double>(3)));
        });
    });
}

TEST_CASE("meeting.capture.spool whole SpoolWriter runs", "[golden]") {
    golden::for_each("meeting.capture.spool", "spool_run", [](const golden::Case& c) {
        replay(c, [&] { return spool_run(c.args().at(0), c.args().at(1)); });
    });
}

TEST_CASE("a 60 s 44.1 kHz microphone through FakeBackend has no drift against the golden "
          "session WAV",
          "[golden]") {
    // The first spool run of the vectors: 60 s at 44.1 kHz in 1024-frame blocks. Here the blocks
    // come from a FakeBackend capture stream through MicSource, each delivered when the clock
    // reads its capture time, so the timestamps are the source's own.
    const Json& run = golden::load("meeting.capture.spool").at("functions").at("spool_run").at(0);
    const Json& step = run.at("args").at(0).at(0);
    REQUIRE(step.at(0) == "stream");
    const int rate = step.at(1).get<int>();
    REQUIRE(rate == 44100);
    const double t0 = step.at(2).get<double>();
    const auto blocks = step.at(4).get<std::size_t>();
    const auto frames = step.at(5).get<std::size_t>();
    const std::vector<std::int16_t> signal = make_signal(step.at(6));

    TempDir folder;
    fv::FakeClock clock;
    set_steady(clock, 1000.0);
    MeetingClock meeting_clock(clock);
    meeting_clock.start();
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(clock);
    auto backend = std::make_shared<fv::audio::testing::FakeBackend>();
    backend->set_layout({{.name = "MME", .default_input_device = 0}},
                        {{.name = "Microphone",
                          .hostapi = 0,
                          .max_input_channels = 1,
                          .default_samplerate = 44100}},
                        0);
    SpoolWriter writer("m_golden", "mic", folder.path(), meeting_clock, repository,
                       [](const fv::meeting_state::SpooledChunk&) {});
    MicSource source(backend, meeting_clock, "mic", 0, rate, 1);
    source.start([&](const CaptureBlock& block) { writer.feed(block); });
    const auto stream = backend->streams().back();
    for (std::size_t k = 0; k < blocks; ++k) {
        const std::size_t index = k * frames;
        set_steady(clock, t0 + static_cast<double>(index) / static_cast<double>(rate));
        while (writer.queued_blocks() + 2 > writer.queue_capacity()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stream->deliver(std::span(signal).subspan(index, frames));
    }
    source.stop();
    writer.flush();

    const Json& expected = run.at("result").at("files");
    std::vector<std::string> names;
    for (const auto& [name, file] : expected.items()) {
        names.push_back(name);
        INFO(name);
        CHECK(file_sha256(folder.path() / name) == file.at("sha256").get<std::string>());
    }
    CHECK(folder.names() == names);
    CHECK(writer.timeline_fixes() == 0);
}
