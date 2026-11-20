// tests/test_meeting_title_consistency.py, the runtime's four tests (an MCP rename through the
// runtime refreshes every dashboard's cache, a failed refresh does not undo it, every title
// write path keeps one contract, MCP advertises it), and
// tests/test_mcp_controls.py::test_dashboard_retitling_updates_cached_state_and_survives_later_edits.
// Dashboards are archive dashboards on loopback servers, called in process through
// DashboardApi::call.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentapi/agent_controls.h"
#include "agentapi/mcp_tools.h"
#include "history/titles.h"
#include "meeting_state/stored.h"
#include "meeting_web/archive.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/server.h"
#include "net/reactor.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

const fv::net::Query kHost{{"token", "host-token"}};

// The title_case fixture: an ended meeting with four finished reports, another meeting, and the
// MCP controls with meeting retitles granted.
struct TitleCase {
    TitleCase() {
        rig.make_meeting(meeting_id, Json{{"host_token", "host-token"},
                                          {"guest_token", "guest-token"},
                                          {"title", "Test meeting"}});
        ms::MeetingState state;
        state.meeting_id = meeting_id;
        state.title = "Test meeting";
        state.status = "ended";
        Json document = state.to_json();
        document["custom_reports"] = Json::array();
        for (int i = 0; i < 4; ++i) {
            document["custom_reports"].push_back(Json{
                {"id", "r_" + std::to_string(i)}, {"request", "Summary"}, {"status", "completed"}});
        }
        rig.repo->persist_state(meeting_id, document);
        rig.make_meeting(other_id, Json{{"host_token", "host-token"},
                                        {"guest_token", "guest-token"},
                                        {"title", "Test meeting"}});
        rig.settings.update(Json{{"mcp_retitle_meetings", true}});
        fv::agentapi::AgentControlsOptions options;
        options.database = rig.root.path() / "fv.db";
        options.settings = &rig.settings;
        agent = std::make_unique<fv::agentapi::AgentControls>(std::move(options));
    }

    // runtime_for: the runtime's owners are these dashboards (the engine, the archive, then
    // background engines), and the MCP renamer goes through its retitle_saved_meeting.
    template <class... D> void runtime_for(D&... dashboards);

    void use_renamer(std::function<void(std::string, std::string)> renamer) {
        fv::agentapi::AgentControlsOptions options;
        options.database = rig.root.path() / "fv.db";
        options.settings = &rig.settings;
        options.meeting_renamer = std::move(renamer);
        agent = std::make_unique<fv::agentapi::AgentControls>(std::move(options));
    }

    RuntimeRig rig;
    fv::net::Reactor reactor;
    std::string meeting_id = "m_test1";
    std::string other_id = "m_other";
    std::unique_ptr<fv::agentapi::AgentControls> agent;
};

// InProcessDashboard: an archive dashboard of one meeting on its own server.
struct Dashboard {
    Dashboard(TitleCase& c, const std::string& id)
        : engine(c.rig.repo, *c.rig.repo->get_meeting(id), "unused", Json::object(), c.rig.clock) {
        server = std::make_unique<mw::MeetingWebServer>(
            c.reactor, engine, c.rig.repo, "localhost", 0,
            [this](fv::net::HttpServer& http, mw::MeetingWebServer&) {
                mw::install_dashboard_routes(http, *api);
            });
        api = std::make_unique<mw::DashboardApi>(
            engine, c.rig.repo, mw::sql_past_meeting_pages(c.rig.repo), *server,
            mw::PostMeetingPasses{}, mw::DashboardOptions{}, c.rig.settings, c.rig.clock);
        REQUIRE(server->start().has_value());
        engine.attach_server(server.get());
        titles = dashboard_titles(*server, *api);
    }
    ~Dashboard() {
        engine.attach_server(nullptr);
        server->stop();
    }

    Json delete_report(const std::string& meeting_id, const std::string& report_id) {
        const auto response =
            api->call("DELETE", "/api/meetings/" + meeting_id + "/reports/" + report_id, kHost);
        REQUIRE(response.status == 200);
        return Json::parse(response.body);
    }
    Json detail(const std::string& meeting_id) {
        const auto response = api->call("GET", "/api/meetings/" + meeting_id, kHost);
        REQUIRE(response.status == 200);
        return Json::parse(response.body);
    }

