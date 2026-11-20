// The paired-computer queries of meeting/persist/repository.py (origin_rows.h): a whole meeting
// copied between computers through export_meeting_rows and replace_meeting_rows, as the record
// kinds copy it, and the owner queries. Not in OpenWhisper's tests; the golden vectors pin the
// same functions against OpenWhisper's repository.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "meeting_state/origin_rows.h"
#include "meeting_state/sql_repository.h"
#include "origin_support.h"
#include "pyport/errors.h"
#include "sql/db.h"

namespace {

using fv::Json;
using fv::meeting_state::export_meeting_rows;
using fv::meeting_state::OriginSpool;
using fv::meeting_state::RecordOwner;
using fv::meeting_state::replace_meeting_rows;
using fv::meeting_state::origin_test::b64_untagged;
using fv::meeting_state::origin_test::dump;
using fv::meeting_state::origin_test::OriginDatabase;

Json session(const std::string& id, const Json& origin = nullptr, const Json& name = nullptr) {
    return {{"id", id},
            {"title", "Meeting " + id},
            {"status", "ended"},
            {"started_at", "2026-03-02T09:00:00+00:00"},
            {"ended_at", "2026-03-02T10:00:00+00:00"},
            {"paused_total_s", 0.0},
            {"host_token", "h-" + id},
            {"guest_token", "g-" + id},
            {"cloud_enabled", true},
            {"spool_dir", "C:/meetings/" + id},
            {"state_json", R"({"title": "x"})"},
            {"state_seq", 4},
            {"origin_device_id", origin},
            {"origin_device_name", name}};
}

Json chunk(std::int64_t id, const std::string& meeting, const std::string& channel,
           std::int64_t seq, const std::string& status = "done") {
    return {{"id", id},
            {"meeting_id", meeting},
            {"channel", channel},
            {"seq", seq},
            {"file_path", "C:/meetings/" + meeting + "/" + channel + ".wav"},
            {"start_s", 0.0},
            {"duration_s", 20.0},
            {"sample_rate", 16000},
            {"asr_status", status},
            {"asr_attempts", 1},
            {"asr_error", nullptr}};
}

Json segment(const std::string& id, const std::string& meeting, const Json& chunk_id,
             const std::string& text) {
    return {{"id", id},
            {"meeting_id", meeting},
            {"chunk_id", chunk_id},
            {"channel", "mic"},
            {"start_s", 1.0},
            {"end_s", 2.5},
            {"text", text},
            {"speaker_participant_id", nullptr},
            {"speaker_source", "channel"},
            {"speaker_pinned", true},
            {"embedding", {{"$b64", "AAECAw=="}}},
            {"created_at", "2026-03-02T09:00:05+00:00"}};
}

Json participant(const std::string& id, const std::string& meeting) {
    return {{"id", id},
            {"meeting_id", meeting},
            {"display_name", "Ana"},
            {"kind", "me"},
            {"name_source", "human"},
            {"is_provisional", false},
            {"created_at", "2026-03-02T09:00:00+00:00"},
            {"updated_at", "2026-03-02T09:00:00+00:00"}};
}

Json event(std::int64_t id, const std::string& meeting, std::int64_t seq) {
    return {{"id", id},
            {"meeting_id", meeting},
            {"seq", seq},
            {"ts", "2026-03-02T09:06:00+00:00"},
            {"actor_type", "host"},
            {"actor_id", nullptr},
            {"action", "add_item"},
            {"target_id", nullptr},
            {"payload_json", "{}"},
            {"inverse_json", nullptr}};
}

// A meeting recorded on the laptop: chunks 4 and 6, a segment of each and one typed note.
Json laptop_meeting() {
    return {{"session", session("m_trip")},
            {"chunks", {chunk(4, "m_trip", "mic", 0), chunk(6, "m_trip", "loopback", 0)}},
            {"segments",
             {segment("sg_1", "m_trip", 4, "mic words"), segment("sg_2", "m_trip", 6, "far words"),
              segment("sg_3", "m_trip", nullptr, "typed note")}},
            {"participants", {participant("p_me", "m_trip")}},
            {"events", {event(9, "m_trip", 1), event(12, "m_trip", 2)}}};
}

// The host already holds one meeting of its own; its chunk ids run past the laptop's first one.
Json host_meeting() {
    return {{"session", session("m_host")},
            {"chunks", {chunk(1, "m_host", "mic", 0), chunk(5, "m_host", "mic", 1)}},
            {"segments", {segment("sg_h", "m_host", 5, "host words")}},
            {"events", {event(1, "m_host", 1)}}};
}

// What the record kinds hand replace_meeting_rows: chunk ids become chunk_ref, event ids go.
Json copy_rows(const Json& exported) {
    Json rows = Json::object();
    for (const std::string_view key :
         {"chunks", "segments", "participants", "state_items", "questions", "events"}) {
        Json items = Json::array();
        for (Json row : exported.at(std::string(key))) {
            if (key == "chunks") {
                row["chunk_ref"] = row.at("id");
                row.erase("id");
            } else if (key == "events") {
                row.erase("id");
            }
            items.push_back(std::move(row));
        }
        rows[std::string(key)] = std::move(items);
    }
    return rows;
}

std::vector<std::int64_t> ids_of(const Json& rows) {
    std::vector<std::int64_t> ids;
    for (const Json& row : rows) {
        ids.push_back(row.at("id").get<std::int64_t>());
    }
    return ids;
}

} // namespace

