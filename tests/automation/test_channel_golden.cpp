// The Claude Code channel against OpenWhisper: the golden vectors of
// services/automation/channel_bridge.py (tools/parity/cases/services.automation.channel_bridge.py)
// and the channel transcript (tools/parity/transcripts/channel.py), replayed through the C++
// ChannelServer over in-memory line streams.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "automation/detail/sse_parser.h"
#include "base/identity.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "support/channel_support.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::golden::Case;
using namespace fv::automation;
using namespace fv::automation::channel_test;

namespace {

constexpr std::string_view kModule = "services.automation.channel_bridge";

Json rebranded(const Json& expected) {
    return fv::py::loads(rebranded_text(fv::py::dumps(expected)));
}

// The product names of messages only: a host the case names keeps its spelling.
Json rebranded_messages(const Json& expected) {
    return fv::py::loads(fv::golden::rebrand(fv::py::dumps(expected)));
}

Json target_answer(const std::function<Target()>& make) {
    try {
        const Target target = make();
        return Json::object({{"host", target.host},
                             {"port", target.port},
                             {"https", target.https},
                             {"base_path", target.base_path}});
    } catch (const LinkError& error) {
        return Json::object({{"error", error.state()}, {"message", error.message()}});
    }
}

std::function<Json()> settings_reader(const Json& saved) {
    return [saved]() -> Json {
        if (saved.is_null()) {
            throw std::runtime_error("unreadable");
        }
        return saved;
    };
}

AppLinkOptions offline_options() {
    AppLinkOptions options;
    options.read_token = [] {
        return std::optional<std::string>();
    };
    options.client = std::make_shared<fv::http::FakeHttpClient>();
    return options;
}

// json.dumps with Python's default separators, non-ASCII escaped.
std::string dumps(const Json& value) {
    return fv::py::dumps(value);
}

Json pairs(const Json& object) {
    Json out = Json::array();
    for (const auto& [key, value] : object.items()) {
        out.push_back(Json::array({key, value}));
    }
    return out;
}

} // namespace

