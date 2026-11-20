// from tests/test_agent_api.py and tests/test_personalize_s9_history_edit.py (the agent half).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

#include "agentapi/history_routes.h"
#include "agentapi/history_store.h"
#include "base/base64.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "sql/db.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;

struct Fixture {
    fv::testkit::TempDataRoot root;
    std::filesystem::path database = make_test_database(root.path());
    HistoryApi api{database, kToken};
};

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("test_every_route_requires_token", "[test_agent_api]") {
    Fixture f;
    const auto [path, query] = GENERATE(table<std::string, Pairs>({
        {"/v1/status", {}},
        {"/v1/transcriptions", {}},
        {"/v1/meetings", {}},
        {"/v1/search", {{"q", "launch"}}},
        {"/openapi.json", {}},
    }));
    const std::string authorization =
        GENERATE(std::string(), std::string("Bearer wrong"), "Basic " + kToken);
    const Reply reply = get(f.api, path, query, {{"Authorization", authorization}});
    CHECK(reply.status() == 401);
    CHECK(reply.header("www-authenticate") == "Bearer");
    CHECK(reply.header("cache-control") == "no-store");
    CHECK(reply.json["error"]["code"] == "unauthorized");
}

TEST_CASE("test_token_is_not_accepted_in_query_or_cookie", "[test_agent_api]") {
    Fixture f;
    const Reply reply = get(f.api, "/v1/status", {{"token", kToken}},
                            {{"Authorization", ""}, {"Cookie", "token=" + kToken}});
    CHECK(reply.status() == 401);
}

TEST_CASE("test_host_and_browser_origin_are_rejected", "[test_agent_api]") {
    Fixture f;
    for (const auto& header : std::vector<std::pair<std::string, std::string>>{
             {"Host", "evil.example"}, {"Origin", "https://evil.example"}, {"Origin", "null"}}) {
        CHECK(get(f.api, "/v1/status", {}, {{header.first, header.second}}).status() == 403);
    }
    CHECK(get(f.api, "/v1/status", {}, {{"Host", "localhost:8766"}}).status() == 200);
}

TEST_CASE("allowed hosts join the trusted names", "[test_agent_api]") {
    fv::testkit::TempDataRoot root;
    HistoryApi api{make_test_database(root.path()), kToken, {"100.64.0.7", "desk.ts.net"}};
    CHECK(get(api, "/v1/status", {}, {{"Host", "100.64.0.7:8767"}}).status() == 200);
    CHECK(get(api, "/v1/status", {}, {{"Host", "desk.ts.net"}}).status() == 200);
    CHECK(get(api, "/v1/status", {}, {{"Host", "other.ts.net"}}).status() == 403);
}

TEST_CASE("test_transcription_pages_have_stable_ties_and_normalized_time", "[test_agent_api]") {
    Fixture f;
    const auto items = all_pages(f.api, "/v1/transcriptions", {{"limit", "1"}});
    CHECK(ids(items) == std::vector<std::string>{"h_c", "h_a", "h_b", "h_old"});
    for (const Json& row : items) {
        CHECK(fv::py::len(row.at("preview").get<std::string>()) <= 320);
        CHECK_FALSE(row.contains("text"));
    }
    CHECK(get(f.api, "/v1/transcriptions/h_a").json["raw_text"] == "raw phrase only");
}

TEST_CASE("test_cursor_survives_newer_insert_and_deleted_previous_row", "[test_agent_api]") {
    Fixture f;
    const Json first = get(f.api, "/v1/transcriptions", {{"limit", "1"}}).json;
    write_sql(f.database, "DELETE FROM transcription_history WHERE id = 'h_c'");
    add_history_entry(f.database, "h_new", "new", "2026-10-01T00:00:00Z", "test");
    const auto remaining =
        all_pages(f.api, "/v1/transcriptions",
                  {{"limit", "1"}, {"cursor", first["next_cursor"].get<std::string>()}});
    CHECK(ids(remaining) == std::vector<std::string>{"h_a", "h_b", "h_old"});
}