TEST_CASE("a meeting exported and replaced into another database keeps every row",
          "[origin_rows]") {
    OriginDatabase laptop(Json::array({laptop_meeting()}));
    OriginDatabase host(Json::array({host_meeting()}));
    const Json host_before = export_meeting_rows(*host.database, "m_host");

    const Json exported = export_meeting_rows(*laptop.database, "m_trip");
    REQUIRE(exported.is_object());
    CHECK(exported.at("session").at("cloud_enabled") == true);
    CHECK(exported.at("segments").at(0).at("speaker_pinned") == true);
    CHECK(exported.at("segments").at(0).at("embedding").is_binary());
    CHECK(exported.at("segments").at(0).at("embedding").get_binary() ==
          Json::binary_t({0, 1, 2, 3}));

    Json values = exported.at("session");
    values["origin_device_id"] = "dev-laptop";
    values["origin_device_name"] = "Laptop";
    values["spool_dir"] = "D:/host/m_trip";
    Json rows = copy_rows(exported);
    // A segment of a chunk the copy does not carry.
    rows["segments"].push_back(b64_untagged(segment("sg_4", "m_trip", 77, "lost chunk")));
    replace_meeting_rows(*host.database, "m_trip", values, rows);

    const Json stored = export_meeting_rows(*host.database, "m_trip");
    CHECK(stored.at("session") == values);
    // New ids after the host's own chunks; segments follow their chunk.
    CHECK(ids_of(stored.at("chunks")) == std::vector<std::int64_t>{6, 7});
    std::vector<Json> chunk_ids;
    for (const Json& row : stored.at("segments")) {
        chunk_ids.push_back(row.at("chunk_id"));
    }
    CHECK(chunk_ids == std::vector<Json>{6, 7, nullptr, nullptr});
    for (std::size_t index = 0; index < 2; ++index) {
        Json expected = exported.at("chunks").at(index);
        expected["id"] = stored.at("chunks").at(index).at("id");
        CHECK(stored.at("chunks").at(index) == expected);
    }
    CHECK(stored.at("participants") == exported.at("participants"));
    CHECK(ids_of(stored.at("events")) == std::vector<std::int64_t>{2, 3});
    CHECK(stored.at("segments").at(0).at("embedding").get_binary() == Json::binary_t({0, 1, 2, 3}));

    CHECK(export_meeting_rows(*host.database, "m_host") == host_before);
    CHECK(export_meeting_rows(*host.database, "m_missing").is_null());
}

TEST_CASE("replacing a meeting again leaves no rows of the earlier copy", "[origin_rows]") {
    OriginDatabase host(Json::array({host_meeting(), laptop_meeting()}));
    const Json exported = export_meeting_rows(*host.database, "m_trip");
    Json rows = copy_rows(exported);
    rows["chunks"].erase(1);
    rows["segments"] = Json::array({rows["segments"][0]});
    rows["participants"] = Json::array();

    replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows);

    const Json stored = export_meeting_rows(*host.database, "m_trip");
    CHECK(stored.at("chunks").size() == 1);
    CHECK(stored.at("segments").size() == 1);
    CHECK(stored.at("participants").empty());
    const Json after = dump(*host.database);
    CHECK(after.at("delivery") == Json::array({{"sg_h", "m_host"}, {"sg_1", "m_trip"}}));
}

