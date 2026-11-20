// llm/streaming.h over real HTTP: the provider scenarios of the agent SDKs' own tests
// (sidecar/src/pi-integration.test.ts, sidecar-opencode/src/adapter.test.ts) replayed by an
// SseProvider on 127.0.0.1 through the curl client, then the failure classes the agent loop's
// retry rule reads. Nothing leaves the loopback interface.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "generation_test_support.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "llm/generation.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "pyport/json.h"
#include "sse_provider.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::SseProvider;
using fv::testing::SseReply;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kKey = "synthetic-secret";

std::shared_ptr<http::Client> direct_client() {
    http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return http::make_curl_client(std::move(options));
}

OpenAiCompatClient client_for(const TextLLMProfile& profile, http::Headers headers = {}) {
    return OpenAiCompatClient(direct_client(), profile.base_url.value_or(""), std::string(kKey),
                              std::move(headers), 15.0, 0);
}

struct Collected {
    std::vector<StreamEvent> events;
    StreamSink sink() {
        return [this](const StreamEvent& event) {
            events.push_back(event);
        };
    }
    bool has(StreamDelta kind) const {
        return std::ranges::any_of(events, [&](const StreamEvent& e) { return e.kind == kind; });
    }
};

std::string path_of(const std::string& target) {
    return target.substr(0, target.find('?'));
}

// sidecar/src/pi-integration.test.ts:26-29: one chat.completion.chunk.
std::string pi_chunk(const Json& delta, const Json& reason = nullptr) {
    return SseReply::frame(
        Json{{"id", "chat-test"},
             {"object", "chat.completion.chunk"},
             {"created", 1},
             {"model", "synthetic"},
             {"choices",
              Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}});
}

const std::string& pi_arguments() {
    static const std::string text =
        py::dumps(Json{{"ops", Json::array({Json{{"op", "set_topic"},
                                                 {"text", "Synthetic topic"},
                                                 {"evidence", Json::array({"sg_first"})}}})}},
                  py::DumpOptions{std::nullopt, true, false, ",", ":"});
    return text;
}

Json patch_state_tool() {
    return Json{{"type", "function"},
                {"function",
                 Json{{"name", "patch_state"},
                      {"description", "Update meeting"},
                      {"parameters", Json{{"type", "object"},
                                          {"properties", Json{{"text", Json{{"type", "string"}}}}},
                                          {"required", Json::array({"text"})}}}}}};
}

