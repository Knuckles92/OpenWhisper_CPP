// The Claude Code channel's MCP handling and event parsing, in process.
// from tests/test_automation_channel_protocol.py, tests/test_automation_fix_channel_gap.py,
// tests/test_automation_fix_channel_link.py and tests/test_automation_fix_channel_origin.py
// (the setup snippets, the update check and the process entry of the protocol file are tested
// with fiorivoce-cli in tests/cli).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "automation/catalog.h"
#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "automation/detail/sse_parser.h"
#include "base/identity.h"
#include "http/testing/fake_http_client.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "support/channel_support.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using namespace fv::automation;
using namespace fv::automation::channel_test;

namespace {

const std::string kToken = "t0ken-for-the-channel-tests-0123456789abcdef";

std::pair<std::vector<SseEvent>, SseParser> parse(const std::vector<std::string>& lines,
                                                  std::string last_id = {}) {
    SseParser parser(std::move(last_id));
    std::vector<SseEvent> events;
    for (const std::string& line : lines) {
        if (auto event = parser.feed(line)) {
            events.push_back(*event);
        }
    }
    return {events, parser};
}

Json rpc(const Json& id, std::string method, Json params = nullptr) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = std::move(method);
    if (!params.is_null()) {
        message["params"] = std::move(params);
    }
    return message;
}

Json notice(std::string method) {
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = std::move(method);
    return message;
}

std::string lower_product() {
    return fv::py::lower(fv::identity().product);
}

AppLinkOptions link_options() {
    AppLinkOptions options;
    options.read_settings = [] {
        return Json::object();
    };
    options.read_token = [] {
        return std::optional<std::string>(kToken);
    };
    options.client = std::make_shared<fv::http::FakeHttpClient>();
    return options;
}

} // namespace

// --- server-sent events -------------------------------------------------------------------------

TEST_CASE("test_sse_parser_reads_typed_events_with_ids", "[test_automation_channel_protocol]") {
    const auto [events, parser] = parse({": ping", "", "id: 7", "event: channel.message",
                                         R"(data: {"a":1})", "", "id: 8", "data: x", ""});
    REQUIRE(events.size() == 2);
    CHECK(events[0] == SseEvent{"channel.message", R"({"a":1})", "7"});
    CHECK(events[1] == SseEvent{"message", "x", "8"});
    CHECK(parser.last_id() == "8");
}

TEST_CASE("test_sse_parser_joins_data_lines_and_strips_one_space",
          "[test_automation_channel_protocol]") {
    const auto [events, parser] = parse({"data:  two spaces", "data:none", "data", ""});
    REQUIRE(events.size() == 1);
    CHECK(events[0].data == " two spaces\nnone\n");
}

TEST_CASE("test_sse_parser_moves_the_resume_point_only_when_a_block_ends",
          "[test_automation_channel_protocol]") {
    SseParser parser("3");
    parser.feed("id: 4");
    parser.feed("data: half an event");
    CHECK(parser.last_id() == "3"); // broken here, the stream resumes before event 4
    parser.feed("");
    CHECK(parser.last_id() == "4");
}

TEST_CASE("test_sse_parser_leaves_the_resume_point_alone_for_a_gap_and_retry",
          "[test_automation_channel_protocol]") {
    // The live API opens with retry: and sends events.gap without an id: line.
    const std::string gap =
        R"({"id": "", "seq": 0, "type": "events.gap", "data": {"missed_from_seq": 4}})";
    const auto [events, parser] =
        parse({"retry: 2000", "", "event: events.gap", "data: " + gap, ""}, "12");
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == "events.gap");
    CHECK(events[0].last_id == "12");
    CHECK(parser.last_id() == "12");
}

TEST_CASE("test_sse_parser_keeps_ids_from_blocks_without_data_and_ignores_nul_ids",
          "[test_automation_channel_protocol]") {
    const auto [events, parser] =
        parse({"id: 9", "", std::string("id: bad\0id", 10), "data: y", ""});
    CHECK(parser.last_id() == "9");
    REQUIRE(events.size() == 1);
    CHECK(events[0].last_id == "9");
}

