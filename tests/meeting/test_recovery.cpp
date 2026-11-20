// tests/test_meeting_recovery.py: the crash-recovery scan, reconciliation and headless finalize.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "meeting/model_lease.h"
#include "meeting/recovery.h"
#include "meeting_capture/session_audio.h"
#include "meeting_state/schema.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "recovery_support.h"

using fv::Json;
using namespace fv::meeting;
using namespace fv::meeting::test;
using Catch::Matchers::ContainsSubstring;

namespace {

std::int64_t own_pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

// 2026-01-01T12:00:00 UTC; the zone is UTC, so naive ISO text reads as UTC too.
const fv::FakeClock& fixed_clock() {
    static const fv::FakeClock clock(
        std::chrono::sys_days{std::chrono::year{2026} / 1 / 1} + std::chrono::hours(12), "UTC");
    return clock;
}

// _iso_age: naive ISO text `seconds` before the clock's now.
std::string iso_age(double seconds) {
    const auto at = fixed_clock().utc() - std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::duration<double>(seconds));
    return std::format("{:%Y-%m-%dT%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(at));
}

std::string dumps(const Json& value) {
    return fv::py::dumps(value);
}

Json state_payload(const std::string& meeting_id, const std::string& status, bool cloud_enabled,
                   const std::string& title = "") {
    fv::meeting_state::MeetingState state;
    state.meeting_id = meeting_id;
    state.status = status;
    state.cloud_enabled = cloud_enabled;
    state.title = title;
    return state.to_json();
}

std::vector<std::string> ids_of(const std::vector<Json>& rows) {
    std::vector<std::string> ids;
    for (const Json& row : rows) {
        ids.push_back(row.at("id").get<std::string>());
    }
    return ids;
}

// _pending_chunk: a registered mic chunk with its WAV written.
std::int64_t pending_chunk(ScriptedRepository& repo, const std::filesystem::path& folder,
                           const std::string& meeting_id = "m_crash") {
    const std::filesystem::path wav = folder / "chunk.wav";
    write_wav(wav, std::vector<std::int16_t>(16000, 1000));
    return repo.inner.register_chunk({{"meeting_id", meeting_id},
                                      {"channel", "mic"},
                                      {"seq", 0},
                                      {"file_path", fv::path_to_utf8(wav)},
                                      {"start_s", 1.5},
                                      {"duration_s", 4.0},
                                      {"sample_rate", 16000}});
}

void create_inner_meeting(ScriptedRepository& repo, const std::string& meeting_id) {
    repo.inner.create_meeting({{"id", meeting_id},
                               {"title", "Crash"},
                               {"status", "active"},
                               {"started_at", "2026-01-01T09:00:00"},
                               {"host_token", "host"},
                               {"guest_token", "guest"},
                               {"cloud_enabled", false},
                               {"spool_dir", "/tmp/spool"}});
}

// _create_meeting of the reconciliation tests.
Json create_meeting(SqlRepo& sql, const std::filesystem::path& spool_dir,
                    const std::string& meeting_id = "m_reconcile",
                    const std::string& status = "active") {
    sql.repo->create_meeting({{"id", meeting_id},
                              {"title", "Interrupted capture"},
                              {"status", status},
                              {"started_at", "2026-01-01T09:00:00"},
                              {"host_token", "host"},
                              {"guest_token", "guest"},
                              {"cloud_enabled", false},
                              {"spool_dir", fv::path_to_utf8(spool_dir)}});
    return *sql.repo->get_meeting(meeting_id);
}

Json chunk_fields(const std::string& meeting_id, const std::filesystem::path& wav,
                  const std::string& status) {
    return {{"meeting_id", meeting_id},
            {"channel", "mic"},
            {"seq", 0},
            {"file_path", fv::path_to_utf8(wav)},
            {"start_s", 0.0},
            {"duration_s", 1.0},
            {"sample_rate", 16000},
            {"asr_status", status}};
}

} // namespace

