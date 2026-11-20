// Replays tests/fixtures/transcripts/meeting_api.json, recorded from OpenWhisper's FastAPI app
// (tools/parity/record_transcripts.py, scenario tools/parity/transcripts/meeting_api.py), against
// the C++ routes on a loopback server over a copy of the same fixture database, with ids and time
// pinned the same way and the same canned post-meeting passes. Every answer must have the same
// status, the same headers (the server's own content-length checked against the body; ETag and
// Last-Modified only present) and the same body.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "api_harness.h"
#include "base/identity.h"
#include "meeting_state/stored.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "meeting_web/voice_help.h"
#include "pyport/errors.h"
#include "pyport/json.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;

namespace {

constexpr std::string_view kNormalized = "<normalized>";

Json load_transcript() {
    const auto path =
        fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" / "meeting_api.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    return fv::py::loads(text.str());
}

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Headers compared as a set of (name, value), without what only the transport decides (the
// recording went through Starlette's TestClient, which adds no date or server header).
std::vector<std::string> comparable(HeaderList headers,
                                    const std::vector<std::string>& normalized) {
    std::vector<std::string> out;
    for (auto& [name, value] : headers) {
        if (name == "content-length" || name == "connection" || name == "date" ||
            name == "server") {
            continue;
        }
        if (std::ranges::find(normalized, name) != normalized.end()) {
            value = std::string(kNormalized);
        }
        out.push_back(name + ": " + value);
    }
    std::ranges::sort(out);
    return out;
}

} // namespace

TEST_CASE("the dashboard API answers every recorded request as OpenWhisper did", "[golden]") {
    const Json transcript = load_transcript();
    const Json& context = transcript.at("context");
    std::vector<std::string> normalized;
    for (const Json& rule : transcript.at("normalize")) {
        REQUIRE(rule.is_string());
        normalized.push_back(rule.get<std::string>());
    }

    SqlWorld world(fv::testkit::source_root() / context.at("fixture").get<std::string>());
    const std::string current = context.at("current_meeting").get<std::string>();
    auto row = world.repo->get_meeting(current);
    REQUIRE(row.has_value());
    auto store = fv::meeting_state::open_store(world.repo, current, *row, false, Json::object(),
                                               world.clock, world.ids);
    const auto meetings_root = world.root.path() / "meetings";
    std::filesystem::create_directories(meetings_root);
    mw::testing::FakeDashboardEngine engine(current, store, meetings_root);
    const Json rotated = context.at("regenerate_tokens");
    engine.set_regenerate([rotated] { return rotated; });

    mw::PostMeetingPasses passes;
    const Json rerun_result = context.at("rerun_result");
    const Json busy = context.at("rerun_busy");
    const Json value_error = context.at("rerun_value_error");
    std::vector<std::string> from_steps;
    passes.rerun_insights = [&](const std::string& meeting_id, const std::string& from_step,
                                const fv::CancelToken&) {
        from_steps.push_back(from_step);
        if (busy.contains(meeting_id)) {
            throw mw::PostMeetingPassBusy(busy.at(meeting_id).get<std::string>());
        }
        if (value_error.contains(meeting_id)) {
            throw fv::py::ValueError(value_error.at(meeting_id).get<std::string>());
        }
        return rerun_result;
    };
    int speaker_reruns = 0;
    passes.rerun_speakers =
        [&speaker_reruns](const std::string&,
                          std::shared_ptr<fv::meeting_state::MeetingStateStore>) {
            ++speaker_reruns; // the recorded speaker re-runs never pass the gate
            return Json::object();
        };
    const std::string refusal = context.at("speaker_gate_refusal").get<std::string>();
    passes.speaker_gate_refusal = [refusal](const Json&) {
        return std::optional(refusal);
    };

    mw::DashboardOptions options;
    options.dist_root = fv::testkit::source_root() / context.at("dist").get<std::string>();
    options.meetings_root = meetings_root;
    ApiServer app(engine, world.repo, mw::sql_past_meeting_pages(world.repo), std::move(passes),
                  std::move(options), world.root.path() / "settings.json", world.clock);
    // OpenWhisper's default wake names, as the recording's settings had them.
    app.server->hub().set_voice_commands(
        [] { return mw::voice_command_guide(Json::object(), fv::legacy_identity()); });

    std::vector<std::string> bodies;
    const Json& exchanges = transcript.at("exchanges");
    for (std::size_t index = 0; index < exchanges.size(); ++index) {
        const Json& exchange = exchanges[index];
        const Json& recorded = exchange.at("request");
        const Json refs =
            exchange.contains("refs") ? exchange.at("refs").at("query") : Json::object();
        Query query;
        for (const Json& pair : recorded.at("query")) {
            const std::string name = pair[0].get<std::string>();
            std::string value = pair[1].get<std::string>();
            if (refs.contains(name)) {
                // The value an earlier answer gave; taken from this replay's own answer.
                const auto from = refs.at(name)[0].get<std::size_t>();
                REQUIRE(from < bodies.size());
                value = fv::py::loads(bodies[from])
                            .at(Json::json_pointer(refs.at(name)[1].get<std::string>()))
                            .get<std::string>();
            }
            query.emplace_back(name, std::move(value));
        }
        HeaderList headers;
        for (const Json& pair : recorded.at("headers")) {
            if (pair[0] == "content-type") {
                headers.emplace_back("Content-Type", pair[1].get<std::string>());
            }
        }
        std::string target = recorded.at("raw_path").get<std::string>();
        if (!query.empty()) {
            target += "?" + query_string(query);
        }
        const std::string method = recorded.at("method").get<std::string>();
        INFO("exchange " << index << ": " << method << " " << recorded.at("path").get<std::string>()
                         << " " << recorded.at("query").dump());
        const HttpResult actual =
            http_request(app.port, method, target, headers, recorded.at("body").get<std::string>());
        bodies.push_back(actual.body);

        const Json& expected = exchange.at("response");
        CHECK(actual.status == expected.at("status").get<int>());
        if (const auto length = actual.header("content-length")) {
            CHECK(std::stoull(*length) == actual.body.size());
        }
        HeaderList expected_headers;
        for (const Json& pair : expected.at("headers")) {
            expected_headers.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
        }
        CHECK(comparable(actual.headers, normalized) == comparable(expected_headers, normalized));
        CHECK(actual.body == expected.at("body").get<std::string>());
    }
    // Every re-run starts from the earliest failed step, as the dashboard's button asks.
    CHECK(from_steps == std::vector<std::string>(from_steps.size(), "failed"));
    CHECK(from_steps.size() == 3);
    CHECK(speaker_reruns == 0);
}