// --- what reaches Claude ------------------------------------------------------------------------

TEST_CASE("test_clean_meta_keeps_only_what_claude_code_accepts",
          "[test_automation_channel_protocol]") {
    const Json meta = fv::py::loads(R"({"job_id": "abc", "app": "  Visual\nStudio   Code ",
        "with-hyphen": "dropped", "9lives": "dropped", "_ok": "kept", "empty": "   ",
        "number": 5, "none": null, "long": ")" +
                                    std::string(500, 'x') + R"("})");
    const Json expected = Json::parse(R"({"job_id": "abc", "app": "Visual Studio Code",
        "_ok": "kept", "long": ")" + std::string(MAX_META_CHARS, 'x') +
                                      R"("})");
    CHECK(clean_meta(meta) == expected);
}

TEST_CASE("test_channel_params_carry_the_text_and_routing_meta",
          "[test_automation_channel_protocol]") {
    const Json event = Json::parse(R"({"type": "channel.message", "data": {
        "job_id": "0123456789abcdef", "source": "hotkey", "origin": "dictated",
        "action_id": "claude", "text": "  fix the flaky test\n",
        "app": {"id": "code", "name": "Visual Studio Code", "category": "editor"}}})");
    const Json expected = Json::parse(R"({"content": "  fix the flaky test\n", "meta": {
        "origin": "dictated", "job_id": "0123456789abcdef", "action": "claude",
        "app": "Visual Studio Code"}})");
    CHECK(channel_params(event) == expected);
}

TEST_CASE("test_channel_params_accept_an_app_name_and_leave_out_empty_meta",
          "[test_automation_channel_protocol]") {
    // An event without an origin is treated as a program's.
    CHECK(channel_params(Json::parse(R"({"data": {"text": "hi", "app": "Terminal"}})")) ==
          Json::parse(R"({"content": "hi", "meta": {"origin": "program", "app": "Terminal"}})"));
    CHECK(channel_params(
              Json::parse(R"({"data": {"text": "hi", "app": null, "action_id": null}})")) ==
          Json::parse(R"({"content": "hi", "meta": {"origin": "program"}})"));
}

TEST_CASE("test_channel_params_skip_events_without_text", "[test_automation_channel_protocol]") {
    const std::string event = GENERATE(as<std::string>{}, "null", "[]", "{}", R"({"data": null})",
                                       R"({"data": {"text": ""}})", R"({"data": {"text": "  \n"}})",
                                       R"({"data": {"text": 5}})");
    INFO(event);
    CHECK_FALSE(channel_params(Json::parse(event)).has_value());
}

// --- the MCP server ------------------------------------------------------------------------------

TEST_CASE("an unknown tool's name is written as Python's f-string writes it", "[unit]") {
    const auto [name, expected] = GENERATE(table<std::string, std::string>({
        {R"({"a": [1, "x", null, true, 1.5]})", "Unknown tool: {'a': [1, 'x', None, True, 1.5]}"},
        {R"([])", "Unknown tool: []"},
        {R"(["it's"])", "Unknown tool: [\"it's\"]"},
        {R"(null)", "Unknown tool: None"},
        {R"("rm_rf")", "Unknown tool: rm_rf"},
    }));
    Harness h;
    h.in.put(rpc(1, "tools/call", Json::object({{"name", fv::py::loads(name)}})));
    CHECK(h.out.messages(1)[0]["error"]["message"] == expected);
}

TEST_CASE("test_initialize_negotiates_a_handshake_revision", "[test_automation_channel_protocol]") {
    const auto [requested, answered] =
        GENERATE(table<Json, std::string>({{"2025-11-25", "2025-11-25"},
                                           {"2025-06-18", "2025-06-18"},
                                           {"2024-11-05", "2024-11-05"},
                                           {"2026-07-28", "2025-11-25"},
                                           {"1999-01-01", "2025-11-25"},
                                           {nullptr, "2025-11-25"}}));
    Harness h;
    h.in.put(rpc(1, "initialize", Json::object({{"protocolVersion", requested}})));
    const Json result = h.out.messages(1)[0]["result"];
    CHECK(result["protocolVersion"] == answered);
    CHECK(result["capabilities"] ==
          Json::parse(R"({"experimental": {"claude/channel": {}}, "tools": {}})"));
    CHECK(result["serverInfo"] == Json::object({{"name", lower_product()},
                                                {"title", std::string(fv::identity().product)},
                                                {"version", "9.9.9"}}));
    CHECK(fv::py::len(result["instructions"].get<std::string>()) < 2048);
    CHECK_THAT(result["instructions"].get<std::string>(),
               ContainsSubstring("<channel source=\"" + lower_product() + "\""));
    CHECK(h.finish() == 0);
}

