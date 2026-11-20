// services/text_llm.py's profiles: named OpenAI-compatible text-model endpoints, their local
// server overrides, snapshots and consent copy.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "llm/cleanup_settings.h"
#include "llm/credentials.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm_test_support.h"

using namespace fv;
using namespace fv::llm;
using namespace fv::llm::test;

namespace {

Json settings_with_custom(const Json& entry) {
    return Json{{"text_llm_profiles", Json::array({entry})}};
}

// A vector's {"raises": "ValueError", "message": ...} for an invalid_argument, else the value.
template <class Fn> Json value_or_error(Fn&& call) {
    try {
        return call();
    } catch (const std::invalid_argument& error) {
        return Json{{"raises", "ValueError"}, {"message", error.what()}};
    }
}

Json profiles_json(const std::vector<TextLLMProfile>& profiles) {
    Json out = Json::array();
    for (const auto& profile : profiles) {
        out.push_back(profile_fields(profile));
    }
    return out;
}

// The case file's settings after an upsert: a new random id written as "<new>".
Json masked(Json settings, const std::string& new_id) {
    if (!new_id.empty() && settings.contains("text_llm_profiles")) {
        for (auto& entry : settings["text_llm_profiles"]) {
            if (entry.is_object() && entry.contains("id") && entry["id"] == Json(new_id)) {
                entry["id"] = "<new>";
            }
        }
    }
    return settings;
}

std::optional<std::string_view> optional_id(const Json& value, std::string& storage) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    storage = value.get<std::string>();
    return storage;
}

} // namespace

// TestProfileValidation

TEST_CASE("normalize base url strips slash and requires http", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileValidation::
    //     test_normalize_base_url_strips_slash_and_requires_http
    CHECK(normalize_base_url("http://127.0.0.1:1234/v1/") == "http://127.0.0.1:1234/v1");
    CHECK_THROWS_AS(normalize_base_url("ftp://127.0.0.1/v1"), std::invalid_argument);
    CHECK_THROWS_AS(normalize_base_url("not-a-url"), std::invalid_argument);
    CHECK_THROWS_AS(normalize_base_url(""), std::invalid_argument);
}

TEST_CASE("validate profile name", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileValidation::test_validate_profile_name
    CHECK(validate_profile_name("  LM Studio  ") == "LM Studio");
    CHECK_THROWS_AS(validate_profile_name("   "), std::invalid_argument);
    CHECK_THROWS_AS(validate_profile_name(std::string(81, 'x')), std::invalid_argument);
}

TEST_CASE("validate api key env allows blank", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileValidation::test_validate_api_key_env_allows_blank
    CHECK(validate_api_key_env("") == "");
    CHECK(validate_api_key_env("  LMSTUDIO_API_KEY ") == "LMSTUDIO_API_KEY");
    CHECK_THROWS_AS(validate_api_key_env("api-key"), std::invalid_argument);
    CHECK_THROWS_AS(validate_api_key_env("1KEY"), std::invalid_argument);
}

// TestProfileCrud

TEST_CASE("builtins always present", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_builtins_always_present
    const auto profiles = list_profiles(Json::object());
    std::vector<std::string> ids;
    for (const auto& profile : profiles) {
        ids.push_back(profile.id);
        CHECK(profile.builtin);
    }
    CHECK(ids == std::vector<std::string>{"openai", "openrouter", "ollama", "lm_studio", "vllm",
                                          "llama_cpp", "localai", "groq", "opencode_go",
                                          "opencode_zen"});
}

TEST_CASE("local server presets ship their standard urls", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestProfileCrud::test_local_server_presets_ship_their_standard_urls
    TestKeys keys;
    const std::vector<std::pair<std::string, std::string>> expected{
        {"ollama", "http://localhost:11434/v1"}, {"lm_studio", "http://127.0.0.1:1234/v1"},
        {"vllm", "http://127.0.0.1:8000/v1"},    {"llama_cpp", "http://127.0.0.1:9931/v1"},
        {"localai", "http://127.0.0.1:8080/v1"},
    };
    for (const auto& [id, url] : expected) {
        const auto profile = get_profile(id, Json::object());
        REQUIRE(profile);
        CHECK(profile->base_url == url);
        CHECK(profile->is_local());
        CHECK_FALSE(profile->requires_api_key());
        CHECK(resolve_api_key(*profile, keys.sources) == "dummy");
    }
}

