// services/settings.py's text-model resolvers: AI cleanup's and meeting intelligence's endpoint
// and model.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "golden/golden.h"
#include "llm/catalog_client.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "llm_test_support.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

TEST_CASE("default models per provider", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_default_models_per_provider
    // TranscriptCleanup(provider=...) starts from default_transcript_cleanup_model.
    CHECK(default_transcript_cleanup_model("openai", Json::object()) == kTranscriptCleanupModel);
    CHECK(default_transcript_cleanup_model("openrouter", Json::object()) ==
          kTranscriptCleanupOpenRouterModel);
}

TEST_CASE("openai model filter keeps chat models only", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanup::test_openai_model_filter_keeps_chat_models_only
    CHECK(filter_openai_chat_models({"gpt-4o-mini", "whisper-1", "gpt-4o-audio-preview",
                                     "text-embedding-3-small", "o4-mini", "dall-e-3",
                                     "gpt-4o-mini-tts", "gpt-4o-realtime-preview"}) ==
          std::vector<std::string>{"gpt-4o-mini", "o4-mini"});
}

TEST_CASE("settings key and config default", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestSettingsIntegration::test_settings_key_and_config_default
    // (config.TRANSCRIPT_CLEANUP_PROMPT's wording belongs to the prompts, fv_text.)
    CHECK(keys::transcript_cleanup_enabled.key() == "transcript_cleanup_enabled");
    CHECK(keys::transcript_cleanup_prompt.key() == "transcript_cleanup_prompt");
    CHECK_FALSE(kTranscriptCleanupEnabled);
    CHECK_FALSE(keys::transcript_cleanup_enabled.default_value());
    CHECK(kTranscriptCleanupTimeoutS == 8.0);
}

TEST_CASE("save and load cleanup setting", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestSettingsIntegration::test_save_and_load_cleanup_setting
    testkit::TempDataRoot root;
    const auto path = root.path() / "settings.json";
    SettingsStore store(path);
    store.save_setting(keys::transcript_cleanup_enabled.key(), true);
    CHECK(store.value(keys::transcript_cleanup_enabled.key(), false) == Json(true));
}

TEST_CASE("resolve provider validates and falls back", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestSettingsIntegration::test_resolve_provider_validates_and_falls_back
    // A provider alone has no model, so the pair falls back as a whole.
    CHECK(resolve_transcript_cleanup_provider(
              Json{{"transcript_cleanup_provider", "openrouter"}}) == "openrouter");
    CHECK(resolve_transcript_cleanup_provider(Json{{"transcript_cleanup_provider", "bogus"}}) ==
          kTranscriptCleanupProvider);
    CHECK(resolve_transcript_cleanup_provider(Json::object()) == kTranscriptCleanupProvider);
}

TEST_CASE("resolve model keeps a complete pair", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestSettingsIntegration::test_resolve_model_keeps_a_complete_pair
    const Json saved{{"transcript_cleanup_provider", "openai"},
                     {"transcript_cleanup_model", "  gpt-4.1-mini  "}};
    CHECK(resolve_transcript_cleanup_provider(saved) == "openai");
    CHECK(resolve_transcript_cleanup_model(saved) == "gpt-4.1-mini");
    CHECK(resolve_transcript_cleanup_model(Json::object()) == kTranscriptCleanupOpenRouterModel);
    CHECK(resolve_transcript_cleanup_model(Json{{"transcript_cleanup_provider", "openrouter"}}) ==
          kTranscriptCleanupOpenRouterModel);
    CHECK(resolve_transcript_cleanup_model(Json{{"transcript_cleanup_model", "  my-model  "}}) ==
          kTranscriptCleanupOpenRouterModel);
}

TEST_CASE("resolve reasoning validates and falls back", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestSettingsIntegration::test_resolve_reasoning_validates_and_falls_back
    CHECK(resolve_transcript_cleanup_reasoning(Json{{"transcript_cleanup_reasoning", "high"}}) ==
          "high");
    CHECK(resolve_transcript_cleanup_reasoning(Json{{"transcript_cleanup_reasoning", "extreme"}}) ==
          "off");
}

TEST_CASE("resolve accepts saved custom profile", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestCustomEndpointCleanup::test_resolve_accepts_saved_custom_profile
    const Json settings{
        {"text_llm_profiles", Json::array({{{"id", "custom_abcd1234"},
                                            {"name", "Local"},
                                            {"base_url", "http://127.0.0.1:1234/v1"},
                                            {"api_key_env", ""}}})},
        {"transcript_cleanup_provider", "custom_abcd1234"},
        {"transcript_cleanup_model", "local-qwen"}};
    CHECK(resolve_transcript_cleanup_provider(settings) == "custom_abcd1234");
    CHECK(resolve_transcript_cleanup_model(settings) == "local-qwen");
}