TEST_CASE("test_mark_ended_normalizes_interrupted_finalization", "[meeting][recovery]") {
    // Headless ASR finalize must leave a durable finalization outcome.
    Json payload = state_payload("m_crash", "active", true, "Crash mid-finalize");
    payload["finalization"] = {{"status", "running"},
                               {"message", "Preparing final cloud insights…"}};
    const Json meeting = {
        {"id", "m_crash"}, {"cloud_enabled", true}, {"state_json", dumps(payload)}};
    ScriptedRepository repo;

    mark_ended(repo, meeting, fixed_clock());

    const auto updates = repo.updates();
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].first == "m_crash");
    const Json& fields = updates[0].second;
    CHECK(fields.at("status") == "ended");
    CHECK(fields.contains("ended_at"));
    const Json patched = fv::py::loads(fields.at("state_json").get<std::string>());
    CHECK(patched.at("status") == "ended");
    CHECK(patched.at("finalization").at("status") == "failed");
    CHECK_THAT(fv::py::lower(patched.at("finalization").at("message").get<std::string>()),
               ContainsSubstring("interrupted"));
}

TEST_CASE("test_mark_ended_settles_a_pending_finalization", "[meeting][recovery]") {
    const auto settle = [](const std::string& meeting_id, const std::string& status,
                           bool cloud_enabled) {
        Json payload = state_payload(meeting_id, status, cloud_enabled);
        payload["finalization"] = {{"status", "pending"}, {"message", ""}};
        const Json meeting = {
            {"id", meeting_id}, {"cloud_enabled", cloud_enabled}, {"state_json", dumps(payload)}};
        ScriptedRepository repo;
        mark_ended(repo, meeting, fixed_clock());
        const Json patched =
            fv::py::loads(repo.updates().at(0).second.at("state_json").get<std::string>());
        return patched.at("finalization").at("status").get<std::string>();
    };
    CHECK(settle("m_pending", "paused", true) == "unavailable");
    CHECK(settle("m_off", "active", false) == "disabled");
}

TEST_CASE("test_mark_ended_keeps_permanent_capture_gap_after_transcription",
          "[meeting][recovery]") {
    Json payload = state_payload("m_gap", "needs_recovery", false);
    payload["capture"]["audio_incomplete"] = true;
    payload["capture"]["integrity_error"] = "mic audio blocks were dropped";
    payload["capture"]["message"] = "mic audio blocks were dropped";
    ScriptedRepository repo;
    mark_ended(repo, {{"id", "m_gap"}, {"state_json", dumps(payload)}}, fixed_clock());

    const Json fields = repo.updates().back().second;
    CHECK(fields.at("status") == "failed");
    const Json saved = fv::py::loads(fields.at("state_json").get<std::string>());
    const auto reloaded = fv::meeting_state::MeetingState::from_json(saved);
    CHECK(reloaded.status == "failed");
    CHECK(reloaded.capture.at("audio_incomplete") == true);
    CHECK_THAT(reloaded.capture.at("integrity_error").get<std::string>(),
               ContainsSubstring("dropped"));
}

TEST_CASE("test_pid_alive_rejects_missing_and_dead_pids", "[meeting][recovery][platform]") {
    // A fresh heartbeat without a pid: no process to be alive.
    CHECK(is_session_dead({{"app_heartbeat_at", iso_age(5)}, {"app_pid", nullptr}}, fixed_clock()));
    CHECK_FALSE(pid_alive(0));
    CHECK_FALSE(pid_alive(-1));
    CHECK(pid_alive(own_pid()));
    CHECK_FALSE(pid_alive(2147483647));
}

