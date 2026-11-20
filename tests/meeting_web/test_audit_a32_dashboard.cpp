// Claims of the user-facing checklist's dashboard lines (docs/port/map/a32-user-facing-features.md,
// section 7) that the ported tests leave unpinned: MD-01, the served bundle carries its own fonts
// and names no host to load from; MD-02, rotated links end the old capability tokens at once.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "api_harness.h"
#include "fake_web_repo.h"
#include "meeting_state/store.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

const std::string kHost(kFakeHostToken);
const std::string kGuest(kFakeGuestToken);

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

struct Client {
    Client() {
        store = std::make_shared<ms::MeetingStateStore>(fake_web_state(), nullptr,
                                                        ms::SegmentHandler{}, clock, ids);
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_test", store,
                                                                    root.path() / "meetings");
        mw::DashboardOptions options;
        options.dist_root = fv::testkit::source_root() / "webui" / "dist";
        options.meetings_root = root.path() / "meetings";
        app = std::make_unique<ApiServer>(*engine, repo, repo, mw::PostMeetingPasses{},
                                          std::move(options), root.path() / "settings.json", clock);
    }

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock{fixture_now()};
    ms::testing::FixedIds ids;
    std::shared_ptr<FakeWebRepo> repo = std::make_shared<FakeWebRepo>();
    std::shared_ptr<ms::MeetingStateStore> store;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::unique_ptr<ApiServer> app;
};

} // namespace

// MD-01: "scripts/fonts (DM Sans, Instrument Serif) bundled, no outside contacts".
TEST_CASE("a32 MD-01: the dashboard bundle carries its fonts and loads nothing from elsewhere",
          "[audit_a32]") {
    const auto dist = fv::testkit::source_root() / "webui" / "dist";
    bool dm_sans = false;
    bool instrument_serif = false;
    std::set<std::string> hosts;
    // A URL the page could fetch: scheme, then the host. XML namespaces and React's error-page
    // link (shown as text in a production error message) are names, never requested.
    const std::regex url(R"((?:https?:)?//([A-Za-z0-9.-]+\.[A-Za-z]{2,})[/"')\s])");
    const std::set<std::string> names{"www.w3.org", "reactjs.org"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dist)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        dm_sans = dm_sans || (name.starts_with("dm-sans-") && name.ends_with(".woff2"));
        instrument_serif =
            instrument_serif || (name.starts_with("instrument-serif-") && name.ends_with(".woff2"));
        const auto extension = entry.path().extension().string();
        if (extension != ".js" && extension != ".css" && extension != ".html") {
            continue;
        }
        // std::regex only around each "//", not across the whole 360 KB script.
        const std::string text = read_file(entry.path());
        for (auto at = text.find("//"); at != std::string::npos; at = text.find("//", at + 2)) {
            const auto from = at >= 6 ? at - 6 : 0;
            const std::string window = text.substr(from, at - from + 260);
            std::smatch found;
            if (std::regex_search(window, found, url) &&
                static_cast<std::size_t>(found.position(0)) + from <= at &&
                !names.contains(found[1].str())) {
                hosts.insert(found[1].str());
            }
        }
    }
    CHECK(dm_sans);
    CHECK(instrument_serif);
    CHECK(hosts.empty());

    // The fonts are served by the app itself, from /assets.
    Client web;
    const std::string font_name = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(dist / "assets")) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("dm-sans-latin-opsz-normal-")) {
                return name;
            }
        }
        return std::string();
    }();
    REQUIRE_FALSE(font_name.empty());
    const auto font = web.app->get("/assets/" + font_name);
    CHECK(font.status == 200);
    CHECK(font.body == read_file(dist / "assets" / font_name));
}

// MD-02: "regenerate both links (old REST tokens die at once ...)". The REST half reads the
// meeting row on every request, so the row's new pair is the only one honoured from then on.
TEST_CASE("a32 MD-02: once the links are rotated the old tokens are refused at once",
          "[audit_a32]") {
    Client web;
    REQUIRE(web.app->get("/api/session", {{"token", kHost}}).json()["role"] == "host");
    REQUIRE(web.app->get("/api/session", {{"token", kGuest}}).json()["role"] == "guest");

    Json rotated = fake_web_meeting();
    rotated["host_token"] = "rotated-host-token-cccccccccccccccccccc";
    rotated["guest_token"] = "rotated-guest-token-dddddddddddddddddddd";
    web.repo->set_meeting(rotated);

    const auto old_host = web.app->get("/api/session", {{"token", kHost}});
    CHECK(old_host.status == 401);
    CHECK(old_host.body == R"({"detail":"invalid token"})");
    CHECK(web.app->get("/api/session", {{"token", kGuest}}).status == 401);
    CHECK(web.app->get("/api/events", {{"token", kHost}}).status == 401);
    CHECK(web.app->get("/m/" + kHost).status == 403);
    CHECK(web.app->get("/m/" + kGuest).status == 403);

    CHECK(web.app->get("/api/session", {{"token", "rotated-host-token-cccccccccccccccccccc"}})
              .json()["role"] == "host");
    CHECK(web.app->get("/api/session", {{"token", "rotated-guest-token-dddddddddddddddddddd"}})
              .json()["role"] == "guest");
}
