#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include "automation/action_model.h"
#include "automation/hooks_model.h"
#include "automation/permissions.h"
#include "pyport/errors.h"
#include "settings/keys.h"

// from tests/test_automation_foundation_models.py

using namespace fv::automation;
using namespace fv::automation::hooks;
using fv::Json;
namespace py = fv::py;

namespace {

// An absolute program path on this platform (Python's sys.executable).
#ifdef _WIN32
const std::string kProgram = "C:\\Python312\\python.exe";
const std::string kRoot = "C:\\";
#else
const std::string kProgram = "/usr/bin/python3";
const std::string kRoot = "/";
#endif

const std::vector<std::string> kSaved = {"history.saved"};

// pytest.raises(ValueError, match=...): the message contains `part`.
template <class Fn> void requires_message(Fn&& fn, const std::string& part) {
    try {
        fn();
        FAIL("no ValueError for \"" << part << "\"");
    } catch (const py::ValueError& error) {
        INFO(error.what());
        CHECK(std::string(error.what()).find(part) != std::string::npos);
    }
}

Action link_action(std::string id = "a1", std::string name = "Ask Claude") {
    Action action;
    action.id = std::move(id);
    action.name = std::move(name);
    action.kind = ActionKind::OPEN_LINK;
    action.params = Json{{"template", "claude://claude.ai/new?q={text}"}};
    return action;
}

Action of_kind(std::string_view kind, Json params, std::string id = "x", std::string name = "X") {
    Action action;
    action.id = std::move(id);
    action.name = std::move(name);
    action.kind = std::string(kind);
    action.params = std::move(params);
    return action;
}

} // namespace

// -- hooks --------------------------------------------------------------------------------------

TEST_CASE("test_webhooks_save_load_and_validate", "[test_automation_foundation_models]") {
    Json settings = Json::object();
    save_webhook(settings, {"h1", "n8n", "http://localhost:5678/hook", {"history.saved", "bogus"}});
    const auto loaded = load_webhooks(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].events == kSaved);
    CHECK(loaded[0].enabled);
    requires_message([&] { save_webhook(settings, {"h2", "N8N", "https://a.example", kSaved}); },
                     "name already exists");
    requires_message([&] { save_webhook(settings, {"h2", "x", "ftp://a.example", kSaved}); },
                     "http");
    requires_message([&] { save_webhook(settings, {"h2", "x", "https://a.example", {}}); },
                     "event");
    delete_webhook(settings, "h1");
    CHECK(load_webhooks(settings).empty());
}

TEST_CASE("test_webhook_limit", "[test_automation_foundation_models]") {
    Json settings = Json::object();
    for (std::size_t n = 0; n < MAX_WEBHOOKS; ++n) {
        save_webhook(settings, {"h" + std::to_string(n), "hook " + std::to_string(n),
                                "https://a.example", kSaved});
    }
    requires_message(
        [&] { save_webhook(settings, {"extra", "extra", "https://a.example", kSaved}); }, "up to");
    // Editing an existing one is still fine at the limit.
    CHECK_NOTHROW(save_webhook(settings, {"h0", "renamed", "https://a.example", kSaved}));
}

TEST_CASE("test_programs_need_a_full_path", "[test_automation_foundation_models]") {
    Json empty = Json::object();
    requires_message([&] { save_program(empty, {"p", "Script", "python", {}, kSaved}); },
                     "full path");
    Json settings = Json::object();
    save_program(settings, {"p", "Script", kProgram, {"x.py"}, kSaved, true, 10});
    const auto programs = load_programs(settings);
    REQUIRE(programs.size() == 1);
    CHECK(programs[0].args == std::vector<std::string>{"x.py"});
    CHECK(programs[0].timeout_s == 10);
}

TEST_CASE("test_filters_by_kind", "[test_automation_foundation_models]") {
    Json settings = Json::object();
    TextFilter http{"f1", "Fix names", std::string(FILTER_HTTP), std::string(AFTER_CLEANUP),
                    "http://127.0.0.1:9000/filter"};
    save_filter(settings, http);
    TextFilter slow{"f2", "Slow", std::string(FILTER_HTTP), std::string(BEFORE_CLEANUP),
                    "http://127.0.0.1:1"};
    slow.timeout_ms = 9000;
    requires_message([&] { save_filter(settings, slow); }, "time limit");
    TextFilter program{"f3", "Local", std::string(FILTER_PROGRAM), std::string(BEFORE_CLEANUP),
                       "",   kProgram};
    save_filter(settings, program);
    std::vector<std::string> ids;
    for (const auto& text_filter : load_filters(settings)) {
        ids.push_back(text_filter.id);
    }
    CHECK(ids == std::vector<std::string>{"f1", "f3"});
}

