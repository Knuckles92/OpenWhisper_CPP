// llm/streaming.h against the hand-written streams of tests/fixtures/llm/streams/: the deltas
// arrive in stream order, the TextResult is the fixture's, it equals what generate() returns for
// the equivalent non-streamed reply of tests/fixtures/llm/generation/, and both results build
// the same next-turn request. The provider is an in-memory http::FakeHttpClient that hands the
// stream over in small pieces.

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "generation_test_support.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/generation.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "pyport/json.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

namespace {

constexpr const char* kBase = "http://provider.test/v1";

http::ScriptedResponse stream_reply(const Json& fixture, std::size_t piece) {
    http::ScriptedResponse reply;
    reply.headers = {{"Content-Type", "text/event-stream; charset=utf-8"}};
    reply.body = fixture_body(fixture);
    for (std::size_t at = 0; at < reply.body.size(); at += piece) {
        reply.chunks.push_back(piece);
    }
    return reply;
}

struct Outcome {
    std::optional<TextResult> result;
    Json failure;
    std::vector<StreamEvent> events;
    std::vector<http::Request> requests;
};

Outcome run_stream(const Json& fixture, std::size_t piece) {
    const auto protocol = fixture.at("protocol").get<std::string>();
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(stream_reply(fixture, piece));
    auto client = quick_client(fake, profile);
    Outcome out;
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    try {
        out.result = stream_generate(client, profile, request, [&](const StreamEvent& event) {
            out.events.push_back(event);
        });
    } catch (...) {
        out.failure = failure_json(std::current_exception());
    }
    out.requests = fake->requests();
    return out;
}

Outcome run_generate(const Json& fixture) {
    const auto protocol = fixture.at("protocol").get<std::string>();
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(json_reply(200, fixture_equivalent(fixture)));
    auto client = quick_client(fake, profile);
    Outcome out;
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = default_messages();
    try {
        out.result = generate(client, profile, request);
    } catch (...) {
        out.failure = failure_json(std::current_exception());
    }
    out.requests = fake->requests();
    return out;
}

// The request generate() sends for the turn after `result`: the assistant message replayed and a
// tool result for each call.
Json next_turn_body(const std::string& protocol, const TextResult& result) {
    const TextLLMProfile profile = synthetic_profile(protocol, kBase);
    Json messages = default_messages();
    messages.push_back(result.assistant_message);
    for (const auto& call : result.tool_calls) {
        messages.push_back(
            Json{{"role", "tool"}, {"tool_call_id", call.id}, {"content", "{\"hits\":[]}"}});
    }
    auto fake = std::make_shared<http::FakeHttpClient>();
    fake->push(json_reply(200, reply_body(protocol)));
    auto client = quick_client(fake, profile);
    GenerateRequest request;
    request.model = "synthetic";
    request.messages = std::move(messages);
    request.tools = Json::array({tool_schema()});
    (void)generate(client, profile, request);
    return request_body(fake->requests().at(0));
}

} // namespace

TEST_CASE("every fixture stream gives its events and result", "[streaming]") {
    REQUIRE(stream_fixtures().size() >= 20);
    for (const auto& [name, fixture] : stream_fixtures()) {
        for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
            INFO(name << " in pieces of " << piece);
            const Outcome streamed = run_stream(fixture, piece);
            CHECK(golden::equal(fixture.at("events"), events_json(streamed.events)));
            if (fixture.contains("raises")) {
                CHECK_FALSE(streamed.result);
                CHECK(golden::equal(fixture.at("raises"), streamed.failure));
            } else {
                REQUIRE(streamed.result);
                INFO(py::dumps(result_json(*streamed.result)));
                CHECK(golden::equal(fixture.at("result"), result_json(*streamed.result)));
            }
            REQUIRE(streamed.requests.size() == 1);
        }
    }
}

TEST_CASE("streaming and non-streaming agree", "[streaming]") {
    for (const auto& [name, fixture] : stream_fixtures()) {
        INFO(name);
        const auto protocol = fixture.at("protocol").get<std::string>();
        const Outcome streamed = run_stream(fixture, 5);
        const Outcome whole = run_generate(fixture);
        if (whole.result) {
            REQUIRE(streamed.result);
            CHECK(golden::equal(result_json(*whole.result), result_json(*streamed.result)));
            CHECK(result_json(*whole.result) == result_json(*streamed.result));
            // The next turn replays either result into the same request.
            CHECK(next_turn_body(protocol, *whole.result) ==
                  next_turn_body(protocol, *streamed.result));
        } else {
            CHECK_FALSE(streamed.result);
            CHECK(golden::equal(whole.failure, streamed.failure));
        }
        // The streamed request is generate()'s with the stream switched on.
        REQUIRE(streamed.requests.size() == 1);
        REQUIRE(whole.requests.size() == 1);
        Json sent = request_body(streamed.requests[0]);
        Json plain = request_body(whole.requests[0]);
        std::string path = url_path(whole.requests[0].url);
        if (protocol == "chat") {
            CHECK(sent.at("stream") == true);
            CHECK(sent.at("stream_options") == Json{{"include_usage", true}});
            sent.erase("stream");
            sent.erase("stream_options");
        } else if (protocol == "responses" || protocol == "anthropic") {
            CHECK(sent.at("stream") == true);
            sent.erase("stream");
        } else {
            CHECK_FALSE(sent.contains("stream"));
            path = "/v1/models/synthetic:streamGenerateContent";
            CHECK(streamed.requests[0].url.ends_with("?alt=sse"));
        }
        CHECK(sent == plain);
        CHECK(url_path(streamed.requests[0].url) == path);
    }
}
