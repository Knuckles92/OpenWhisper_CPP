// llm/streaming.h: how a failed transfer and a failure reported inside the stream reach the
// caller (the classes the agent loop's retry rule reads), against an in-memory provider.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "generation_test_support.h"
#include "http/testing/fake_http_client.h"
#include "llm/generation.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "pyport/json.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::SseReply;

namespace {

constexpr const char* kBase = "http://provider.test/v1";

http::ScriptedResponse sse(const std::vector<Json>& frames, bool named = false) {
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/event-stream"}};
    for (const auto& frame : frames) {
        reply.body += frame.is_string() ? frame.get<std::string>() : SseReply::frame(frame, named);
    }
    return reply;
}

struct Run {
    std::optional<TextResult> result;
    std::optional<StreamError> stream_error;
    std::optional<std::string> other_error;
    std::vector<StreamEvent> events;
    std::vector<http::Request> requests;
};

Run run(const std::string& protocol, http::ScriptedResponse reply, CancelToken cancel = {},
        StreamSink sink = {}) {
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(std::move(reply));
    auto client = quick_client(fake, profile, 2);
    Run out;
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    request.cancel = std::move(cancel);
    try {
        out.result = stream_generate(client, profile, request, [&](const StreamEvent& event) {
            out.events.push_back(event);
            if (sink) {
                sink(event);
            }
        });
    } catch (const StreamError& error) {
        out.stream_error = error;
    } catch (const std::exception& error) {
        out.other_error = error.what();
    }
    out.requests = fake->requests();
    return out;
}

Json chunk(const Json& delta, const Json& reason = nullptr) {
    return Json{{"choices",
                 Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}};
}

} // namespace

TEST_CASE("stream delta names are the SDK's", "[streaming]") {
    CHECK(stream_delta_name(StreamDelta::ThinkingStart) == "thinking_start");
    CHECK(stream_delta_name(StreamDelta::ThinkingDelta) == "thinking_delta");
    CHECK(stream_delta_name(StreamDelta::ThinkingEnd) == "thinking_end");
    CHECK(stream_delta_name(StreamDelta::TextDelta) == "text_delta");
    CHECK(stream_delta_name(StreamDelta::ToolCallStart) == "toolcall_start");
    CHECK(stream_delta_name(StreamDelta::ToolCallDelta) == "toolcall_delta");
    CHECK(stream_delta_name(StreamDelta::ToolCallEnd) == "toolcall_end");
}

TEST_CASE("a cancelled token throws before anything is sent", "[streaming]") {
    CancelSource cancel;
    cancel.cancel();
    const Run out = run("chat", sse({chunk(Json{{"content", "x"}}, "stop")}), cancel.token());
    CHECK(out.other_error == "canceled");
    CHECK_FALSE(out.stream_error);
    CHECK(out.requests.empty());
}

TEST_CASE("a cancel between pieces ends the stream as canceled", "[streaming]") {
    CancelSource cancel;
    http::ScriptedResponse reply =
        sse({chunk(Json{{"content", "a"}}), chunk(Json{{"content", "b"}}, "stop")});
    reply.chunks = {10};
    const Run out =
        run("chat", reply, cancel.token(), [&](const StreamEvent&) { cancel.cancel(); });
    CHECK(out.other_error == "canceled");
    CHECK_FALSE(out.result);
}

TEST_CASE("transport failures are transient", "[streaming]") {
    http::ScriptedResponse refused;
    refused.error = http::Error{http::ErrorKind::Connect, "[WinError 10061] refused"};
    Run out = run("chat", refused);
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamConnectionError);
    CHECK(out.stream_error->transient);
    CHECK_FALSE(out.stream_error->status);
    CHECK_FALSE(out.stream_error->output_seen);
    CHECK(out.requests.size() == 1);

    http::ScriptedResponse silent;
    silent.error = http::Error{http::ErrorKind::Timeout, "timed out"};
    out = run("chat", silent);
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamTimeoutError);
    CHECK(out.stream_error->transient);

    http::ScriptedResponse cut = sse({chunk(Json{{"content", "Part"}})});
    cut.cut = http::Error{http::ErrorKind::Protocol, "transfer closed"};
    out = run("chat", cut);
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamDroppedError);
    CHECK(out.stream_error->transient);
    CHECK(out.stream_error->output_seen);
}

