// tests/test_settings.py: the engine selection and its legacy API migration, and the one-time
// install rules (keep the engine a new install started with, seed the new-install tones).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "settings/install.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "settings_test_support.h"

using fv::Json;
using fv::settings_test::read_bytes;
using fv::settings_test::TempDir;
using fv::settings_test::write_bytes;

namespace {

constexpr std::chrono::year_month_day kBeforeRetirement{
    std::chrono::year{2026}, std::chrono::month{11}, std::chrono::day{14}};

} // namespace

TEST_CASE("api backend migration preserves model", "[test_settings]") {
    // from tests/test_settings.py::test_api_backend_migration_preserves_model
    // OpenWhisper then saves "local_whisper" and "api" again; OpenWhisper_CPP has no Local Whisper,
    // so the round trip goes through "parakeet" instead.
    const std::vector<std::pair<std::string, std::string>> legacy{
        {"api_whisper", "whisper-1"},
        {"api_gpt4o", "gpt-4o-transcribe"},
        {"api_gpt4o_mini", "gpt-4o-mini-transcribe"},
    };
    for (const auto& [name, expected] : legacy) {
        INFO(name);
        TempDir temp;
        const auto path = temp.path() / "settings.json";
        fv::SettingsStore store(path);
        store.save_all(Json{{"selected_model", name}, {"unrelated", true}});
        CHECK(fv::load_model_selection(store, kBeforeRetirement) == "api");
        const auto settings = store.snapshot();
        CHECK(settings->at("api_transcription_model") == expected);
        CHECK(settings->at("selected_model") == "api");
        CHECK(settings->at("unrelated") == true);
        fv::SettingsStore restarted(path);
        CHECK(fv::load_model_selection(restarted, kBeforeRetirement) == "api");
        fv::save_model_selection(restarted, "parakeet");
        fv::save_model_selection(restarted, "api");
        CHECK(restarted.value("api_transcription_model") == expected);
    }
}

TEST_CASE("model selection falls back to platform default", "[test_settings]") {
    // from tests/test_settings.py::test_model_selection_falls_back_to_platform_default
    // OpenWhisper saves and reads back "local_whisper"; OpenWhisper_CPP's other engine is "remote".
    TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    CHECK(fv::load_model_selection(store) == "parakeet");
    store.save_all(Json{{"selected_model", "not-a-backend"}});
    CHECK(fv::load_model_selection(store) == "parakeet");
    fv::save_model_selection(store, "remote");
    CHECK(fv::load_model_selection(store) == "remote");
}

TEST_CASE("a saved engine OpenWhisper_CPP does not have reads as the default and stays saved") {
    TempDir temp;
    const auto path = temp.path() / "settings.json";
    write_bytes(path, R"({"selected_model": "local_whisper"})");
    fv::SettingsStore store(path);
    CHECK(fv::load_model_selection(store) == "parakeet");
    CHECK(store.value("selected_model") == "local_whisper");
}

TEST_CASE("save model selection refuses an engine OpenWhisper_CPP does not have") {
    fv::MemorySettingsStore store;
    try {
        fv::save_model_selection(store, "local_whisper");
        FAIL("accepted local_whisper");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) ==
              "Invalid model 'local_whisper'. Valid models: ['api', 'parakeet', 'remote']");
    }
    CHECK_THROWS_WITH(fv::save_model_selection(store, ""),
                      "model_value must be a non-empty string");
    CHECK(store.contents() == Json::object());
}

TEST_CASE("the retirement date moves a migrated legacy model to the default API model") {
    fv::MemorySettingsStore store(Json{{"selected_model", "api_gpt4o_mini"}});
    CHECK(fv::load_model_selection(
              store, std::chrono::year_month_day{std::chrono::year{2027}, std::chrono::month{2},
                                                 std::chrono::day{26}}) == "api");
    CHECK(store.value("api_transcription_model") == "gpt-transcribe");
}

TEST_CASE("a new install keeps the default it started with", "[test_settings]") {
    // from tests/test_settings.py::test_a_new_install_keeps_the_default_it_started_with
    TempDir temp;
    fv::SettingsStore store(temp.path() / "openwhisper_cpp_settings.json");
    CHECK(store.is_new_install());
    CHECK(fv::keep_unchosen_engine(store));
    CHECK(store.value("selected_model") == "parakeet");
    CHECK_FALSE(fv::keep_unchosen_engine(store));
}

TEST_CASE("a chosen engine is never replaced", "[test_settings]") {
    // from tests/test_settings.py::test_a_chosen_engine_is_never_replaced
    for (const char* saved : {"parakeet", "local_whisper", "api"}) {
        INFO(saved);
        TempDir temp;
        const auto path = temp.path() / "openwhisper_cpp_settings.json";
        write_bytes(path, Json{{"selected_model", saved}}.dump());
        fv::SettingsStore store(path);
        const auto stamp = std::filesystem::last_write_time(path);
        CHECK_FALSE(fv::keep_unchosen_engine(store));
        // OpenWhisper_CPP reads local_whisper as its default, and keeps it saved.
        CHECK(fv::load_model_selection(store) ==
              (std::string(saved) == "local_whisper" ? "parakeet" : saved));
        CHECK(std::filesystem::last_write_time(path) == stamp);
        CHECK(store.value("selected_model") == saved);
    }
}

TEST_CASE("existing installs elsewhere still follow the default", "[test_settings]") {
    // from tests/test_settings.py::test_existing_installs_elsewhere_still_follow_the_default
    // Python runs it for win32, darwin and linux hosts; OpenWhisper_CPP has the one rule on every
    // platform: an existing install is never given an engine.
    TempDir temp;
    const auto path = temp.path() / "openwhisper_cpp_settings.json";
    write_bytes(path, "{}");
    fv::SettingsStore store(path);
    CHECK_FALSE(fv::keep_unchosen_engine(store));
    CHECK_FALSE(store.snapshot()->contains("selected_model"));
    CHECK(read_bytes(path) == "{}");
}

TEST_CASE("a new install that already saved an engine keeps it") {
    fv::MemorySettingsStore store(std::nullopt);
    store.save_setting("selected_model", "remote");
    CHECK_FALSE(fv::keep_unchosen_engine(store));
    CHECK(store.value("selected_model") == "remote");
}

TEST_CASE("a new install is seeded with casual chat tones once") {
    fv::MemorySettingsStore store(std::nullopt);
    CHECK(fv::seed_new_install_settings(store));
    const Json tones = store.snapshot()->at("app_style_tones");
    CHECK(tones.dump() ==
          R"({"email":"formal","work":"casual","personal":"casual","other":"formal"})");
    CHECK_FALSE(fv::seed_new_install_settings(store));
    CHECK(store.snapshot()->at("app_style_tones") == tones);
}

TEST_CASE("an existing install is not seeded") {
    fv::MemorySettingsStore store;
    CHECK_FALSE(fv::seed_new_install_settings(store));
    CHECK(store.contents() == Json::object());
    CHECK(store.file().writes() == 0);
}

TEST_CASE("a failed seed or engine write reads false") {
    fv::MemorySettingsStore seed(std::nullopt);
    seed.file().fail_next_replace("disk full");
    CHECK_FALSE(fv::seed_new_install_settings(seed));
    fv::MemorySettingsStore engine(std::nullopt);
    engine.file().fail_next_replace("disk full");
    CHECK_FALSE(fv::keep_unchosen_engine(engine));
    CHECK_FALSE(engine.file().bytes().has_value());
}