    mw::ArchivedMeetingDashboard engine;
    std::unique_ptr<mw::MeetingWebServer> server;
    std::unique_ptr<mw::DashboardApi> api;
    std::shared_ptr<DashboardTitles> titles;
};

// The engine slot of the runtime: the dashboard's meeting and title surface.
std::shared_ptr<FakeEngine> owner_engine(Dashboard& dashboard) {
    auto engine = std::make_shared<FakeEngine>(dashboard.engine.meeting_id());
    engine->active = false;
    engine->titles = dashboard.titles;
    return engine;
}

template <class... D> void TitleCase::runtime_for(D&... dashboards) {
    std::vector<Dashboard*> list{&dashboards...};
    auto probe = rig.probe();
    if (!list.empty()) {
        probe.set_engine(owner_engine(*list[0]));
    }
    if (list.size() > 1) {
        auto archive = std::make_shared<FakeArchive>(list[1]->engine.meeting_id(), "");
        archive->titles = list[1]->titles;
        probe.set_archive(archive);
    }
    for (std::size_t i = 2; i < list.size(); ++i) {
        probe.add_background(std::to_string(i - 2), owner_engine(*list[i]));
    }
    use_renamer([this](std::string id, std::string title) {
        try {
            rig.rt->retitle_saved_meeting(id, title);
        } catch (const MeetingControlError& error) {
            throw fv::agentapi::ControlError(error.what());
        }
    });
}

void assert_saved_title(TitleCase& c, const std::string& meeting_id, const std::string& title) {
    const Json row = *c.rig.repo->get_meeting(meeting_id);
    CHECK(row["title"] == title);
    CHECK(fv::py::loads(row["state_json"].get<std::string>())["title"] == title);
    CHECK(ms::open_store(c.rig.repo, meeting_id, row, true, Json::object(), c.rig.clock)
              ->snapshot()["title"] == title);
}

} // namespace

// from tests/test_meeting_title_consistency.py::test_mcp_rename_refreshes_all_dashboard_caches
TEST_CASE("test_mcp_rename_refreshes_all_dashboard_caches", "[test_meeting_title_consistency]") {
    const bool selected_is_owner = GENERATE(false, true);
    TitleCase c;
    Dashboard primary(c, selected_is_owner ? c.meeting_id : c.other_id);
    Dashboard secondary(c, c.other_id);
    Dashboard background(c, c.other_id);
    secondary.delete_report(c.meeting_id, "r_0");
    background.delete_report(c.meeting_id, "r_1");
    c.runtime_for(primary, secondary, background);

    const Json result = c.agent->retitle("meeting", c.meeting_id, "Renamed through MCP");
    CHECK(result["title"] == "Renamed through MCP");
    for (Dashboard* dashboard : {&primary, &secondary, &background}) {
        const Json detail = dashboard->detail(c.meeting_id);
        CHECK(detail["meeting"]["title"] == result["title"]);
        CHECK(detail["state"]["title"] == result["title"]);
    }
    for (auto [dashboard, report] : {std::pair{&secondary, "r_2"}, std::pair{&background, "r_3"}}) {
        CHECK(dashboard->delete_report(c.meeting_id, report)["state"]["title"] == result["title"]);
        assert_saved_title(c, c.meeting_id, result["title"].get<std::string>());
        CHECK(dashboard->detail(c.meeting_id)["state"]["title"] == result["title"]);
    }
    Dashboard reopened(c, c.meeting_id);
    CHECK(reopened.detail(c.meeting_id)["state"]["title"] == result["title"]);
}

// from
// tests/test_meeting_title_consistency.py::test_cache_refresh_failure_does_not_report_committed_rename_as_failed
TEST_CASE("test_cache_refresh_failure_does_not_report_committed_rename_as_failed",
          "[test_meeting_title_consistency]") {
    TitleCase c;
    Dashboard primary(c, c.meeting_id);
    Dashboard secondary(c, c.other_id);
    secondary.delete_report(c.meeting_id, "r_0");
    // The secondary dashboard's refresh fails ("closed").
    class Failing final : public DashboardTitles {
    public:
        explicit Failing(std::shared_ptr<DashboardTitles> inner) : inner_(std::move(inner)) {}
        bool is_running() const override { return inner_->is_running(); }
        bool retitle_saved_meeting(const std::string& id, const std::string& title) override {
            return inner_->retitle_saved_meeting(id, title);
        }
        void refresh_saved_meeting_title(const std::string&) override {
            throw std::runtime_error("closed");
        }

    private:
        std::shared_ptr<DashboardTitles> inner_;
    };
    secondary.titles = std::make_shared<Failing>(secondary.titles);
    c.runtime_for(primary, secondary);
    c.agent->retitle("meeting", c.meeting_id, "Committed");
    CHECK(secondary.delete_report(c.meeting_id, "r_1")["state"]["title"] == "Committed");
    assert_saved_title(c, c.meeting_id, "Committed");
}

