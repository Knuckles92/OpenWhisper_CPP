// Golden vectors of meeting.recovery and meeting.refinalize (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "meeting/recovery.h"
#include "meeting/refinalize.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "pyport/json.h"
#include "recovery_support.h"
#include "testkit/testkit.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting;
using namespace fv::meeting::test;

namespace {

// NOW_ISO of the cases: 2026-01-01T12:00:00.123456Z, in a UTC zone.
const fv::FakeClock& now_clock() {
    static const fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 1 / 1} +
                                         std::chrono::hours(12) + std::chrono::microseconds(123456),
                                     "UTC");
    return clock;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

// _signal: the described segments as int16 samples.
std::vector<std::int16_t> signal(const Json& segments) {
    std::vector<std::int16_t> out;
    std::int64_t offset = 0;
    for (const Json& segment : segments) {
        const std::string kind = segment.at(0).get<std::string>();
        const std::int64_t frames = segment.at(1).get<std::int64_t>();
        const std::int64_t amp = segment.at(2).get<std::int64_t>();
        const std::int64_t step = segment.at(3).get<std::int64_t>();
        for (std::int64_t index = offset; index < offset + frames; ++index) {
            std::int64_t value = 0;
            if (kind == "saw") {
                value = floor_div(((index * step) % 65536) * 2 * amp, 65536) - amp;
            } else if (kind == "const") {
                value = amp;
            }
            out.push_back(static_cast<std::int16_t>(static_cast<std::uint16_t>(value & 0xFFFF)));
        }
        offset += frames;
    }
    return out;
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json recover_pcm_tails_run(const Json& spec) {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path folder = root.path() / "spool";
    std::filesystem::create_directories(folder);
    const Json sessions = spec.value("sessions", Json::object());
    for (const auto& [channel, session] : sessions.items()) {
        if (session.contains("meta") && !session.at("meta").is_null()) {
            std::ofstream(folder / fv::path_from_utf8(channel + "_session.json"), std::ios::binary)
                << fv::py::dumps(session.at("meta"));
        }
        if (session.contains("prefix") && !session.at("prefix").is_null()) {
            write_bytes(folder / fv::path_from_utf8(channel + "_session.16k.pcm"),
                        signal(session.at("prefix")));
        }
        if (session.contains("raw") && !session.at("raw").is_null()) {
            write_bytes(folder / fv::path_from_utf8(channel + "_session.pcm"),
                        signal(session.at("raw")));
        }
        if (session.contains("wav") && !session.at("wav").is_null()) {
            write_wav(folder / fv::path_from_utf8(channel + "_session.wav"),
                      signal(Json::array({Json::array({"saw", session.at("wav"), 3000, 641})})));
        }
    }
    std::vector<Json> chunks;
    for (const Json& row : spec.value("chunks", Json::array())) {
        chunks.push_back(row);
    }
    const std::size_t given = chunks.size();
    ScriptedRepository repo;
    repo.inner.create_meeting({{"id", "m_gold"},
                               {"title", "Golden"},
                               {"status", "ended"},
                               {"started_at", "2026-01-01T09:00:00"},
                               {"host_token", "host"},
                               {"guest_token", "guest"},
                               {"cloud_enabled", false},
                               {"spool_dir", "/tmp/spool"}});
    repo.fail_register(spec.value("register_failures", 0));
    const Json meeting = {{"id", "m_gold"}, {"spool_dir", fv::path_to_utf8(folder)}};

    const std::int64_t recovered = recover_pcm_tails(repo, meeting, chunks);

    Json rows = Json::array();
    for (std::size_t i = given; i < chunks.size(); ++i) {
        const Json& row = chunks[i];
        const std::filesystem::path path =
            fv::path_from_utf8(row.at("file_path").get<std::string>());
        const auto info = read_wav_duration(path);
        rows.push_back({{"channel", row.at("channel")},
                        {"seq", row.at("seq")},
                        {"file", fv::path_to_utf8(path.filename())},
                        {"start_s", row.at("start_s")},
                        {"duration_s", row.at("duration_s")},
                        {"sample_rate", row.at("sample_rate")},
                        {"asr_status", row.at("asr_status")},
                        {"frames", info ? std::llround(info->second * info->first) : 0},
                        {"sha256", fv::sha256_hex(file_bytes(path))}});
    }
    std::set<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.insert(fv::path_to_utf8(entry.path().filename()));
    }
    return {{"recovered", recovered}, {"rows", rows}, {"files", Json(names)}};
}

Json mark_ended_run(const Json& meeting) {
    ScriptedRepository repo;
    mark_ended(repo, meeting, now_clock());
    Json updates = Json::array();
    for (const auto& [id, fields] : repo.updates()) {
        updates.push_back(Json::array({id, fields}));
    }
    return updates;
}

bool is_session_dead_at(const Json& meeting, const Json& alive) {
    const std::set<std::int64_t> pids(alive.begin(), alive.end());
    return is_session_dead(meeting, now_clock(),
                           [&pids](std::int64_t pid) { return pids.contains(pid); });
}

