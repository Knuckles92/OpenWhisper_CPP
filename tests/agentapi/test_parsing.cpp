// FastAPI's query validation, the cursors and the server-side client cursors, below the routes.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>

#include "agentapi/detail/query_params.h"
#include "agentapi/detail/standalone_history.h"
#include "agentapi/history_cursor.h"
#include "agentapi/history_models.h"
#include "agentapi/history_store.h"
#include "base/base64.h"
#include "base/testing/fake_clock.h"
#include "pyport/json.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
namespace detail = fv::agentapi::detail;

std::string iso(std::string_view text) {
    const auto parsed = detail::parse_datetime(text);
    REQUIRE(parsed);
    return parsed->utc_offset_us ? detail::to_utc(*parsed).isoformat() : parsed->isoformat();
}

} // namespace

TEST_CASE("query booleans parse as pydantic's lax bool") {
    for (const std::string_view yes : {"true", "TRUE", "tRuE", "1", "yes", "On", "t", "Y"}) {
        CHECK(detail::parse_bool(yes) == true);
    }
    for (const std::string_view no : {"false", "0", "No", "off", "f", "N"}) {
        CHECK(detail::parse_bool(no) == false);
    }
    for (const std::string_view bad : {"", " true", "true ", "2", "maybe", "yess"}) {
        CHECK_FALSE(detail::parse_bool(bad));
    }
}

TEST_CASE("query integers parse as pydantic's lax int") {
    CHECK(detail::parse_int("20") == 20);
    CHECK(detail::parse_int(" 20\n") == 20);
    CHECK(detail::parse_int("\xc2\xa0"
                            "20") == 20);
    CHECK(detail::parse_int("+3") == 3);
    CHECK(detail::parse_int("-0") == 0);
    CHECK(detail::parse_int("1_0") == 10);
    CHECK(detail::parse_int("20.0") == 20);
    CHECK(detail::parse_int("0020.000") == 20);
    for (const std::string_view bad :
         {"", "20.", "20.5", "2e1", "0x14", "1__0", "_10", "10_", "+-1", "abc", "\xd9\xa2"}) {
        CAPTURE(bad);
        CHECK_FALSE(detail::parse_int(bad));
    }
}

TEST_CASE("query floats parse as pydantic's lax float") {
    CHECK(detail::parse_float(" 1e0 ") == 1.0);
    CHECK(detail::parse_float("1_0") == 10.0);
    CHECK(detail::parse_float(".5") == 0.5);
    CHECK(std::isinf(*detail::parse_float("inf")));
    CHECK(std::isnan(*detail::parse_float("nan")));
    CHECK_FALSE(detail::parse_float("0x1"));
    CHECK_FALSE(detail::parse_float(""));
    CHECK_FALSE(detail::parse_float("\xd9\xa2"));
}

TEST_CASE("query datetimes parse as pydantic's datetime") {
    CHECK(iso("2026-09-01T00:00:00Z") == "2026-09-01T00:00:00+00:00");
    CHECK(iso("2026-09-01 00:00:00+02:00") == "2026-08-31T22:00:00+00:00");
    CHECK(iso("2026-09-01t00:00:00+0530") == "2026-08-31T18:30:00+00:00");
    CHECK(iso("2026-09-01_00:00Z") == "2026-09-01T00:00:00+00:00");
    CHECK(iso("2026-09-01T00:00:00.123456789Z") == "2026-09-01T00:00:00.123456+00:00");
    CHECK(iso("2026-09-01T00:00:00,5Z") == "2026-09-01T00:00:00.500000+00:00");
    CHECK(iso("2026-09-01T00:00:00-23:59") == "2026-09-01T23:59:00+00:00");
    CHECK(iso("2026-09-01") == "2026-09-01T00:00:00");
    CHECK(iso("2026-09-01T00:00") == "2026-09-01T00:00:00");
    // Unix times, in milliseconds beyond 2e10.
    CHECK(iso("1700000000") == "2023-11-14T22:13:20+00:00");
    CHECK(iso("1700000000.123456789") == "2023-11-14T22:13:20.123457+00:00");
    CHECK(iso("1700000000000") == "2023-11-14T22:13:20+00:00");
    CHECK(iso("1700000000000.5") == "2023-11-14T22:13:20.000500+00:00");
    CHECK(iso("-1.5") == "1969-12-31T23:59:58.500000+00:00");
    CHECK(iso("-20000000001") == "1969-05-14T12:26:39.999000+00:00");
    CHECK(iso(".5") == "1970-01-01T00:00:00.500000+00:00");
    CHECK(iso("1.7e9") == "2023-11-14T22:13:20+00:00");
    CHECK(iso("20000000000") == "2603-10-11T11:33:20+00:00");
    for (const std::string_view bad : {"bad",
                                       "",
                                       " 2026-09-01T00:00:00Z",
                                       "2026-09-01T00:00:00Z ",
                                       "2026-09-01T00",
                                       "2026-09-01T00:00:00+05",
                                       "2026-09-01T00:00:00+05:3",
                                       "2026-09-01T00:00:00+05:30:15",
                                       "2026-09-01T00:00:00+24:00",
                                       "2026-09-01T24:00:00Z",
                                       "2026-09-01T00:00:60Z",
                                       "2026-02-30T00:00:00Z",
                                       "2026-9-01T00:00:00Z",
                                       "2026-09-01T00:00:00.Z",
                                       "1e3",
                                       "inf",
                                       "nan",
                                       "1_700_000_000",
                                       "99999999999999999",
                                       "+2026-09-01T00:00:00Z"}) {
        CAPTURE(bad);
        CHECK_FALSE(detail::parse_datetime(bad));
    }
}

