// tests/test_segment_cursor_sources.py: a meeting's segment pages when the reader switches
// between the paired client's own copy (asked over the client history source) and the copy this
// host saved of it (include_remote with the client's device id). The rules live in HistoryApi's
// segments route and HistoryStore::segments (one cursor format whose scope ignores the source);
// FederatedHistory::query carries the client's page. No product path gives a server a client
// history source (docs/port/differences.md a20-peer-history-session), so the client here is
// in process, as in OpenWhisper's test: its history store answering the host's queries.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agentapi/history_routes.h"
#include "agentapi/history_store.h"
#include "base/base64.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "db/database.h"
#include "remote/client_history_source.h"
#include "remote/errors.h"
#include "sql/db.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

constexpr std::string_view kDevice = "fixture-device";

// InProcessClient over services/remote_history/client.py's HistoryReader: the client's store
// answers the host's segment queries while it is online; HistoryReader's errors come back as
// ClientUnavailable with their code.
class InProcessClient final : public fv::remote::ClientHistorySource {
public:
    explicit InProcessClient(fs::path database) : store_(std::move(database)) {}

    std::atomic<bool> online{true};

    std::vector<fv::remote::ClientHistoryStatus> clients() override {
        return {{std::string(kDevice), "Fixture laptop", online ? "online" : "unavailable"}};
    }

    Json query(std::string_view device, std::string_view operation, const Json& params) override {
        if (device != kDevice || !online) {
            throw fv::remote::ClientUnavailable();
        }
        if (operation != "segments") {
            throw fv::remote::ClientUnavailable("invalid_query");
        }
        SegmentQuery query{params.at("meeting_id").get<std::string>()};
        if (const auto at = params.find("start_s"); at != params.end() && at->is_number()) {
            query.start_s = at->get<double>();
        }
        if (const auto at = params.find("end_s"); at != params.end() && at->is_number()) {
            query.end_s = at->get<double>();
        }
        query.limit = params.value("limit", std::int64_t{20});
        if (const auto at = params.find("cursor"); at != params.end() && at->is_string()) {
            query.cursor = at->get<std::string>();
        }
        try {
            return store_.segments(query);
        } catch (const NotFound&) {
            throw fv::remote::ClientUnavailable("not_found");
        } catch (const InvalidQuery&) {
            throw fv::remote::ClientUnavailable("invalid_query");
        }
    }

private:
    HistoryStore store_;
};

void insert(fv::sql::Connection& connection, std::string_view sql,
            const std::vector<fv::sql::Value>& values) {
    fv::sql::Statement statement = connection.prepare(sql);
    for (std::size_t i = 0; i < values.size(); ++i) {
        statement.bind(static_cast<int>(i) + 1, values[i]);
    }
    statement.step();
}

// The history_copies fixture (the ORM's defaults written out): client.db (the client's own
// meetings) and host.db (the host's saved copy of them, origin fixture-device), each with "meeting"
// and "other-meeting" and five segments of "meeting" starting at 0, 0, 10, 20 and 30 s.
fs::path make_copy(const fs::path& folder, std::string_view source,
                   std::optional<std::string_view> origin) {
    const fs::path path = folder / fv::path_from_utf8(std::string(source) + ".db");
    fv::Database database = fv::Database::open_or_create(path);
    database.write([&](fv::sql::Connection& connection) {
        using V = std::vector<fv::sql::Value>;
        for (const std::string meeting : {"meeting", "other-meeting"}) {
            insert(connection,
                   "INSERT INTO meeting_sessions (id, title, status, started_at, host_token, "
                   "guest_token, spool_dir, origin_device_id, paused_total_s, cloud_enabled, "
                   "state_seq) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0.0, 0, 0)",
                   V{meeting, std::string("Fixture meeting"), std::string("ended"),
                     std::string(kDate), std::string("fixture"), std::string("fixture"),
                     std::string("unused"),
                     origin ? fv::sql::Value(std::string(*origin)) : fv::sql::Value(nullptr)});
        }
        const double starts[] = {0, 0, 10, 20, 30};
        for (int number = 0; number < 5; ++number) {
            insert(connection,
                   "INSERT INTO meeting_segments (id, meeting_id, start_s, end_s, channel, text, "
                   "created_at, speaker_source, speaker_pinned) VALUES (?, ?, ?, ?, ?, ?, ?, "
                   "'channel', 0)",
                   V{"segment-" + std::to_string(number), std::string("meeting"), starts[number],
                     starts[number] + 3, std::string("mic"),
                     std::string(source) + " segment " + std::to_string(number),
                     std::string(kDate)});
        }
    });
    database.close();
    return path;
}