template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    REQUIRE_FALSE(c.raises());
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::equal(c.result<Json>(), actual));
}

fv::meeting_state::MeetingStateStore store_of(const Json& state) {
    return fv::meeting_state::MeetingStateStore(fv::meeting_state::MeetingState::from_json(state));
}

} // namespace

TEST_CASE("golden meeting.recovery _recover_pcm_tails", "[meeting][recovery][golden]") {
    golden::for_each("meeting.recovery", "recover_pcm_tails_run", [](const golden::Case& c) {
        replay(c, [&] { return recover_pcm_tails_run(c.arg<Json>(0)); });
    });
}

TEST_CASE("golden meeting.recovery _mark_ended", "[meeting][recovery][golden]") {
    golden::for_each("meeting.recovery", "mark_ended_run", [](const golden::Case& c) {
        replay(c, [&] { return mark_ended_run(c.arg<Json>(0)); });
    });
}

TEST_CASE("golden meeting.recovery is_session_dead", "[meeting][recovery][golden]") {
    golden::for_each("meeting.recovery", "is_session_dead_at", [](const golden::Case& c) {
        replay(c, [&] { return Json(is_session_dead_at(c.arg<Json>(0), c.arg<Json>(1))); });
    });
}

TEST_CASE("golden meeting.refinalize _run_ids", "[meeting][refinalize][golden]") {
    golden::for_each("meeting.refinalize", "_run_ids", [](const golden::Case& c) {
        replay(c, [&] { return Json(run_ids(c.arg<Json>(0), c.arg<std::string>(1))); });
    });
}

TEST_CASE("golden meeting.refinalize _overall_from_steps", "[meeting][refinalize][golden]") {
    golden::for_each("meeting.refinalize", "overall_from_steps", [](const golden::Case& c) {
        replay(c, [&] {
            const auto [status, message] = overall_from_steps(c.arg<Json>(0), c.arg<bool>(1));
            return Json::array({status, message});
        });
    });
}

TEST_CASE("golden meeting.refinalize _steps_from_state", "[meeting][refinalize][golden]") {
    golden::for_each("meeting.refinalize", "steps_from_state", [](const golden::Case& c) {
        INFO(c.label());
        // The redecode step's detail names Parakeet where OpenWhisper names Whisper (lead
        // amendment 19, wording).
        Json expected = c.result<Json>();
        for (Json& entry : expected) {
            if (entry.value("detail", "") == "High-accuracy full session Whisper decode") {
                entry["detail"] = "High-accuracy full session Parakeet decode";
            }
        }
        const Json actual =
            steps_from_state(c.arg<Json>(0), c.arg<std::string>(1), c.arg<bool>(2), c.arg<bool>(3));
        INFO("actual " << actual.dump());
        CHECK(golden::equal(expected, actual));
    });
}

TEST_CASE("golden meeting.refinalize _coverage_refusal", "[meeting][refinalize][golden]") {
    golden::for_each("meeting.refinalize", "coverage_refusal", [](const golden::Case& c) {
        replay(c, [&] {
            ScriptedRepository repo;
            const Json existing = c.arg<Json>(0);
            if (existing == "raise") {
                repo.set_segments(std::nullopt, true);
            } else {
                std::vector<Json> rows;
                int index = 0;
                for (const Json& text : existing) {
                    rows.push_back({{"id", "sg_" + std::to_string(index++)}, {"text", text}});
                }
                repo.set_segments(rows);
            }
            std::vector<fv::meeting_state::TranscriptSegment> decoded;
            int index = 0;
            for (const Json& text : c.arg<Json>(1)) {
                fv::meeting_state::TranscriptSegment segment;
                segment.segment_id = "sg_new_" + std::to_string(index);
                segment.meeting_id = "m_gold";
                segment.channel = "mic";
                segment.start_s = index;
                segment.end_s = index + 1;
                segment.text = text.get<std::string>();
                decoded.push_back(segment);
                ++index;
            }
            const auto refusal = coverage_refusal(repo, "m_gold", decoded, c.arg<bool>(2));
            return refusal ? *refusal : Json();
        });
    });
}

TEST_CASE("golden meeting.refinalize strip_unevidenced_proposed", "[meeting][refinalize][golden]") {
    golden::for_each("meeting.refinalize", "strip_unevidenced_proposed", [](const golden::Case& c) {
        replay(c, [&] {
            auto store = store_of(c.arg<Json>(0));
            strip_unevidenced_proposed(store);
            Json cards = Json::object();
            const auto snapshot = store.snapshot();
            for (const auto& [card, items] : snapshot.at("cards").items()) {
                Json out = Json::array();
                for (const Json& item : items) {
                    out.push_back({{"id", item.at("id")}, {"status", item.at("status")}});
                }
                cards[card] = out;
            }
            return cards;
        });
    });
}