TEST_CASE("a conversion to UTC outside the calendar is an OverflowError") {
    const auto early = detail::parse_datetime("0001-01-01T00:00:00+01:00");
    REQUIRE(early);
    CHECK_THROWS_AS(detail::to_utc(*early), fv::py::OverflowError);
    const auto late = detail::parse_datetime("9999-12-31T23:59:59-01:00");
    REQUIRE(late);
    CHECK_THROWS_AS(detail::to_utc(*late), fv::py::OverflowError);
}

TEST_CASE("a scalar parameter takes its last value") {
    const fv::net::Query query{{"limit", "1"}, {"q", "x"}, {"limit", "2"}};
    CHECK(detail::last_value(query, "limit") == "2");
    CHECK_FALSE(detail::last_value(query, "cursor"));
}

TEST_CASE("cursors round-trip with Python's bytes and bind to their scope") {
    const Json scope = fv::py::loads(
        R"(["transcriptions", null, {"include_remote": false, "since": null, "before": null}])");
    const std::string own = fingerprint(scope);
    // tools/parity/transcripts/agent_api.json: the first page of /v1/transcriptions?limit=1.
    CHECK(own == "f4acff25f69b3576a97f1ef1");
    const std::string cursor = encode_cursor(own, fv::py::loads(R"([2461285.1666666665, "h_c"])"));
    CHECK(cursor == "eyJzY29wZSI6ICJmNGFjZmYyNWY2OWIzNTc2YTk3ZjFlZjEiLCAia2V5cyI6IFsyNDYxMjg1LjE2"
                    "NjY2NjY2NjUsICJoX2MiXX0=");
    CHECK(cursor_keys(cursor, {own}, 2) == fv::py::loads(R"([2461285.1666666665, "h_c"])"));
    CHECK_THROWS_AS(cursor_keys(cursor, {"other"}, 2), InvalidQuery);
    CHECK_THROWS_AS(cursor_keys(cursor, {own}, 3), InvalidQuery);
    std::string stripped = cursor;
    stripped.pop_back();
    CHECK_THROWS_AS(cursor_keys(stripped, {own}, 2), InvalidQuery);
    CHECK_THROWS_AS(cursor_keys(std::string(2049, 'A'), {own}, 2), InvalidQuery);
    // json.loads of bytes skips a UTF-8 byte order mark.
    const std::string with_bom = fv::b64encode(
        "\xef\xbb\xbf" +
            fv::py::dumps(fv::py::loads(R"({"scope": ")" + own + R"(", "keys": [1, "h"]})")),
        true);
    CHECK(cursor_keys(with_bom, {own}, 2) == fv::py::loads(R"([1, "h"])"));
}