TEST_CASE("local server url override comes from its own setting", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestProfileCrud::test_local_server_url_override_comes_from_its_own_setting
    const auto profile =
        get_profile("vllm", Json{{"vllm_base_url", "http://192.168.1.50:8000/v1/"}});
    CHECK(profile->base_url == "http://192.168.1.50:8000/v1");
    // One preset's setting never leaks into another preset.
    const auto other =
        get_profile("lm_studio", Json{{"vllm_base_url", "http://192.168.1.50:8000/v1"}});
    CHECK(other->base_url == "http://127.0.0.1:1234/v1");
}

TEST_CASE("save local server normalizes per preset", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_save_local_server_normalizes_per_preset
    Json settings = Json::object();
    save_local_server(settings, "lm_studio", "http://localhost:1234/");
    CHECK(settings ==
          Json{{"lm_studio_base_url", "http://localhost:1234/v1"}, {"lm_studio_api_key_env", ""}});
    const Json saved = settings;
    for (const auto* url : {"http://user:secret@localhost", "http://localhost/?key=secret"}) {
        CHECK_THROWS_AS(save_local_server(settings, "lm_studio", url), std::invalid_argument);
    }
    CHECK_THROWS_AS(
        save_local_server(settings, "lm_studio", "http://localhost:1234/v1", "not a name"),
        std::invalid_argument);
    CHECK_THROWS_AS(save_local_server(settings, "openai", "http://localhost:1234/v1"),
                    std::invalid_argument);
    // Only the first call saved anything.
    CHECK(settings == saved);
}

TEST_CASE("local preset can require an api key", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_local_preset_can_require_an_api_key
    const Json settings{{"llama_cpp_api_key_env", "LLAMA_SERVER_KEY"}};
    const auto profile = get_profile("llama_cpp", settings);
    CHECK(profile->requires_api_key());
    CHECK(credential_label(*profile) == "llama.cpp API key");
    {
        TestKeys keys;
        keys.store.set("LLAMA_SERVER_KEY", "sk-local-key-value");
        CHECK(resolve_api_key(*profile, keys.sources) == "sk-local-key-value");
    }
    {
        TestKeys keys;
        CHECK_FALSE(resolve_api_key(*profile, keys.sources));
    }
    const auto snapshot = snapshot_from_profile(*profile);
    CHECK(snapshot.to_profile().api_key_env == "LLAMA_SERVER_KEY");
    // A malformed saved variable name falls back to no key.
    CHECK_FALSE(
        get_profile("llama_cpp", Json{{"llama_cpp_api_key_env", "bad name"}})->requires_api_key());
}

TEST_CASE("upsert and remove custom profile", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_upsert_and_remove_custom_profile
    Json settings = Json::object();
    const auto profile =
        upsert_custom_profile(settings, "LM Studio", "http://127.0.0.1:1234/v1/", "");
    CHECK(profile.id.starts_with("custom_"));
    CHECK(profile.base_url == "http://127.0.0.1:1234/v1");
    CHECK(profile.kind == "custom");
    CHECK_FALSE(profile.builtin);
    const Json stored = settings.at("text_llm_profiles");
    CHECK(stored[0].at("id") == profile.id);
    CHECK_FALSE(stored[0].contains("api_key"));

    const auto updated = upsert_custom_profile(settings, "Home Lab", "http://127.0.0.1:8000/v1",
                                               "HOME_LLM_KEY", profile.id);
    CHECK(updated.id == profile.id);
    CHECK(updated.name == "Home Lab");
    CHECK(get_profile(profile.id, settings)->api_key_env == "HOME_LLM_KEY");

    CHECK(remove_custom_profile(settings, profile.id));
    CHECK_FALSE(get_profile(profile.id, settings));
}

