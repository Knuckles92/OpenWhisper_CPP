// What a bundle does to the settings it carries: OpenWhisper's
// tests/test_automation_fix_pipeline_backup.py and the pure test of
// tests/test_personalize_foundation_backup.py.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "backup/detail/action_rows.h"
#include "backup/sanitize.h"

using fv::Json;
using fv::backup::sanitize_settings;
using fv::backup::detail::ActionRow;
using fv::backup::detail::load_action_rows;

namespace {

constexpr const char* kLinkTemplate = "https://api.example.com/x?key=LINK-KEY&q={text}";

Json restored(const Json& rows) {
    return sanitize_settings(Json{{"actions", rows}});
}

std::map<std::string, ActionRow> by_id(const Json& settings) {
    std::map<std::string, ActionRow> actions;
    for (auto& row : load_action_rows(settings.at("actions"))) {
        actions.emplace(row.id, row);
    }
    return actions;
}

} // namespace

TEST_CASE("a kind with stray spaces comes back off like the app reads it",
          "[test_automation_fix_pipeline_backup]") {
    // from
    // tests/test_automation_fix_pipeline_backup.py::test_a_kind_with_stray_spaces_comes_back_off_like_the_app_reads_it
    const Json settings = restored(Json::array({
        Json{{"id", "x1"},
             {"name", "Notes"},
             {"kind", "program "}, // the loader strips it
             {"params",
              {{"program", "C:\\Windows\\System32\\cmd.exe"},
               {"args", Json::array({"/c", "calc"})},
               {"output", "none"},
               {"timeout_s", 30}}},
             {"hotkey", "ctrl+alt+9"},
             {"spoken_names", Json::array({"notes"})},
             {"enabled", true}},
        Json{{"id", "x2"},
             {"name", "Hook"},
             {"kind", " Webhook"},
             {"params", {{"url", "https://h.example"}}},
             {"enabled", true}},
    }));
    const auto actions = by_id(settings);
    REQUIRE(actions.contains("x1"));
    CHECK(actions.at("x1").kind == "program");
    CHECK_FALSE(actions.at("x1").enabled);
    // Saved exactly as the app reads it, so nothing differs on the next load.
    CHECK(settings.at("actions")[0].at("kind") == "program");
    CHECK_FALSE(actions.contains("x2")); // not a kind the app knows: dropped
}

TEST_CASE("webhook program and agent actions come back off",
          "[test_automation_fix_pipeline_backup]") {
    // from
    // tests/test_automation_fix_pipeline_backup.py::test_webhook_program_and_agent_actions_come_back_off
    const Json settings = restored(Json::array({
        Json{{"id", "w"},
             {"name", "W"},
             {"kind", "webhook"},
             {"params", {{"url", "https://h.example/x"}}}},
        Json{{"id", "p"},
             {"name", "P"},
             {"kind", "program"},
             {"params", {{"program", "notes.exe"}}}},
        Json{{"id", "a"}, {"name", "A"}, {"kind", "agent"}, {"params", {{"agent", "claude_code"}}}},
        Json{{"id", "l"},
             {"name", "L"},
             {"kind", "open_link"},
             {"params", {{"template", kLinkTemplate}}}},
        Json{{"id", "c"}, {"name", "C"}, {"kind", "clipboard"}},
    }));
    std::map<std::string, bool> enabled;
    for (const auto& [id, action] : by_id(settings)) {
        enabled[id] = action.enabled;
    }
    CHECK(enabled == std::map<std::string, bool>{
                         {"w", false}, {"p", false}, {"a", false}, {"l", true}, {"c", true}});
}

TEST_CASE("urls inside actions are scrubbed except a link template",
          "[test_automation_fix_pipeline_backup]") {
    // from
    // tests/test_automation_fix_pipeline_backup.py::test_urls_inside_actions_are_scrubbed_except_a_link_template
    const Json settings = restored(Json::array({
        Json{{"id", "w"},
             {"name", "Slack"},
             {"kind", "webhook"},
             {"params",
              {{"url", "https://user:hunter2@hooks.example.com/hook?token=ACTION-SECRET"},
               {"output", "none"}}}},
        Json{{"id", "p"},
             {"name", "P"},
             {"kind", "program"},
             {"params",
              {{"program", "curl.exe"},
               {"args", Json::array({"https://u:pw@example.com/a?sig=SECRET"})}}}},
        Json{{"id", "l"},
             {"name", "Link"},
             {"kind", "open_link"},
             {"params", {{"template", kLinkTemplate}}}},
    }));
    const auto actions = by_id(settings);
    CHECK(actions.at("w").params.at("url") == "https://hooks.example.com/hook");
    CHECK(actions.at("p").params.at("args") == Json::array({"https://example.com/a"}));
    CHECK(actions.at("l").params.at("template") == kLinkTemplate);
    std::string text = settings.at("actions").dump();
    for (auto at = text.find("LINK-KEY"); at != std::string::npos; at = text.find("LINK-KEY")) {
        text.erase(at, 8);
    }
    CHECK(text.find("SECRET") == std::string::npos);
}

TEST_CASE("Restored cleanup settings need fresh approval for the installed agent") {
    const Json cleaned = sanitize_settings(Json{{"transcript_cleanup_provider", "codex"},
                                                {"transcript_cleanup_model", ""},
                                                {"fv_cleanup_agent_consent", "codex"}});
    CHECK(cleaned.at("transcript_cleanup_provider") == "codex");
    CHECK_FALSE(cleaned.contains("fv_cleanup_agent_consent"));
}

TEST_CASE("snippets keep their urls", "[test_automation_fix_pipeline_backup]") {
    // from tests/test_automation_fix_pipeline_backup.py::test_snippets_keep_their_urls
    const Json settings = sanitize_settings(
        Json{{"dictation_snippets", Json::array({Json{{"text", "https://x.example/?a=1"}}})}});
    CHECK(settings.at("dictation_snippets") ==
          Json::array({Json{{"text", "https://x.example/?a=1"}}}));
}

TEST_CASE("snippet text keeps its urls while endpoints are scrubbed",
          "[test_personalize_foundation_backup]") {
    // from
    // tests/test_personalize_foundation_backup.py::test_snippet_text_keeps_its_urls_while_endpoints_are_scrubbed
    const Json snippets = Json::array({Json{
        {"id", "s1"},
        {"trigger", "my calendar"},
        {"text", "Book a time: https://cal.example.com/dana?ref=email#slots"},
        {"formatted", false},
    }});
    const Json settings = Json{
        {"dictation_snippets", snippets},
        {"transcript_cleanup_base_url", "https://user:pw@api.example.com/v1?key=abc"},
    };
    const Json cleaned = sanitize_settings(settings);
    CHECK(cleaned.at("dictation_snippets") == snippets);
    CHECK(cleaned.at("transcript_cleanup_base_url") == "https://api.example.com/v1");
    const Json round_trip = Json::parse(cleaned.dump());
    CHECK(round_trip.at("dictation_snippets")[0].at("text").get<std::string>().ends_with("#slots"));
}
