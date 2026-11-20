// What the built-in meeting agent's loop asks of a streamed turn beyond generate()'s reply:
// a turn cut off at the output limit settles with what arrived (StreamOptions, Pi's "length"
// stop), and a failed request carries the provider's own message, cleaned (provider_message,
// M10.22's gain), never its body. The cleaning's vectors are OpenWhisper_CPP's own: OpenWhisper's
// Python never showed a provider message, and OpenCode never surfaced one.
#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "generation_test_support.h"
#include "http/testing/fake_http_client.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::SseReply;

namespace {

constexpr const char* kBase = "http://provider.test/v1";

const Json& fixture(std::string_view name) {
    for (const auto& entry : stream_fixtures()) {
        if (entry.name == name) {
            return entry.document;
        }
    }
    throw std::runtime_error("no stream fixture " + std::string(name));
}

struct Outcome {
    std::optional<TextResult> result;
    Json failure;
    std::optional<StreamError> stream_error;
    std::size_t requests = 0;
};

Outcome run(const std::string& protocol, http::ScriptedResponse reply, StreamOptions options) {
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(std::move(reply));
    auto client = quick_client(fake, profile);
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    Outcome out;
    try {
        out.result = stream_generate(client, profile, request, {}, options);
    } catch (const StreamError& error) {
        out.stream_error = error;
        out.failure = failure_json(std::current_exception());
    } catch (...) {
        out.failure = failure_json(std::current_exception());
    }
    out.requests = fake->requests().size();
    return out;
}

http::ScriptedResponse stream_of(const Json& document) {
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/event-stream"}};
    reply.body = fixture_body(document);
    return reply;
}

Json chunk(const Json& delta, const Json& reason = nullptr) {
    return Json{{"choices",
                 Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}};
}

} // namespace

TEST_CASE("a turn cut off at the output limit settles with what arrived", "[streaming]") {
    for (const auto* name :
         {"chat_length", "responses_incomplete", "anthropic_max_tokens", "google_max_tokens"}) {
        CAPTURE(name);
        const Json& document = fixture(name);
        const auto protocol = document.at("protocol").get<std::string>();

        // Without the option the stop still raises generate()'s error.
        const Outcome strict = run(protocol, stream_of(document), {});
        CHECK(strict.failure == document.at("raises"));

        const Outcome settled = run(protocol, stream_of(document), {.settle_length_stop = true});
        INFO(settled.failure.dump());
        REQUIRE(settled.result);
        CHECK(settled.result->text == document.at("events").at(0).at(1).get<std::string>());
        CHECK(settled.result->finish_reason == "length");
        CHECK(settled.result->tool_calls.empty());
        CHECK(settled.result->assistant_message.value("content", Json()) == settled.result->text);
        // The partial reply is read again, never fetched again.
        CHECK(settled.requests == 1);
    }
}

TEST_CASE("a length stop keeps the tool calls that arrived whole", "[streaming]") {
    const Json call = {{"index", 0},
                       {"id", "call_1"},
                       {"type", "function"},
                       {"function", {{"name", "patch_state"}, {"arguments", "{\"ops\":[]}"}}}};
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/event-stream"}};
    reply.body = SseReply::frame(chunk({{"tool_calls", Json::array({call})}})) +
                 SseReply::frame(chunk(Json::object(), "length")) + "data: [DONE]\n\n";
    const Outcome out = run("chat", reply, {.settle_length_stop = true});
    INFO(out.failure.dump());
    REQUIRE(out.result);
    CHECK(out.result->finish_reason == "length");
    REQUIRE(out.result->tool_calls.size() == 1);
    CHECK(out.result->tool_calls[0].id == "call_1");
    CHECK(out.result->tool_calls[0].function.name == "patch_state");
    CHECK(out.result->tool_calls[0].function.arguments == "{\"ops\":[]}");
}

TEST_CASE("a content filter or a failed reply still raises with the option", "[streaming]") {
    for (const auto* name : {"chat_content_filter", "chat_refusal", "responses_failed"}) {
        CAPTURE(name);
        const Json& document = fixture(name);
        const Outcome out = run(document.at("protocol").get<std::string>(), stream_of(document),
                                {.settle_length_stop = true});
        CHECK_FALSE(out.result);
        CHECK(out.failure == document.at("raises"));
    }
    // A Responses reply incomplete for its content filter is not a length stop.
    http::ScriptedResponse filtered =
        json_reply(200, {{"id", "resp_1"},
                         {"status", "incomplete"},
                         {"incomplete_details", {{"reason", "content_filter"}}},
                         {"output", Json::array()}});
    const Outcome out = run("responses", filtered, {.settle_length_stop = true});
    CHECK_FALSE(out.result);
    CHECK(out.failure == Json{{"raises", "TextGenerationError"},
                              {"message", "Model output was incomplete or failed."}});
}