TEST_CASE("legacy openai cleanup default resolves to luna without mutating settings",
          "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::test_legacy_openai_cleanup_default_resolves_to_luna_without_mutating_settings
    for (const auto* model : {"gpt-4o-mini", "  gpt-4o-mini  "}) {
        const Json settings{{"transcript_cleanup_provider", "openai"},
                            {"transcript_cleanup_model", model},
                            {"transcript_cleanup_enabled", false},
                            {"meeting_llm_provider", "openai"},
                            {"meeting_llm_model", "gpt-4o-mini"}};
        const Json original = settings;
        CHECK(resolve_transcript_cleanup_model(settings) == "gpt-6-luna");
        CHECK(resolve_transcript_cleanup_provider(settings) == "openai");
        CHECK(resolve_meeting_llm_model(settings) == "gpt-4o-mini");
        CHECK(settings == original);
        CHECK(resolve_transcript_cleanup_model(settings) == "gpt-6-luna");
    }
}

TEST_CASE("cleanup migration preserves other assignments", "[test_transcript_cleanup]") {
    // from tests/test_transcript_cleanup.py::test_cleanup_migration_preserves_other_assignments
    for (const auto& [provider, model] : std::vector<std::pair<std::string, std::string>>{
             {"openai", "gpt-4.1-mini"},
             {"openai", "gpt-4o-mini-2024-07-18"},
             {"openai", "ft:gpt-4o-mini:my-org:custom"},
             {"openai", "gpt-6-luna"},
             {"openrouter", "gpt-4o-mini"},
             {"openrouter", "openai/gpt-4o-mini"}}) {
        CHECK(resolve_transcript_cleanup_model(Json{{"transcript_cleanup_provider", provider},
                                                    {"transcript_cleanup_model", model}}) == model);
    }
}

TEST_CASE("luna default keeps cleanup opt in and openrouter default", "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::test_luna_default_keeps_cleanup_opt_in_and_openrouter_default
    CHECK(default_transcript_cleanup_model("openai", Json::object()) == "gpt-6-luna");
    CHECK_FALSE(kTranscriptCleanupEnabled);
    CHECK(kTranscriptCleanupProvider == "openrouter");
    CHECK(resolve_transcript_cleanup_model(Json::object()) == "openrouter/free");
}

TEST_CASE("an installed agent keeps its saved model, even blank", "[cleanup_settings]") {
    CHECK(resolve_transcript_cleanup_provider(Json{{"transcript_cleanup_provider", "claude_code"},
                                                   {"transcript_cleanup_model", ""}}) ==
          "claude_code");
    CHECK(resolve_transcript_cleanup_model(Json{{"transcript_cleanup_provider", "codex"},
                                                {"transcript_cleanup_model", " gpt-5 "}}) ==
          "gpt-5");
    // Without a saved model it is not a pair.
    CHECK(resolve_transcript_cleanup_provider(Json{{"transcript_cleanup_provider", "codex"}}) ==
          "openrouter");
    // The meeting pair never keeps an agent.
    CHECK(resolve_meeting_llm_provider(Json{{"meeting_llm_provider", "claude_code"},
                                            {"meeting_llm_model", "sonnet"}}) == "openrouter");
    CHECK(default_transcript_cleanup_model("claude_code", Json::object()).empty());
    CHECK(resolve_transcript_cleanup_model_sort(
              Json{{"transcript_cleanup_model_sort", "newest"}}) == "newest");
    CHECK(resolve_transcript_cleanup_model_sort(Json{{"transcript_cleanup_model_sort", "bogus"}}) ==
          "alphabetical");
}

TEST_CASE("cleanup settings replay services.settings", "[golden]") {
    const std::string module = "services.settings";
    golden::for_each(module, "default_transcript_cleanup_model", [](const golden::Case& c) {
        CHECK(default_transcript_cleanup_model(c.arg<std::string>(0), Json::object()) ==
              c.result<std::string>());
    });
    golden::for_each(module, "resolve_transcript_cleanup_provider", [](const golden::Case& c) {
        CHECK(resolve_transcript_cleanup_provider(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "resolve_transcript_cleanup_model", [](const golden::Case& c) {
        CHECK(resolve_transcript_cleanup_model(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "resolve_transcript_cleanup_reasoning", [](const golden::Case& c) {
        CHECK(resolve_transcript_cleanup_reasoning(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "resolve_meeting_llm_provider", [](const golden::Case& c) {
        CHECK(resolve_meeting_llm_provider(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "resolve_meeting_llm_model", [](const golden::Case& c) {
        CHECK(resolve_meeting_llm_model(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "resolve_meeting_llm_profile", [](const golden::Case& c) {
        const auto profile = resolve_meeting_llm_profile(c.arg<Json>(0));
        const Json actual{{"id", profile.id},
                          {"name", profile.name},
                          {"kind", profile.kind},
                          {"base_url", profile.base_url ? Json(*profile.base_url) : Json()},
                          {"api_key_env", profile.api_key_env},
                          {"builtin", profile.builtin},
                          {"protocol", profile.protocol}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "resolve_meeting_llm_endpoint", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), resolve_meeting_llm_endpoint(c.arg<Json>(0))));
    });
}