TEST_CASE("client cursors last ten minutes and keep at most 128") {
    fv::FakeClock clock;
    detail::StandaloneHistory history(clock);
    int reads = 0;
    const auto read = [&reads](const std::optional<std::string>& cursor, std::int64_t,
                               const std::optional<std::string>& origin) {
        ++reads;
        CHECK(origin == "device_a");
        Json item = Json::object();
        item["id"] = cursor ? "second" : "first";
        item["origin_device_id"] = "device_a";
        Json items = Json::array({item});
        return page_json(items, cursor ? std::nullopt : std::optional<std::string>("next"));
    };
    const Json params = Json::object();
    const Json first = history.page("transcriptions", params, false, std::string("device_a"), true,
                                    std::nullopt, 1, read);
    REQUIRE(first["next_cursor"].is_string());
    CHECK(first["clients"] == fv::py::loads(
                                  R"([{"device_id": "device_a", "name": "Paired computer",
                                       "status": "unavailable"}])"));
    const std::string cursor = first["next_cursor"].get<std::string>();
    CHECK(cursor.starts_with("clients-"));
    CHECK(cursor.size() == 8 + 32);

    clock.advance(std::chrono::seconds(599));
    const Json second = history.page("transcriptions", params, false, std::string("device_a"), true,
                                     cursor, 1, read);
    CHECK(second["items"][0]["id"] == "second");
    CHECK(second["next_cursor"].is_null());
    CHECK_THROWS_AS(history.page("transcriptions", Json::object({{"q", "x"}}), false,
                                 std::string("device_a"), true, cursor, 1, read),
                    InvalidQuery);
    clock.advance(std::chrono::seconds(2));
    CHECK_THROWS_AS(history.page("transcriptions", params, false, std::string("device_a"), true,
                                 cursor, 1, read),
                    InvalidQuery);

    // The oldest of 129 is dropped.
    std::vector<std::string> cursors;
    for (int i = 0; i < 129; ++i) {
        cursors.push_back(history
                              .page("transcriptions", params, false, std::string("device_a"), true,
                                    std::nullopt, 1, read)["next_cursor"]
                              .get<std::string>());
    }
    CHECK_THROWS_AS(history.page("transcriptions", params, false, std::string("device_a"), true,
                                 cursors.front(), 1, read),
                    InvalidQuery);
    CHECK_NOTHROW(history.page("transcriptions", params, false, std::string("device_a"), true,
                               cursors[1], 1, read));
}

TEST_CASE("requests that name clients answer as the standalone app does") {
    fv::FakeClock clock;
    detail::StandaloneHistory history(clock);
    const auto read = [](const std::optional<std::string>&, std::int64_t,
                         const std::optional<std::string>&) {
        return page_json(Json::array(), {});
    };
    try {
        history.page("search", Json::object(), true, std::nullopt, false, std::nullopt, 20, read);
        FAIL("include_clients was served");
    } catch (const detail::ClientUnavailable& error) {
        CHECK(error.code() == "unsupported");
        CHECK(error.status() == 503);
        CHECK(std::string(error.what()) ==
              "Direct client queries are not available on this server.");
    }
    try {
        history.page("search", Json::object(), false, std::string("d"), false, std::nullopt, 20,
                     read);
        FAIL("a device without include_remote was served");
    } catch (const detail::ClientUnavailable& error) {
        CHECK(error.code() == "unavailable");
        CHECK(std::string(error.what()) ==
              "Client history is unavailable. The client may be offline.");
    }
    CHECK(detail::ClientUnavailable("not_found").status() == 404);
    CHECK(detail::ClientUnavailable("bogus").code() == "unavailable");
}

TEST_CASE("snapshot models validate as pydantic's lax mode") {
    const Insight insight = insight_from(fv::py::loads(
        R"({"id": "k", "card": "key_points", "pinned": "yes", "data": {"start_s": " 12 ", "x": 1}})"));
    CHECK(insight.pinned);
    CHECK(insight.data.start_s == 12.0);
    CHECK(insight.to_json()["data"] ==
          fv::py::loads(R"({"heading": null, "start_s": 12.0, "owner_participant_id": null,
                            "due_date": null, "severity": null})"));
    // inf is kept, and written as null in JSON.
    CHECK(insight_from(fv::py::loads(R"({"id": "k", "card": "c", "data": {"start_s": "inf"}})"))
              .to_json()["data"]["start_s"]
              .is_null());
    for (const char* bad :
         {R"({"card": "c"})", R"({"id": 5, "card": "c"})",
          R"({"id": "k", "card": "c", "pinned": 2})", R"({"id": "k", "card": "c", "pinned": 0.5})",
          R"({"id": "k", "card": "c", "text": null})", R"({"id": "k", "card": "c", "data": null})",
          R"({"id": "k", "card": "c", "evidence": ["a", 1]})",
          R"({"id": "k", "card": "c", "data": {"start_s": ""}})", R"([])"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(insight_from(fv::py::loads(bad)), ModelError);
    }
    CHECK(participant_from(fv::py::loads(R"({"id": "p", "display_name": "A"})")).kind ==
          "others_cluster");
    CHECK_THROWS_AS(participant_from(fv::py::loads(R"({"id": "p"})")), ModelError);
    CHECK(question_from(fv::py::loads(R"({"id": "q"})")).status == "open");
    CHECK(report_from(fv::py::loads(R"({"id": "r"})")).status == "running");
}