TEST_CASE("test_cursor_is_validated_and_bound_to_filters", "[test_agent_api]") {
    Fixture f;
    const std::string cursor =
        get(f.api, "/v1/transcriptions", {{"limit", "1"}}).json["next_cursor"].get<std::string>();
    for (const auto& [path, query] : std::vector<std::pair<std::string, Pairs>>{
             {"/v1/transcriptions", {{"cursor", "bad"}}},
             {"/v1/transcriptions", {{"cursor", cursor}, {"q", "launch"}}},
             {"/v1/transcriptions", {{"cursor", cursor}, {"include_remote", "true"}}},
             {"/v1/meetings", {{"cursor", cursor}}},
             {"/v1/transcriptions", {{"cursor", fv::b64encode("null", true)}}},
         }) {
        CAPTURE(path, query);
        CHECK(get(f.api, path, query).status() == 400);
    }
}

TEST_CASE("test_search_returns_fetchable_citations_and_raw_matches", "[test_agent_api]") {
    Fixture f;
    const auto items = all_pages(f.api, "/v1/search", {{"q", "launch"}, {"limit", "1"}});
    std::set<std::string> kinds;
    std::set<std::pair<std::string, std::string>> citations;
    for (const Json& row : items) {
        kinds.insert(row["kind"].get<std::string>());
        citations.emplace(row["kind"].get<std::string>(), row["id"].get<std::string>());
    }
    CHECK(kinds == std::set<std::string>{"transcription", "meeting", "segment"});
    CHECK(citations.size() == items.size());
    CHECK(items.size() == 7);
    for (const Json& row : items) {
        const std::string resource = row["resource"].get<std::string>();
        CAPTURE(resource);
        const Reply detail = get(f.api, resource);
        CHECK(detail.status() == 200);
        CHECK(detail.json["id"] == row["id"]);
    }
    const Json raw = get(f.api, "/v1/search", {{"q", "raw phrase"}}).json["items"];
    REQUIRE(raw.size() == 1);
    CHECK(raw[0]["matched_field"] == "raw_text");
    CHECK(contains(raw[0]["excerpt"].get<std::string>(), "raw phrase"));
    CHECK(get(f.api, "/v1/transcriptions", {{"q", "raw phrase"}}).json["items"][0]["id"] == "h_a");
    const Json late = get(f.api, "/v1/search", {{"q", "needle"}}).json["items"][0];
    CHECK(contains(late["excerpt"].get<std::string>(), "Needle"));
    CHECK(fv::py::len(late["excerpt"].get<std::string>()) <= 320);
}

TEST_CASE("test_search_treats_wildcards_and_sql_as_literal_text", "[test_agent_api]") {
    Fixture f;
    const std::string query = GENERATE("50%_", "' OR 1=1 --", "%", "_");
    const Json items = get(f.api, "/v1/search", {{"q", query}}).json["items"];
    REQUIRE(items.size() == 1);
    CHECK(items[0]["id"] == "h_b");
}

TEST_CASE("test_date_kind_and_title_filters", "[test_agent_api]") {
    Fixture f;
    const auto items = all_pages(f.api, "/v1/search",
                                 {{"q", "launch"},
                                  {"kind", "transcription"},
                                  {"since", "2026-09-01T00:00:00Z"},
                                  {"before", "2026-09-02T00:00:00Z"}});
    CHECK(ids(items) == std::vector<std::string>{"h_a"});
    CHECK_FALSE(get(f.api, "/v1/meetings", {{"q", "review"}}).json["items"].empty());
    CHECK(get(f.api, "/v1/meetings", {{"q", "Friday"}}).json["items"].empty());
    CHECK_FALSE(
        get(f.api, "/v1/search", {{"q", "Friday"}, {"kind", "meeting"}}).json["items"].empty());
}

TEST_CASE("test_non_ascii_excerpt_uses_same_matching_rules_as_search", "[test_agent_api]") {
    Fixture f;
    add_history_entry(f.database, "h_unicode", std::string(900, 'x') + " ÉTÉ 東京", kDate, "test");
    for (const std::string query : {"ÉTÉ", "東京"}) {
        const Json hit = get(f.api, "/v1/search", {{"q", query}}).json["items"][0];
        CHECK(hit["id"] == "h_unicode");
        CHECK(contains(hit["excerpt"].get<std::string>(), query));
    }
}

