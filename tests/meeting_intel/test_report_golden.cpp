// Golden vectors of meeting/custom_report.py (tools/parity/cases/meeting.custom_report.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "pyport/errors.h"
#include "report_support.h"

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using fv::Json;
using fv::golden::Case;

namespace {

constexpr std::string_view kModule = "meeting.custom_report";

// A vector value with its {"$float": ...} tags turned back into floats.
Json untag(const Json& value) {
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$float")) {
            return fv::golden::decode<double>(value);
        }
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = untag(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(untag(item));
        }
        return out;
    }
    return value;
}

std::vector<Json> rows(const Case& c, std::size_t index) {
    std::vector<Json> out;
    for (const auto& row : c.args().at(index)) {
        out.push_back(untag(row));
    }
    return out;
}

void check_equal(const Case& c, const Json& actual) {
    INFO(c.label());
    INFO("actual: " << actual.dump());
    CHECK(fv::golden::equal(c.raw().at("result"), actual));
}

// Runs `body`, checking the Python exception a raising case recorded.
template <class Body> void check_raises(const Case& c, Body body) {
    INFO(c.label());
    try {
        body();
        FAIL("no exception; Python raised " << c.raises_type());
    } catch (const fv::py::PyError& error) {
        CHECK(error.type_name() == c.raises_type());
    }
}

TranscriptCorpus corpus_for(const Json& state, const std::vector<Json>& segments) {
    const Json& participants =
        state.is_object() && state.contains("participants") ? state.at("participants") : Json();
    return TranscriptCorpus(segments, participants);
}

} // namespace

TEST_CASE("format_clock matches OpenWhisper", "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "format_clock", [](const Case& c) {
        const Json seconds = untag(c.args().at(0));
        if (c.raises()) {
            check_raises(c, [&] { format_clock(seconds); });
            return;
        }
        CHECK(format_clock(seconds) == c.result<std::string>());
    });
}