TEST_CASE("cannot delete builtin", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_cannot_delete_builtin
    Json settings = Json::object();
    CHECK_THROWS_AS(remove_custom_profile(settings, "openai"), std::invalid_argument);
}

TEST_CASE("responses endpoint persists and edits preserve protocol", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestProfileCrud::test_responses_endpoint_persists_and_edits_preserve_protocol
    Json settings = Json::object();
    const auto profile =
        upsert_custom_profile(settings, "Gateway", "https://gateway.test/v1", "", {}, "responses");
    CHECK(get_profile(profile.id, settings)->protocol == "responses");
    const auto updated =
        upsert_custom_profile(settings, "Renamed", *profile.base_url, "", profile.id);
    CHECK(updated.protocol == "responses");
    // The endpoint-level choice also survives a snapshot without a model.
    const auto saved =
        snapshot_from_mapping(snapshot_from_profile(updated).to_dict())->to_profile();
    CHECK(model_spec(saved, "synthetic-model").protocol == "responses");
    CHECK_THROWS_AS(
        upsert_custom_profile(settings, "Invalid", *profile.base_url, "", {}, "unknown"),
        std::invalid_argument);
}

TEST_CASE("legacy custom endpoints keep chat protocol", "[test_text_llm]") {
    // from tests/test_text_llm.py::TestProfileCrud::test_legacy_custom_endpoints_keep_chat_protocol
    const Json settings = settings_with_custom({{"id", "custom_legacy"},
                                                {"name", "Local"},
                                                {"base_url", "http://localhost:1234/v1"},
                                                {"api_key_env", ""}});
    CHECK(get_profile("custom_legacy", settings)->protocol == "chat");
}

// TestSnapshotsAndConsent

TEST_CASE("old meeting row reconstructs builtin profile", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_old_meeting_row_reconstructs_builtin_profile
    const auto snapshot = snapshot_from_meeting(Json{{"agent_provider", "openai"}}, Json::object());
    CHECK(snapshot.profile_id == "openai");
    CHECK(snapshot.kind == "openai");
    CHECK_FALSE(snapshot.base_url);
}

TEST_CASE("deleted endpoint uses the chosen meeting endpoint not a cloud", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_deleted_endpoint_uses_the_chosen_meeting_endpoint_not_a_cloud
    const Json settings{{"meeting_llm_provider", "ollama"}, {"meeting_llm_model", "llama3.2"}};
    const auto snapshot = snapshot_from_meeting(Json{{"agent_provider", "custom_deleted"}},
                                                settings, "custom_deleted");
    CHECK(snapshot.profile_id == "ollama");
    const auto profile = profile_from_agent_config("custom_deleted", nullptr, settings);
    CHECK(profile.id == "ollama");
    CHECK(profile.is_local());
}

TEST_CASE("stored snapshot wins over current settings", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_stored_snapshot_wins_over_current_settings
    const auto snapshot = snapshot_from_meeting(Json{{"agent_provider", "openai"},
                                                     {"agent_endpoint_json",
                                                      {{"profile_id", "custom_deadbeef"},
                                                       {"name", "Deleted Lab"},
                                                       {"kind", "custom"},
                                                       {"base_url", "http://10.0.0.8:8080/v1"},
                                                       {"api_key_env", "LAB_KEY"}}}},
                                                Json::object());
    CHECK(snapshot.profile_id == "custom_deadbeef");
    CHECK(snapshot.base_url == "http://10.0.0.8:8080/v1");
    const auto profile = snapshot.to_profile();
    CHECK(profile.kind == "custom");
    CHECK_FALSE(profile.builtin);
}

TEST_CASE("profile from agent config prefers endpoint snapshot", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_profile_from_agent_config_prefers_endpoint_snapshot
    const auto profile = profile_from_agent_config("openai",
                                                   Json{{"profile_id", "custom_abcd1234"},
                                                        {"name", "LM Studio"},
                                                        {"kind", "custom"},
                                                        {"base_url", "http://127.0.0.1:1234/v1"},
                                                        {"api_key_env", ""}},
                                                   Json::object());
    CHECK(profile.id == "custom_abcd1234");
    CHECK(profile.is_local());
}