TEST_CASE("test_remote_records_require_explicit_opt_in_on_every_surface", "[test_agent_api]") {
    Fixture f;
    for (const std::string path :
         {"/v1/transcriptions/h_remote", "/v1/meetings/m_remote", "/v1/meetings/m_remote/segments",
          "/v1/meetings/m_remote/segments/sg_remote", "/v1/meetings/m_remote/insights"}) {
        CAPTURE(path);
        CHECK(get(f.api, path).status() == 404);
        CHECK(get(f.api, path, {{"include_remote", "true"}}).status() == 200);
    }
    for (const auto& [path, query, remote_id] :
         std::vector<std::tuple<std::string, Pairs, std::string>>{
             {"/v1/transcriptions", {}, "h_remote"},
             {"/v1/meetings", {}, "m_remote"},
             {"/v1/search", {{"q", "launch"}}, "sg_remote"}}) {
        const auto local = ids(all_pages(f.api, path, query));
        CHECK(std::find(local.begin(), local.end(), remote_id) == local.end());
        Pairs with_remote = query;
        with_remote.emplace_back("include_remote", "true");
        const auto all = ids(all_pages(f.api, path, with_remote));
        CHECK(std::find(all.begin(), all.end(), remote_id) != all.end());
    }
    for (const Json& hit :
         all_pages(f.api, "/v1/search", {{"q", "launch"}, {"include_remote", "true"}})) {
        CHECK(get(f.api, hit["resource"].get<std::string>()).status() == 200);
    }
}

TEST_CASE("test_segment_pagination_ranges_and_meeting_scope", "[test_agent_api]") {
    Fixture f;
    CHECK(ids(all_pages(f.api, "/v1/meetings/m_a/segments", {{"limit", "1"}})) ==
          std::vector<std::string>{"sg_a", "sg_b", "sg_c"});
    CHECK(ids(all_pages(f.api, "/v1/meetings/m_a/segments", {{"start_s", "0"}, {"end_s", "10"}})) ==
          std::vector<std::string>{"sg_a", "sg_b"});
    CHECK(get(f.api, "/v1/meetings/m_empty/segments/sg_a").status() == 404);
    CHECK(get(f.api, "/v1/meetings/m_missing/segments").status() == 404);
}

TEST_CASE("test_insights_preserve_evidence_and_omit_removed_items", "[test_agent_api]") {
    Fixture f;
    const Json insights = get(f.api, "/v1/meetings/m_a/insights").json;
    CHECK(insights["snapshot_available"] == true);
    CHECK(insights["state_seq"] == 7);
    CHECK(insights["rolling_summary_evidence"] == Json::array({"sg_a"}));
    REQUIRE(insights["cards"].size() == 1);
    CHECK(insights["cards"][0]["data"]["owner_participant_id"] == "p_a");
    CHECK(insights["cards"][0]["evidence"] == Json::array({"sg_a"}));
    CHECK(insights["participants"][0]["display_name"] == "Alex");
    CHECK(insights["questions"][0]["answer"] == "Friday");
    CHECK(insights["custom_reports"][0]["markdown"] == "Launch Friday");
    CHECK(get(f.api, "/v1/meetings/m_empty/insights").json["snapshot_available"] == false);
}

TEST_CASE("test_corrupt_snapshot_does_not_hide_transcript", "[test_agent_api]") {
    Fixture f;
    write_sql(f.database, "UPDATE meeting_sessions SET state_json = 'not json' WHERE id = 'm_a'");
    const Reply reply = get(f.api, "/v1/meetings/m_a/insights");
    CHECK(reply.status() == 503);
    CHECK(reply.json["error"]["code"] == "snapshot_unavailable");
    CHECK(get(f.api, "/v1/meetings/m_a/segments").status() == 200);
}

