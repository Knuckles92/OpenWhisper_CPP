#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <span>

#include "base/atomic_file.h"
#include "base/base64.h"
#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "download/verified_files.h"
#include "history/repository.h"
#include "meeting_state/origin_rows.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::remote_records;
using namespace std::chrono_literals;

TEST_CASE("remote record names reject traversal and Windows aliases") {
    for (const auto* name : {"../a.wav", "/absolute", "C:/file", "a//b", "a/../b", "a/b/c/d/e",
                             "audio/CON.wav", "x."}) {
        CHECK_THROWS_AS(safe_name(name), py::ValueError);
    }
    CHECK(safe_name("audio/clip-03.wav") == "audio/clip-03.wav");
    CHECK(safe_name("meeting/guests/12/source.wav") == "meeting/guests/12/source.wav");
    CHECK(safe_record_id("ab_123-a") == "ab_123-a");
    CHECK_THROWS_AS(safe_record_id("../id"), py::ValueError);
    CHECK_THROWS_AS(safe_kind("invented"), py::ValueError);
}

TEST_CASE("a failed backup pause reopens the record gate") {
    RecordGate gate;
    {
        const auto active = gate.request();
        CHECK_THROWS_AS(gate.paused(1ms), RecordsBusy);
        const auto second = gate.request();
        (void)second;
    }
    {
        const auto pause = gate.paused(1ms);
        CHECK_THROWS_AS(gate.request(), RecordsBusy);
        CHECK_THROWS_AS(gate.paused(1ms), RecordsBusy);
    }
    const auto request = gate.request();
    (void)request;
}

TEST_CASE("a resumed dictation upload is badged with its authenticated owner") {
    testkit::TempDataRoot data;
    const auto path = data.path();
    auto db = Database::open_or_create(path / "openwhisper-cpp.db");
    HistoryRepository history(db);
    FakeClock clock;
    HostRecordStore store(path / "transfers",
                          {{"dictation", dictation_records(path / "recordings", db)}}, clock,
                          [](const std::filesystem::path&) { return 8ULL << 30; });

    const remote::PairedDevice device{.id = "owner_1", .name = "Laptop"};
    const remote::PairedDevice stranger{.id = "owner_2", .name = "Other laptop"};
    const Json key{{"kind", "dictation"}, {"record_id", "dict_1"}};
    const auto call = [&](std::string_view operation, const remote::PairedDevice& owner,
                          Json fields = Json::object(), std::string_view bytes = {}) {
        fields.update(key);
        return store.handle(operation, fields, std::as_bytes(std::span(bytes.data(), bytes.size())),
                            owner);
    };
    std::string audio("RIFF");
    audio.append(4, char{0});
    audio += "WAVE";
    const auto record = Json{{"kind", "dictation"},
                             {"entry",
                              {{"id", "dict_1"},
                               {"text", "synthetic text"},
                               {"timestamp", "2026-01-01T00:00:00+00:00"},
                               {"model", "parakeet"},
                               {"origin_device_id", "owner_2"},
                               {"app_id", "private-source"}}},
                             {"entry_ext", {{"language", "fr"}, {"unknown", true}}},
                             {"audio", "audio/voice.wav"}};
    const auto manifest = record.dump();
    const Json begin{{"files", 2}, {"bytes", manifest.size() + audio.size()}};
    CHECK(call("records_begin", device, begin)["done"].empty());
    auto manifest_file = path / "manifest";
    write_file_atomic(manifest_file, manifest);
    Json manifest_fields{{"name", "record.json"},
                         {"size", manifest.size()},
                         {"sha256", download::sha256_file(manifest_file)},
                         {"offset", 0}};
    const auto half = manifest.size() / 2;
    CHECK(call("records_put", device, manifest_fields,
               std::string_view(manifest).substr(0, half))["received"] == half);
    CHECK(call("records_put", device, manifest_fields, manifest)["resume"] == true);
    CHECK(call("records_begin", device, begin)["received"]["record.json"] == half);
    manifest_fields["offset"] = half;
    CHECK(call("records_put", device, manifest_fields,
               std::string_view(manifest).substr(half))["done"] == true);

    auto audio_file = path / "audio";
    write_file_atomic(audio_file, audio);
    const Json audio_fields{{"name", "audio/voice.wav"},
                            {"size", audio.size()},
                            {"sha256", download::sha256_file(audio_file)},
                            {"offset", 0}};
    CHECK(call("records_put", device, audio_fields, audio)["done"] == true);
    CHECK(call("records_commit", device)["stored"] == true);
    const auto row = history.get_history_entry_by_id("dict_1");
    REQUIRE(row);
    CHECK(row->origin_device_id == device.id);
    CHECK(row->origin_device_name == device.name);
    CHECK(row->language == "fr");
    CHECK_FALSE(row->app_id);
    CHECK(call("records_list", stranger)["records"].empty());
    CHECK_THROWS_AS(call("records_open", stranger), RecordNotFound);
    CHECK(call("records_open", device)["files"].size() == 2);
    const auto fetched =
        call("records_fetch", device, Json{{"name", "audio/recording.wav"}, {"offset", 0}});
    CHECK(fetched["data"] == b64encode(audio));
    CHECK(fetched["eof"] == true);
    CHECK(call("records_delete", device)["deleted"] == true);
    CHECK_FALSE(history.get_history_entry_by_id("dict_1"));
}

