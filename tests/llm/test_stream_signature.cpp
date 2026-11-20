// A provider's 400 that names a corrupted thought signature (Gemini refusing a replayed
// reasoning signature) reads as that fixed sentence, which the built-in agent's polish retry
// looks for (sidecar.py's "corrupted thought signature" check); nothing else of the body leaves.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "http/client.h"
#include "llm/openai_client.h"
#include "llm/streaming.h"
#include "llm_test_support.h"
#include "sse_provider.h"
#include "streaming_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;
using fv::testing::SseProvider;
using fv::testing::SseReply;

namespace {

std::shared_ptr<http::Client> direct_client() {
    http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    return http::make_curl_client(std::move(options));
}

GenerateRequest request_for(std::string model, Json messages) {
    GenerateRequest request;
    request.model = std::move(model);
    request.messages = std::move(messages);
    return request;
}

std::optional<StreamError> failure_for(const SseReply& reply) {
    SseProvider provider({reply});
    const TextLLMProfile profile = synthetic_profile("chat", provider.url("/v1"));
    OpenAiCompatClient client(direct_client(), profile.base_url.value_or(""), "synthetic-secret",
                              {}, 15.0, 0);
    try {
        (void)stream_generate(client, profile, request_for("synthetic", default_messages()), {});
    } catch (const StreamError& error) {
        return error;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a 400 naming a corrupted thought signature says so and nothing more", "[streaming]") {
    const auto named = failure_for(
        SseReply::json(400, {{"error",
                              {{"code", 400},
                               {"message", "Corrupted thought signature. private diagnostic"},
                               {"status", "INVALID_ARGUMENT"}}}}));
    REQUIRE(named);
    CHECK(std::string(named->what()) == "Corrupted thought signature.");
    CHECK(named->status == 400);
    CHECK_FALSE(named->transient);

    // Any other 400 keeps the SDK's bodiless text.
    const auto other =
        failure_for(SseReply::json(400, {{"error", {{"message", "private diagnostic"}}}}));
    REQUIRE(other);
    CHECK(std::string(other->what()) == "Error code: 400");

    // Only a 400 is read: another status naming it is still just its status.
    const auto server = failure_for(
        SseReply::json(500, {{"error", {{"message", "Corrupted thought signature."}}}}));
    REQUIRE(server);
    CHECK(std::string(server->what()) == "Error code: 500");
}