TEST_CASE("a stream that ends before its terminator was dropped", "[streaming]") {
    // Chat: no finish_reason and no [DONE].
    Run out = run("chat", sse({chunk(Json{{"content", "Part"}})}));
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamDroppedError);
    CHECK(out.stream_error->transient);
    CHECK(out.stream_error->output_seen);
    // Responses: no terminal event.
    out = run("responses", sse({Json{{"type", "response.created"},
                                     {"response", Json{{"status", "in_progress"}}}}}));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->transient);
    CHECK_FALSE(out.stream_error->output_seen);
    // Anthropic: no message_stop.
    out =
        run("anthropic",
            sse({Json{{"type", "message_start"}, {"message", Json{{"content", Json::array()}}}}}));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->transient);
    // Google: no candidate carried a finishReason (pi-ai: "Google stream ended without a
    // finish reason").
    const Json hi{
        {"candidates",
         Json::array({Json{{"content", Json{{"parts", Json::array({Json{{"text", "Hi"}}})}}}}})}};
    out = run("google", sse({hi}));
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamDroppedError);
    CHECK(out.stream_error->transient);
    CHECK(out.stream_error->output_seen);
    // An empty body is no reply either.
    out = run("google", sse({}));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->transient);
    CHECK_FALSE(out.stream_error->output_seen);
    // The finishReason on the last chunk ends it, and the stream has no other terminator.
    Json done = hi;
    done["candidates"][0]["finishReason"] = "STOP";
    out = run("google", sse({hi, done}));
    REQUIRE(out.result);
    CHECK(out.result->text == "HiHi");
}

TEST_CASE("errors reported inside the stream", "[streaming]") {
    // OpenRouter's mid-stream provider failure.
    Run out = run("chat",
                  sse({chunk(Json{{"content", "Pa"}}),
                       Json{{"error", Json{{"code", 502}, {"message", "Provider returned error"}}},
                            {"choices", Json::array({Json{{"index", 0},
                                                          {"delta", Json{{"content", ""}}},
                                                          {"finish_reason", "error"}}})}}}));
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamFailedError);
    CHECK(out.stream_error->status == 502);
    CHECK(out.stream_error->transient);
    CHECK(out.stream_error->output_seen);
    CHECK(std::string(out.stream_error->what()).find("Provider returned") == std::string::npos);

    // Anthropic's error event: overloaded is transient, an invalid request is not.
    out = run("anthropic",
              sse({Json{{"type", "error"},
                        {"error", Json{{"type", "overloaded_error"}, {"message", "Overloaded"}}}}},
                  true));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->status == 529);
    CHECK(out.stream_error->transient);
    out = run("anthropic",
              sse({Json{{"type", "error"},
                        {"error", Json{{"type", "invalid_request_error"}, {"message", "bad"}}}}},
                  true));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->status == 400);
    CHECK_FALSE(out.stream_error->transient);

    // The Responses error event.
    out = run("responses", sse({Json{{"type", "error"},
                                     {"code", "rate_limit_exceeded"},
                                     {"message", "slow down"},
                                     {"param", nullptr}}}));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->status == 429);
    CHECK(out.stream_error->transient);

    // Gemini's error object.
    out = run("google",
              sse({Json{{"error",
                         Json{{"code", 503}, {"message", "busy"}, {"status", "UNAVAILABLE"}}}}}));
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->status == 503);
    CHECK(out.stream_error->transient);
}

TEST_CASE("data that is not JSON is a malformed stream", "[streaming]") {
    Run out = run("chat", sse({Json("data: {not json\n\n")}));
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamMalformedError);
    CHECK_FALSE(out.stream_error->transient);
    // A tool_use input whose joined fragments are not JSON.
    out = run("anthropic",
              sse({Json{{"type", "message_start"}, {"message", Json{{"content", Json::array()}}}},
                   Json{{"type", "content_block_start"},
                        {"index", 0},
                        {"content_block", Json{{"type", "tool_use"},
                                               {"id", "c"},
                                               {"name", "n"},
                                               {"input", Json::object()}}}},
                   Json{{"type", "content_block_delta"},
                        {"index", 0},
                        {"delta", Json{{"type", "input_json_delta"}, {"partial_json", "{\"a\":"}}}},
                   Json{{"type", "content_block_stop"}, {"index", 0}}}));
    REQUIRE(out.stream_error);
    CHECK(std::string(out.stream_error->what()) == kStreamMalformedError);
    CHECK(out.stream_error->output_seen);
}