TEST_CASE("a provider's message is cleaned before anyone sees it", "[streaming]") {
    // OpenRouter and OpenAI: {"error": {"message", "type", "code"}}.
    CHECK(provider_error_message(R"({"error":{"message":"Provider returned error","code":502}})") ==
          "Provider returned error");
    CHECK(
        provider_error_message(
            R"({"error":{"message":"Incorrect API key provided: sk-proj-abc123XYZ. You can find your API key at https://platform.openai.com/account/api-keys.","type":"invalid_request_error"}})") ==
        "Incorrect API key provided: You can find your API key at "
        "https://platform.openai.com/account/api-keys.");
    // Anthropic: {"type": "error", "error": {"type", "message"}}.
    CHECK(provider_error_message(
              R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})") ==
          "Overloaded");
    // Google: {"error": {"code", "message", "status"}}, or a list of one on the streamed path.
    CHECK(
        provider_error_message(
            R"([{"error":{"code":429,"message":"Resource has been exhausted (e.g. check quota).","status":"RESOURCE_EXHAUSTED"}}])") ==
        "Resource has been exhausted (e.g. check quota).");
    // A top-level message.
    CHECK(provider_error_message(R"({"message":"Upstream timed out"})") == "Upstream timed out");
    // First line only; bearer tokens, key-shaped runs and URL queries go.
    CHECK(
        provider_error_message(
            R"({"error":{"message":"Bad header Bearer abc.def at https://x.test/v1/chat?key=AIzaSyA1b2C3d4E5f6G7h8I9j0 now\nsecond line"}})") ==
        "Bad header at https://x.test/v1/chat now");
    CHECK(
        provider_error_message(
            R"({"error":{"message":"token ghp_A1b2C3d4E5f6G7h8I9j0K1l2 rejected; streamGenerateContent failed"}})") ==
        "token rejected; streamGenerateContent failed");
    // At most 200 code points, counted in UTF-8.
    std::string long_message;
    for (int i = 0; i < 250; ++i) {
        long_message += "\xC3\xA9"; // é
    }
    const std::string capped =
        provider_error_message(Json{{"error", {{"message", long_message}}}}.dump());
    CHECK(capped == long_message.substr(0, 400));
    // No message, no text: a string error, a body that is not JSON, an empty body.
    CHECK(provider_error_message(R"({"error":"private response body"})").empty());
    CHECK(provider_error_message("<html>Bad gateway</html>").empty());
    CHECK(provider_error_message("").empty());
}

TEST_CASE("a failed request carries the provider's cleaned message, never its body",
          "[streaming]") {
    const Outcome status =
        run("chat",
            json_reply(503, {{"error",
                              {{"message", "The model is overloaded. Retry shortly."},
                               {"metadata", {{"raw", "transcript excerpt"}}}}}}),
            {});
    REQUIRE(status.stream_error);
    CHECK(std::string(status.stream_error->what()) == "Error code: 503");
    CHECK(status.stream_error->status == 503);
    CHECK(status.stream_error->provider_message == "The model is overloaded. Retry shortly.");

    // An error event inside the stream.
    http::ScriptedResponse event;
    event.headers = {{"Content-Type", "text/event-stream"}};
    event.body = SseReply::frame(
        {{"type", "error"}, {"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}},
        true);
    const Outcome streamed = run("anthropic", event, {});
    REQUIRE(streamed.stream_error);
    CHECK(std::string(streamed.stream_error->what()) == kStreamFailedError);
    CHECK(streamed.stream_error->status == 529);
    CHECK(streamed.stream_error->provider_message == "Overloaded");

    // A rejected replayed signature names only the fixed sentence, whatever else the body said.
    const Outcome signature =
        run("chat",
            json_reply(
                400, {{"error", {{"message", "Invalid request.\nCorrupted thought signature."}}}}),
            {});
    REQUIRE(signature.stream_error);
    CHECK(signature.stream_error->provider_message == "Corrupted thought signature.");
}