TEST_CASE("replace_meeting_rows rolls back on a bad row", "[origin_rows]") {
    OriginDatabase host(Json::array({host_meeting(), laptop_meeting()}));
    const Json before = dump(*host.database);
    const Json exported = export_meeting_rows(*host.database, "m_trip");

    SECTION("a constraint SQLite refuses") {
        Json rows = copy_rows(exported);
        rows["participants"][0].erase("display_name");
        CHECK_THROWS_AS(
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows),
            fv::sql::Error);
    }
    SECTION("a column the table lacks") {
        Json rows = copy_rows(exported);
        rows["events"][1]["bogus"] = 1;
        try {
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows);
            FAIL("expected TypeError");
        } catch (const fv::py::TypeError& error) {
            CHECK(std::string(error.what()) ==
                  "'bogus' is an invalid keyword argument for MeetingEvent");
        }
    }
    SECTION("values SQLAlchemy's bind processors refuse") {
        // SQLAlchemy wraps what a bind processor raises in StatementError.
        const auto statement_error = [&](const Json& rows) {
            try {
                replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows);
            } catch (const fv::py::PyError& error) {
                CHECK(error.type_name() == "StatementError");
                return std::string(error.what());
            }
            FAIL("expected StatementError");
            return std::string();
        };
        Json rows = copy_rows(exported);
        rows["segments"][0]["speaker_pinned"] = 2;
        CHECK(statement_error(rows) == "(builtins.ValueError) Value 2 is not None, True, or False");
        rows["segments"][0]["speaker_pinned"] = 2.0;
        CHECK(statement_error(rows) == "(builtins.TypeError) Not a boolean value: 2.0");
        rows["segments"][0]["speaker_pinned"] = "yes";
        CHECK(statement_error(rows) == "(builtins.TypeError) Not a boolean value: 'yes'");
        rows["segments"][0]["speaker_pinned"] = 1.0;
        rows["segments"][0]["embedding"] = "AAECAw==";
        CHECK(statement_error(rows) ==
              "(builtins.TypeError) memoryview: a bytes-like object is required, not 'str'");
    }
    SECTION("a key that is no column is refused before a value SQLAlchemy refuses") {
        // Python constructs every row before the flush at commit binds any of them.
        Json rows = copy_rows(exported);
        rows["segments"][0]["speaker_pinned"] = "yes";
        rows["events"][0]["bogus"] = 1;
        CHECK_THROWS_AS(
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows),
            fv::py::TypeError);
    }
    SECTION("a chunk without chunk_ref, or rows that are not a mapping") {
        Json rows = copy_rows(exported);
        rows["chunks"][0].erase("chunk_ref");
        try {
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows);
            FAIL("expected KeyError");
        } catch (const fv::py::PyError& error) {
            CHECK(error.type_name() == "KeyError");
            CHECK(std::string(error.what()) == "'chunk_ref'");
        }
        try {
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), Json::array());
            FAIL("expected AttributeError");
        } catch (const fv::py::PyError& error) {
            CHECK(error.type_name() == "AttributeError");
            CHECK(std::string(error.what()) == "'list' object has no attribute 'get'");
        }
        try {
            replace_meeting_rows(*host.database, "m_trip", Json::array(), copy_rows(exported));
            FAIL("expected TypeError");
        } catch (const fv::py::TypeError& error) {
            CHECK(std::string(error.what()) == "services.models.MeetingSession() argument after "
                                               "** must be a mapping, not list");
        }
        rows = copy_rows(exported);
        rows["chunks"][0] = 5;
        try {
            replace_meeting_rows(*host.database, "m_trip", exported.at("session"), rows);
            FAIL("expected TypeError");
        } catch (const fv::py::TypeError& error) {
            CHECK(std::string(error.what()) == "'int' object is not iterable");
        }
    }
    CHECK(dump(*host.database) == before);
}

TEST_CASE("record owners, renames and spools of paired computers", "[origin_rows]") {
    Json desk = host_meeting();
    desk["session"] = session("m_desk", "dev-desk", nullptr);
    desk.erase("chunks");
    desk.erase("segments");
    desk.erase("events");
    Json first = laptop_meeting();
    first["session"] = session("m_trip", "dev-laptop", "Laptop");
    OriginDatabase db(Json::array({host_meeting(), desk, first}));

    using fv::meeting_state::record_owners;
    CHECK(
        record_owners(*db.database) ==
        std::vector<RecordOwner>{{"dev-desk", "Unnamed computer", 1}, {"dev-laptop", "Laptop", 1}});
    CHECK(fv::meeting_state::rename_origin(*db.database, "dev-desk", "Desk") == 1);
    CHECK(fv::meeting_state::rename_origin(*db.database, "dev-none", "Nobody") == 0);
    CHECK(record_owners(*db.database).front() == RecordOwner{"dev-desk", "Desk", 1});
    CHECK(export_meeting_rows(*db.database, "m_desk").at("session").at("origin_device_id") ==
          "dev-desk");

    CHECK(fv::meeting_state::origin_spools(*db.database, "dev-laptop") ==
          std::vector<OriginSpool>{{"m_trip", "C:/meetings/m_trip"}});
    CHECK(fv::meeting_state::origin_spools(*db.database, "dev-none").empty());
    CHECK(fv::meeting_state::count_unfinished_chunks(*db.database, "m_host") == 0);
}
