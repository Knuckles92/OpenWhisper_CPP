// A crashed meeting's spool written by OpenWhisper (tests/fixtures/meeting/make_fixtures.py) is
// found, reconciled and finalized into the chunk rows, segments and files OpenWhisper's recovery
// produced from it, with a decoder that answers "level <first sample>" for each chunk.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "base/hash.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "meeting/model_lease.h"
#include "meeting/recovery.h"
#include "pyport/json.h"
#include "recovery_support.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting;
using namespace fv::meeting::test;

namespace {

std::filesystem::path fixture_dir() {
    return fv::testkit::source_root() / "tests" / "fixtures" / "meeting";
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json chunk_rows(fv::meeting_state::MeetingRepository& repo, const std::string& meeting_id) {
    Json rows = Json::array();
    for (const Json& row : repo.get_audio_chunks(meeting_id)) {
        rows.push_back(
            {{"channel", row.at("channel")},
             {"seq", row.at("seq")},
             {"file", fv::path_to_utf8(
                          fv::path_from_utf8(row.at("file_path").get<std::string>()).filename())},
             {"start_s", row.at("start_s")},
             {"duration_s", row.at("duration_s")},
             {"sample_rate", row.at("sample_rate")},
             {"asr_status", row.at("asr_status")}});
    }
    return rows;
}

// One segment over the whole window, named after its first sample.
ScriptedDecode level_of(const fv::speech::DecodeJob& job) {
    const auto& clip = std::get<fv::speech::AudioClip>(job.input);
    const float first = (*clip.samples)[clip.begin];
    const auto level = static_cast<long>(std::lround(static_cast<double>(first) * 32768.0));
    const double seconds = static_cast<double>(clip.end - clip.begin) / clip.rate;
    return {{fv::Segment{"level " + std::to_string(level), 0.0, seconds}}, std::nullopt};
}

} // namespace

TEST_CASE("a reference crash spool is reconciled and finalized as the reference app does",
          "[meeting][recovery][golden]") {
    const Json expected = fv::py::loads(file_bytes(fixture_dir() / "crash.json"));
    SqlRepo sql;
    const std::filesystem::path spool = sql.root.path() / "spool";
    std::filesystem::copy(fixture_dir() / "crash_spool", spool);

    Json fields = expected.at("meeting");
    const std::string meeting_id = fields.at("id").get<std::string>();
    fields["spool_dir"] = fv::path_to_utf8(spool);
    fields["state_json"] = expected.at("state_json");
    sql.repo->create_meeting(fields);
    for (const Json& row : expected.at("initial_chunks")) {
        const std::int64_t id = sql.repo->register_chunk(
            {{"meeting_id", meeting_id},
             {"channel", row.at("channel")},
             {"seq", row.at("seq")},
             {"file_path", fv::path_to_utf8(spool / row.at("file").get<std::string>())},
             {"start_s", row.at("start_s")},
             {"duration_s", row.at("duration_s")},
             {"sample_rate", row.at("sample_rate")}});
        const std::string status = row.at("asr_status").get<std::string>();
        if (status != "pending") {
            sql.repo->set_chunk_status(id, status, std::nullopt);
        }
    }

    // The startup scan.
    std::vector<std::string> recoverable;
    for (const Json& meeting : find_recoverable_meetings(*sql.repo, sql.clock)) {
        recoverable.push_back(meeting.at("id").get<std::string>());
    }
    CHECK(Json(recoverable) == expected.at("recoverable"));
    const Json after_scan = chunk_rows(*sql.repo, meeting_id);
    INFO("after scan " << after_scan.dump());
    CHECK(fv::golden::equal(expected.at("after_scan"), after_scan));

    // Finalize.
    SpeechRig rig;
    rig.script->set_responder(level_of);
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    NullModelLease lease;
    std::mutex mutex;
    std::pair<int, int> last{-1, -1};
    const bool finalized =
        finalize_meeting(*sql.repo, *sql.repo->get_meeting(meeting_id),
                         recording_factory(rig, sql.repo, calls), lease, [&](int done, int total) {
                             const std::lock_guard lock(mutex);
                             last = {done, total};
                         });

    CHECK(finalized == expected.at("finalized").get<bool>());
    CHECK(Json::array({last.first, last.second}) == expected.at("progress"));
    const Json chunks = chunk_rows(*sql.repo, meeting_id);
    INFO("chunks " << chunks.dump());
    CHECK(fv::golden::equal(expected.at("chunks"), chunks));

    std::map<std::int64_t, Json> by_id;
    for (const Json& row : sql.repo->get_audio_chunks(meeting_id)) {
        by_id[row.at("id").get<std::int64_t>()] = row;
    }
    Json segments = Json::array();
    for (const Json& row : sql.repo->get_segments(meeting_id, -1.0, std::nullopt)) {
        const Json& chunk = by_id.at(row.at("chunk_id").get<std::int64_t>());
        segments.push_back({{"id", row.at("id")},
                            {"chunk", Json::array({chunk.at("channel"), chunk.at("seq")})},
                            {"channel", row.at("channel")},
                            {"start_s", row.at("start_s")},
                            {"end_s", row.at("end_s")},
                            {"text", row.at("text")}});
    }
    INFO("segments " << segments.dump());
    CHECK(fv::golden::matches(expected.at("segments"), segments, 1e-9));

    const Json ended = *sql.repo->get_meeting(meeting_id);
    CHECK(ended.at("status") == expected.at("status"));
    const Json ended_at = ended.value("ended_at", Json());
    CHECK((ended_at.is_string() && !ended_at.get<std::string>().empty()) ==
          expected.at("has_ended_at").get<bool>());
    const Json state = fv::py::loads(ended.at("state_json").get<std::string>());
    CHECK(fv::golden::equal(expected.at("finalization"), state.at("finalization")));

    Json files = Json::object();
    for (const auto& entry : std::filesystem::directory_iterator(spool)) {
        files[fv::path_to_utf8(entry.path().filename())] = fv::sha256_hex(file_bytes(entry.path()));
    }
    INFO("files " << files.dump());
    CHECK(fv::golden::equal(expected.at("files"), files));
}