// sidecar/src/pi-integration.test.ts:31-69: the tool turn, then the plain answer.
std::vector<SseReply> pi_replies(const std::string& protocol, bool reasoning) {
    std::vector<SseReply> out;
    if (protocol == "chat") {
        out.push_back(SseReply::stream(
            {pi_chunk(Json{{"role", "assistant"}, {"reasoning_content", "Synthetic reasoning"}}),
             pi_chunk(
                 Json{{"role", "assistant"},
                      {"tool_calls",
                       Json::array({Json{{"index", 0},
                                         {"id", "call_1"},
                                         {"type", "function"},
                                         {"function", Json{{"name", "patch_state"},
                                                           {"arguments", pi_arguments()}}}}})}}),
             pi_chunk(Json::object(), "tool_calls"), "data: [DONE]\n\n"}));
        out.push_back(SseReply::stream({pi_chunk(Json{{"role", "assistant"}, {"content", "Done"}}),
                                        pi_chunk(Json::object(), "stop"), "data: [DONE]\n\n"}));
        return out;
    }
    for (const bool tool_turn : {true, false}) {
        const Json item =
            tool_turn ? Json{{"type", "function_call"}, {"id", "fc_1"},
                             {"call_id", "call_1"},     {"name", "patch_state"},
                             {"status", "completed"},   {"arguments", pi_arguments()}}
                      : Json{{"type", "message"},
                             {"id", "msg_1"},
                             {"role", "assistant"},
                             {"status", "completed"},
                             {"content", Json::array({Json{{"type", "output_text"},
                                                           {"text", "Done"},
                                                           {"annotations", Json::array()}}})}};
        Json output = Json::array();
        const Json response{
            {"id", "resp_1"},
            {"object", "response"},
            {"created_at", 1},
            {"status", "completed"},
            {"model", "synthetic"},
            {"usage", Json{{"input_tokens", 5}, {"output_tokens", 5}, {"total_tokens", 10}}}};
        std::vector<Json> events;
        Json created = response;
        created["status"] = "in_progress";
        created["output"] = Json::array();
        events.push_back(Json{{"type", "response.created"}, {"response", created}});
        if (reasoning) {
            const Json thought{{"type", "reasoning"},
                               {"id", "rs_1"},
                               {"summary", Json::array()},
                               {"encrypted_content", "signed-reasoning"}};
            events.push_back(Json{
                {"type", "response.output_item.added"}, {"output_index", 0}, {"item", thought}});
            events.push_back(Json{
                {"type", "response.output_item.done"}, {"output_index", 0}, {"item", thought}});
            output.push_back(thought);
        }
        const int index = static_cast<int>(output.size());
        Json added = item;
        added["status"] = "in_progress";
        if (tool_turn) {
            added["arguments"] = "";
        } else {
            added["content"] = Json::array();
        }
        events.push_back(
            Json{{"type", "response.output_item.added"}, {"output_index", index}, {"item", added}});
        events.push_back(tool_turn ? Json{{"type", "response.function_call_arguments.delta"},
                                          {"output_index", index},
                                          {"item_id", "fc_1"},
                                          {"delta", pi_arguments()}}
                                   : Json{{"type", "response.output_text.delta"},
                                          {"output_index", index},
                                          {"item_id", "msg_1"},
                                          {"content_index", 0},
                                          {"delta", "Done"}});
        events.push_back(
            Json{{"type", "response.output_item.done"}, {"output_index", index}, {"item", item}});
        output.push_back(item);
        Json completed = response;
        completed["output"] = output;
        events.push_back(Json{{"type", "response.completed"}, {"response", completed}});
        out.push_back(SseReply::events(events));
    }
    return out;
}