TEST_CASE("test_discover_probe_and_unknown_methods_are_method_not_found",
          "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put(rpc("probe", "server/discover", Json::object()));
    h.in.put(rpc(2, "resources/list"));
    Json unknown = notice("notifications/something_new");
    h.in.put(unknown);
    h.in.put(rpc(3, "ping"));
    const std::vector<Json> messages = h.out.messages(3);
    CHECK(messages[0] == Json::parse(R"({"jsonrpc": "2.0", "id": "probe",
        "error": {"code": -32601, "message": "Method not found: server/discover"}})"));
    CHECK(messages[1]["id"] == 2);
    CHECK(messages[1]["error"]["code"] == -32601);
    CHECK(messages[2] == Json::parse(R"({"jsonrpc": "2.0", "id": 3, "result": {}})"));
    CHECK(h.finish() == 0);
    CHECK(h.out.messages(3).size() == 3); // the notification got no answer
}

TEST_CASE("test_malformed_input_gets_json_rpc_errors", "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put_line("{not json\n");
    h.in.put_line(R"([{"jsonrpc": "2.0", "id": 1, "method": "ping"}])"
                  "\n");
    h.in.put_line("\"just a string\"\n");
    h.in.put(Json::parse(R"({"jsonrpc": "2.0", "id": 4})"));
    h.in.put(Json::parse(R"({"jsonrpc": "2.0", "id": 5, "method": "tools/list", "params": [1]})"));
    h.in.put(Json::parse(R"({"jsonrpc": "2.0", "id": 6, "result": {}})")); // a response: ignored
    h.in.put_line("\n");
    h.in.put(rpc(7, "ping"));
    std::vector<std::pair<Json, Json>> codes;
    for (const Json& message : h.out.messages(6)) {
        codes.emplace_back(message["id"],
                           message.contains("error") ? message["error"]["code"] : Json());
    }
    const std::vector<std::pair<Json, Json>> expected = {{nullptr, -32700}, {nullptr, -32600},
                                                         {nullptr, -32600}, {4, -32600},
                                                         {5, -32602},       {7, nullptr}};
    CHECK(codes == expected);
    CHECK(h.finish() == 0);
}

TEST_CASE("test_tools_list_offers_the_reply_tool", "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put(rpc(1, "tools/list"));
    const Json tools = h.out.messages(1)[0]["result"]["tools"];
    CHECK(tools == Json::array({reply_tool()}));
    const Json& schema = tools[0]["inputSchema"];
    CHECK(schema["required"] == Json::array({"text"}));
    CHECK(schema["properties"]["text"]["maxLength"] == 500);
}

TEST_CASE("test_reply_posts_through_the_link", "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put(
        rpc(1, "tools/call", Json::parse(R"({"name": "reply", "arguments": {"text": "  Done.  ",
                     "title": " Tests \n passed "}})")));
    const Json result = h.out.messages(1)[0]["result"];
    CHECK(result == Json::parse(R"({"content": [{"type": "text",
        "text": "Shown to the user near their pointer."}], "isError": false})"));
    CHECK(h.link.sent() ==
          std::vector<std::pair<std::string, std::string>>{{"Done.", "Tests passed"}});
}

