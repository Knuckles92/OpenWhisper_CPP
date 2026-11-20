// services/text_model_catalog.py: the OpenCode route table, OpenAI's id rules, saved metadata.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <stdexcept>
#include <string>

#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/credentials.h"
#include "llm/local_servers.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm_test_support.h"
#include "pyport/errors.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

namespace {

TextLLMProfile builtin(std::string_view id, const Json& settings = Json::object()) {
    return *get_profile(id, settings);
}

std::string error_of(const TextLLMProfile& profile, std::string_view model) {
    try {
        model_spec(profile, model);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("snapshot retains ollama host and protocol", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_snapshot_retains_ollama_host_and_protocol
    forget_server_specs();
    TestKeys keys;
    http::FakeHttpClient client; // host-a is not reachable: learning keeps the defaults
    client.set_handler([](const http::Request&) {
        http::ScriptedResponse reply;
        reply.error = http::Error{http::ErrorKind::Dns, "getaddrinfo failed"};
        return reply;
    });
    const auto profile = builtin("ollama", Json{{"ollama_base_url", "http://host-a:11434/v1"}});
    const auto snapshot = snapshot_from_profile(profile, "llama3.2", client, keys.sources);
    const auto restored = snapshot_from_mapping(snapshot.to_dict())->to_profile();
    CHECK(restored.base_url == "http://host-a:11434/v1");
    CHECK(model_spec(restored, "llama3.2").protocol == "chat");
    CHECK_FALSE(snapshot.to_dict().contains("api_key"));
    REQUIRE_FALSE(client.requests().empty());
    CHECK(client.requests()[0].url == "http://host-a:11434/v1/models");
}

TEST_CASE("unknown opencode route is not guessed", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_unknown_opencode_route_is_not_guessed
    for (const auto* provider : {"opencode_go", "opencode_zen"}) {
        const auto message = error_of(builtin(provider), "future-model");
        CHECK(message.find("no supported API route") != std::string::npos);
    }
}

TEST_CASE("same model has distinct go and zen protocols", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_same_model_has_distinct_go_and_zen_protocols
    CHECK(model_spec(builtin("opencode_go"), "minimax-m3").protocol == "anthropic");
    CHECK(model_spec(builtin("opencode_zen"), "minimax-m3").protocol == "chat");
}

TEST_CASE("openai model routes are reasoning aware", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_openai_model_routes_are_reasoning_aware
    const std::vector<std::tuple<std::string, std::string, bool>> cases{
        {"o4-mini", "responses", true},
        {"gpt-5.6-luna", "responses", true},
        {"gpt-4o-mini", "responses", false},
        {"gpt-4o-2024-08-06", "responses", false},
        {"gpt-4.1-mini", "responses", false},
        {"ft:gpt-4.1-mini-2025-04-14:org:custom:123", "responses", false},
        {"gpt-4", "chat", false},
        {"gpt-3.5-turbo", "chat", false},
        {"chatgpt-4o-latest", "chat", false},
        {"gpt-4o-audio-preview", "chat", false},
        {"gpt-4o-search-preview", "chat", false},
        {"gpt-4o-mini-transcribe", "chat", false},
    };
    for (const auto& [model, protocol, reasoning] : cases) {
        const auto spec = model_spec(builtin("openai"), model);
        CHECK(spec.protocol == protocol);
        CHECK(spec.reasoning == reasoning);
    }
}

TEST_CASE("opencode credentials and headers are separate", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_opencode_credentials_and_headers_are_separate
    const auto go = builtin("opencode_go");
    const auto zen = builtin("opencode_zen");
    CHECK(go.api_key_env != zen.api_key_env);
    const auto groq_headers = provider_headers(builtin("groq"), "meeting");
    CHECK_FALSE(groq_headers);
    CHECK_FALSE(builtin("ollama").requires_api_key());
}

TEST_CASE("luna off mapping does not change other models or custom endpoints",
          "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_luna_off_mapping_does_not_change_other_models_or_custom_endpoints
    for (const auto& [provider, model] :
         std::vector<std::pair<std::string, std::string>>{{"openai", "o4-mini"},
                                                          {"openai", "gpt-5.6-luna"},
                                                          {"openai", "gpt-4o-mini"},
                                                          {"openrouter", "openai/gpt-6-luna"}}) {
        const auto levels = model_spec(builtin(provider), model).thinking_levels;
        const bool off_mapped = levels && levels->contains("off") && !(*levels)["off"].is_null();
        CHECK_FALSE(off_mapped);
    }
    // Luna itself on OpenAI does map Off.
    CHECK(model_spec(builtin("openai"), "gpt-6-luna").thinking_levels == Json{{"off", "none"}});
}

TEST_CASE("every capability entry is reachable through a route", "[test_text_generation]") {
    // from tests/test_text_generation.py::test_every_capability_entry_is_reachable_through_a_route
    for (const auto& [kind, capabilities] : catalog_capabilities().items()) {
        const auto routed = catalog_models(kind);
        for (const auto& [model, entry] : capabilities.items()) {
            INFO(kind << " " << model);
            CHECK(std::ranges::find(routed, model) != routed.end());
        }
    }
}

TEST_CASE("every capability reasoning format survives a saved snapshot", "[test_text_generation]") {
    // from
    // tests/test_text_generation.py::test_every_capability_reasoning_format_survives_a_saved_snapshot
    for (const auto& [kind, capabilities] : catalog_capabilities().items()) {
        for (const auto& [model, entry] : capabilities.items()) {
            if (!entry.contains("reasoning_format")) {
                continue;
            }
            Json saved = entry;
            saved["model"] = model;
            CHECK(spec_from_mapping(saved).reasoning_format ==
                  entry.at("reasoning_format").get<std::string>());
        }
    }
}

TEST_CASE("saved metadata wins for the same model, except on OpenAI", "[model_catalog]") {
    auto zen = builtin("opencode_zen");
    zen.model_metadata = Json{{"model", "future-model"}, {"protocol", "google"}};
    CHECK(model_spec(zen, "future-model").protocol == "google");
    CHECK_THROWS_AS(model_spec(zen, "other-future"), std::invalid_argument);
    auto openai = builtin("openai");
    openai.model_metadata = Json{{"model", "o4-mini"}, {"protocol", "chat"}};
    CHECK(model_spec(openai, "o4-mini").protocol == "responses");
}

TEST_CASE("spec_from_mapping clamps its limits and refuses an unknown protocol",
          "[model_catalog]") {
    CHECK(spec_from_mapping(Json{{"context_window", 10}}).context_window == 1024);
    CHECK(spec_from_mapping(Json{{"context_window", 5'000'000}}).context_window == 2'000'000);
    CHECK(spec_from_mapping(Json{{"max_output_tokens", 1}}).max_output_tokens == 16);
    CHECK(spec_from_mapping(Json{{"max_output_tokens", 999'999}}).max_output_tokens == 131072);
    CHECK_THROWS_AS(spec_from_mapping(Json{{"protocol", "other"}}), std::invalid_argument);
    CHECK_THROWS_AS(spec_from_mapping(Json{{"context_window", nullptr}}), py::TypeError);
    CHECK_THROWS_AS(spec_from_mapping(Json{{"context_window", "abc"}}), py::ValueError);
}

TEST_CASE("the server spec cache is per base URL and replaced whole", "[model_catalog]") {
    forget_server_specs();
    const auto vllm = builtin("vllm");
    CHECK_FALSE(server_specs_known(*vllm.base_url));
    CHECK(model_spec(vllm, "m") == TextModelSpec{});
    TextModelSpec reported;
    reported.context_window = 8192;
    remember_server_specs(*vllm.base_url, {{"m", reported}});
    CHECK(server_specs_known(*vllm.base_url));
    CHECK(model_spec(vllm, "m").context_window == 8192);
    remember_server_specs(*vllm.base_url, {});
    CHECK(model_spec(vllm, "m") == TextModelSpec{});
    forget_server_specs();
}

TEST_CASE("model catalog replays services.text_model_catalog", "[golden]") {
    forget_server_specs();
    golden::for_each("services.text_model_catalog", "model_spec", [](const golden::Case& c) {
        Json actual;
        try {
            actual = model_spec(profile_from(c.arg<Json>(0)), c.arg<std::string>(1)).to_dict();
        } catch (const std::invalid_argument& error) {
            actual = Json{{"raises", "ValueError"}, {"message", error.what()}};
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each("services.text_model_catalog", "spec_from_mapping", [](const golden::Case& c) {
        Json actual;
        try {
            actual = spec_from_mapping(c.arg<Json>(0)).to_dict();
        } catch (const std::invalid_argument& error) {
            actual = Json{{"raises", "ValueError"}, {"message", error.what()}};
        } catch (const py::PyError& error) {
            actual = Json{{"raises", std::string(error.type_name())}, {"message", error.what()}};
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}