// sidecar-opencode/src/adapter.test.ts:37-71 reply(protocol, done).
SseReply opencode_reply(const std::string& protocol, bool done) {
    if (protocol == "anthropic") {
        const Json content = done ? Json{{"type", "text"}, {"text", ""}}
                                  : Json{{"type", "tool_use"},
                                         {"id", "call_test"},
                                         {"name", "patch_state"},
                                         {"input", Json::object()}};
        return SseReply::events(
            {Json{{"type", "message_start"},
                  {"message", Json{{"id", "msg_test"},
                                   {"type", "message"},
                                   {"role", "assistant"},
                                   {"content", Json::array()},
                                   {"model", "synthetic"},
                                   {"stop_reason", nullptr},
                                   {"stop_sequence", nullptr},
                                   {"usage", Json{{"input_tokens", 5}, {"output_tokens", 0}}}}}},
             Json{{"type", "content_block_start"}, {"index", 0}, {"content_block", content}},
             Json{{"type", "content_block_delta"},
                  {"index", 0},
                  {"delta", done ? Json{{"type", "text_delta"}, {"text", "Done"}}
                                 : Json{{"type", "input_json_delta"},
                                        {"partial_json", "{\"text\":\"Synthetic\"}"}}}},
             Json{{"type", "content_block_stop"}, {"index", 0}},
             Json{{"type", "message_delta"},
                  {"delta", Json{{"stop_reason", done ? "end_turn" : "tool_use"},
                                 {"stop_sequence", nullptr}}},
                  {"usage", Json{{"output_tokens", 5}}}},
             Json{{"type", "message_stop"}}},
            true);
    }
    if (protocol == "google") {
        const Json parts =
            done ? Json::array({Json{{"text", "Done"}}})
                 : Json::array(
                       {Json{{"functionCall", Json{{"name", "patch_state"},
                                                   {"args", Json{{"text", "Synthetic"}}}}}}});
        return SseReply::events({Json{
            {"candidates", Json::array({Json{{"content", Json{{"role", "model"}, {"parts", parts}}},
                                             {"finishReason", "STOP"},
                                             {"index", 0}}})},
            {"usageMetadata",
             Json{{"promptTokenCount", 5}, {"candidatesTokenCount", 5}, {"totalTokenCount", 10}}},
            {"modelVersion", "synthetic"}}});
    }
    if (protocol == "responses") {
        const Json item =
            done ? Json{{"type", "message"},
                        {"id", "msg_test"},
                        {"role", "assistant"},
                        {"status", "completed"},
                        {"content", Json::array({Json{{"type", "output_text"},
                                                      {"text", "Done"},
                                                      {"annotations", Json::array()}}})}}
                 : Json{{"type", "function_call"},
                        {"id", "fc_test"},
                        {"call_id", "call_test"},
                        {"name", "patch_state"},
                        {"arguments", "{\"text\":\"Synthetic\"}"},
                        {"status", "completed"}};
        const Json response{
            {"id", "resp_test"},
            {"object", "response"},
            {"created_at", 1},
            {"status", "completed"},
            {"model", "synthetic"},
            {"output", Json::array({item})},
            {"usage", Json{{"input_tokens", 5}, {"output_tokens", 5}, {"total_tokens", 10}}}};
        Json created = response;
        created["status"] = "in_progress";
        created["output"] = Json::array();
        Json added = item;
        added["status"] = "in_progress";
        if (done) {
            added["content"] = Json::array();
        } else {
            added["arguments"] = "";
        }
        return SseReply::events(
            {Json{{"type", "response.created"}, {"response", created}},
             Json{{"type", "response.output_item.added"}, {"output_index", 0}, {"item", added}},
             done ? Json{{"type", "response.output_text.delta"},
                         {"item_id", "msg_test"},
                         {"output_index", 0},
                         {"content_index", 0},
                         {"delta", "Done"}}
                  : Json{{"type", "response.function_call_arguments.delta"},
                         {"item_id", "fc_test"},
                         {"output_index", 0},
                         {"delta", "{\"text\":\"Synthetic\"}"}},
             Json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", item}},
             Json{{"type", "response.completed"}, {"response", response}}});
    }
    const Json delta =
        done ? Json{{"content", "Done"}}
             : Json{{"tool_calls",
                     Json::array(
                         {Json{{"index", 0},
                               {"id", "call_test"},
                               {"type", "function"},
                               {"function", Json{{"name", "patch_state"},
                                                 {"arguments", "{\"text\":\"Synthetic\"}"}}}}})}};
    return SseReply::events(
        {Json{{"id", "chatcmpl-test"},
              {"object", "chat.completion.chunk"},
              {"choices",
               Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}})}},
         Json{{"id", "chatcmpl-test"},
              {"object", "chat.completion.chunk"},
              {"choices", Json::array({Json{{"index", 0},
                                            {"delta", Json::object()},
                                            {"finish_reason", done ? "stop" : "tool_calls"}}})},
              {"usage",
               Json{{"prompt_tokens", 5}, {"completion_tokens", 5}, {"total_tokens", 10}}}}});
}

// sidecar-opencode/src/adapter.test.ts:133-135: an error answer whose body must never surface.
SseReply provider_error(int status) {
    return SseReply::json(
        status, Json{{"error", Json{{"message", "private provider diagnostic synthetic-secret"},
                                    {"type", "api_error"}}}});
}

GenerateRequest request_for(std::string model, Json messages, CancelToken cancel = {}) {
    GenerateRequest request;
    request.model = std::move(model);
    request.messages = std::move(messages);
    request.tools = Json::array({patch_state_tool()});
    request.cancel = std::move(cancel);
    return request;
}

} // namespace