// from tests/test_meeting_title_consistency.py::test_every_title_write_path_uses_the_same_contract
TEST_CASE("test_every_title_write_path_uses_the_same_contract",
          "[test_meeting_title_consistency]") {
    const std::vector<std::string> valid_titles = {"x",
                                                   std::string(120, 'x'),
                                                   std::string(121, 'x'),
                                                   std::string(200, 'x'),
                                                   [] {
                                                       std::string text;
                                                       for (int i = 0; i < 200; ++i) {
                                                           text += "📝";
                                                       }
                                                       return text;
                                                   }(),
                                                   "  New title  "};
    const std::vector<Json> invalid_titles = {nullptr,
                                              123,
                                              "",
                                              "   ",
                                              std::string(201, 'x'),
                                              [] {
                                                  std::string text;
                                                  for (int i = 0; i < 201; ++i) {
                                                      text += "📝";
                                                  }
                                                  return text;
                                              }(),
                                              "bad\nname",
                                              std::string("bad\0name", 8),
                                              "bad\x7fname"};
    const std::string route =
        GENERATE("repository", "state", "mcp", "rest_current", "rest_cached", "rest_uncached",
                 "mcp_current", "mcp_cached", "mcp_uncached");
    const bool all_titles =
        route == "repository" || route == "state" || route == "mcp" || route == "rest_current";
    std::vector<Json> titles(valid_titles.begin(), valid_titles.end());
    if (all_titles) {
        titles.insert(titles.end(), invalid_titles.begin(), invalid_titles.end());
    }
    for (const Json& title : titles) {
        CAPTURE(route, title.dump());
        const bool valid =
            title.is_string() &&
            std::ranges::find(valid_titles, title.get<std::string>()) != valid_titles.end();
        TitleCase c;
        std::unique_ptr<Dashboard> dashboard;
        const bool current = route.find("current") != std::string::npos;
        if (current || route.find("cached") != std::string::npos) {
            dashboard = std::make_unique<Dashboard>(c, current ? c.meeting_id : c.other_id);
            if (route.ends_with("_cached")) {
                dashboard->delete_report(c.meeting_id, "r_0");
            }
            c.runtime_for(*dashboard);
        }
        const Json before = *c.rig.repo->get_meeting(c.meeting_id);
        const auto events_before = c.rig.repo->list_events(c.meeting_id, std::nullopt, 100);
        if (!valid && route == "mcp") {
            // Rejecting a title never reaches a meeting runtime.
            c.use_renamer([](std::string, std::string) { FAIL("invalid title dispatched"); });
        }
        if (route.starts_with("rest")) {
            const auto response = dashboard->api->call(
                "POST", "/api/meetings/" + c.meeting_id + "/rename", kHost,
                Json{{"title", title}}.dump(), {{"content-type", "application/json"}});
            CHECK(response.status == (valid ? 200 : 400));
            if (valid) {
                CHECK(Json::parse(response.body) ==
                      Json{{"ok", true}, {"title", fv::py::strip(title.get<std::string>())}});
            }
        } else if (route == "state") {
            auto store =
                ms::open_store(c.rig.repo, c.meeting_id, *c.rig.repo->get_meeting(c.meeting_id),
                               true, Json::object(), c.rig.clock);
            const Json original = store->snapshot();
            const auto results = store->apply(
                "host", std::nullopt, Json::array({Json{{"op", "set_title"}, {"text", title}}}));
            REQUIRE(results.size() == 1);
            CHECK(results[0].ok == valid);
            if (!valid) {
                CHECK_FALSE(results[0].seq.has_value());
                CHECK(store->snapshot() == original);
            }
        } else if (valid) {
            if (route == "repository") {
                c.rig.repo->rename_meeting(c.meeting_id, title.get<std::string>());
            } else {
                c.agent->retitle("meeting", c.meeting_id, title.get<std::string>());
            }
        } else {
            // Only the repository and MCP routes see the invalid titles; a non-string title
            // cannot reach either as text, so it is rejected as the empty one is.
            const std::string text = title.is_string() ? title.get<std::string>() : std::string();
            try {
                if (route == "repository") {
                    c.rig.repo->rename_meeting(c.meeting_id, text);
                } else {
                    c.agent->retitle("meeting", c.meeting_id, text);
                }
                FAIL("an invalid title was accepted");
            } catch (const std::exception& error) {
                CHECK_THAT(std::string(error.what()), ContainsSubstring("1–200"));
            }
        }
        assert_saved_title(c, c.meeting_id,
                           valid ? fv::py::strip(title.get<std::string>()) : "Test meeting");
        if (!valid) {
            CHECK(*c.rig.repo->get_meeting(c.meeting_id) == before);
            CHECK(c.rig.repo->list_events(c.meeting_id, std::nullopt, 100) == events_before);
        }
    }
}

