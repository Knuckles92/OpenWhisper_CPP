// The meeting resolvers of services/settings.py where FioriVoce's identity or the platform
// matters (the rest is in test_golden.cpp).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "intel_support.h"
#include "meeting_intel/intel_settings.h"

using namespace fv::meeting_intel;
using fv::Json;
namespace fs = std::filesystem;

TEST_CASE("the default wake names are FioriVoce's", "[intel_settings]") {
    const std::vector<std::string> expected{"note taker", "notetaker", "assistant",
                                            std::string(fv::identity().wake_name),
                                            std::string(fv::identity().wake_name_spaced)};
    CHECK(resolve_typesafe_voice_command_names(Json::object()) == expected);
    CHECK(resolve_typesafe_voice_command_names(
              Json{{"typesafe_voice_command_names", Json::array()}}) == expected);
    // Names imported from OpenWhisper are kept as stored.
    const std::string legacy(fv::legacy_identity().wake_name);
    CHECK(resolve_typesafe_voice_command_names(
              Json{{"typesafe_voice_command_names", Json::array({legacy, "  Note  Taker "})}}) ==
          std::vector<std::string>{legacy, "note taker"});
}

TEST_CASE("an unknown Fast judgments feature is refused", "[intel_settings]") {
    CHECK_THROWS_AS(resolve_typesafe_feature_enabled(Json{{"typesafe_enabled", true}}, "recap"),
                    std::out_of_range);
}

TEST_CASE("the review is enabled only with its consent and the master switch", "[intel_settings]") {
    Json settings{{"meeting_insight_review", true},
                  {"meeting_insight_review_consent", std::string(kInsightReviewConsent)},
                  {"typesafe_enabled", true}};
    CHECK(resolve_meeting_insight_review(settings) ==
          Json{{"enabled", true}, {"consent", "typesafe-text-v1"}, {"sensitivity", "normal"}});
    settings["typesafe_enabled"] = false;
    CHECK(resolve_meeting_insight_review(settings).at("enabled") == false);
}

TEST_CASE("the knowledge folder path is absolute and normalized", "[intel_settings]") {
    fv::MapEnvReader env;
#if defined(_WIN32)
    env.set("USERPROFILE", "C:\\Users\\Pat");
    CHECK(resolve_meeting_context_folder_path(
              Json{{"meeting_context_folder_path", " ~/Vault/./Notes/.. "}}, env) ==
          "C:\\Users\\Pat\\Vault");
#else
    env.set("HOME", "/home/pat/");
    CHECK(resolve_meeting_context_folder_path(
              Json{{"meeting_context_folder_path", " ~/Vault/./Notes/.. "}}, env) ==
          "/home/pat/Vault");
    CHECK(resolve_meeting_context_folder_path(Json{{"meeting_context_folder_path", "//a//b/"}},
                                              env) == "//a/b");
    CHECK(resolve_meeting_context_folder_path(Json{{"meeting_context_folder_path", "/../x"}},
                                              env) == "/x");
#endif
    CHECK(resolve_meeting_context_folder_path(Json{{"meeting_context_folder_path", "   "}}, env)
              .empty());
    const std::string relative =
        resolve_meeting_context_folder_path(Json{{"meeting_context_folder_path", "vault"}}, env);
    CHECK(fs::path(relative) == (fs::current_path() / "vault"));
}