TEST_CASE("real Pi SDK and runner: tools, scoped requests and provider reasoning",
          "[pi-integration.test.ts]") {
    // from sidecar/src/pi-integration.test.ts: the provider variants' model requests and replies
    struct Variant {
        std::string protocol, model;
        bool reasoning;
    };
    for (const auto& [protocol, model, reasoning] :
         {Variant{"chat", "deepseek-v4-synthetic", true},
          Variant{"responses", "gpt-4.1-mini", false}, Variant{"responses", "o4-mini", true}}) {
        INFO(model);
        SseProvider provider(pi_replies(protocol, reasoning));
        TextLLMProfile profile =
            *get_profile(protocol == "responses" ? "openai" : "openrouter", Json::object());
        profile.base_url = provider.url("/v1");
        OpenAiCompatClient client = client_for(profile);
        Json messages =
            Json::array({Json{{"role", "system"}, {"content", "MEETING_HOST_PROMPT"}},
                         Json{{"role", "user"}, {"content", "HOST_PROMPT Transcript first"}}});
        GenerateRequest request = request_for(model, messages);
        request.reasoning_level = "medium";
        Collected first;
        const TextResult tool_turn = stream_generate(client, profile, request, first.sink());
        REQUIRE(tool_turn.tool_calls.size() == 1);
        CHECK(tool_turn.tool_calls[0].id == "call_1");
        CHECK(tool_turn.tool_calls[0].function.name == "patch_state");
        CHECK(tool_turn.tool_calls[0].function.arguments == pi_arguments());
        CHECK(first.has(StreamDelta::ToolCallStart));
        CHECK(first.has(StreamDelta::ToolCallEnd));
        CHECK(first.has(StreamDelta::ThinkingStart) == reasoning);

        messages.push_back(tool_turn.assistant_message);
        messages.push_back(Json{{"role", "tool"},
                                {"tool_call_id", "call_1"},
                                {"content", "{\"results\":[{\"ok\":true}]}"}});
        request.messages = messages;
        Collected second;
        const TextResult answer = stream_generate(client, profile, request, second.sink());
        CHECK(answer.text == "Done");
        CHECK(answer.tool_calls.empty());
        CHECK(second.has(StreamDelta::TextDelta));

        const auto requests = provider.requests();
        REQUIRE(requests.size() == 2);
        for (const auto& sent : requests) {
            CHECK(path_of(sent.path) ==
                  (protocol == "responses" ? "/v1/responses" : "/v1/chat/completions"));
            CHECK(sent.header("authorization") == SseProvider::mask("Bearer synthetic-secret"));
        }
        const Json body = requests[0].json();
        const Json names = body.at("tools");
        CHECK(names.size() == 1);
        CHECK(py::dumps(body).find("MEETING_HOST_PROMPT") != std::string::npos);
        CHECK(body.at("stream") == true);
        const Json replayed = requests[1].json();
        if (protocol == "responses") {
            CHECK(body.at("store") == false);
            CHECK(body.contains("reasoning") == reasoning);
            CHECK(body.contains("stream_options") == false);
            bool output = false;
            bool thought = false;
            for (const auto& item : replayed.at("input")) {
                output = output || (item.value("type", "") == "function_call_output" &&
                                    item.value("call_id", "") == "call_1");
                thought = thought || (item.value("type", "") == "reasoning" &&
                                      item.value("encrypted_content", "") == "signed-reasoning");
            }
            CHECK(output);
            CHECK(thought == reasoning);
        } else {
            CHECK(body.at("stream_options") == Json{{"include_usage", true}});
            bool tool = false;
            bool thought = false;
            for (const auto& message : replayed.at("messages")) {
                tool = tool || message.value("role", "") == "tool";
                thought =
                    thought || (message.value("role", "") == "assistant" &&
                                message.value("reasoning_content", "") == "Synthetic reasoning");
            }
            CHECK(tool);
            CHECK(thought);
        }
    }
}