// from tests/test_meeting_title_consistency.py::test_mcp_advertises_the_shared_length_contract
TEST_CASE("test_mcp_advertises_the_shared_length_contract", "[test_meeting_title_consistency]") {
    std::size_t titled = 0;
    for (const Json& tool : fv::agentapi::mcp_catalog().tools) {
        const Json& properties = tool["inputSchema"].value("properties", Json::object());
        // The retitle tools share the title contract (notify_user's title is a notice's own).
        if (properties.contains("title") &&
            tool["name"].get<std::string>().starts_with("retitle_")) {
            ++titled;
            CHECK(properties["title"]["maxLength"] == fv::MAX_TITLE_LENGTH);
        }
    }
    CHECK(titled == 2);
    std::string longest;
    for (std::size_t i = 0; i < fv::MAX_TITLE_LENGTH; ++i) {
        longest += "📝";
    }
    CHECK_FALSE(fv::title_error(std::string_view(longest)).has_value());
    CHECK(fv::title_error(std::string_view(longest + "x")).has_value());
}

// from
// tests/test_mcp_controls.py::test_dashboard_retitling_updates_cached_state_and_survives_later_edits
TEST_CASE("test_dashboard_retitling_updates_cached_state_and_survives_later_edits",
          "[test_mcp_controls]") {
    TitleCase c;
    Dashboard dashboard(c, c.meeting_id);
    c.runtime_for(dashboard);

    c.agent->retitle("meeting", c.meeting_id, "Updated through MCP");
    CHECK(dashboard.engine.store()->snapshot()["title"] == "Updated through MCP");
    // A later dashboard edit keeps working on the cached state.
    dashboard.engine.store()->apply(
        "host", std::nullopt, Json::array({Json{{"op", "set_title"}, {"text", "User edit"}}}));
    CHECK(c.rig.repo->get_meeting(c.meeting_id)->at("title") == "User edit");

    // While the meeting is busy here the rename is refused with the runtime's own text.
    FinalizationClaim claim(c.meeting_id);
    CHECK_THROWS_WITH(c.agent->retitle("meeting", c.meeting_id, "Too soon"),
                      ContainsSubstring("record_busy: Wait for the meeting's current work"));
    claim.release();
    assert_saved_title(c, c.meeting_id, "User edit");
}

TEST_CASE("a dashboard that rejects a rename is the update_rejected ControlError",
          "[test_meeting_title_consistency]") {
    // services/runtime/meeting.py retitle_saved_meeting (audit A04-104).
    TitleCase c;
    Dashboard primary(c, c.meeting_id);
    class Rejecting final : public DashboardTitles {
    public:
        explicit Rejecting(std::shared_ptr<DashboardTitles> inner) : inner_(std::move(inner)) {}
        bool is_running() const override { return inner_->is_running(); }
        bool retitle_saved_meeting(const std::string&, const std::string&) override {
            return false;
        }
        void refresh_saved_meeting_title(const std::string& id) override {
            inner_->refresh_saved_meeting_title(id);
        }

    private:
        std::shared_ptr<DashboardTitles> inner_;
    };
    primary.titles = std::make_shared<Rejecting>(primary.titles);
    c.runtime_for(primary);
    CHECK_THROWS_WITH(c.rig.rt->retitle_saved_meeting(c.meeting_id, "Rejected"),
                      "update_rejected: The meeting title could not be changed.");
    assert_saved_title(c, c.meeting_id, "Test meeting");
}
