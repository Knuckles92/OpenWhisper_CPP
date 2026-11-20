// The Hugging Face access policy: the seven tests of OpenWhisper's tests/test_settings.py
// (TestSettingsManager) that cover it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

#include "base/testing/map_env_reader.h"
#include "models/hf_access_policy.h"
#include "models_test_support.h"
#include "settings/store.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;

namespace {

struct TempSettings {
    TempDir temp;
    fs::path file = temp.path() / "settings.json";
    fv::SettingsStore store{file};
};

} // namespace

TEST_CASE("TestSettingsManager::test_is_hf_hub_offline_env_set", "[test_settings]") {
    fv::MapEnvReader env;
    CHECK_FALSE(is_hf_hub_offline_env_set(env));
    env.set("HF_HUB_OFFLINE", "1");
    CHECK(is_hf_hub_offline_env_set(env));
    env.set("HF_HUB_OFFLINE", "0");
    CHECK_FALSE(is_hf_hub_offline_env_set(env));
    // Stripped and case-insensitive, as the other truthy spellings.
    env.set("HF_HUB_OFFLINE", "  TRUE ");
    CHECK(is_hf_hub_offline_env_set(env));
    env.set("HF_HUB_OFFLINE", "Yes");
    CHECK(is_hf_hub_offline_env_set(env));
    env.set("HF_HUB_OFFLINE", "on");
    CHECK(is_hf_hub_offline_env_set(env));
}

TEST_CASE("TestSettingsManager::test_hf_access_policy_defaults_to_ask", "[test_settings]") {
    TempSettings settings;
    CHECK(load_hf_access_policy(settings.store) == hf_access_policy::kAsk);
    // Reading the default creates no settings file.
    CHECK_FALSE(fs::exists(settings.file));
}

TEST_CASE("TestSettingsManager::test_hf_access_policy_migrates_legacy_offline_true_to_never",
          "[test_settings]") {
    TempSettings settings;
    settings.store.save_all(Json{{"hf_hub_offline", true}});
    CHECK(load_hf_access_policy(settings.store) == hf_access_policy::kNever);
    // The migration is saved and the legacy key removed.
    const auto stored = settings.store.snapshot();
    CHECK(stored->value("hf_access_policy", "") == "never");
    CHECK_FALSE(stored->contains("hf_hub_offline"));
}

TEST_CASE("TestSettingsManager::test_hf_access_policy_migrates_legacy_offline_false_to_ask",
          "[test_settings]") {
    TempSettings settings;
    settings.store.save_all(Json{{"hf_hub_offline", false}});
    CHECK(load_hf_access_policy(settings.store) == hf_access_policy::kAsk);
    const auto stored = settings.store.snapshot();
    CHECK(stored->value("hf_access_policy", "") == "ask");
    CHECK_FALSE(stored->contains("hf_hub_offline"));
}

TEST_CASE("TestSettingsManager::test_hf_access_policy_invalid_value_falls_back_to_ask",
          "[test_settings]") {
    TempSettings settings;
    settings.store.save_all(Json{{"hf_access_policy", "yolo"}});
    CHECK(load_hf_access_policy(settings.store) == hf_access_policy::kAsk);
    CHECK(settings.store.snapshot()->value("hf_access_policy", "") == "ask");
}

TEST_CASE("TestSettingsManager::test_save_hf_access_policy_roundtrip_and_legacy_cleanup",
          "[test_settings]") {
    TempSettings settings;
    settings.store.save_all(Json{{"hf_hub_offline", true}});
    save_hf_access_policy(settings.store, hf_access_policy::kAlways);
    CHECK(load_hf_access_policy(settings.store) == hf_access_policy::kAlways);
    CHECK_FALSE(settings.store.snapshot()->contains("hf_hub_offline"));
}

TEST_CASE("TestSettingsManager::test_save_hf_access_policy_rejects_invalid_value",
          "[test_settings]") {
    TempSettings settings;
    CHECK_THROWS_AS(save_hf_access_policy(settings.store, "sometimes"), std::invalid_argument);
    CHECK_THROWS_WITH(save_hf_access_policy(settings.store, "sometimes"),
                      "Invalid HF access policy 'sometimes'. Valid values: ['ask', 'always', "
                      "'never']");
}