TEST_CASE("test_is_session_dead_uses_heartbeat_then_pid", "[meeting][recovery][platform]") {
    const fv::FakeClock& clock = fixed_clock();
    CHECK(is_session_dead(Json::object(), clock));
    CHECK(is_session_dead({{"app_heartbeat_at", nullptr}}, clock));
    CHECK(is_session_dead({{"app_heartbeat_at", "not-a-timestamp"}}, clock));

    const Json stale = {{"app_heartbeat_at", iso_age(STALE_HEARTBEAT_S + 5)},
                        {"app_pid", own_pid()}};
    CHECK(is_session_dead(stale, clock));

    const Json live = {{"app_heartbeat_at", iso_age(5)}, {"app_pid", own_pid()}};
    CHECK_FALSE(is_session_dead(live, clock));

    const Json fresh_dead_pid = {{"app_heartbeat_at", iso_age(5)}, {"app_pid", 2147483647}};
    CHECK(is_session_dead(fresh_dead_pid, clock));
}

TEST_CASE("test_find_recoverable_meetings_keeps_only_dead_sessions", "[meeting][recovery]") {
    ScriptedRepository repo;
    repo.set_interrupted({
        {{"id", "m_live"}, {"app_heartbeat_at", iso_age(5)}, {"app_pid", own_pid()}},
        {{"id", "m_dead"},
         {"app_heartbeat_at", iso_age(STALE_HEARTBEAT_S + 10)},
         {"app_pid", own_pid()}},
        {{"id", "m_no_beat"}},
    });

    const std::vector<Json> recovered = find_recoverable_meetings(repo, fixed_clock());

    CHECK(ids_of(recovered) == std::vector<std::string>{"m_dead", "m_no_beat"});
}

TEST_CASE("test_find_recoverable_meetings_skips_deferred_cards", "[meeting][recovery]") {
    ScriptedRepository repo;
    Json payload = state_payload("m_kept", "needs_recovery", true);
    payload["finalization"] = {
        {"status", "unavailable"}, {"message", "ASR unfinished"}, {"card_deferred", true}};
    repo.set_interrupted({
        {{"id", "m_kept"}, {"status", "needs_recovery"}, {"state_json", dumps(payload)}},
        {{"id", "m_crash"}},
    });

    const std::vector<Json> recovered = find_recoverable_meetings(repo, fixed_clock());

    CHECK(ids_of(recovered) == std::vector<std::string>{"m_crash"});
}

TEST_CASE("test_find_recoverable_meetings_raises_on_query_failure", "[meeting][recovery]") {
    ScriptedRepository repo;
    repo.fail_list(true);
    try {
        find_recoverable_meetings(repo, fixed_clock());
        FAIL("no RecoveryScanError");
    } catch (const RecoveryScanError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("query interrupted"));
    }
}

TEST_CASE("test_find_recoverable_meetings_raises_on_reconciliation_inventory_failure",
          "[meeting][recovery]") {
    ScriptedRepository repo;
    repo.fail_list_meetings("disk unavailable");
    try {
        find_recoverable_meetings(repo, fixed_clock());
        FAIL("no RecoveryScanError");
    } catch (const RecoveryScanError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("list meetings"));
    }
}

TEST_CASE("test_finalize_meeting_rejects_missing_id", "[meeting][recovery]") {
    ScriptedRepository repo;
    NullModelLease lease;
    CHECK_FALSE(finalize_meeting(repo, Json::object(), {}, lease));
}

TEST_CASE("test_finalize_meeting_returns_false_when_pending_chunks_cannot_load",
          "[meeting][recovery]") {
    ScriptedRepository repo;
    repo.fail_pending(true);
    NullModelLease lease;
    CHECK_FALSE(finalize_meeting(repo, {{"id", "m_crash"}}, {}, lease));
    CHECK(repo.updates().empty());
}

TEST_CASE("test_finalize_meeting_with_no_pending_chunks_marks_ended", "[meeting][recovery]") {
    ScriptedRepository repo;
    NullModelLease lease;
    const Json meeting = {{"id", "m_empty"}, {"cloud_enabled", false}};

    CHECK(finalize_meeting(repo, meeting, {}, lease));
    CHECK(repo.reset_calls() == std::vector<std::string>{"m_empty"});
    REQUIRE_FALSE(repo.updates().empty());
    CHECK(repo.updates().back().first == "m_empty");
    CHECK(repo.updates().back().second.at("status") == "ended");
}

