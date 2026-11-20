// Replays tests/fixtures/transcripts/agent_api.json, recorded from OpenWhisper's History API
// (tools/parity/record_transcripts.py, scenario tools/parity/transcripts/agent_api.py), against
// agentapi::HistoryApi::handle over the same fixture database.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "agentapi/history_routes.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/json.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;

constexpr std::string_view kNormalized = "<normalized>";

Json load_transcript(std::string_view name) {
    const auto path = fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" /
                      (std::string(name) + ".json");
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return fv::py::loads(text.str());
}

const Json& at_pointer(const Json& document, const std::string& pointer) {
    return document.at(Json::json_pointer(pointer));
}

std::string render(const Json& value) {
    return render_json(value);
}

} // namespace

TEST_CASE("the History API answers every recorded request as OpenWhisper did", "[golden]") {
    const Json transcript = load_transcript("agent_api");
    const Json& context = transcript.at("context");
    fv::testkit::TempDataRoot root;
    const auto database =
        test::copy_history_fixture(root.path(), context.at("database_name").get<std::string>());
    HistoryApi api(database, context.at("token").get<std::string>());

    std::vector<std::pair<std::string, std::string>> rules;
    for (const Json& rule : transcript.at("normalize")) {
        if (rule.is_object()) {
            rules.emplace_back(rule.at("pointer").get<std::string>(),
                               rule.at("pattern").get<std::string>());
        }
    }

    std::vector<std::string> bodies;
    const Json& exchanges = transcript.at("exchanges");
    for (std::size_t index = 0; index < exchanges.size(); ++index) {
        const Json& exchange = exchanges[index];
        const Json& recorded = exchange.at("request");
        fv::net::HttpRequest request;
        request.method = recorded.at("method").get<std::string>();
        request.path = recorded.at("path").get<std::string>();
        request.tls = false;
        request.peer = {"127.0.0.1", 50000};
        request.body = recorded.at("body").get<std::string>();
        for (const Json& pair : recorded.at("headers")) {
            request.headers.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
        }
        const Json refs =
            exchange.contains("refs") ? exchange.at("refs").at("query") : Json::object();
        for (const Json& pair : recorded.at("query")) {
            const std::string name = pair[0].get<std::string>();
            std::string value = pair[1].get<std::string>();
            if (refs.contains(name)) {
                // The value an earlier answer gave; taken from this replay's own answer.
                const auto from = refs.at(name)[0].get<std::size_t>();
                const std::string pointer = refs.at(name)[1].get<std::string>();
                REQUIRE(from < bodies.size());
                value = at_pointer(fv::py::loads(bodies[from]), pointer).get<std::string>();
            }
            request.query.emplace_back(name, value);
        }

        const Json& expected = exchange.at("response");
        const ApiResponse actual = api.handle(request);
        bodies.push_back(actual.body);

        INFO("exchange " << index << ": " << request.method << " " << request.path << " "
                         << recorded.at("query").dump());
        CHECK(actual.status == expected.at("status").get<int>());

        // Content-Length is the server's; it must match the body the handler gave.
        fv::net::Headers expected_headers;
        std::optional<std::size_t> content_length;
        for (const Json& pair : expected.at("headers")) {
            const std::string name = pair[0].get<std::string>();
            const std::string value = pair[1].get<std::string>();
            if (name == "content-length") {
                content_length = std::stoull(value);
            } else {
                expected_headers.emplace_back(name, value);
            }
        }
        CHECK(actual.headers == expected_headers);
        const std::string recorded_body = expected.at("body").get<std::string>();
        const std::string expected_body = fv::golden::rebrand(recorded_body);
        if (content_length) {
            // The recorded length less what the rebranding shortened (the OpenAPI title).
            CHECK(actual.body.size() + recorded_body.size() ==
                  *content_length + expected_body.size());
        }
        if (request.method == "HEAD") {
            continue; // the server sends HEAD answers without their body
        }

        std::string body = actual.body;
        if (exchange.contains("normalized")) {
            Json document = fv::py::loads(body);
            for (const Json& pointer_json : exchange.at("normalized")) {
                const std::string pointer = pointer_json.get<std::string>();
                for (const auto& [rule_pointer, pattern] : rules) {
                    if (rule_pointer == pointer) {
                        const std::string value = at_pointer(document, pointer).get<std::string>();
                        CHECK(std::regex_match(value, std::regex(pattern)));
                    }
                }
                document[Json::json_pointer(pointer)] = std::string(kNormalized);
            }
            body = render(document);
        }
        CHECK(body == expected_body);
    }
}