struct HistoryCopies {
    fv::testkit::TempDataRoot root;
    fs::path client_db = make_copy(root.path(), "client", std::nullopt);
    fs::path host_db = make_copy(root.path(), "host", kDevice);
    std::shared_ptr<InProcessClient> client = std::make_shared<InProcessClient>(client_db);
    HistoryApi history{host_db, kToken, {}, fv::system_clock(), client};

    // history.segments(meeting_id, device_id=..., include_remote=..., ...): the route, its
    // answer's status and body.
    Reply segments(std::string meeting, Pairs query) {
        return get(history, "/v1/meetings/" + meeting + "/segments", std::move(query));
    }
};

Pairs with(Pairs query, std::string name, std::string value) {
    query.emplace_back(std::move(name), std::move(value));
    return query;
}

std::string text_of(const Reply& page) {
    REQUIRE(page.status() == 200);
    REQUIRE(page.json["items"].size() == 1);
    return page.json["items"][0]["text"].get<std::string>();
}

// _fingerprint: sha256 of json.dumps(scope, sort_keys=True), 24 hex characters.
std::string fingerprint(bool include_remote) {
    return fv::sha256_hex(std::string(R"(["segments", "meeting", null, null, )") +
                          (include_remote ? "true" : "false") + "]")
        .substr(0, 24);
}

std::string scope_of(const Json& cursor) {
    const auto bytes = fv::b64decode(cursor.get<std::string>(), true);
    REQUIRE(bytes);
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    return Json::parse(text).at("scope").get<std::string>();
}

} // namespace

TEST_CASE("test_segment_cursor_survives_switching_sources", "[test_segment_cursor_sources]") {
    HistoryCopies copies;
    const bool initially_online = GENERATE(false, true);
    copies.client->online = initially_online;
    const Pairs params{
        {"device_id", std::string(kDevice)}, {"include_remote", "true"}, {"limit", "1"}};
    auto page = copies.segments("meeting", params);
    REQUIRE(page.status() == 200);
    std::vector<std::string> ids{page.json["items"][0]["id"].get<std::string>()};
    for (int number = 1; number < 5; ++number) {
        REQUIRE(page.json["next_cursor"].is_string());
        copies.client->online = !copies.client->online;
        page = copies.segments("meeting",
                               with(params, "cursor", page.json["next_cursor"].get<std::string>()));
        CHECK(text_of(page) == std::string(copies.client->online ? "client" : "host") +
                                   " segment " + std::to_string(number));
        ids.push_back(page.json["items"][0]["id"].get<std::string>());
    }
    CHECK(ids == std::vector<std::string>{"segment-0", "segment-1", "segment-2", "segment-3",
                                          "segment-4"});
    CHECK(page.json["next_cursor"].is_null());
}

TEST_CASE("test_existing_segment_cursor_is_accepted_on_either_source",
          "[test_segment_cursor_sources]") {
    HistoryCopies copies;
    const bool legacy_include_remote = GENERATE(false, true);
    const std::string cursor = fv::b64encode(R"({"scope": ")" + fingerprint(legacy_include_remote) +
                                                 R"(", "keys": [0.0, "segment-0"]})",
                                             true);
    for (const bool online : {true, false}) {
        copies.client->online = online;
        const auto page = copies.segments("meeting", {{"device_id", std::string(kDevice)},
                                                      {"include_remote", "true"},
                                                      {"cursor", cursor},
                                                      {"limit", "1"}});
        CHECK(text_of(page) == std::string(online ? "client" : "host") + " segment 1");
        CHECK(page.json["items"][0]["id"] == "segment-1");
        // The next cursor carries the canonical (local-only) scope, whichever source made it.
        CHECK(scope_of(page.json["next_cursor"]) == fingerprint(false));
    }
}

