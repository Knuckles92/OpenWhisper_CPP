#pragma once

// A dashboard server on 127.0.0.1 with a real state store, the fake engine and the hello
// repository: tests/test_meeting_ws.py's ws_app (canned engine actions over a prepared
// document) and authz_app (the store's own authorization).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "meeting_web/server.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "net/reactor.h"
#include "web_test_support.h"

namespace fv::meeting_web::test_support {

inline constexpr std::string_view kHostToken = "host-ws-token-cccccccccccccccccccc";
inline constexpr std::string_view kGuestToken = "guest-ws-token-dddddddddddddddddddd";
inline const std::string kGuestUrl = "http://127.0.0.1:9/m/" + std::string(kGuestToken);

inline Json ws_meeting_row() {
    return Json{{"id", "m_ws"},
                {"title", "WS Meeting"},
                {"status", "active"},
                {"started_at", "2026-01-01T12:00:00"},
                {"host_token", kHostToken},
                {"guest_token", kGuestToken},
                {"state_json", "{}"}};
}

inline std::vector<Json> ws_segments() {
    return {Json{{"id", "sg_1"},
                 {"start_s", 1.0},
                 {"end_s", 2.0},
                 {"text", "hi"},
                 {"channel", "mic"},
                 {"speaker_participant_id", nullptr}}};
}

enum class EngineKind {
    // ws_app: apply_client_action answers a canned add_item result with seq 4.
    Canned,
    // authz_app: every action and undo goes through the store.
    Store,
};

class Harness {
public:
    explicit Harness(EngineKind kind, Json meeting = ws_meeting_row(),
                     std::vector<Json> segments = ws_segments())
        : repo(std::make_shared<HelloRepository>(std::move(meeting), std::move(segments))) {
        meeting_state::MeetingState state;
        state.meeting_id = "m_ws";
        if (kind == EngineKind::Canned) {
            state.seq = 3;
            state.title = "WS Meeting";
            state.status = "active";
            state.topic.current = "Kickoff";
            state.intelligence_online = true;
            meeting_state::Participant me;
            me.id = "p_me";
            me.display_name = "Host";
            me.kind = "me";
            state.participants.set("p_me", me);
        }
        store = std::make_shared<meeting_state::MeetingStateStore>(
            std::move(state), nullptr, meeting_state::SegmentHandler{}, clock, ids);
        if (kind == EngineKind::Store) {
            store->apply("system", std::nullopt,
                         Json::array({Json{{"op", "upsert_participant"},
                                           {"display_name", "Me"},
                                           {"kind", "me"},
                                           {"is_provisional", false}}}));
        }
        engine = std::make_unique<testing::FakeDashboardEngine>("m_ws", store);
        if (kind == EngineKind::Canned) {
            engine->set_apply(
                [](std::string_view, const std::optional<std::string>&, const Json& op) {
                    meeting_state::OpResult result;
                    result.ok = true;
                    result.op = op;
                    result.target_id = "it_new";
                    result.seq = 4;
                    result.effect =
                        Json{{"entity", "item"},
                             {"item", Json{{"id", "it_new"}, {"text", op.value("text", Json())}}}};
                    return std::vector<meeting_state::OpResult>{result};
                });
            engine->set_undo([](std::int64_t, const std::optional<std::string>&) {
                return std::vector<meeting_state::OpResult>{};
            });
        }
        server = std::make_unique<MeetingWebServer>(reactor, *engine, repo, "localhost", 0);
        server->hub().set_guest_url_provider([] { return kGuestUrl; });
        auto url = server->start();
        REQUIRE(url.has_value());
        port = static_cast<std::uint16_t>(std::stoi(url->substr(url->rfind(':') + 1)));
    }

    ~Harness() { server->stop(); }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    std::unique_ptr<Client> connect(std::string_view query) {
        return std::make_unique<Client>(port, query);
    }
    std::unique_ptr<Client> host() { return connect("token=" + std::string(kHostToken)); }
    std::unique_ptr<Client> guest(std::string_view name, std::string_view guest_id = {}) {
        std::string query = "token=" + std::string(kGuestToken) + "&name=" + std::string(name);
        if (!guest_id.empty()) {
            query += "&guest_id=" + std::string(guest_id);
        }
        return connect(query);
    }

    std::size_t add_guest_calls() const { return engine->calls("add_guest").size(); }

    FakeClock clock;
    meeting_state::testing::FixedIds ids;
    std::shared_ptr<HelloRepository> repo;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    std::unique_ptr<testing::FakeDashboardEngine> engine;
    net::Reactor reactor;
    std::unique_ptr<MeetingWebServer> server;
    std::uint16_t port = 0;
};

} // namespace fv::meeting_web::test_support