TEST_CASE("test_malformed_rows_are_skipped", "[test_automation_foundation_models]") {
    const Json settings = {
        {"automation_filters",
         Json::array({
             {{"id", "a"},
              {"name", "A"},
              {"kind", "shell"},
              {"stage", "after_cleanup"},
              {"url", "x"}},
             {{"id", "b"}, {"name", "B"}, {"kind", "http"}, {"stage", "later"}, {"url", "x"}},
             {{"id", "c"}, {"name", "C"}, {"kind", "http"}, {"stage", "after_cleanup"}},
             "junk",
             {{"id", "d"},
              {"name", "D"},
              {"kind", "http"},
              {"stage", "after_cleanup"},
              {"url", "http://x"},
              {"timeout_ms", 99999},
              {"enabled", "yes"}},
         })}};
    const auto loaded = load_filters(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].id == "d");
    CHECK(loaded[0].timeout_ms == FILTER_TIMEOUT_MAX_MS);
    CHECK(loaded[0].enabled);
}

TEST_CASE("test_private_urls", "[test_automation_foundation_models]") {
    const auto [url, is_private] = GENERATE(table<std::string, bool>({
        {"http://localhost:1/x", true},
        {"http://127.0.0.1/x", true},
        {"http://192.168.1.20/x", true},
        {"http://homeassistant.local:8123", true},
        {"https://hooks.example.com/x", false},
        {"http://8.8.8.8", false},
    }));
    CAPTURE(url);
    CHECK(url_is_private(url) == is_private);
}

// Python 3.12.10's urlsplit refuses a non-ASCII netloc that NFKC normalization gives a
// separator (_checknetloc), so such an address is invalid and never private; other non-ASCII
// hosts and a separator lookalike after the netloc are fine (checked with Python's urlsplit).
TEST_CASE("an address whose host gains a separator under NFKC is refused", "[models]") {
    // U+FF0F FULLWIDTH SOLIDUS is "/" under NFKC.
    CHECK_FALSE(url_is_private("http://a\xEF\xBC\x8F"
                               "b.lan/x"));
    // U+FF20 FULLWIDTH COMMERCIAL AT is "@".
    CHECK_FALSE(url_is_private("http://user\xEF\xBC\xA0host.local/"));
    // U+2100 ACCOUNT OF is "a/c".
    CHECK_FALSE(url_is_private("http://\xE2\x84\x80.lan"));
    requires_message([] { validate_url("https://hooks\xEF\xBC\x83.example.com/x"); },
                     "That address isn't valid.");
    CHECK(url_is_private("http://caf\xC3\xA9.lan/x"));
    CHECK(url_is_private("http://a.lan/\xEF\xBC\x8F"));
    CHECK_NOTHROW(validate_url("https://caf\xC3\xA9.example/x"));
}

// -- actions ------------------------------------------------------------------------------------

TEST_CASE("test_actions_round_trip_and_drop_unknown_params",
          "[test_automation_foundation_models]") {
    Action action = link_action();
    action.params = Json{{"template", "claude://claude.ai/new?q={text}"}, {"evil", "x"}};
    action.spoken_names = {"Claude", "claude", " ask   Claude "};
    Json settings = Json::object();
    save_action(settings, action);
    const auto loaded = load_actions(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].params == Json{{"template", "claude://claude.ai/new?q={text}"}});
    CHECK(loaded[0].spoken_names == std::vector<std::string>{"Claude", "ask Claude"});
    CHECK(find_action(settings, "a1") == loaded[0]);
}

TEST_CASE("test_public_dict_hides_urls_paths_and_folders", "[test_automation_foundation_models]") {
    const Action agent =
        of_kind(ActionKind::AGENT, Json{{"agent", "codex"}, {"folder", kRoot}}, "ag", "Agent");
    const Json data = agent.public_dict();
    CHECK_FALSE(data.contains("params"));
    CHECK(data["agent"] == "codex");
    CHECK_FALSE(data.contains("folder"));
    const Action hook =
        of_kind(ActionKind::WEBHOOK, Json{{"url", "https://x.example/secret"}}, "w", "Hook");
    CHECK(hook.public_dict().dump().find("secret") == std::string::npos);
}