TEST_CASE("consent destination distinguishes local and remote", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_consent_destination_distinguishes_local_and_remote
    Json a = Json::object();
    Json b = Json::object();
    const auto local = upsert_custom_profile(a, "LM Studio", "http://127.0.0.1:1234/v1");
    const auto remote =
        upsert_custom_profile(b, "Work gateway", "https://llm.example.com/v1", "WORK_LLM_KEY");
    CHECK(consent_destination(local).find("127.0.0.1:1234") != std::string::npos);
    CHECK_FALSE(destination_is_remote(local));
    CHECK(consent_destination(remote).find("Work gateway") != std::string::npos);
    CHECK(destination_is_remote(remote));
    CHECK(consent_destination(*get_profile("openrouter", Json::object())) ==
          "OpenRouter (openrouter.ai)");
    CHECK(destination_is_remote(*get_profile("openai", Json::object())));
}

TEST_CASE("profile display name falls back to id", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_profile_display_name_falls_back_to_id
    // (SIDECAR_API_KEY_ENV, the Pi sidecar's variable, arrives with the sidecar's replacement.)
    CHECK(profile_display_name("openai", Json::object()) == "OpenAI");
    CHECK(profile_display_name("missing_profile", Json::object()) == "missing_profile");
}

TEST_CASE("snapshot from unreadable json is none", "[test_text_llm]") {
    // from
    // tests/test_text_llm.py::TestSnapshotsAndConsent::test_snapshot_from_unreadable_json_is_none
    CHECK_FALSE(snapshot_from_mapping("{not json"));
    CHECK_FALSE(snapshot_from_mapping("   "));
    CHECK_FALSE(snapshot_from_mapping("[1, 2]"));
}

TEST_CASE("upserting the 51st endpoint is refused and an edit of a missing one too", "[profiles]") {
    Json settings = Json::object();
    for (int i = 0; i < 50; ++i) {
        upsert_custom_profile(settings, "E" + std::to_string(i), "http://e.test/v1");
    }
    CHECK(list_custom_profiles(settings).size() == 50);
    try {
        upsert_custom_profile(settings, "One more", "http://e.test/v1");
        FAIL("expected a refusal");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()) == "At most 50 custom endpoints can be saved");
    }
    CHECK_THROWS_AS(upsert_custom_profile(settings, "X", "http://x", "", "custom_missing"),
                    std::invalid_argument);
}