TEST_CASE("finished shared meetings retain owner-scoped rows and portable recordings") {
    testkit::TempDataRoot data;
    const auto root = data.path();
    auto db = Database::open_or_create(root / "openwhisper-cpp.db");
    FakeClock clock;
    auto records = meeting_records(root / "meetings", db, clock);
    const remote::PairedDevice owner{.id = "laptop", .name = "Synthetic laptop"};
    const auto staging = root / "incoming";
    std::filesystem::create_directories(staging / "audio");
    std::string riff("RIFF");
    riff.append(4, char{0});
    riff += "WAVE";
    write_file_atomic(staging / "audio" / "chunk.wav", riff);
    Json session{
        {"id", "meeting_1"},
        {"title", "Synthetic Planning"},
        {"status", "ended"},
        {"started_at", "2026-02-20T20:00:00+00:00"},
        {"ended_at", "2026-02-20T20:01:00+00:00"},
        {"host_token", "guest-supplied token"},
        {"guest_token", "another guest-supplied token"},
        {"spool_dir", "../outside"},
        {"origin_device_id", "impostor"},
        {"cloud_enabled", true},
        {"state_json",
         R"({"participants":{"reused-speaker":{"id":"reused-speaker","display_name":"Speaker"}}})"},
        {"asr_remote_json", R"({"fingerprint":"not-our-host"})"},
        {"agent_endpoint_json", R"({"url":"not-our-endpoint"})"}};
    Json chunk{{"id", 77},
               {"channel", "mic"},
               {"seq", 0},
               {"file_path", "chunk.wav"},
               {"start_s", 0.0},
               {"duration_s", 1.0},
               {"sample_rate", 16000}};
    Json segment{{"id", "segment-1"},
                 {"chunk_id", 77},
                 {"channel", "mic"},
                 {"start_s", 0.1},
                 {"end_s", 0.3},
                 {"text", "synthetic agenda"},
                 {"speaker_participant_id", "reused-speaker"},
                 {"embedding", {{"$b64", b64encode(std::string(4, char{0}))}}},
                 {"created_at", "2026-02-20T20:00:01+00:00"}};
    Json record{{"kind", "meeting"},
                {"format", 1},
                {"session", session},
                {"chunks", Json::array({chunk})},
                {"segments", Json::array({segment})},
                {"participants", Json::array({
                                     {{"id", "reused-speaker"},
                                      {"display_name", "Speaker"},
                                      {"kind", "person"},
                                      {"created_at", "2026-02-20T20:00:00Z"},
                                      {"updated_at", "2026-02-20T20:00:00Z"}},
                                 })},
                {"files", {{"audio/chunk.wav", riff.size()}}}};

    auto active = record;
    active["session"]["status"] = "active";
    CHECK_THROWS_AS(records->import_record("meeting_1", staging, active, owner), py::ValueError);
    CHECK(records->import_record("meeting_1", staging, record, owner).at("files") == 1);
    CHECK(records->import_record("meeting_1", staging, record, owner).at("unchanged") == true);

    meeting_state::SqlMeetingRepository repository(db, clock);
    const auto saved = repository.get_meeting("meeting_1");
    REQUIRE(saved);
    CHECK(saved->at("origin_device_id") == "laptop");
    CHECK(saved->at("origin_device_name") == "Synthetic laptop");
    CHECK(saved->at("host_token") != "guest-supplied token");
    CHECK(saved->at("asr_remote_json").is_null());
    CHECK(saved->at("agent_endpoint_json").is_null());
    CHECK(saved->at("cloud_enabled") == false);
    const auto delivery = db.read([](sql::Connection& connection) {
        auto query = connection.prepare("SELECT COUNT(*) FROM meeting_segment_delivery");
        query.step();
        return std::get<std::int64_t>(query.column(0));
    });
    CHECK(delivery == 0);
    const auto imported = meeting_state::export_meeting_rows(db, "meeting_1");
    CHECK(imported["segments"][0]["chunk_id"] == imported["chunks"][0]["id"]);
    CHECK(records->owns("laptop", "meeting_1"));
    CHECK_FALSE(records->owns("impostor", "meeting_1"));
    CHECK(records->list("impostor", "", 20).empty());
    CHECK(records->list("laptop", "Planning", 20).size() == 1);
    CHECK(records->summary("laptop").at("count") == 1);
    CHECK(records->owners().front().counts.at("meeting") == 1);

    const auto exported = root / "export";
    std::filesystem::create_directories(exported);
    CHECK_THROWS_AS(records->export_record("impostor", "meeting_1", exported), RecordNotFound);
    const auto files = records->export_record("laptop", "meeting_1", exported);
    CHECK(std::filesystem::file_size(files.at("audio/chunk.wav")) == riff.size());
    std::ifstream content(exported / "record.json");
    const auto manifest = Json::parse(content);
    CHECK_FALSE(manifest["session"].contains("host_token"));
    CHECK_FALSE(manifest["session"].contains("spool_dir"));
    CHECK(manifest["chunks"][0]["file_path"] == "chunk.wav");
    CHECK(manifest["segments"][0]["embedding"]["$b64"] == b64encode(std::string(4, char{0})));

    const auto speaker = imported["participants"][0]["id"].get<std::string>();
    CHECK(imported["segments"][0]["speaker_participant_id"] == speaker);
    CHECK(Json::parse(saved->at("state_json").get<std::string>())
              .at("participants")
              .contains(speaker));
    auto repeated = record;
    repeated["session"]["id"] = "meeting_2";
    CHECK(records->import_record("meeting_2", staging, repeated, owner).at("files") == 1);
    const auto other = meeting_state::export_meeting_rows(db, "meeting_2");
    CHECK(other["participants"][0]["id"] != speaker);
    CHECK(other["segments"][0]["speaker_participant_id"] == other["participants"][0]["id"]);
    CHECK(other["segments"][0]["id"] != imported["segments"][0]["id"]);

    auto malformed = record;
    malformed["session"]["id"] = "invalid_meeting";
    malformed["segments"][0]["embedding"]["$b64"] = std::string(100'000, 'A');
    CHECK_THROWS_AS(records->import_record("invalid_meeting", staging, malformed, owner),
                    py::ValueError);
    CHECK_FALSE(repository.get_meeting("invalid_meeting"));
    malformed = record;
    malformed["session"]["id"] = "invalid_meeting";
    malformed["files"]["audio/../outside.wav"] = 4;
    CHECK_THROWS_AS(records->import_record("invalid_meeting", staging, malformed, owner),
                    py::ValueError);
    CHECK_FALSE(repository.get_meeting("invalid_meeting"));

    auto changed = record;
    changed["segments"][0]["text"] = "distinct local version";
    CHECK_THROWS_AS(records->import_record("meeting_1", staging, changed, owner),
                    std::runtime_error);
    CHECK(meeting_state::export_meeting_rows(db, "meeting_1")["segments"][0]["text"] ==
          "synthetic agenda");
    CHECK_FALSE(records->remove("impostor", "meeting_1"));
    CHECK(records->remove("laptop", "meeting_1"));
    CHECK_FALSE(repository.get_meeting("meeting_1"));
}