TEST_CASE("test_private_fields_are_never_serialized", "[test_agent_api]") {
    Fixture f;
    for (const auto& [path, query] :
         std::vector<std::pair<std::string, Pairs>>{{"/v1/transcriptions", {}},
                                                    {"/v1/transcriptions/h_a", {}},
                                                    {"/v1/meetings", {}},
                                                    {"/v1/meetings/m_a", {}},
                                                    {"/v1/meetings/m_a/insights", {}},
                                                    {"/v1/meetings/m_a/segments", {}},
                                                    {"/v1/search", {{"q", "launch"}}}}) {
        CAPTURE(path);
        const Reply reply = get(f.api, path, query);
        CHECK(reply.status() == 200);
        CHECK_FALSE(contains(reply.response.body, "private-marker"));
        for (const std::string_view field : {"host_token", "guest_token", "audio_file", "spool_dir",
                                             "embedding", "agent_endpoint_json"}) {
            CHECK_FALSE(contains(reply.response.body, field));
        }
    }
}

TEST_CASE("test_parameter_bounds", "[test_agent_api]") {
    Fixture f;
    const auto [path, query] = GENERATE(table<std::string, Pairs>({
        {"/v1/search", {{"q", ""}}},
        {"/v1/search", {}},
        {"/v1/search", {{"q", "x"}, {"limit", "101"}}},
        {"/v1/search", {{"q", "x"}, {"kind", "bad"}}},
        {"/v1/transcriptions", {{"limit", "0"}}},
        {"/v1/meetings", {{"since", "bad"}}},
        {"/v1/meetings/m_a/segments", {{"start_s", "-1"}}},
        {"/v1/meetings/m_a/segments", {{"end_s", "nan"}}},
        {"/v1/meetings/m_a/segments", {{"end_s", "inf"}}},
    }));
    CAPTURE(path, query);
    CHECK(get(f.api, path, query).status() == 422);
}

TEST_CASE("test_invalid_semantic_parameters", "[test_agent_api]") {
    Fixture f;
    const auto [path, query] = GENERATE(table<std::string, Pairs>({
        {"/v1/search", {{"q", "  "}}},
        {"/v1/meetings", {{"since", "2026-09-01T00:00:00"}}},
        {"/v1/meetings", {{"since", "2026-09-02T00:00:00Z"}, {"before", "2026-09-01T00:00:00Z"}}},
        {"/v1/meetings/m_a/segments", {{"start_s", "10"}, {"end_s", "5"}}},
    }));
    CAPTURE(path, query);
    CHECK(get(f.api, path, query).status() == 400);
}

TEST_CASE("test_openapi_documents_auth_and_stable_operations", "[test_agent_api]") {
    Fixture f;
    const Json schema = get(f.api, "/openapi.json").json;
    CHECK(schema["components"]["securitySchemes"]["HistoryToken"]["scheme"] == "bearer");
    for (const auto& [name, path] : schema["paths"].items()) {
        CAPTURE(name);
        CHECK(path.size() == 1);
        CHECK(path.contains("get"));
        CHECK(path["get"]["security"] == fv::py::loads(R"([{"HistoryToken": []}])"));
    }
    CHECK(schema["paths"]["/v1/search"]["get"]["operationId"] == "search_history");
    CHECK(schema["info"]["title"] == std::string(fv::identity().product) + " History API");
    CHECK(f.api.handle(request("/v1/transcriptions/h_a", {}, {}, "DELETE")).status == 405);
    CHECK(get(f.api, "/v1/transcriptions/h_a").status() == 200);
}

TEST_CASE("test_storage_is_read_only_and_does_not_create_or_migrate", "[test_agent_api]") {
    fv::testkit::TempDataRoot root;
    const auto database = make_test_database(root.path());
    {
        HistoryStore store(database);
        fv::sql::Connection connection = open_read_only(database);
        try {
            connection.exec("DELETE FROM transcription_history");
            FAIL("a read-only connection wrote");
        } catch (const fv::sql::Error& error) {
            CHECK(contains(error.what(), "readonly"));
        }
    }
    const auto missing = root.path() / "missing.db";
    try {
        HistoryStore store(missing);
        FAIL("a missing database opened");
    } catch (const std::invalid_argument& error) {
        CHECK(contains(error.what(), "does not exist"));
        CHECK(std::string(error.what()) == "Database does not exist. Open " +
                                               std::string(fv::identity().product) +
                                               " first or select --database.");
    }
    CHECK_FALSE(std::filesystem::exists(missing));
    write_sql(database, "UPDATE schema_version SET version=1");
    try {
        HistoryStore store(database);
        FAIL("an incompatible database opened");
    } catch (const std::invalid_argument& error) {
        CHECK(contains(error.what(), "incompatible"));
    }
    fv::sql::Connection connection = open_read_only(database);
    CHECK(connection.scalar("SELECT version FROM schema_version") ==
          fv::sql::Value(std::int64_t{1}));
}

