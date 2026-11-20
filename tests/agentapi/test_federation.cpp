#include <catch2/catch_test_macros.hpp>

#include "agentapi/federation.h"
#include "agentapi/history_store.h"
#include "base/testing/fake_clock.h"
#include "remote/testing/fake_client_history_source.h"

using namespace fv;
using namespace fv::agentapi;

namespace {

Json item(std::string id, std::string day, Json device = nullptr) {
    return {{"id", std::move(id)},
            {"model", "parakeet"},
            {"preview", "test record"},
            {"timestamp", std::move(day) + "T00:00:00+00:00"},
            {"origin_device_id", std::move(device)}};
}

} // namespace

TEST_CASE("federated history binds a cursor to paired devices and deduplicates stored copies") {
    FakeClock clock;
    auto source = std::make_shared<remote::testing::FakeClientHistorySource>();
    source->set_clients({{"peer", "Laptop", "online"}});
    auto remote_item = item("a", "2026-01-03", "spoofed");
    remote_item["private_field"] = "should not travel";
    source->answer("peer", "transcriptions", page_json(Json::array({remote_item}), {}));
    FederatedHistory history(source, clock);
    const auto annotated = history.query("peer", "transcriptions", Json{{"limit", 1}});
    REQUIRE(annotated["items"].size() == 1);
    CHECK(annotated["items"][0]["origin_device_id"] == "peer");
    CHECK_FALSE(annotated["items"][0].contains("private_field"));
    const auto read = [](const auto& cursor, std::int64_t, const auto&) {
        if (!cursor) {
            return page_json(Json::array({item("a", "2026-01-03", "peer")}), "local-page-two");
        }
        return page_json(Json::array({item("b", "2026-01-02")}), {});
    };
    const Json params{{"include_remote", true}};
    const auto first = history.page("transcriptions", params, {}, true, {}, 1, read);
    REQUIRE(first["items"].size() == 1);
    CHECK(first["items"][0]["id"] == "a");
    CHECK(first["items"][0]["origin_device_id"] == "peer");
    CHECK_FALSE(first["items"][0].contains("private_field"));
    REQUIRE(first["next_cursor"].is_string());
    const auto second = history.page("transcriptions", params, {}, true,
                                     first["next_cursor"].get<std::string>(), 1, read);
    REQUIRE(second["items"].size() == 1);
    CHECK(second["items"][0]["id"] == "b");
}

TEST_CASE("the peer history directory reports disabled and revoked devices without querying them") {
    FakeClock clock;
    auto source = std::make_shared<remote::testing::FakeClientHistorySource>();
    source->set_clients({{"peer", "Laptop", "sharing_disabled"}});
    FederatedHistory history(source, clock);
    REQUIRE(history.clients().size() == 1);
    CHECK(history.clients()[0].status == "sharing_disabled");
    CHECK_THROWS_AS(history.query("peer", "transcription", Json{{"record_id", "x"}}),
                    remote::ClientUnavailable);
    source->set_clients({});
    CHECK(history.clients().empty());
    CHECK_THROWS_AS(history.query("peer", "transcription", Json{{"record_id", "x"}}),
                    remote::ClientUnavailable);
    CHECK(source->queries().empty());
}