TEST_CASE("tool round trip uses app endpoint, auth, headers, and charter", "[adapter.test.ts]") {
    // from sidecar-opencode/src/adapter.test.ts: the five kind and protocol round trips
    struct Variant {
        std::string kind, protocol;
    };
    for (const auto& [kind, protocol] :
         {Variant{"custom", "chat"}, Variant{"custom", "responses"}, Variant{"openai", "responses"},
          Variant{"custom", "anthropic"}, Variant{"custom", "google"}}) {
        INFO(kind << " " << protocol);
        SseProvider provider({opencode_reply(protocol, false), opencode_reply(protocol, true)});
        const std::string base = provider.url("/gateway/v1");
        TextLLMProfile profile;
        std::string model = "synthetic";
        if (kind == "openai") {
            // OpenAI's routes come from the model id; "synthetic" has none.
            profile = *get_profile("openai", Json::object());
            profile.base_url = base;
            model = "gpt-4.1-mini";
        } else {
            profile = synthetic_profile(protocol, base, Json{{"max_output_tokens", 3000}});
        }
        OpenAiCompatClient client = client_for(profile, {{"x-fiorivoce-test", "yes"}});
        Json messages =
            Json::array({Json{{"role", "system"}, {"content", "Only the meeting host charter."}},
                         Json{{"role", "user"}, {"content", "Update"}}});
        GenerateRequest request = request_for(model, messages);
        Collected progress;
        const TextResult first = stream_generate(client, profile, request, progress.sink());
        REQUIRE(first.tool_calls.size() == 1);
        CHECK(py::loads(first.tool_calls[0].function.arguments).at("text") == "Synthetic");
        messages.push_back(first.assistant_message);
        messages.push_back(Json{
            {"role", "tool"}, {"tool_call_id", first.tool_calls[0].id}, {"content", "Applied"}});
        request.messages = messages;
        const TextResult result = stream_generate(client, profile, request, progress.sink());
        CHECK(result.text == "Done");
        CHECK_FALSE(progress.events.empty());

        const auto seen = provider.requests();
        REQUIRE(seen.size() == 2);
        CHECK(seen[0].header("x-fiorivoce-test") == "yes");
        const std::string expected_path = protocol == "google"
                                              ? "/gateway/v1/models/synthetic:streamGenerateContent"
                                          : protocol == "chat"      ? "/gateway/v1/chat/completions"
                                          : protocol == "anthropic" ? "/gateway/v1/messages"
                                                                    : "/gateway/v1/responses";
        CHECK(path_of(seen[0].path) == expected_path);
        if (protocol == "google") {
            CHECK(seen[0].path.ends_with("?alt=sse"));
        }
        const std::string auth_header = protocol == "anthropic" ? "x-api-key"
                                        : protocol == "google"  ? "x-goog-api-key"
                                                                : "authorization";
        const std::string auth_value = protocol == "chat" || protocol == "responses"
                                           ? "Bearer synthetic-secret"
                                           : "synthetic-secret";
        CHECK(seen[0].header(auth_header) == SseProvider::mask(auth_value));
        if (protocol == "anthropic" || protocol == "google") {
            CHECK_FALSE(seen[0].header("authorization"));
        }
        if (protocol == "responses") {
            CHECK(seen[0].json().at("store") == false);
            CHECK_FALSE(seen[0].json().contains("reasoning"));
            bool output = false;
            const Json replay = seen[1].json();
            for (const auto& item : replay.at("input")) {
                output = output || item.value("type", "") == "function_call_output";
            }
            CHECK(output);
        }
        const std::string prompt = seen[0].body;
        CHECK(prompt.find("Only the meeting host charter.") != std::string::npos);
        CHECK(prompt.find("bash") == std::string::npos);
        REQUIRE(result.usage.is_object());
        CHECK(result.usage.at("prompt_tokens").get<int>() > 0);
    }
}