TEST_CASE("test_reply_rejects_bad_arguments_as_tool_errors", "[test_automation_channel_protocol]") {
    const auto [arguments, expected] = GENERATE(table<std::optional<std::string>, std::string>({
        {std::nullopt, "text is required"},
        {"{}", "text is required"},
        {R"({"text": "   "})", "text is required"},
        {R"({"text": 5})", "text is required"},
        {R"({"text": ")" + std::string(501, 'x') + R"("})", "limited to 500 characters"},
        {R"({"text": "ok", "title": 5})", "title must be text"},
        {R"({"text": "ok", "title": ")" + std::string(81, 't') + R"("})",
         "limited to 80 characters"},
        {R"("text")", "Pass the reply as"},
    }));
    Harness h;
    Json params = Json::object({{"name", "reply"}});
    if (arguments) {
        params["arguments"] = Json::parse(*arguments);
    }
    h.in.put(rpc(1, "tools/call", params));
    const Json result = h.out.messages(1)[0]["result"];
    CHECK(result["isError"] == true);
    CHECK_THAT(result["content"][0]["text"].get<std::string>(), ContainsSubstring(expected));
    CHECK(h.link.sent().empty());
}

TEST_CASE("test_reply_failures_reach_claude_as_tool_errors", "[test_automation_channel_protocol]") {
    Harness h;
    h.link.fail = [](const std::string&) {
        throw LinkError("http_403", "allow running actions");
    };
    h.in.put(rpc(1, "tools/call", Json::parse(R"({"name": "reply", "arguments": {"text": "x"}})")));
    CHECK(h.out.messages(1)[0]["result"] ==
          Json::parse(R"({"content": [{"type": "text", "text": "allow running actions"}],
              "isError": true})"));
}

TEST_CASE("test_unknown_tools_are_invalid_params", "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put(rpc(1, "tools/call", Json::parse(R"({"name": "rm_rf", "arguments": {}})")));
    CHECK(h.out.messages(1)[0]["error"] ==
          Json::parse(R"({"code": -32602, "message": "Unknown tool: rm_rf"})"));
}

TEST_CASE("test_the_stream_starts_once_after_initialized_and_stops_at_eof",
          "[test_automation_channel_protocol]") {
    Harness h;
    h.in.put(rpc(1, "initialize", Json::object()));
    h.out.messages(1);
    CHECK(h.streams().empty());
    h.in.put(notice("notifications/initialized"));
    h.in.put(notice("notifications/initialized"));
    h.in.put(rpc(2, "ping"));
    h.out.messages(2);
    REQUIRE(h.streams().size() == 1);
    CHECK(h.streams()[0]->started == 1);
    CHECK(h.finish() == 0);
    CHECK(h.streams()[0]->stopped == 1);
}

TEST_CASE("test_channel_messages_are_notifications_on_their_own_line",
          "[test_automation_channel_protocol]") {
    Harness h;
    h.server->send_channel_message(Json::parse(R"({"content": "a\nb", "meta": {"job_id": "1"}})"));
    h.out.messages(1);
    const std::string raw = h.out.raw();
    CHECK(std::count(raw.begin(), raw.end(), '\n') == 1);
    CHECK(fv::py::loads(raw) == Json::parse(R"({"jsonrpc": "2.0",
        "method": "notifications/claude/channel",
        "params": {"content": "a\nb", "meta": {"job_id": "1"}}})"));
}