TEST_CASE("test_database_errors_do_not_leak_sql", "[test_agent_api]") {
    Fixture f;
    write_sql(f.database, "ALTER TABLE transcription_history RENAME TO private_marker_history");
    const Reply reply = get(f.api, "/v1/status");
    CHECK(reply.status() == 503);
    CHECK(reply.json["error"]["code"] == "database_unavailable");
    CHECK_FALSE(contains(reply.response.body, "private_marker"));
    CHECK_FALSE(contains(reply.response.body, "transcription_history"));
    CHECK_FALSE(contains(reply.response.body, "no such table"));
}

TEST_CASE("test_invalid_startup_tokens", "[test_agent_api]") {
    fv::testkit::TempDataRoot root;
    const auto database = make_test_database(root.path());
    std::string accented;
    for (int i = 0; i < 32; ++i) {
        accented += "é";
    }
    const std::string token =
        GENERATE_COPY(std::string(), std::string("short"), std::string(32, 'a') + " ", accented);
    try {
        HistoryApi api(database, token);
        FAIL("the token was accepted");
    } catch (const std::invalid_argument& error) {
        CHECK(contains(error.what(), "TOKEN"));
        CHECK(std::string(error.what()) ==
              std::string(fv::identity().env_prefix) +
                  "API_TOKEN must be 32–512 printable ASCII characters without whitespace.");
    }
}

TEST_CASE("tokens of 32 and 512 printable characters are accepted", "[test_agent_api]") {
    CHECK_NOTHROW(check_token(std::string(32, '~')));
    CHECK_NOTHROW(check_token(std::string(512, '!')));
    CHECK_THROWS_AS(check_token(std::string(513, 'a')), std::invalid_argument);
    CHECK_THROWS_AS(check_token(std::string(31, 'a') + "\t"), std::invalid_argument);
}

TEST_CASE("test_export_and_agents_read_the_chosen_text", "[test_personalize_s9_history_edit]") {
    fv::testkit::TempDataRoot root;
    const auto database = make_test_database(root.path());
    write_sql(database,
              "INSERT INTO transcription_history (id, text, raw_text, cleaned_text, app_name, "
              "timestamp, model) VALUES ('e1', 'hello world um', 'hello world um', "
              "'Hello, world.', 'Slack', '2026-01-01T00:00:00Z', 'base')");
    HistoryStore store(database);
    const Json detail = store.transcription("e1");
    CHECK(detail["text"] == "hello world um");
    CHECK_FALSE(detail.contains("cleaned_text"));
    CHECK_FALSE(detail.contains("app_name"));
}

TEST_CASE("the store's limit check stands behind the route's", "[test_agent_api]") {
    fv::testkit::TempDataRoot root;
    HistoryStore store(make_test_database(root.path()));
    for (const std::int64_t limit : {std::int64_t{0}, std::int64_t{101}}) {
        try {
            store.transcriptions(ListQuery{.limit = limit});
            FAIL("the limit was accepted");
        } catch (const InvalidQuery& error) {
            CHECK(std::string(error.what()) == "limit must be between 1 and 100.");
        }
    }
}

TEST_CASE("a scoped MCP history API cannot enumerate or read another computer's copies") {
    fv::testkit::TempDataRoot root;
    HistoryApi api{make_test_database(root.path()), kToken, {}, fv::system_clock(), {}, true};
    CHECK(get(api, "/v1/transcriptions/h_a").status() == 200);
    CHECK(get(api, "/v1/transcriptions/h_remote").status() == 404);
    CHECK(get(api, "/v1/transcriptions", {{"include_remote", "true"}}).status() == 403);
    CHECK(get(api, "/v1/meetings", {{"include_clients", "true"}}).status() == 403);
    CHECK(get(api, "/v1/transcriptions/h_remote", {{"device_id", "device_a"}}).status() == 403);
    CHECK(api.handle(request("/v1/transcriptions/h_a", {}, {}, "DELETE")).status == 405);
}