TEST_CASE("reasoning streams report activity and preserve provider reasoning through a tool call",
          "[adapter.test.ts]") {
    // from sidecar-opencode/src/adapter.test.ts: the reasoning stream arrives over time
    const auto frame = [](const Json& part) {
        Json chunk{{"id", "chatcmpl-test"}, {"object", "chat.completion.chunk"}};
        for (const auto& [key, value] : part.items()) {
            chunk[key] = value;
        }
        return SseReply::frame(chunk);
    };
    SseReply thinking = SseReply::stream(
        {frame(Json{{"choices", Json::array({Json{
                                    {"index", 0},
                                    {"delta", Json{{"reasoning_content", "Synthetic reasoning"}}},
                                    {"finish_reason", nullptr}}})}}),
         frame(Json{
             {"choices",
              Json::array({Json{
                  {"index", 0},
                  {"delta",
                   Json{{"tool_calls",
                         Json::array({Json{
                             {"index", 0},
                             {"id", "call_test"},
                             {"type", "function"},
                             {"function", Json{{"name", "patch_state"},
                                               {"arguments", "{\"text\":\"Synthetic\"}"}}}}})}}},
                  {"finish_reason", nullptr}}})}}),
         frame(Json{{"choices", Json::array({Json{{"index", 0},
                                                  {"delta", Json::object()},
                                                  {"finish_reason", "tool_calls"}}})}})});
    thinking.piece_delay = 150ms;
    SseProvider provider({thinking, opencode_reply("chat", true)});
    const TextLLMProfile profile =
        synthetic_profile("chat", provider.url("/v1"),
                          Json{{"reasoning", true},
                               {"max_output_tokens", 2048},
                               {"thinking_levels", Json{{"medium", nullptr}}}});
    OpenAiCompatClient client = client_for(profile);
    Json messages =
        Json::array({Json{{"role", "system"}, {"content", "Only the meeting host charter."}},
                     Json{{"role", "user"}, {"content", "Update"}}});
    GenerateRequest request = request_for("synthetic", messages);
    request.reasoning_level = "medium";
    Collected progress;
    const TextResult first = stream_generate(client, profile, request, progress.sink());
    CHECK(progress.has(StreamDelta::ThinkingDelta));
    messages.push_back(first.assistant_message);
    messages.push_back(
        Json{{"role", "tool"}, {"tool_call_id", "call_test"}, {"content", "Applied"}});
    request.messages = messages;
    (void)stream_generate(client, profile, request, progress.sink());
    const auto bodies = provider.requests();
    REQUIRE(bodies.size() == 2);
    // The output budget of a custom endpoint is the agent loop's to set, not the request's.
    CHECK_FALSE(bodies[0].json().contains("reasoning_effort"));
    bool replayed = false;
    const Json replay = bodies[1].json();
    for (const auto& message : replay.at("messages")) {
        replayed = replayed || (message.value("role", "") == "assistant" &&
                                message.value("reasoning_content", "") == "Synthetic reasoning");
    }
    CHECK(replayed);
}

TEST_CASE("HTTP 400 and 401 fail once without leaking provider body", "[adapter.test.ts]") {
    // from sidecar-opencode/src/adapter.test.ts: HTTP <status> fails once without leaking
    // provider body or retrying
    for (const int status : {400, 401}) {
        INFO(status);
        SseProvider provider({provider_error(status), provider_error(status)});
        const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
        OpenAiCompatClient client(direct_client(), profile.base_url.value_or(""), std::string(kKey),
                                  {}, 15.0, 2);
        Collected progress;
        try {
            (void)stream_generate(client, profile, request_for("synthetic", default_messages()),
                                  progress.sink());
            FAIL("no error");
        } catch (const StreamError& error) {
            CHECK(error.status == status);
            CHECK_FALSE(error.transient);
            CHECK_FALSE(error.output_seen);
            CHECK(std::string(error.what()) == "Error code: " + std::to_string(status));
            CHECK(std::string(error.what()).find("synthetic-secret") == std::string::npos);
        }
        CHECK(provider.requests().size() == 1);
        CHECK(progress.events.empty());
    }
}

TEST_CASE("rate limits and outages are transient with the server's wait", "[adapter.test.ts]") {
    // from sidecar-opencode/src/adapter.test.ts: a rate-limited request and a persistent
    // outage; the retries are the agent loop's, so one attempt is made here
    struct Case {
        int status;
        std::string header, value;
        double wait;
    };
    for (const auto& [status, header, value, wait] :
         {Case{429, "Retry-After", "2", 2.0}, Case{503, "retry-after-ms", "1500", 1.5}}) {
        INFO(status);
        SseReply reply = provider_error(status);
        reply.headers.emplace_back(header, value);
        SseProvider provider({reply, reply});
        const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
        OpenAiCompatClient client(direct_client(), profile.base_url.value_or(""), std::string(kKey),
                                  {}, 15.0, 3);
        try {
            (void)stream_generate(client, profile, request_for("synthetic", default_messages()),
                                  {});
            FAIL("no error");
        } catch (const StreamError& error) {
            CHECK(error.status == status);
            CHECK(error.transient);
            REQUIRE(error.retry_after_s);
            CHECK(std::abs(*error.retry_after_s - wait) < 1e-9);
            CHECK(std::string(error.what()).find("diagnostic") == std::string::npos);
        }
        CHECK(provider.requests().size() == 1);
    }
}