TEST_CASE("channel_bridge golden: SseParser.feed", "[golden][test_automation_channel_protocol]") {
    fv::golden::for_each(kModule, "sse_feed", [](const Case& c) {
        SseParser parser(c.arg<std::string>(1));
        Json events = Json::array();
        for (const std::string& line : c.arg<std::vector<std::string>>(0)) {
            if (auto event = parser.feed(line)) {
                events.push_back(Json::array({event->type, event->data, event->last_id}));
            }
        }
        const Json actual = Json::object({{"events", events}, {"last_id", parser.last_id()}});
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("channel_bridge golden: clean_meta, origin_of, channel_params", "[golden]") {
    fv::golden::for_each(kModule, "clean_meta", [](const Case& c) {
        const Json meta = fv::py::loads(c.arg<std::string>(0));
        CHECK(dumps(pairs(clean_meta(meta))) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "origin_of", [](const Case& c) {
        CHECK(origin_of(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "channel_params", [](const Case& c) {
        const std::optional<Json> params = channel_params(fv::py::loads(c.arg<std::string>(0)));
        const std::optional<std::string> expected = c.result<std::optional<std::string>>();
        REQUIRE(params.has_value() == expected.has_value());
        if (params) {
            const Json actual = Json::object(
                {{"content", (*params)["content"]}, {"meta", pairs((*params)["meta"])}});
            CHECK(dumps(actual) == *expected);
        }
    });
}

TEST_CASE("channel_bridge golden: reply arguments and results", "[golden]") {
    fv::golden::for_each(kModule, "reply_arguments", [](const Case& c) {
        const Json expected = c.result<Json>();
        try {
            const auto [text, title] = reply_arguments(fv::py::loads(c.arg<std::string>(0)));
            REQUIRE(expected.contains("text_json"));
            CHECK(dumps(Json(text)) == expected["text_json"].get<std::string>());
            CHECK(dumps(Json(title)) == expected["title_json"].get<std::string>());
        } catch (const std::invalid_argument& error) {
            REQUIRE(expected.contains("error"));
            CHECK(std::string(error.what()) == expected["error"].get<std::string>());
        }
    });
    fv::golden::for_each(kModule, "tool_result", [](const Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(),
                                tool_result(c.arg<std::string>(0), c.arg<bool>(1))));
    });
}

TEST_CASE("channel_bridge golden: refusals", "[golden]") {
    fv::golden::for_each(kModule, "reply_refusal", [](const Case& c) {
        CHECK(fv::golden::equal_rebranded(c.result<std::string>(),
                                          reply_refusal(c.arg<int>(0), c.arg<std::string>(1))));
    });
    fv::golden::for_each(kModule, "stream_refusal", [](const Case& c) {
        const auto [state, message] = stream_refusal(c.arg<int>(0), c.arg<std::string>(1));
        const Json expected = c.result<Json>();
        CHECK(state == expected[0].get<std::string>());
        CHECK(fv::golden::equal_rebranded(expected[1].get<std::string>(), message));
    });
    fv::golden::for_each(kModule, "error_message", [](const Case& c) {
        CHECK(error_message(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("channel_bridge golden: parse_url and targets", "[golden]") {
    fv::golden::for_each(kModule, "parse_url", [](const Case& c) {
        const std::string url = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(parse_url(url), fv::py::ValueError);
            return;
        }
        CHECK(fv::golden::equal(rebranded_messages(c.result<Json>()),
                                target_answer([&] { return parse_url(url); })));
    });
    fv::golden::for_each(kModule, "target", [](const Case& c) {
        const Target target{c.arg<std::string>(0), c.arg<int>(1), c.arg<bool>(2), ""};
        const Json actual = Json::object(
            {{"label", target.label()}, {"on_this_computer", target.on_this_computer()}});
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
    fv::golden::for_each(kModule, "link_target", [](const Case& c) {
        AppLinkOptions options = offline_options();
        options.url = c.arg<std::string>(0);
        options.token = c.arg<std::string>(1);
        options.read_settings = settings_reader(c.arg<Json>(2));
        AppLink link(options);
        CHECK(fv::golden::equal(rebranded_messages(c.result<Json>()),
                                target_answer([&] { return link.target(); })));
    });
    fv::golden::for_each(kModule, "configured_port", [](const Case& c) {
        CHECK(configured_port(c.arg<Json>(0)) == c.result<int>());
    });
    fv::golden::for_each(kModule, "unreachable_message", [](const Case& c) {
        AppLinkOptions options = offline_options();
        options.url = c.arg<std::string>(0);
        options.read_settings = settings_reader(c.arg<Json>(1));
        CHECK(fv::golden::equal_rebranded(c.result<std::string>(),
                                          AppLink(options).unreachable_message()));
    });
}

TEST_CASE("channel_bridge golden: headers, gaps, initialize", "[golden]") {
    fv::golden::for_each(kModule, "headers", [](const Case& c) {
        AppLinkOptions options = offline_options();
        options.version = c.arg<std::string>(0);
        fv::http::Headers extra;
        const auto environment = c.arg<Json>(2);
        for (const auto& [name, value] : environment.items()) {
            extra.emplace_back(name, value.get<std::string>());
        }
        Json actual = Json::array();
        for (const auto& [name, value] : AppLink(options).headers(c.arg<std::string>(1), extra)) {
            actual.push_back(Json::array({name, value}));
        }
        CHECK(fv::golden::equal(rebranded(c.result<Json>()), actual));
    });
    fv::golden::for_each(kModule, "gap_reason", [](const Case& c) {
        CHECK(gap_reason(SseEvent{"events.gap", c.arg<std::string>(0), ""}) ==
              c.result<std::string>());
    });
    fv::golden::for_each(kModule, "initialize", [](const Case& c) {
        Pipe in;
        Output out;
        FakeLink link;
        ChannelServerOptions options;
        options.version = c.arg<std::string>(1);
        ChannelServer server(in, out, link, std::move(options));
        CHECK(fv::golden::equal(rebranded(c.result<Json>()),
                                server.initialize(fv::py::loads(c.arg<std::string>(0)))));
    });
}

TEST_CASE("channel_bridge golden: constants", "[golden]") {
    fv::golden::for_each(kModule, "constants", [](const Case& c) {
        const Json expected = rebranded(c.result<Json>());
        CHECK(expected["PROTOCOL_VERSIONS"] ==
              Json(std::vector<std::string>(PROTOCOL_VERSIONS.begin(), PROTOCOL_VERSIONS.end())));
        CHECK(expected["CLIENT_HEADER"] == channel_client_header());
        CHECK(expected["CLIENT_NAME"] == std::string(CHANNEL_CLIENT_NAME));
        CHECK(expected["EVENTS_PATH"] == std::string(EVENTS_PATH));
        CHECK(expected["NOTIFY_PATH"] == std::string(NOTIFY_PATH));
        CHECK(expected["URL_ENV"] == url_env());
        CHECK(expected["TOKEN_ENV"] == token_env());
        CHECK(expected["BACKOFF_FIRST_S"].get<double>() * 1000 == CHANNEL_BACKOFF_FIRST.count());
        CHECK(expected["BACKOFF_MAX_S"].get<double>() * 1000 == CHANNEL_BACKOFF_MAX.count());
        CHECK(expected["CONNECT_TIMEOUT_S"].get<double>() * 1000 ==
              CHANNEL_CONNECT_TIMEOUT.count());
        CHECK(expected["READ_TIMEOUT_S"].get<double>() * 1000 == CHANNEL_READ_TIMEOUT.count());
        CHECK(expected["NOTIFY_TIMEOUT_S"].get<double>() * 1000 == CHANNEL_NOTIFY_TIMEOUT.count());
        CHECK(expected["MAX_REPLY_CHARS"] == MAX_REPLY_CHARS);
        CHECK(expected["MAX_TITLE_CHARS"] == MAX_TITLE_CHARS);
        CHECK(expected["MAX_META_CHARS"] == MAX_META_CHARS);
        CHECK(expected["PARSE_ERROR"] == PARSE_ERROR);
        CHECK(expected["INVALID_REQUEST"] == INVALID_REQUEST);
        CHECK(expected["METHOD_NOT_FOUND"] == METHOD_NOT_FOUND);
        CHECK(expected["INVALID_PARAMS"] == INVALID_PARAMS);
        CHECK(expected["ORIGIN_OPENWHISPER"] == channel_notice_origin());
        CHECK(expected["GAP_NOTICE"] == gap_notice());
        CHECK(expected["INSTRUCTIONS"] == channel_instructions());
        CHECK(expected["REPLY_TOOL"] == reply_tool());
        // The environment block's names are padded to one column, which the longer legacy
        // names pushed out by two characters; the words are the same.
        const auto words = [](std::string_view text) {
            return fv::py::split(text);
        };
        const std::string usage = channel_usage(fv::py::lower(fv::identity().product));
        CHECK(words(usage) == words(expected["USAGE"].get<std::string>()));
        CHECK(usage.find("\n  " + url_env() + "       The app's address") != std::string::npos);
        CHECK(usage.find("\n  " + token_env() + "     The access token") != std::string::npos);
    });
}

// --- the transcript ------------------------------------------------------------------------------

namespace {

Json load_transcript() {
    std::ifstream in(fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" /
                         "channel.json",
                     std::ios::binary);
    REQUIRE(in);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return fv::py::loads(text);
}

std::vector<std::string> expected_lines(const Json& exchange) {
    std::vector<std::string> out;
    for (const Json& line : exchange["receive"]) {
        out.push_back(rebranded_text(line.get<std::string>()));
    }
    return out;
}

} // namespace

TEST_CASE("channel transcript: the server answers line for line as OpenWhisper's",
          "[golden][test_automation_channel_protocol]") {
    const Json transcript = load_transcript();
    Pipe in;
    Output out;
    FakeLink link;
    link.fail = [](const std::string& text) {
        if (text == "refuse") {
            throw LinkError("http_403", reply_refusal(403, ""));
        }
        if (text == "explode") {
            throw std::runtime_error("boom");
        }
    };
    auto counts = std::make_shared<StreamCounts>();
    ChannelServerOptions options;
    options.version = transcript["version"].get<std::string>();
    options.stream_factory = [counts] {
        return std::make_unique<FakeStream>(counts);
    };
    ChannelServer server(in, out, link, std::move(options));
    std::thread serving([&] { server.serve(); });

    std::size_t sent_lines = 0;
    std::size_t written = 0;
    std::size_t notified = 0;
    EventStream gaps(link, [&server](const Json& params) { server.send_channel_message(params); });
    int index = 0;
    for (const Json& exchange : transcript["exchanges"]) {
        INFO("exchange " << index++ << ": " << fv::py::dumps(exchange));
        const std::vector<std::string> expected = expected_lines(exchange);
        if (exchange.contains("send")) {
            in.put_line(exchange["send"].get<std::string>() + "\n");
            ++sent_lines;
            // The line is handled once the server asks for the next one; a reply answers from a
            // thread of its own, so its line is waited for.
            REQUIRE(in.wait_reads(sent_lines + 1));
            if (!expected.empty()) {
                out.messages(written + expected.size());
            }
        } else if (exchange.contains("deliver")) {
            const std::optional<Json> params = channel_params(exchange["event"]);
            REQUIRE(params);
            CHECK(fv::py::dumps(*params) == fv::py::dumps(exchange["deliver"]));
            server.send_channel_message(*params);
        } else {
            const Json& gap = exchange["gap"];
            gaps.handle(SseEvent{gap["type"].get<std::string>(), gap["data"].get<std::string>(),
                                 gap["last_id"].get<std::string>()});
        }
        const std::vector<std::string> lines = out.lines();
        REQUIRE(lines.size() == written + expected.size());
        CHECK(std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(written),
                                       lines.end()) == expected);
        written = lines.size();
        if (exchange.contains("send")) {
            const auto sent = link.sent();
            std::vector<std::pair<std::string, std::string>> recorded;
            for (const Json& pair : exchange["notified"]) {
                recorded.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
            }
            CHECK(std::vector<std::pair<std::string, std::string>>(
                      sent.begin() + static_cast<std::ptrdiff_t>(notified), sent.end()) ==
                  recorded);
            notified = sent.size();
            CHECK(counts->started == exchange["streams"].get<int>());
        }
    }
    in.close();
    serving.join();
}
