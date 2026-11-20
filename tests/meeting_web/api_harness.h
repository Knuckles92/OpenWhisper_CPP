#pragma once

// The dashboard API on a loopback server for tests: a SQL world (a database in a temporary data
// folder, OpenWhisper's `repo` and `db` fixtures) and the server, the routes and a client.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "http_client.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/testing/fixed_ids.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/server.h"
#include "net/reactor.h"
#include "settings/store.h"
#include "testkit/testkit.h"

namespace fv::meeting_web::test_support {

using Query = std::vector<std::pair<std::string, std::string>>;

// 2026-09-19T09:30:00Z, the fixture database's "now".
inline std::chrono::system_clock::time_point fixture_now() {
    using namespace std::chrono;
    return sys_days{year{2026} / 9 / 19} + hours{9} + minutes{30};
}

// A database in a temporary data folder: a fresh one, or a copy of `fixture`.
struct SqlWorld {
    explicit SqlWorld(std::optional<std::filesystem::path> fixture = std::nullopt)
        : clock(fixture_now()) {
        const auto path = root.path() / "meetings.db";
        if (fixture) {
            std::filesystem::copy_file(*fixture, path);
        }
        database = std::make_unique<Database>(Database::open_or_create(path));
        repo = std::make_shared<meeting_state::SqlMeetingRepository>(*database, clock);
    }

    // Declared first, so the database closes before the folder is removed.
    fv::testkit::TempDataRoot root;
    FakeClock clock;
    meeting_state::testing::FixedIds ids;
    std::unique_ptr<Database> database;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repo;
};

// The fixture database of the recorded transcript.
inline std::filesystem::path meeting_web_fixture(std::string_view name) {
    return fv::testkit::source_root() / "tests" / "fixtures" / "meeting_web" / name;
}

// A dashboard server with the routes installed, on 127.0.0.1 and an ephemeral port.
class ApiServer {
public:
    ApiServer(DashboardEngine& engine, std::shared_ptr<meeting_state::MeetingRepository> repo,
              std::shared_ptr<PastMeetingPages> pages, PostMeetingPasses passes,
              DashboardOptions options, const std::filesystem::path& settings_file,
              const Clock& clock)
        : settings(settings_file) {
        server =
            std::make_unique<MeetingWebServer>(reactor, engine, repo, "localhost", 0,
                                               [this](net::HttpServer& http, MeetingWebServer&) {
                                                   install_dashboard_routes(http, *api);
                                               });
        api =
            std::make_unique<DashboardApi>(engine, repo, std::move(pages), *server,
                                           std::move(passes), std::move(options), settings, clock);
        const auto url = server->start();
        REQUIRE(url.has_value());
        port = static_cast<std::uint16_t>(std::stoi(url->substr(url->rfind(':') + 1)));
    }
    ~ApiServer() { server->stop(); }
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    HttpResult get(std::string_view path, const Query& query = {},
                   const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        return http_request(port, "GET", target(path, query), headers);
    }
    HttpResult post(std::string_view path, const Query& query = {}, std::string_view body = {}) {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!body.empty()) {
            headers.emplace_back("Content-Type", "application/json");
        }
        return http_request(port, "POST", target(path, query), headers, body);
    }
    HttpResult post_json(std::string_view path, const Query& query, const Json& body) {
        return post(path, query, body.dump());
    }
    HttpResult del(std::string_view path, const Query& query = {}) {
        return http_request(port, "DELETE", target(path, query));
    }

    SettingsStore settings;
    net::Reactor reactor;
    std::unique_ptr<MeetingWebServer> server;
    std::unique_ptr<DashboardApi> api;
    std::uint16_t port = 0;

private:
    static std::string target(std::string_view path, const Query& query) {
        std::string out(path);
        if (!query.empty()) {
            out += "?" + query_string(query);
        }
        return out;
    }
};

} // namespace fv::meeting_web::test_support
