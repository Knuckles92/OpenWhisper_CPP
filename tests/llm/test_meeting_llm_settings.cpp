// services/settings.py's meeting and cleanup text-model resolvers over custom endpoint profiles
// (tests/test_settings.py::TestMeetingSettings).

#include <catch2/catch_test_macros.hpp>

#include "llm/cleanup_settings.h"
#include "llm/profiles.h"

using namespace fv;
using namespace fv::llm;

namespace {

Json lm_studio_profiles() {
    return Json::array({{{"id", "custom_abcd1234"},
                         {"name", "LM Studio"},
                         {"base_url", "http://127.0.0.1:1234/v1"},
                         {"api_key_env", ""}}});
}

} // namespace

TEST_CASE("a saved custom profile id is accepted for meetings and cleanup", "[test_settings]") {
    // from tests/test_settings.py::TestMeetingSettings::test_custom_profile_id_is_accepted
    const Json settings{{"text_llm_profiles", lm_studio_profiles()},
                        {"meeting_llm_provider", "custom_abcd1234"},
                        {"meeting_llm_model", "local-qwen"},
                        {"transcript_cleanup_provider", "custom_abcd1234"},
                        {"transcript_cleanup_model", "local-qwen"}};
    CHECK(resolve_meeting_llm_provider(settings) == "custom_abcd1234");
    CHECK(resolve_transcript_cleanup_provider(settings) == "custom_abcd1234");
}

TEST_CASE("an unknown custom profile falls back to the default endpoint", "[test_settings]") {
    // from tests/test_settings.py::TestMeetingSettings::test_unknown_custom_profile_falls_back
    const Json settings{{"meeting_llm_provider", "custom_missing"},
                        {"transcript_cleanup_provider", "custom_missing"}};
    CHECK(resolve_meeting_llm_provider(settings) == "openrouter");
    CHECK(resolve_transcript_cleanup_provider(settings) == kTranscriptCleanupProvider);
}

TEST_CASE("a text model assignment keeps its model only with a known provider", "[test_settings]") {
    // from tests/test_settings.py::TestMeetingSettings::test_text_llm_assignment_is_a_pair
    const Json leftover{{"transcript_cleanup_provider", "custom_abcd1234"},
                        {"transcript_cleanup_model", "other-local"},
                        {"meeting_llm_provider", "custom_abcd1234"},
                        {"meeting_llm_model", "other-local"}};
    CHECK(resolve_transcript_cleanup_provider(leftover) == "openrouter");
    CHECK(resolve_transcript_cleanup_model(leftover) == "openrouter/free");
    CHECK(resolve_meeting_llm_provider(leftover) == "openrouter");
    CHECK(resolve_meeting_llm_model(leftover) == "openrouter/free");

    const Json custom{{"text_llm_profiles", lm_studio_profiles()},
                      {"transcript_cleanup_provider", "custom_abcd1234"},
                      {"transcript_cleanup_model", "local-qwen"},
                      {"meeting_llm_provider", "custom_abcd1234"},
                      {"meeting_llm_model", "local-qwen"}};
    CHECK(resolve_transcript_cleanup_provider(custom) == "custom_abcd1234");
    CHECK(resolve_transcript_cleanup_model(custom) == "local-qwen");
    CHECK(resolve_meeting_llm_provider(custom) == "custom_abcd1234");
    CHECK(resolve_meeting_llm_model(custom) == "local-qwen");

    const Json openai{{"transcript_cleanup_provider", "openai"},
                      {"transcript_cleanup_model", "gpt-4o-mini"},
                      {"meeting_llm_provider", "openai"},
                      {"meeting_llm_model", "gpt-4o-mini"}};
    CHECK(resolve_transcript_cleanup_provider(openai) == "openai");
    CHECK(resolve_transcript_cleanup_model(openai) == "gpt-6-luna");
    CHECK(resolve_meeting_llm_provider(openai) == "openai");
    CHECK(resolve_meeting_llm_model(openai) == "gpt-4o-mini");
}