TEST_CASE("test_finalize_meeting_transcribes_pending_chunks", "[meeting][recovery]") {
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    const std::int64_t chunk_id = pending_chunk(*repo, root.path());
    SpeechRig rig;
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    // Reported from the engine's worker and from the caller.
    std::mutex progress_mutex;
    std::vector<std::pair<int, int>> progress;
    FinalizeOptions options;
    options.asr_language = "en";
    NullModelLease lease;

    const bool ok = finalize_meeting(
        *repo, {{"id", "m_crash"}, {"asr_model", "base"}}, recording_factory(rig, repo, calls),
        lease,
        [&](int done, int total) {
            const std::lock_guard lock(progress_mutex);
            progress.emplace_back(done, total);
        },
        {}, options);

    CHECK(ok);
    REQUIRE(calls->size() == 1);
    CHECK(calls->front().meeting_id == "m_crash");
    CHECK(calls->front().model == "base");
    CHECK(calls->front().language == "en");
    const auto decodes = rig.script->calls();
    REQUIRE(decodes.size() == 1);
    CHECK(decodes.front().first == fv::speech::Lane::Meeting);
    CHECK(decodes.front().second == "en");
    CHECK(repo->commits() == std::vector<std::tuple<std::string, std::int64_t, std::size_t>>{
                                 {"m_crash", chunk_id, 0}});
    CHECK(repo->updates().back().second.at("status") == "ended");
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == std::pair<int, int>{1, 1});
}

TEST_CASE("test_finalize_meeting_unavailable_asr_leaves_session_recoverable",
          "[meeting][recovery]") {
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    pending_chunk(*repo, root.path());
    SpeechRig rig;
    rig.script->set_ready({fv::speech::ReadyState::ModelMissing,
                           "Download Parakeet TDT 0.6B v3 in Downloads.",
                           "Download Parakeet TDT 0.6B v3 in Downloads."});
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    NullModelLease lease;

    CHECK_FALSE(
        finalize_meeting(*repo, {{"id", "m_crash"}}, recording_factory(rig, repo, calls), lease));
    CHECK(calls->size() == 1);
    CHECK(rig.script->calls().empty());
    CHECK(repo->updates().empty());
}

TEST_CASE("test_finalize_meeting_drain_timeout_does_not_mark_ended", "[meeting][recovery]") {
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    pending_chunk(*repo, root.path());
    SpeechRig rig;
    rig.select();
    rig.script->hold();
    // The held decode is let go once the drain has given up, so the engine can stop.
    std::thread release([&rig] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        rig.script->resume();
    });
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    FinalizeOptions options;
    options.drain_timeout = std::chrono::milliseconds(100);
    NullModelLease lease;

    const bool ok = finalize_meeting(*repo, {{"id", "m_crash"}},
                                     recording_factory(rig, repo, calls), lease, {}, {}, options);
    release.join();

    CHECK_FALSE(ok);
    CHECK(calls->size() == 1);
    for (const auto& [id, fields] : repo->updates()) {
        CHECK(fields.value("status", "") != "ended");
    }
}

TEST_CASE("test_finalize_meeting_unfinished_chunks_mark_needs_recovery", "[meeting][recovery]") {
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    pending_chunk(*repo, root.path());
    repo->set_unfinished(1);
    SpeechRig rig;
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    NullModelLease lease;

    CHECK_FALSE(
        finalize_meeting(*repo, {{"id", "m_crash"}}, recording_factory(rig, repo, calls), lease));
    REQUIRE_FALSE(repo->updates().empty());
    CHECK(repo->updates().back() ==
          std::pair<std::string, Json>{"m_crash", {{"status", "needs_recovery"}}});
}