TEST_CASE("new custom ids are custom_ and eight hex digits", "[profiles]") {
    const auto id = new_custom_profile_id();
    REQUIRE(id.size() == 15);
    CHECK(id.starts_with("custom_"));
    CHECK(id.substr(7).find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(new_custom_profile_id() != id);
}

TEST_CASE("profiles replay services.text_llm", "[golden]") {
    const std::string module = "services.text_llm";
    golden::for_each(module, "normalize_base_url", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), value_or_error([&] {
                                return Json(normalize_base_url(c.arg<std::string>(0)));
                            })));
    });
    golden::for_each(module, "validate_profile_name", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), value_or_error([&] {
                                return Json(validate_profile_name(c.arg<std::string>(0)));
                            })));
    });
    golden::for_each(module, "validate_api_key_env", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), value_or_error([&] {
                                return Json(validate_api_key_env(c.arg<std::string>(0)));
                            })));
    });
    golden::for_each(module, "validate_text_protocol", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), value_or_error([&] {
                                return Json(validate_text_protocol(c.arg<Json>(0)));
                            })));
    });
    golden::for_each(module, "normalize_local_server_url", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), value_or_error([&] {
                                return Json(normalize_local_server_url(c.arg<std::string>(0)));
                            })));
    });
    golden::for_each(module, "builtin_profiles", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), profiles_json(builtin_profiles())));
    });
    golden::for_each(module, "list_profiles", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), profiles_json(list_profiles(c.arg<Json>(0)))));
    });
    golden::for_each(module, "known_profile_ids", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), Json(known_profile_ids(c.arg<Json>(0)))));
    });
    golden::for_each(module, "parse_custom_profile", [](const golden::Case& c) {
        CHECK(
            golden::equal(c.result<Json>(), profile_fields(parse_custom_profile(c.arg<Json>(0)))));
    });
    golden::for_each(module, "get_profile", [](const golden::Case& c) {
        std::string storage;
        const auto id = optional_id(c.args()[0], storage);
        CHECK(golden::equal(c.result<Json>(), profile_fields(get_profile(id, c.arg<Json>(1)))));
    });
    golden::for_each(module, "upsert_custom_profile", [](const golden::Case& c) {
        Json settings = c.arg<Json>(0);
        const Json kwargs = c.arg<Json>(1);
        std::optional<std::string> profile_id;
        if (kwargs.contains("profile_id")) {
            profile_id = kwargs.at("profile_id").get<std::string>();
        }
        std::optional<std::string> protocol;
        if (kwargs.contains("protocol")) {
            protocol = kwargs.at("protocol").get<std::string>();
        }
        const Json actual = value_or_error([&] {
            const auto profile = upsert_custom_profile(
                settings, kwargs.at("name").get<std::string>(),
                kwargs.at("base_url").get<std::string>(),
                kwargs.value("api_key_env", std::string()),
                profile_id ? std::optional<std::string_view>(*profile_id) : std::nullopt,
                protocol ? std::optional<std::string_view>(*protocol) : std::nullopt);
            const std::string new_id = profile.id != profile_id.value_or("") ? profile.id : "";
            Json fields = profile_fields(profile);
            if (!new_id.empty()) {
                fields["id"] = "<new>";
            }
            return Json{{"profile", fields},
                        {"settings", masked(settings, new_id)},
                        {"new_id_shape",
                         !new_id.empty() && new_id.starts_with("custom_") && new_id.size() == 15}};
        });
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "remove_custom_profile", [](const golden::Case& c) {
        Json settings = c.arg<Json>(0);
        const Json actual = value_or_error([&] {
            const bool removed = remove_custom_profile(settings, c.arg<std::string>(1));
            return Json{{"removed", removed}, {"settings", settings}};
        });
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "save_local_server", [](const golden::Case& c) {
        Json settings = c.arg<Json>(0);
        const Json actual = value_or_error([&] {
            save_local_server(settings, c.arg<std::string>(1), c.arg<std::string>(2),
                              c.arg<std::string>(3));
            return settings;
        });
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "snapshot_from_mapping", [](const golden::Case& c) {
        Json actual;
        try {
            const auto snapshot = snapshot_from_mapping(c.arg<Json>(0));
            actual = snapshot ? Json{{"dict", snapshot->to_dict()},
                                     {"profile", profile_fields(snapshot->to_profile())}}
                              : Json(nullptr);
        } catch (const std::invalid_argument& error) {
            actual = Json{{"raises", "ValueError"}, {"message", error.what()}};
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "snapshot_from_meeting", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), snapshot_from_meeting(c.arg<Json>(0), c.arg<Json>(1),
                                                                    c.arg<std::string>(2))
                                                  .to_dict()));
    });
    golden::for_each(module, "profile_from_agent_config", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            profile_fields(profile_from_agent_config(
                                c.arg<std::string>(0), c.arg<Json>(1), c.arg<Json>(2)))));
    });
    golden::for_each(module, "profile_fields", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), profile_fields(profile_from(c.arg<Json>(0)))));
    });
    golden::for_each(module, "consent_destination", [](const golden::Case& c) {
        CHECK(consent_destination(profile_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(module, "destination_is_remote", [](const golden::Case& c) {
        CHECK(destination_is_remote(profile_from(c.arg<Json>(0))) == c.result<bool>());
    });
    golden::for_each(module, "profile_display_name", [](const golden::Case& c) {
        std::string storage;
        const auto id = optional_id(c.args()[0], storage);
        CHECK(profile_display_name(id, c.arg<Json>(1)) == c.result<std::string>());
    });
    golden::for_each(module, "default_model_for_profile", [](const golden::Case& c) {
        CHECK(default_model_for_profile(profile_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(module, "chat_request_options", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), chat_request_options(profile_from(c.arg<Json>(0)),
                                                                   c.arg<std::string>(1))));
    });
}