TEST_CASE("test_new_folder_choice_is_saved_and_old_actions_keep_their_folder",
          "[test_automation_foundation_models]") {
    Json settings = Json::object();
    save_action(settings, of_kind(ActionKind::AGENT, Json{{"agent", "codex"}, {"folder", kRoot}},
                                  "ag", "Agent"));
    auto loaded = load_actions(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].param("folder") == kRoot);
    CHECK(loaded[0].param("new_folder") == false);
    save_action(settings, of_kind(ActionKind::AGENT,
                                  Json{{"agent", "codex"}, {"folder", kRoot}, {"new_folder", true}},
                                  "ag", "Agent"));
    loaded = load_actions(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].param("folder") == kRoot);
    CHECK(loaded[0].param("new_folder") == true);
}

TEST_CASE("test_links_are_percent_encoded_and_dangerous_schemes_refused",
          "[test_automation_foundation_models]") {
    CHECK(expand_link("claude://claude.ai/new?q={text}", "a & b?/c") ==
          "claude://claude.ai/new?q=a%20%26%20b%3F%2Fc");
    for (const char* template_text : {"file:///etc/passwd", "javascript:alert(1)",
                                      "DATA:text/html,x", "no scheme {text}", ""}) {
        CAPTURE(template_text);
        CHECK_THROWS_AS(expand_link(template_text, "x"), py::ValueError);
    }
    Action file_link = link_action();
    file_link.params = Json{{"template", "file:///{text}"}};
    Json settings = Json::object();
    CHECK_THROWS_AS(save_action(settings, file_link), py::ValueError);
}

TEST_CASE("test_spoken_names_must_be_unique_across_actions",
          "[test_automation_foundation_models]") {
    Json settings = Json::object();
    Action first = link_action();
    first.spoken_names = {"Claude"};
    save_action(settings, first);
    Action other = link_action("a2", "Other");
    other.spoken_names = {"claude"};
    requires_message([&] { save_action(settings, other); }, "already names Ask Claude");
    requires_message([&] { save_action(settings, link_action("a2", "Claude")); },
                     "already names Ask Claude");
}

TEST_CASE("test_action_hotkeys_conflict_with_other_bindings",
          "[test_automation_foundation_models]") {
    Json settings = {
        {"text_transforms",
         Json::array(
             {{{"id", "t"}, {"name", "Polish"}, {"instruction", "x"}, {"hotkey", "ctrl+alt+p"}}})}};
    Action taken = link_action();
    taken.hotkey = "ctrl+alt+p";
    requires_message([&] { save_action(settings, taken); }, "Polish");
    Action free = link_action();
    free.hotkey = "ctrl+alt+k";
    save_action(settings, free);
    CHECK(action_hotkey_conflict("ctrl+alt+k", settings) == "Ask Claude");
    CHECK(action_hotkey_conflict("ctrl+alt+k", settings, "a1").empty());
}

TEST_CASE("test_shaping_must_point_at_something_that_exists",
          "[test_automation_foundation_models]") {
    Action profile = link_action();
    profile.shaping = "profile:nope";
    Json empty = Json::object();
    requires_message([&] { save_action(empty, profile); }, "profile");
    Action transform = link_action();
    transform.shaping = "transform:nope";
    Json no_transforms = {{"text_transforms", Json::array()}};
    requires_message([&] { save_action(no_transforms, transform); }, "transform");
    Action polish = link_action();
    polish.shaping = "transform:polish";
    Json settings = Json::object();
    save_action(settings, polish);
    const auto loaded = load_actions(settings);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].transform_id() == "polish");
    CHECK(loaded[0].profile_id().empty());
}

TEST_CASE("test_kind_parameters_are_checked", "[test_automation_foundation_models]") {
    const auto [kind, params, message] = GENERATE(table<std::string, Json, std::string>({
        {"agent", Json{{"agent", "gpt"}}, "Claude Code"},
        {"agent", Json{{"folder", "relative/dir"}}, "full path"},
        {"webhook", Json{{"url", "nope"}}, "address"},
        {"program", Json{{"program", "python"}}, "full path"},
    }));
    CAPTURE(kind, message);
    Json settings = Json::object();
    requires_message([&] { save_action(settings, of_kind(kind, params)); }, message);
}