namespace {

// A lease that records its calls, as test_meeting_refinalize.py's (acquire, release) lambdas do.
struct RecordingLease {
    std::shared_ptr<std::vector<std::string>> order = std::make_shared<std::vector<std::string>>();
    FunctionModelLease lease{[order = order] {
                                 order->emplace_back("acquire");
                                 return true;
                             },
                             [order = order] {
                                 order->emplace_back("release");
                             }};
};

} // namespace

TEST_CASE("a headless finalize never holds the model lease after it returns",
          "[meeting][recovery]") {
    // Spec acceptance: whatever fails, the lease taken around the engine is given back.
    const auto run =
        [](const std::function<AsrFactory(SpeechRig&, std::shared_ptr<ScriptedRepository>,
                                          std::shared_ptr<std::vector<std::string>>)>& factory,
           const std::function<void(SpeechRig&)>& prepare, FinalizeOptions options = {}) {
            fv::testkit::TempDataRoot root;
            auto repo = std::make_shared<ScriptedRepository>();
            create_inner_meeting(*repo, "m_crash");
            pending_chunk(*repo, root.path());
            SpeechRig rig;
            prepare(rig);
            rig.select();
            RecordingLease recording;
            const bool ok =
                finalize_meeting(*repo, {{"id", "m_crash"}}, factory(rig, repo, recording.order),
                                 recording.lease, {}, {}, options);
            return std::pair<bool, std::vector<std::string>>{ok, *recording.order};
        };
    // A real engine over the rig; "build" marks where the factory ran.
    const auto engine_factory = [](SpeechRig& rig, std::shared_ptr<ScriptedRepository> repo,
                                   std::shared_ptr<std::vector<std::string>> order) -> AsrFactory {
        return recording_factory(rig, std::move(repo), std::make_shared<std::vector<FactoryCall>>(),
                                 [order] { order->emplace_back("build"); });
    };
    const auto nothing = [](SpeechRig&) {
    };

    SECTION("success") {
        const auto [ok, order] = run(engine_factory, nothing);
        CHECK(ok);
        CHECK(order == std::vector<std::string>{"acquire", "build", "release"});
    }
    SECTION("the engine cannot load its model") {
        const auto [ok, order] = run(engine_factory, [](SpeechRig& rig) {
            rig.script->set_ready({fv::speech::ReadyState::ModelMissing,
                                   "Download Parakeet TDT 0.6B v3 in Downloads.",
                                   "Download Parakeet TDT 0.6B v3 in Downloads."});
        });
        CHECK_FALSE(ok);
        CHECK(order == std::vector<std::string>{"acquire", "build", "release"});
    }
    SECTION("no engine can be built") {
        const auto [ok, order] = run(
            [](SpeechRig&, std::shared_ptr<ScriptedRepository>,
               std::shared_ptr<std::vector<std::string>>) -> AsrFactory {
                return [](const std::string&, const std::string&, const std::string&) {
                    return std::unique_ptr<MeetingAsrEngine>();
                };
            },
            nothing);
        CHECK_FALSE(ok);
        CHECK(order == std::vector<std::string>{"acquire", "release"});
    }
    SECTION("building the engine throws") {
        const auto [ok, order] = run(
            [](SpeechRig&, std::shared_ptr<ScriptedRepository>,
               std::shared_ptr<std::vector<std::string>>) -> AsrFactory {
                return [](const std::string& id, const std::string&,
                          const std::string&) -> std::unique_ptr<MeetingAsrEngine> {
                    throw std::runtime_error("no engine for " + id);
                };
            },
            nothing);
        CHECK_FALSE(ok);
        CHECK(order == std::vector<std::string>{"acquire", "release"});
    }
    SECTION("the drain times out") {
        FinalizeOptions options;
        options.drain_timeout = std::chrono::milliseconds(50);
        std::thread resume;
        const auto [ok, order] = run(
            engine_factory,
            [&resume](SpeechRig& rig) {
                rig.script->hold();
                // Let go once the drain has given up, so the engine can stop.
                resume = std::thread([script = rig.script] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    script->resume();
                });
            },
            options);
        resume.join();
        CHECK_FALSE(ok);
        CHECK(order == std::vector<std::string>{"acquire", "build", "release"});
    }
}