TEST_CASE("cancellation interrupts a stalled provider at once", "[adapter.test.ts]") {
    // from sidecar-opencode/src/adapter.test.ts: cancellation interrupts a stalled provider and
    // the next pass starts cleanly
    SseReply stalled = SseReply::stream(
        {pi_chunk(Json{{"reasoning_content", "Thinking"}}), pi_chunk(Json{{"content", "late"}})});
    stalled.stall_after = 1;
    SseProvider provider({stalled, opencode_reply("chat", true)});
    const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
    OpenAiCompatClient client = client_for(profile);
    CancelSource cancel;
    std::atomic<bool> first_delta{false};
    std::atomic<std::int64_t> cancelled_at{0};
    std::thread canceller([&] {
        while (!first_delta.load()) {
            std::this_thread::sleep_for(1ms);
        }
        // Let the transfer settle into waiting on the silent connection.
        std::this_thread::sleep_for(200ms);
        cancelled_at = std::chrono::steady_clock::now().time_since_epoch().count();
        cancel.cancel();
    });
    std::optional<std::string> failure;
    std::int64_t returned_at = 0;
    try {
        (void)stream_generate(client, profile,
                              request_for("synthetic", default_messages(), cancel.token()),
                              [&](const StreamEvent&) { first_delta = true; });
    } catch (const StreamError&) {
        failure = "StreamError";
    } catch (const TextGenerationError& error) {
        failure = error.what();
    }
    returned_at = std::chrono::steady_clock::now().time_since_epoch().count();
    canceller.join();
    CHECK(failure == "canceled");
    const auto latency = std::chrono::steady_clock::duration(returned_at - cancelled_at.load());
    INFO(std::chrono::duration_cast<std::chrono::microseconds>(latency).count() << " us");
    CHECK(latency < 50ms);

    // The next pass starts cleanly.
    const TextResult next =
        stream_generate(client, profile, request_for("synthetic", default_messages()), {});
    CHECK(next.text == "Done");
}

TEST_CASE("a connection dropped after one delta is a transient failure with output seen",
          "[streaming]") {
    SseReply dropped = SseReply::stream({pi_chunk(Json{{"content", "Partial"}}),
                                         pi_chunk(Json{{"content", " answer"}}),
                                         pi_chunk(Json::object(), "stop"), "data: [DONE]\n\n"});
    dropped.drop_after = 1;
    SseProvider provider({dropped});
    const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
    OpenAiCompatClient client = client_for(profile);
    Collected progress;
    try {
        (void)stream_generate(client, profile, request_for("synthetic", default_messages()),
                              progress.sink());
        FAIL("no error");
    } catch (const StreamError& error) {
        CHECK(std::string(error.what()) == kStreamDroppedError);
        CHECK(error.transient);
        CHECK(error.output_seen);
        CHECK_FALSE(error.status);
    }
    REQUIRE(progress.events.size() == 1);
    CHECK(progress.events[0].text == "Partial");
}

TEST_CASE("a server that ignores stream answers with the whole reply", "[streaming]") {
    SseProvider provider({SseReply::json(200, reply_body("chat"))});
    const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
    OpenAiCompatClient client = client_for(profile);
    Collected progress;
    const TextResult result = stream_generate(
        client, profile, request_for("synthetic", default_messages()), progress.sink());
    CHECK(result.text == "Clean text.");
    CHECK(result.assistant_message.at("reasoning_content") == "private reasoning");
    CHECK(progress.events.empty());
}

TEST_CASE("nothing of a request or a stream reaches the log", "[streaming]") {
    LogScope log;
    SseProvider provider({opencode_reply("chat", false)});
    const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
    OpenAiCompatClient client = client_for(profile);
    (void)stream_generate(
        client, profile,
        request_for("synthetic",
                    Json::array({Json{{"role", "user"}, {"content", "PRIVATE-PROMPT"}}})),
        {});
    const std::string logged = log.sink->all();
    CHECK(logged.find("PRIVATE-PROMPT") == std::string::npos);
    CHECK(logged.find("Synthetic") == std::string::npos);
    CHECK(logged.find("synthetic-secret") == std::string::npos);
}