TEST_CASE("test_stepping_aside_for_an_update_closes_stdout_and_ends_the_process",
          "[test_automation_channel_protocol]") {
    // The default stream calls exit_for_update when it finds the app gone while an update runs.
    std::vector<int> exits;
    std::mutex mutex;
    Pipe in;
    Output out;
    AppLinkOptions options = link_options();
    options.url = "http://127.0.0.1:9";
    options.token = kToken;
    auto client = std::make_shared<fv::http::FakeHttpClient>();
    fv::http::Error refused{fv::http::ErrorKind::Connect, "refused"};
    client->set_handler([refused](const fv::http::Request&) {
        fv::http::ScriptedResponse response;
        response.error = refused;
        return response;
    });
    options.client = client;
    AppLink link(options);
    ChannelServerOptions server_options;
    server_options.exit_process = [&](int code) {
        std::lock_guard lock(mutex);
        exits.push_back(code);
    };
    server_options.update_check = [] {
        return true;
    };
    ChannelServer server(in, out, link, std::move(server_options));
    std::unique_ptr<EventStream> stream = server.default_stream();
    stream->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (stream->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stream->stop();
    std::lock_guard lock(mutex);
    CHECK(exits == std::vector<int>{0});
    CHECK(out.closed());
}

TEST_CASE("test_writer_sends_ascii_json_and_survives_a_lone_surrogate",
          "[test_automation_channel_protocol]") {
    Output out;
    MessageWriter writer(out);
    const Json message = fv::py::loads(R"({"text": "café ✓ \ud800"})");
    CHECK(writer.send(message));
    const std::string line = out.raw();
    CHECK(std::all_of(line.begin(), line.end(),
                      [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    CHECK(line.ends_with('\n'));
    CHECK(fv::py::loads(line)["text"] == message["text"]);
}

TEST_CASE("test_writer_reports_a_closed_pipe", "[test_automation_channel_protocol]") {
    Output broken;
    broken.broken = true;
    MessageWriter writer(broken);
    CHECK_FALSE(writer.send(Json::object({{"a", 1}})));
    CHECK(writer.closed());
}

// --- reaching the app ----------------------------------------------------------------------------

TEST_CASE("test_the_port_comes_from_settings_like_the_app_reads_it",
          "[test_automation_channel_protocol]") {
    const auto [saved, port] = GENERATE(table<std::string, int>({{R"({"mcp_port": 9123})", 9123},
                                                                 {"{}", 8767},
                                                                 {R"({"mcp_port": true})", 8767},
                                                                 {R"({"mcp_port": 70000})", 8767},
                                                                 {R"({"mcp_port": "9"})", 8767}}));
    AppLinkOptions options = link_options();
    options.read_settings = [saved] {
        return Json::parse(saved);
    };
    AppLink link(options);
    CHECK(link.target() == Target{"127.0.0.1", port, false, ""});
}

TEST_CASE("test_unreadable_settings_fall_back_to_the_default_port",
          "[test_automation_channel_protocol]") {
    AppLinkOptions options = link_options();
    options.read_settings = []() -> Json {
        throw std::runtime_error("locked");
    };
    CHECK(AppLink(options).target().port == 8767);
}

TEST_CASE("test_the_url_override_wins", "[test_automation_channel_protocol]") {
    const auto [url, target] = GENERATE(table<std::string, Target>({
        {"http://127.0.0.1:9000", Target{"127.0.0.1", 9000, false, ""}},
        {"http://localhost:9000/", Target{"localhost", 9000, false, ""}},
        {"https://host.example/base/", Target{"host.example", 443, true, "/base"}},
        {"http://[::1]:8767", Target{"::1", 8767, false, ""}},
    }));
    // Another computer needs its own token (the saved one stays on this one).
    AppLinkOptions options = link_options();
    options.url = url;
    options.token = kToken;
    bool settings_read = false;
    options.read_settings = [&settings_read] {
        settings_read = true;
        return Json::object();
    };
    CHECK(AppLink(options).target() == target);
    CHECK_FALSE(settings_read);
}

TEST_CASE("test_a_bad_url_override_is_reported", "[test_automation_channel_protocol]") {
    const std::string url =
        GENERATE(as<std::string>{}, "127.0.0.1:8767", "ftp://host", "http://", "http://host:99999");
    AppLinkOptions options = link_options();
    options.url = url;
    try {
        (void)AppLink(options).target();
        FAIL("no LinkError for " << url);
    } catch (const LinkError& error) {
        CHECK(error.state() == "bad_url");
    }
}

TEST_CASE("test_the_token_is_read_once_and_again_after_a_refusal",
          "[test_automation_channel_protocol]") {
    std::vector<std::string> reads{"first-token", "second-token"};
    std::size_t next = 0;
    AppLinkOptions options = link_options();
    options.read_token = [&] {
        return std::optional<std::string>(reads.at(next++));
    };
    AppLink link(options);
    CHECK(link.token() == "first-token");
    CHECK(link.token() == "first-token");
    link.forget_token();
    CHECK(link.token() == "second-token");
}

TEST_CASE("test_a_missing_token_is_read_again_on_the_next_try",
          "[test_automation_channel_protocol]") {
    std::vector<std::optional<std::string>> reads{std::nullopt, "late-token"};
    std::size_t next = 0;
    AppLinkOptions options = link_options();
    options.read_token = [&] {
        return reads.at(next++);
    };
    AppLink link(options);
    try {
        (void)link.token();
        FAIL("no LinkError");
    } catch (const LinkError& error) {
        CHECK(error.state() == "no_token");
    }
    CHECK(link.token() == "late-token");
}

TEST_CASE("test_the_token_override_is_never_reread", "[test_automation_channel_protocol]") {
    AppLinkOptions options = link_options();
    options.token = " fixed ";
    bool read = false;
    options.read_token = [&read] {
        read = true;
        return std::optional<std::string>("stored");
    };
    AppLink link(options);
    link.forget_token();
    CHECK(link.token() == "fixed");
    CHECK_FALSE(read);
}

TEST_CASE("test_unreachable_copy_names_mcp_when_it_is_off", "[test_automation_channel_protocol]") {
    const auto message = [](std::string url, std::string saved) {
        AppLinkOptions options = link_options();
        options.url = std::move(url);
        options.read_settings = [saved] {
            return Json::parse(saved);
        };
        return AppLink(options).unreachable_message();
    };
    CHECK_THAT(message("", R"({"mcp_enabled": false})"), ContainsSubstring("API & MCP is off"));
    CHECK_THAT(message("", R"({"mcp_enabled": true})"), ContainsSubstring("isn't running"));
    CHECK_THAT(message("http://127.0.0.1:1", R"({"mcp_enabled": false})"),
               ContainsSubstring("isn't running"));
}

TEST_CASE("test_headers_identify_the_channel_client", "[test_automation_channel_protocol]") {
    AppLinkOptions options = link_options();
    options.version = "2.7.0";
    const fv::http::Headers headers =
        AppLink(options).headers(kToken, {{"Accept", "text/event-stream"}});
    const std::string product(fv::identity().product);
    const fv::http::Headers expected = {{"Authorization", "Bearer " + kToken},
                                        {"X-" + product + "-Client", "claude-channel"},
                                        {"User-Agent", product + "-channel/2.7.0"},
                                        {"Accept", "text/event-stream"}};
    CHECK(headers == expected);
}

// --- tests/test_automation_fix_channel_gap.py
// -----------------------------------------------------

namespace {

SseEvent gap(std::string last_id, int missed_from_seq,
             std::string_view reason = catalog::GAP_EVICTED) {
    Json payload = Json::parse(R"({"id": "", "seq": 0, "type": "events.gap",
        "time": "2026-10-10T12:00:00.000Z", "v": 1})");
    payload["data"] =
        Json::object({{"missed_from_seq", missed_from_seq}, {"reason", std::string(reason)}});
    return SseEvent{"events.gap", fv::py::dumps(payload), std::move(last_id)};
}

SseEvent message(int seq, std::string text) {
    Json payload = Json::object();
    payload["id"] = "evt_" + std::to_string(seq);
    payload["seq"] = seq;
    payload["type"] = "channel.message";
    payload["v"] = 1;
    payload["data"] = Json::object({{"job_id", "j"},
                                    {"source", "hotkey"},
                                    {"origin", "dictated"},
                                    {"action_id", "c"},
                                    {"text", std::move(text)},
                                    {"app", nullptr}});
    return SseEvent{"channel.message", fv::py::dumps(payload), std::to_string(seq)};
}

struct GapStream {
    GapStream()
        : link([] {
              AppLinkOptions options = link_options();
              options.token = std::string(40, 't');
              return options;
          }()),
          stream(link, [this](const Json& params) { delivered.push_back(params); }) {}
    AppLink link;
    std::vector<Json> delivered;
    EventStream stream;
};

} // namespace

TEST_CASE("test_a_gap_reaches_claude_as_a_notice_and_the_log_as_one_warning",
          "[test_automation_fix_channel_gap]") {
    LogCapture logs;
    GapStream s;
    s.stream.handle(message(4, "before"));
    s.stream.handle(gap("4", 5));
    s.stream.handle(message(9, "after"));
    REQUIRE(s.delivered.size() == 3);
    CHECK(s.delivered[0]["content"] == "before");
    CHECK(s.delivered[2]["content"] == "after");
    const Json& notice_params = s.delivered[1];
    CHECK(notice_params["meta"] == Json::object({{"origin", channel_notice_origin()}}));
    CHECK_THAT(notice_params["content"].get<std::string>(), ContainsSubstring("missed"));
    const std::vector<std::string> warnings = logs.warnings();
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings[0], ContainsSubstring("missed"));
}

TEST_CASE("test_the_same_gap_on_a_reconnect_is_announced_once",
          "[test_automation_fix_channel_gap]") {
    LogCapture logs;
    GapStream s;
    // After an app restart no channel message has moved the resume point, so every reconnect
    // brings the same gap.
    for (int i = 0; i < 3; ++i) {
        s.stream.handle(gap("4", 5));
    }
    CHECK(s.delivered.size() == 1);
    CHECK(logs.warnings().size() == 1);
    // A later gap from a newer position is news again.
    s.stream.handle(message(12, "hello"));
    s.stream.handle(gap("12", 13));
    std::vector<Json> origins;
    for (const Json& item : s.delivered) {
        origins.push_back(item["meta"].value("origin", Json()));
    }
    CHECK(origins ==
          std::vector<Json>{channel_notice_origin(), "dictated", channel_notice_origin()});
}

TEST_CASE("test_a_gap_the_replay_fills_is_not_announced", "[test_automation_fix_channel_gap]") {
    const std::string_view reason = GENERATE(catalog::GAP_RESTARTED, catalog::GAP_SLOW);
    LogCapture logs;
    GapStream s;
    s.stream.handle(message(4, "before"));
    // The app restarted (it replays all it sent since), or the stream fell behind (the
    // reconnect replays the rest): no message is lost.
    s.stream.handle(gap("4", 5, reason));
    s.stream.handle(message(9, "after"));
    REQUIRE(s.delivered.size() == 2);
    CHECK(s.delivered[0]["content"] == "before");
    CHECK(s.delivered[1]["content"] == "after");
    CHECK(logs.warnings().empty());
}

// --- tests/test_automation_fix_channel_link.py
// ----------------------------------------------------

namespace {

const std::string kStored = "stored-token-from-the-credential-store-0123456789";

// A link that must neither connect nor read the saved token.
struct NoNetworkLink {
    explicit NoNetworkLink(std::string url, std::string token = {}) {
        AppLinkOptions options;
        options.url = std::move(url);
        options.token = std::move(token);
        options.read_settings = [] {
            return Json::object();
        };
        options.read_token = [this] {
            store_read = true;
            return std::optional<std::string>(kStored);
        };
        options.client = client;
        link = std::make_unique<AppLink>(options);
    }
    std::shared_ptr<fv::http::FakeHttpClient> client = std::make_shared<fv::http::FakeHttpClient>();
    std::atomic<bool> store_read{false};
    std::unique_ptr<AppLink> link;
};

} // namespace

TEST_CASE("test_another_computer_needs_its_own_token", "[test_automation_fix_channel_link]") {
    const std::string url =
        GENERATE(as<std::string>{}, "https://host.example/", "https://100.82.22.3:8767");
    NoNetworkLink n(url);
    try {
        n.link->notify("Done.");
        FAIL("no LinkError");
    } catch (const LinkError& error) {
        CHECK(error.state() == "no_token");
        CHECK_THAT(error.message(), ContainsSubstring(token_env()));
    }
    CHECK_THROWS_AS(n.link->target(), LinkError);
    CHECK(n.client->requests().empty());
    CHECK_FALSE(n.store_read.load());
}

TEST_CASE("test_another_computer_needs_https_even_with_a_token",
          "[test_automation_fix_channel_link]") {
    const std::string url =
        GENERATE(as<std::string>{}, "http://203.0.113.5:8767", "http://host.example/");
    NoNetworkLink n(url, "explicit-token-0123456789abcdef0123");
    try {
        (void)n.link->target();
        FAIL("no LinkError");
    } catch (const LinkError& error) {
        CHECK(error.state() == "bad_url");
        CHECK_THAT(error.message(), ContainsSubstring("https://"));
    }
    CHECK(n.client->requests().empty());
    CHECK_FALSE(n.store_read.load());
}

TEST_CASE("test_https_with_its_own_token_is_allowed", "[test_automation_fix_channel_link]") {
    NoNetworkLink n("https://host.example/base", "explicit-token-0123456789abcdef0123");
    CHECK(n.link->target() == Target{"host.example", 443, true, "/base"});
    CHECK(n.link->token() == "explicit-token-0123456789abcdef0123");
    CHECK_FALSE(n.store_read.load());
}

TEST_CASE("test_this_computer_still_uses_the_saved_token", "[test_automation_fix_channel_link]") {
    const std::string url =
        GENERATE(as<std::string>{}, "http://127.0.0.1:8767", "http://localhost:8767",
                 "http://[::1]:8767", "http://127.0.0.2:9");
    NoNetworkLink n(url);
    (void)n.link->target();
    CHECK(n.link->token() == kStored);
}

// --- tests/test_automation_fix_channel_origin.py
// --------------------------------------------------

namespace {

Json origin_message(std::optional<std::string> origin, std::string source = "hotkey") {
    Json data = Json::object({{"job_id", "0123456789abcdef"},
                              {"source", std::move(source)},
                              {"action_id", "claude"},
                              {"text", "push to main"},
                              {"app", nullptr}});
    if (origin) {
        data["origin"] = *origin;
    }
    return Json::object({{"type", "channel.message"}, {"data", data}});
}

} // namespace

TEST_CASE("test_words_the_app_calls_dictated_are_marked_dictated",
          "[test_automation_fix_channel_origin]") {
    const std::optional<Json> params = channel_params(origin_message("dictated"));
    REQUIRE(params);
    CHECK((*params)["meta"] == Json::parse(R"({"origin": "dictated", "job_id": "0123456789abcdef",
              "action": "claude"})"));
}

TEST_CASE("test_anything_else_is_marked_as_a_program", "[test_automation_fix_channel_origin]") {
    const std::optional<std::string> origin =
        GENERATE(std::optional<std::string>("program"), std::optional<std::string>(""),
                 std::optional<std::string>(), std::optional<std::string>("something-new"));
    const std::optional<Json> params = channel_params(origin_message(origin));
    REQUIRE(params);
    CHECK((*params)["meta"]["origin"] == "program");
    CHECK_FALSE((*params)["meta"].contains("source"));
}

TEST_CASE("test_a_user_started_job_with_a_programs_words_is_a_program",
          "[test_automation_fix_channel_origin]") {
    // A program's text filter changed the words of the user's own dictation.
    const std::string source = GENERATE(as<std::string>{}, "hotkey", "ui", "dbus");
    const std::optional<Json> params = channel_params(origin_message("program", source));
    REQUIRE(params);
    CHECK((*params)["meta"]["origin"] == "program");
}

TEST_CASE("test_the_instructions_say_how_to_treat_each_origin",
          "[test_automation_fix_channel_origin]") {
    const std::string instructions = channel_instructions();
    CHECK_THAT(instructions,
               ContainsSubstring("<channel source=\"" + lower_product() + "\" origin=\""));
    CHECK_THAT(instructions, ContainsSubstring("origin=\"dictated\""));
    CHECK_THAT(instructions, ContainsSubstring("user's own request"));
    CHECK_THAT(instructions, ContainsSubstring("origin=\"program\""));
    CHECK_THAT(instructions, ContainsSubstring("sent or changed"));
    CHECK_THAT(instructions, ContainsSubstring("not the user's"));
    CHECK_THAT(fv::py::lower(instructions), ContainsSubstring("ask the user"));
    CHECK_THAT(instructions, ContainsSubstring("origin=\"" + lower_product() + "\""));
    CHECK_THAT(instructions, ContainsSubstring("reply tool"));
    CHECK(fv::py::len(instructions) < 2048);
}