TEST_CASE("a headless finalize continues without a lease whose acquire fails",
          "[meeting][recovery]") {
    // acquire_model_lease never throws: a failed acquire reads as nothing taken, and nothing is
    // given back.
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    pending_chunk(*repo, root.path());
    SpeechRig rig;
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    int released = 0;
    FunctionModelLease lease([]() -> bool { throw std::runtime_error("controller gone"); },
                             [&released] { ++released; });

    CHECK(finalize_meeting(*repo, {{"id", "m_crash"}}, recording_factory(rig, repo, calls), lease));
    CHECK(calls->size() == 1);
    CHECK(released == 0);
}

TEST_CASE("a headless finalize with nothing pending takes no lease", "[meeting][recovery]") {
    ScriptedRepository repo;
    RecordingLease recording;
    CHECK(finalize_meeting(repo, {{"id", "m_empty"}}, {}, recording.lease));
    CHECK(recording.order->empty());
}

TEST_CASE("test_reconcile_registers_orphan_wav_once", "[meeting][recovery]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    const Json meeting = create_meeting(sql, spool);
    const std::filesystem::path canonical = spool / "mic_00000.wav";
    const std::filesystem::path orphan = spool / "mic_00000.wav.orphan";
    write_arange_wav(canonical, 1.25);
    std::filesystem::rename(canonical, orphan);
    write_chunk_recovery_meta(orphan, "m_reconcile", "mic", 0, 2.5, 1.25);

    const ReconcileResult first = reconcile_meeting_audio(*sql.repo, meeting);
    const ReconcileResult second = reconcile_meeting_audio(*sql.repo, meeting);

    const std::vector<Json> rows = sql.repo->get_audio_chunks("m_reconcile");
    CHECK(first == ReconcileResult{1, 0});
    CHECK(second == ReconcileResult{0, 0});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("seq") == 0);
    CHECK(rows[0].at("start_s") == 2.5);
    CHECK(rows[0].at("duration_s") == 1.25);
    CHECK(rows[0].at("file_path") == fv::path_to_utf8(canonical));
    CHECK(std::filesystem::is_regular_file(canonical));
    CHECK_FALSE(std::filesystem::exists(orphan));
    CHECK_FALSE(std::filesystem::exists(chunk_recovery_meta_path(canonical)));
}

TEST_CASE("test_reconcile_sidecar_after_db_commit_does_not_duplicate", "[meeting][recovery]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    const Json meeting = create_meeting(sql, spool);
    const std::filesystem::path wav = spool / "mic_00000.wav";
    write_arange_wav(wav);
    sql.repo->register_chunk(chunk_fields("m_reconcile", wav, "pending"));
    write_chunk_recovery_meta(wav, "m_reconcile", "mic", 0, 0.0, 1.0);

    const ReconcileResult result = reconcile_meeting_audio(*sql.repo, meeting);

    CHECK(result == ReconcileResult{0, 0});
    CHECK(sql.repo->get_audio_chunks("m_reconcile").size() == 1);
}