TEST_CASE("an error status is reported from the head without its body", "[streaming]") {
    http::ScriptedResponse reply = json_reply(500, Json{{"error", "secret diagnostic"}});
    reply.headers.emplace_back("retry-after", "Wed, 21 Oct 2015 07:28:00 GMT");
    const Run out = run("chat", reply);
    REQUIRE(out.stream_error);
    CHECK(out.stream_error->status == 500);
    CHECK(out.stream_error->transient);
    // A date in the past asks for no wait.
    CHECK_FALSE(out.stream_error->retry_after_s);
    CHECK(std::string(out.stream_error->what()) == "Error code: 500");
}

TEST_CASE("an exception from the sink ends the stream", "[streaming]") {
    const Run out = run("chat",
                        sse({chunk(Json{{"content", "a"}}), chunk(Json{{"content", "b"}}, "stop"),
                             Json("data: [DONE]\n\n")}),
                        {}, [](const StreamEvent&) { throw std::runtime_error("sink refused"); });
    CHECK(out.other_error == "sink refused");
    CHECK(out.events.size() == 1);
}

TEST_CASE("what generate() refuses before sending, stream_generate refuses too", "[streaming]") {
    TextLLMProfile profile = synthetic_profile("chat", kBase, Json{{"tools", false}});
    auto fake = std::make_shared<http::FakeHttpClient>();
    auto client = quick_client(fake, profile);
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    request.tools = Json::array({tool_schema()});
    try {
        (void)stream_generate(client, profile, request, {});
        FAIL("no error");
    } catch (const StreamError&) {
        FAIL("a StreamError");
    } catch (const TextGenerationError& error) {
        CHECK(std::string(error.what()) == "This model does not support tool calls.");
    }
    request.tools.reset();
    request.model = "";
    CHECK_THROWS_AS(stream_generate(client, profile, request, {}), std::invalid_argument);
    CHECK(fake->requests().empty());
}

TEST_CASE("the key moves to the native protocols' own headers", "[streaming]") {
    for (const std::string protocol : {"chat", "responses", "anthropic", "google"}) {
        INFO(protocol);
        const Json done =
            protocol == "chat" ? chunk(Json{{"content", "x"}}, "stop")
            : protocol == "responses"
                ? Json{{"type", "response.completed"},
                       {"response", Json{{"status", "completed"}, {"output", Json::array()}}}}
            : protocol == "anthropic"
                ? Json{{"type", "message_stop"}}
                : Json{{"candidates", Json::array({Json{{"finishReason", "STOP"}}})}};
        const Run out = run(protocol, sse({done}));
        CHECK_FALSE(out.stream_error);
        REQUIRE(out.requests.size() == 1);
        const auto& headers = out.requests[0].headers;
        const auto bearer = http::find_header(headers, "authorization");
        if (protocol == "anthropic" || protocol == "google") {
            CHECK_FALSE(bearer);
            CHECK(http::find_header(headers,
                                    protocol == "anthropic" ? "x-api-key" : "x-goog-api-key") ==
                  std::string(kSecret));
            if (protocol == "anthropic") {
                CHECK(http::find_header(headers, "anthropic-version") == "2023-06-01");
            }
        } else {
            CHECK(bearer == "Bearer " + std::string(kSecret));
        }
    }
}

TEST_CASE("the stream request keeps the caller's options in the SDK's order", "[streaming]") {
    const TextLLMProfile profile = synthetic_profile("chat", kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(sse({chunk(Json{{"content", "x"}}, "stop")}));
    auto client = quick_client(fake, profile);
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    request.options = Json{{"temperature", 0.2}, {"max_tokens", 50}};
    (void)stream_generate(client, profile, request, {});
    const auto body = std::get<std::string>(fake->requests().at(0).body);
    CHECK(body.find("\"max_tokens\":50,\"stream\":true,\"stream_options\":{\"include_usage\":true},"
                    "\"temperature\":0.2") != std::string::npos);
}