TEST_CASE("the transcript corpus matches OpenWhisper", "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "corpus", [](const Case& c) {
        const auto segments = rows(c, 0);
        const Json participants = untag(c.args().at(1));
        if (c.raises()) {
            check_raises(c, [&] { TranscriptCorpus(segments, participants); });
            return;
        }
        const TranscriptCorpus corpus(segments, participants);
        check_equal(c, Json{{"lines", corpus.lines()},
                            {"duration_s", corpus.duration_s()},
                            {"total_chars", corpus.total_chars()},
                            {"len", corpus.size()}});
    });
    fv::golden::for_each(kModule, "render_edges", [](const Case& c) {
        const TranscriptCorpus corpus(rows(c, 0), untag(c.args().at(1)));
        CHECK(corpus.render_edges(c.arg<std::size_t>(2)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "window", [](const Case& c) {
        const TranscriptCorpus corpus(rows(c, 0), Json::object());
        const double start = c.arg<double>(1);
        const double end = c.arg<double>(2);
        if (c.raises()) {
            check_raises(c, [&] { corpus.window(start, end); });
            return;
        }
        CHECK(corpus.window(start, end) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "search", [](const Case& c) {
        const TranscriptCorpus corpus(rows(c, 0), Json::object());
        const std::string query = c.arg<std::string>(1);
        const Json limit = untag(c.args().at(2));
        const auto run = [&] {
            return limit == "<default>" ? corpus.search(query) : corpus.search(query, limit);
        };
        if (c.raises()) {
            check_raises(c, run);
            return;
        }
        CHECK(run() == c.result<std::string>());
    });
}

TEST_CASE("the corpus helpers match OpenWhisper", "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "terms", [](const Case& c) {
        CHECK(detail::report_terms(c.arg<std::string>(0)) == c.result<std::vector<std::string>>());
    });
    fv::golden::for_each(kModule, "clip_lines", [](const Case& c) {
        CHECK(detail::clip_lines(c.arg<std::vector<std::string>>(0), c.arg<std::size_t>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("the digest and the opening prompt match OpenWhisper",
          "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "build_digest", [](const Case& c) {
        const Json meeting = untag(c.args().at(0));
        const Json state = untag(c.args().at(1));
        CHECK(build_digest(meeting, state, corpus_for(state, rows(c, 2))) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "build_corpus_prompt", [](const Case& c) {
        const Json meeting = untag(c.args().at(0));
        const Json state = untag(c.args().at(1));
        CHECK(build_corpus_prompt(meeting, state, corpus_for(state, rows(c, 2)),
                                  c.arg<std::string>(3)) == c.result<std::string>());
    });
}

TEST_CASE("the title, the cleanup and the request check match OpenWhisper",
          "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "report_title", [](const Case& c) {
        CHECK(report_title(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "_clean_markdown", [](const Case& c) {
        CHECK(detail::clean_markdown(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "validated_request", [](const Case& c) {
        Json actual = Json::object();
        try {
            actual["result"] = detail::validated_request(c.arg<std::string>(0));
        } catch (const ReportUnavailable& error) {
            actual["error"] = error.what();
        }
        check_equal(c, actual);
    });
}

TEST_CASE("the read tools match OpenWhisper", "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "tool_schemas", [](const Case& c) {
        check_equal(c, detail::report_tool_schemas(c.arg<bool>(0), c.arg<bool>(1)));
    });
    fv::golden::for_each(kModule, "run_tool", [](const Case& c) {
        const TranscriptCorpus corpus(rows(c, 0), Json::object());
        // The scratch settings' defaults: recall and the knowledge folder are off.
        detail::ReportTools tools(corpus, nullptr, [] { return Json::object(); }, "m_report");
        const fv::llm::ToolCall call{"c1", {c.arg<std::string>(1), c.arg<std::string>(2)}};
        const std::string content = detail::run_report_tool(tools, call);
        check_equal(c, Json{{"content", content}, {"used", tools.used()}});
    });
}

TEST_CASE("a report pass sends OpenWhisper's requests and returns its result",
          "[golden][meeting.custom_report]") {
    fv::golden::for_each(kModule, "generate_report", [](const Case& c) {
        const Json turns = c.args().at(0);
        // Python's fake client answers every call past the script with its last turn.
        std::vector<fv::testing::SseReply> replies;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kReportMaxToolRounds); ++i) {
            const Json& turn = turns.at(std::min(i, turns.size() - 1));
            replies.push_back(fv::testing::SseReply::json(
                200, chat_reply(turn.at("text").get<std::string>(), turn.at("calls"),
                                turn.at("finish_reason").get<std::string>(),
                                turn.contains("usage") ? turn.at("usage") : Json())));
        }
        const bool recall = c.arg<bool>(2);
        ReportHarness harness(std::move(replies), recall);
        harness.consent(recall, c.arg<bool>(3));
        Json endpoint = c.args().at(4);
        endpoint["base_url"] = harness.provider.url("/v1");
        Json state = ended_state().to_json();
        state["participants"] = Json{{"p_1", {{"display_name", "Priya"}}}};

        Json actual = Json::object();
        try {
            actual["result"] =
                generate_report(report_meeting(), state, report_segments(), c.arg<std::string>(1),
                                ReportRoute{"custom_report", "test-model", endpoint}, harness.deps);
        } catch (const ReportUnavailable& error) {
            actual["error"] = error.what();
        }
        const Json& expected = c.raw().at("result");
        INFO(c.label());
        INFO("actual: " << actual.dump());
        if (expected.contains("error")) {
            CHECK(actual.value("error", std::string()) == expected.at("error").get<std::string>());
        } else {
            CHECK(fv::golden::equal(expected.at("result"), actual.at("result")));
        }
        // The request bodies, on the fields the report pass decides.
        const auto bodies = harness.bodies();
        const Json& requests = expected.at("requests");
        REQUIRE(bodies.size() == requests.size());
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            INFO("request " << i);
            const Json& want = requests.at(i);
            CHECK(bodies[i].at("model") == want.at("model"));
            CHECK(fv::golden::equal(want.at("temperature"), bodies[i].at("temperature")));
            CHECK(fv::golden::equal(want.at("messages"), bodies[i].at("messages")));
            CHECK(bodies[i].contains("tools") == want.contains("tools"));
            if (want.contains("tools") && bodies[i].contains("tools")) {
                CHECK(fv::golden::equal(want.at("tools"), bodies[i].at("tools")));
            }
        }
    });
}