TEST_CASE("test_reconcile_materializes_only_unregistered_pcm_tail", "[meeting][recovery]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    const Json meeting = create_meeting(sql, spool);
    const std::vector<std::int16_t> source = arange_int16(2 * 16000);
    write_bytes(spool / "mic_session.pcm", source);
    write_session_meta(spool / "mic_session.json", 16000, static_cast<std::int64_t>(source.size()),
                       0.0, 0.0);
    const std::filesystem::path first_wav = spool / "mic_00000.wav";
    write_arange_wav(first_wav);
    sql.repo->register_chunk(chunk_fields("m_reconcile", first_wav, "done"));

    const ReconcileResult first = reconcile_meeting_audio(*sql.repo, meeting);
    const ReconcileResult second = reconcile_meeting_audio(*sql.repo, meeting);

    const std::vector<Json> rows = sql.repo->get_audio_chunks("m_reconcile");
    CHECK(first == ReconcileResult{0, 1});
    CHECK(second == ReconcileResult{0, 0});
    REQUIRE(rows.size() == 2);
    const Json& recovered = rows[1];
    CHECK(recovered.at("seq") == 1);
    CHECK(recovered.at("start_s") == 1.0);
    CHECK(recovered.at("duration_s") == 1.0);
    CHECK(recovered.at("asr_status") == "pending");
    const auto wav_info =
        read_wav_duration(fv::path_from_utf8(recovered.at("file_path").get<std::string>()));
    REQUIRE(wav_info);
    CHECK(wav_info->first == 16000);
    CHECK(wav_info->second == 1.0);
}

TEST_CASE("test_reconcile_ignores_raw_file_already_converted_into_prefix", "[meeting][recovery]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    const Json meeting = create_meeting(sql, spool);
    const std::vector<std::int16_t> samples = arange_int16(16000);
    write_bytes(spool / "mic_session.16k.pcm", samples);
    // Windows refusing the best-effort unlink after conversion.
    write_bytes(spool / "mic_session.pcm", samples);
    write_session_meta(spool / "mic_session.json", 16000, static_cast<std::int64_t>(samples.size()),
                       0.0, 0.0, static_cast<std::int64_t>(samples.size()), false);

    const ReconcileResult result = reconcile_meeting_audio(*sql.repo, meeting);

    const std::vector<Json> rows = sql.repo->get_audio_chunks("m_reconcile");
    CHECK(result == ReconcileResult{0, 1});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("duration_s") == 1.0);
}

TEST_CASE("test_startup_scan_discovers_terminal_meeting_with_stranded_wav", "[meeting][recovery]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    const Json meeting = create_meeting(sql, spool, "m_reconcile", "ended");
    const std::filesystem::path canonical = spool / "loopback_00000.wav";
    const std::filesystem::path orphan = spool / "loopback_00000.wav.orphan";
    write_arange_wav(canonical, 0.5);
    std::filesystem::rename(canonical, orphan);
    write_chunk_recovery_meta(orphan, "m_reconcile", "loopback", 0, 4.0, 0.5);

    const std::vector<Json> recovered = find_recoverable_meetings(*sql.repo, fixed_clock());

    CHECK(ids_of(recovered) == std::vector<std::string>{"m_reconcile"});
    const std::vector<Json> chunks = sql.repo->get_audio_chunks("m_reconcile");
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].at("asr_status") == "pending");
}

// test_meeting_recovery.py asserts the backend's cleanup() and stop() before the lease's release:
// a headless finalize stops and destroys its engine (joining the worker) before it gives the
// dictation model back.
TEST_CASE("a headless finalize destroys its engine before the model lease is released",
          "[meeting][recovery]") {
    fv::testkit::TempDataRoot root;
    auto repo = std::make_shared<ScriptedRepository>();
    create_inner_meeting(*repo, "m_crash");
    pending_chunk(*repo, root.path());
    SpeechRig rig;
    rig.select();
    auto alive = std::make_shared<std::weak_ptr<void>>();
    auto order = std::make_shared<std::vector<std::string>>();
    FunctionModelLease lease(
        [order] {
            order->emplace_back("acquire");
            return true;
        },
        [order, alive] {
            order->emplace_back(alive->expired() ? "release after the engine"
                                                 : "release with the engine alive");
        });

    CHECK(finalize_meeting(*repo, {{"id", "m_crash"}}, tracking_factory(rig, repo, alive), lease));
    CHECK(*order == std::vector<std::string>{"acquire", "release after the engine"});
}