TEST_CASE("test_fallback_segment_cursor_remains_bound_to_meeting_and_range",
          "[test_segment_cursor_sources]") {
    HistoryCopies copies;
    const auto [meeting, changed] = GENERATE(table<std::string, Pairs>({
        {"meeting", {{"start_s", "1"}}},
        {"meeting", {{"end_s", "25"}}},
        {"other-meeting", {}},
    }));
    const Pairs params{
        {"device_id", std::string(kDevice)}, {"include_remote", "true"}, {"limit", "1"}};
    const auto first = copies.segments("meeting", params);
    REQUIRE(first.json["next_cursor"].is_string());
    copies.client->online = false;
    Pairs query = with(params, "cursor", first.json["next_cursor"].get<std::string>());
    query.insert(query.end(), changed.begin(), changed.end());
    const auto refused = copies.segments(meeting, query);
    CHECK(refused.status() == 400);
    CHECK(refused.json["error"]["code"] == "invalid_query");
    CHECK_THAT(refused.json["error"]["message"].get<std::string>(),
               ContainsSubstring("different filters"));
}

TEST_CASE("test_offline_segment_fallback_still_requires_opt_in_and_matching_origin",
          "[test_segment_cursor_sources]") {
    HistoryCopies copies;
    const auto first = copies.segments(
        "meeting",
        {{"device_id", std::string(kDevice)}, {"include_remote", "true"}, {"limit", "1"}});
    REQUIRE(first.json["next_cursor"].is_string());
    const std::string cursor = first.json["next_cursor"].get<std::string>();
    copies.client->online = false;
    // Without include_remote the saved copy is not read: the client is unavailable.
    const auto without_opt_in =
        copies.segments("meeting", {{"device_id", std::string(kDevice)}, {"cursor", cursor}});
    CHECK(without_opt_in.status() == 503);
    CHECK(without_opt_in.json["error"]["code"] == "unavailable");
    // The saved copy came from fixture-device, not from the device named.
    const auto other_device = copies.segments(
        "meeting", {{"device_id", "other-device"}, {"include_remote", "true"}, {"cursor", cursor}});
    CHECK(other_device.status() == 503);
    CHECK(other_device.json["error"]["code"] == "unavailable");
    // history.store.segments: this computer's own meetings only.
    HistoryStore store(copies.host_db);
    CHECK_THROWS_AS(store.segments(SegmentQuery{.meeting_id = "meeting", .cursor = cursor}),
                    NotFound);
    CHECK(copies.segments("meeting", {{"cursor", cursor}}).status() == 404);
}

TEST_CASE("test_fallback_segment_cursor_keeps_time_range_and_deleted_boundary",
          "[test_segment_cursor_sources]") {
    HistoryCopies copies;
    const Pairs params{{"device_id", std::string(kDevice)},
                       {"include_remote", "true"},
                       {"limit", "1"},
                       {"start_s", "0.0"},
                       {"end_s", "30.0"}};
    const auto first = copies.segments("meeting", params);
    REQUIRE(first.status() == 200);
    write_sql(copies.host_db, "DELETE FROM meeting_segments WHERE id = 'segment-0'");
    copies.client->online = false;
    std::vector<std::string> ids;
    for (const auto& item : first.json["items"]) {
        ids.push_back(item["id"].get<std::string>());
    }
    auto page = first;
    while (page.json["next_cursor"].is_string()) {
        page = copies.segments("meeting",
                               with(params, "cursor", page.json["next_cursor"].get<std::string>()));
        REQUIRE(page.status() == 200);
        for (const auto& item : page.json["items"]) {
            ids.push_back(item["id"].get<std::string>());
        }
    }
    CHECK(ids == std::vector<std::string>{"segment-0", "segment-1", "segment-2", "segment-3"});
}