TEST_CASE("test_presets_validate_once_filled_in", "[test_automation_foundation_models]") {
    for (const Preset& preset : PRESETS()) {
        CAPTURE(preset.id);
        Json params = preset.params;
        if (preset.kind == ActionKind::WEBHOOK) {
            params["url"] = "https://hooks.example.com/x";
        }
        if (preset.kind == ActionKind::PROGRAM) {
            params["program"] = kProgram;
        }
        Action action = of_kind(preset.kind, params, preset.id, preset.name);
        action.shaping = preset.shaping;
        CHECK_NOTHROW(validate_action(action, Json::object()));
    }
}

// -- permissions --------------------------------------------------------------------------------

TEST_CASE("test_permissions_default_and_ignore_malformed_values",
          "[test_automation_foundation_models]") {
    namespace perms = fv::automation::permissions;
    CHECK(perms::granted(Json::object(), perms::LIVE_EVENTS));
    for (const auto key : {perms::RUN_ACTIONS, perms::CONTROL_RECORDING, perms::TEXT_FILTERS}) {
        CAPTURE(key);
        CHECK_FALSE(perms::granted(Json::object(), key));
        CHECK_FALSE(perms::granted(Json{{std::string(key), "true"}}, key));
        CHECK(perms::granted(Json{{std::string(key), true}}, key));
        CHECK_THROWS_AS(perms::require(Json::object(), key), perms::PermissionDenied);
    }
    CHECK_FALSE(perms::granted(Json::object(), "mcp_enabled"));
    std::vector<std::string> names;
    const auto permissions = perms::describe(Json::object());
    for (const auto& [name, value] : permissions.items()) {
        names.push_back(name);
    }
    CHECK(names == std::vector<std::string>{"live_events", "run_actions", "control_recording",
                                            "text_filters", "agent_prompts"});
    CHECK(fv::keys::api_live_events.default_value() == true);
}

// -- what the Python tests reach only through the vectors -----------------------------------------

TEST_CASE("a refused permission says what to turn on in this product's Settings", "[permissions]") {
    namespace perms = fv::automation::permissions;
    try {
        perms::require(Json::object(), perms::Permission::RunActions);
        FAIL("granted");
    } catch (const perms::PermissionDenied& denied) {
        CHECK(denied.permission() == "api_run_actions");
        CHECK(denied.message() == "Allow running actions in FioriVoce Settings > API & MCP.");
    }
    try {
        perms::require(Json::object(), "nope");
        FAIL("granted");
    } catch (const perms::PermissionDenied& denied) {
        CHECK(denied.message() == "This action is not allowed.");
    }
}

TEST_CASE("secret names carry this product's variable prefix", "[models]") {
    CHECK(action_secret_name("a1") == "FIORIVOCE_ACTION_SECRET_a1");
    CHECK(webhook_secret_name("h1") == "FIORIVOCE_WEBHOOK_SECRET_h1");
    CHECK(fv::automation::new_id().size() == 12);
    CHECK(fv::automation::hooks::new_id().size() == 12);
}

TEST_CASE("the pipeline's view of an action keeps its name, kind and parameters", "[models]") {
    Action agent = of_kind(ActionKind::AGENT, Json{{"agent", "codex"}, {"include_selection", true}},
                           "ag", "Agent");
    const auto recording = to_recording_action(agent);
    CHECK(recording.name == "Agent");
    CHECK(recording.kind == "agent");
    CHECK(recording.param("agent") == "codex");
    CHECK(fv::dictation::reads_selection(recording));
    CHECK(fv::dictation::recording_destination(&recording) == "Agent \xC2\xB7 Codex");
    // What the transcription runtime routes and shapes by (M5.14's RecordingAction fields).
    CHECK(recording.id == "ag");
    CHECK(recording.shaping == "cleanup");
    CHECK(recording.enabled);
}

TEST_CASE("the pipeline's view of an action keeps its id, shaping and enabled state", "[models]") {
    Action raw = of_kind(ActionKind::SCRIPTS, Json::object(), "a2", "Raw notes");
    raw.shaping = "raw";
    raw.enabled = false;
    const auto off = to_recording_action(raw);
    CHECK(off.id == "a2");
    CHECK(off.shaping == "raw");
    CHECK_FALSE(off.enabled);

    Action formal = of_kind(ActionKind::SCRIPTS, Json::object(), "a4", "Formal note");
    formal.shaping = "profile:email";
    CHECK(to_recording_action(formal).profile_id() == "email");
    formal.shaping = "transform:polish";
    CHECK(to_recording_action(formal).transform_id() == "polish");
}
