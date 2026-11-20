// Ports tests/test_personalize_s1_catalog.py and the catalogue part of
// tests/test_personalize_fix2_g1_paste_target_tab.py, and replays the
// services.focus_context.catalog golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <tuple>

#include "focus/catalog.h"
#include "focus_support.h"
#include "golden/golden.h"
#include "pyport/text.h"

using namespace fv;
using namespace fv::focus;
using fv::focus::test::identity_from;

namespace {

AppIdentity app(std::string app_id, std::string name = "", std::string title_hint = "") {
    return AppIdentity{
        .app_id = std::move(app_id), .name = std::move(name), .title_hint = std::move(title_hint)};
}

AppIdentity named(const std::string& app_id, const std::string& title_hint = "") {
    return app(app_id, catalog::app_name(app_id), title_hint);
}

std::vector<catalog::Override> overrides(const Json& raw) {
    return catalog::parse_overrides(raw);
}

} // namespace

// from tests/test_personalize_s1_catalog.py::test_constants_match_app_styles
TEST_CASE("constants match app styles", "[test_personalize_s1_catalog]") {
    // The app styles' AppCategory.ALL and Surface values (services/app_styles.py).
    const std::vector<std::string_view> categories(std::begin(catalog::CATEGORIES),
                                                   std::end(catalog::CATEGORIES));
    CHECK(categories == std::vector<std::string_view>{"email", "work", "personal", "other"});
    CHECK(std::tuple(catalog::TEXT, catalog::CODE, catalog::TERMINAL) ==
          std::tuple(std::string_view("text"), std::string_view("code"),
                     std::string_view("terminal")));
}

// from tests/test_personalize_s1_catalog.py::test_known_apps
TEST_CASE("known apps", "[test_personalize_s1_catalog]") {
    const auto [app_id, name, category, surface] =
        GENERATE(table<std::string, std::string, std::string, std::string>({
            {"OUTLOOK.EXE", "Outlook", "email", "text"},
            {"olk.exe", "Outlook", "email", "text"},
            {"thunderbird", "Thunderbird", "email", "text"},
            {"com.apple.mail", "Mail", "email", "text"},
            {"slack.exe", "Slack", "work", "text"},
            {"ms-teams.exe", "Teams", "work", "text"},
            {"com.tinyspeck.slackmacgap", "Slack", "work", "text"},
            {"WhatsApp.exe", "WhatsApp", "personal", "text"},
            {"com.apple.MobileSMS", "Messages", "personal", "text"},
            {"discord", "Discord", "personal", "text"},
            {"Code.exe", "VS Code", "other", "code"},
            {"pycharm64.exe", "PyCharm", "other", "code"},
            {"devenv.exe", "Visual Studio", "other", "code"},
            {"WindowsTerminal.exe", "Windows Terminal", "other", "terminal"},
            {"conhost.exe", "Console", "other", "terminal"},
            {"OpenConsole.exe", "Windows Terminal", "other", "terminal"},
            {"pwsh.exe", "PowerShell", "other", "terminal"},
            {"com.googlecode.iterm2", "iTerm2", "other", "terminal"},
            {"winword.exe", "Word", "other", "text"},
        }));
    CAPTURE(app_id);
    const auto kind = catalog::classify(app(py::lower(app_id), catalog::app_name(app_id)));
    REQUIRE(kind);
    CHECK(std::tuple(kind->name, kind->category, kind->surface) ==
          std::tuple(name, category, surface));
}

// from tests/test_personalize_s1_catalog.py::test_hyprland_terminal_classes_are_terminals
TEST_CASE("hyprland terminal classes are terminals", "[test_personalize_s1_catalog]") {
    const std::string app_class = GENERATE(as<std::string>{}, "foot", "kitty", "alacritty",
                                           "com.mitchellh.ghostty", "org.wezfurlong.wezterm");
    CAPTURE(app_class);
    CHECK(catalog::is_terminal(app(app_class, app_class)));
}

// from tests/test_personalize_s1_catalog.py::test_unknown_apps_are_other_text_with_a_tidy_name
TEST_CASE("unknown apps are other text with a tidy name", "[test_personalize_s1_catalog]") {
    const auto kind = catalog::classify(app("sumatra_pdf.exe", ""));
    CHECK(kind == catalog::AppKind{"other", "text", "Sumatra Pdf"});
    CHECK(catalog::pretty_name("com.example.FooBar") == "FooBar");
    CHECK(catalog::pretty_name("my-tool") == "My Tool");
    CHECK(catalog::app_name("whatever.exe", "Whatever Studio") == "Whatever Studio");
    CHECK_FALSE(catalog::classify(std::nullopt));
    CHECK_FALSE(catalog::classify(app("", "")));
}

// from tests/test_personalize_s1_catalog.py::test_sites_from_browser_titles
TEST_CASE("sites from browser titles", "[test_personalize_s1_catalog]") {
    const auto [title, site] = GENERATE(table<std::string, std::string>({
        {"Inbox (3) - dylan@example.com - Gmail - Google Chrome", "Gmail"},
        {"Inbox (3,021) - dylan@example.com - Gmail", "Gmail"},
        // Edge writes a zero-width space into its own name, and a profile.
        {"Mail - Dylan - Outlook - Personal - Microsoft\u200b Edge", "Outlook"},
        {"Inbox - Gmail and 3 more pages - Work - Microsoft\u200b Edge", "Gmail"},
        {"general (Channel) - Acme - Slack — Mozilla Firefox", "Slack"},
        {"Chat | Microsoft Teams - Google Chrome", "Teams"},
        {"Discord | #general | Friends - Brave", "Discord"},
        {"(2) WhatsApp - Google Chrome", "WhatsApp"},
        {"Messenger | Facebook - Opera", "Messenger"},
        {"Telegram Web - Google Chrome", "Telegram"},
        {"(5) Messaging | LinkedIn - Google Chrome", "LinkedIn"},
        {"Roadmap notes - Google Docs - Google Chrome", "Google Docs"},
        {"Notes about Slack - Google Docs", "Google Docs"},
        {"[ABC-12] Fix login - Jira - Google Chrome", "Jira"},
        {"Pull requests · acme/app · GitHub - Arc", "GitHub"},
        {"Weather in Paris - Google Search - Google Chrome", ""},
        {"", ""},
    }));
    CAPTURE(title);
    CHECK(catalog::site_from_title(title) == site);
}

// from tests/test_personalize_s1_catalog.py::test_title_hints_come_only_from_browsers
TEST_CASE("title hints come only from browsers", "[test_personalize_s1_catalog]") {
    const std::string title = "Inbox - Gmail - Google Chrome";
    CHECK(catalog::title_hint("chrome.exe", title) == "Gmail");
    CHECK(catalog::title_hint("msedge.exe", "Chat | Microsoft Teams - Microsoft\u200b Edge") ==
          "Teams");
    CHECK(catalog::title_hint("code.exe", "github - repo - Visual Studio Code").empty());
    CHECK(catalog::title_hint("winword.exe", title).empty());
}

// from tests/test_personalize_s1_catalog.py::test_a_site_in_a_browser_takes_the_site_category
TEST_CASE("a site in a browser takes the site category", "[test_personalize_s1_catalog]") {
    CHECK(catalog::classify(app("chrome.exe", "Chrome", "Gmail")) ==
          catalog::AppKind{"email", "text", "Gmail"});
    CHECK(catalog::classify(app("chrome.exe", "Chrome")) ==
          catalog::AppKind{"other", "text", "Chrome"});
}

// from
// tests/test_personalize_s1_catalog.py::test_overrides_win_and_match_any_name_case_insensitively
TEST_CASE("overrides win and match any name case insensitively", "[test_personalize_s1_catalog]") {
    const auto chosen = overrides(Json::parse(R"([
        {"match": "notion", "category": "work"},
        {"match": "WINWORD", "category": "email"},
        {"match": "Gmail", "category": "personal"}])"));
    CHECK(catalog::classify(named("notion.exe"), chosen)->category == "work");
    CHECK(catalog::classify(named("winword.exe"), chosen)->category == "email");
    CHECK(catalog::classify(app("chrome.exe", "Chrome", "Gmail"), chosen)->category == "personal");
}

// from tests/test_personalize_s1_catalog.py::test_a_built_in_site_beats_a_choice_for_its_browser
TEST_CASE("a built in site beats a choice for its browser", "[test_personalize_s1_catalog]") {
    const auto chosen = overrides(Json::parse(R"([{"match": "chrome", "category": "work"}])"));
    CHECK(catalog::classify(app("chrome.exe", "Chrome"), chosen)->category == "work");
    CHECK(catalog::classify(app("chrome.exe", "Chrome", "Gmail"), chosen)->category == "email");
}

// from tests/test_personalize_s1_catalog.py::test_parse_overrides_drops_bad_and_repeated_rows
TEST_CASE("parse overrides drops bad and repeated rows", "[test_personalize_s1_catalog]") {
    Json raw = Json::parse(R"([
        {"match": " Slack ", "category": "personal"},
        {"match": "slack", "category": "email"},
        {"match": "", "category": "work"},
        {"match": "Zoom", "category": "loud"},
        {"match": 3, "category": "work"},
        "Teams"])");
    raw.push_back({{"match", std::string(500, 'x')}, {"category", "other"}});
    const auto pairs = catalog::parse_overrides(raw);
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[0] == catalog::Override{"Slack", "personal"});
    CHECK(py::len(pairs[1].first) == catalog::MAX_MATCH_CHARS);
    CHECK(catalog::parse_overrides(Json("nope")).empty());
    Json many = Json::array();
    for (int index = 0; index < 300; ++index) {
        many.push_back({{"match", "app" + std::to_string(index)}, {"category", "work"}});
    }
    CHECK(catalog::parse_overrides(many).size() == catalog::MAX_OVERRIDES);
}

// from tests/test_personalize_s1_catalog.py::test_matches_uses_id_stem_name_and_site
TEST_CASE("matches uses id stem name and site", "[test_personalize_s1_catalog]") {
    const auto identity = app("slack.exe", "Slack", "");
    CHECK(catalog::matches(identity, "SLACK.EXE"));
    CHECK(catalog::matches(identity, "slack"));
    CHECK_FALSE(catalog::matches(identity, "teams"));
    CHECK_FALSE(catalog::matches(std::nullopt, "slack"));
    CHECK_FALSE(catalog::matches(identity, "  "));
    const auto site = app("firefox.exe", "Firefox", "Gmail");
    CHECK(catalog::matches(site, "gmail"));
    CHECK(catalog::matches(site, "Firefox"));
}

// from
// tests/test_personalize_s1_catalog.py::test_known_names_and_featured_examples_are_in_the_catalogue
TEST_CASE("known names and featured examples are in the catalogue",
          "[test_personalize_s1_catalog]") {
    const auto names = catalog::known_names();
    auto sorted = names;
    std::ranges::stable_sort(sorted, {},
                             [](const std::string& name) { return py::casefold(name); });
    CHECK(names == sorted);
    CHECK(std::ranges::adjacent_find(names) == names.end());
    for (const auto& [category, examples] : catalog::featured()) {
        for (const auto& name : examples) {
            CAPTURE(category, name);
            CHECK(std::ranges::find(names, name) != names.end());
            CHECK(catalog::builtin_category(name) == category);
        }
    }
    CHECK_FALSE(catalog::builtin_category("Unknown thing"));
}

// from tests/test_personalize_fix2_g1_paste_target_tab.py::test_the_titles_name_the_sites
TEST_CASE("the titles name the sites", "[test_personalize_fix2_g1_paste_target_tab]") {
    const auto chrome = [](const std::string& title) {
        return AppIdentity{.app_id = "chrome.exe",
                           .name = catalog::app_name("chrome.exe"),
                           .pid = 4242,
                           .window = "0x1a2b",
                           .title_hint = catalog::title_hint("chrome.exe", title),
                           .platform = "windows"};
    };
    const auto gmail = chrome("Inbox (12) - someone@gmail.com - Gmail - Google Chrome");
    const auto compose = chrome("Compose Mail - someone@gmail.com - Gmail - Google Chrome");
    const auto whatsapp = chrome("(3) WhatsApp - Google Chrome");
    const auto unknown = chrome("Some recipe blog - Google Chrome");
    CHECK(std::tuple(gmail.title_hint, compose.title_hint, whatsapp.title_hint) ==
          std::tuple(std::string("Gmail"), std::string("Gmail"), std::string("WhatsApp")));
    CHECK(unknown.title_hint.empty());
}

TEST_CASE("the catalogue matches the generated tables", "[test_personalize_s1_catalog]") {
    CHECK(catalog::apps().size() > 100);
    CHECK(catalog::sites().size() == 32);
    const auto* chrome = catalog::lookup(" Chrome.EXE ");
    REQUIRE(chrome != nullptr);
    CHECK(chrome->browser);
    CHECK(catalog::site("  OUTLOOK web app ")->name == "Outlook");
    CHECK(catalog::surface_for(app("notepad.exe")) == catalog::TEXT);
    CHECK(catalog::surface_for(std::nullopt) == catalog::TEXT);
    CHECK_FALSE(catalog::is_terminal(std::nullopt));
}

// --- golden vectors ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kModule = "services.focus_context.catalog";

Json kind_json(const std::optional<catalog::AppKind>& kind) {
    if (!kind) {
        return nullptr;
    }
    return Json::array({kind->category, kind->surface, kind->name});
}

} // namespace

TEST_CASE("golden site_from_title", "[golden]") {
    golden::for_each(kModule, "site_from_title", [](const golden::Case& c) {
        CHECK(catalog::site_from_title(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden title_hint", "[golden]") {
    golden::for_each(kModule, "title_hint", [](const golden::Case& c) {
        CHECK(catalog::title_hint(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden classify", "[golden]") {
    golden::for_each(kModule, "classify", [](const golden::Case& c) {
        const auto kind =
            catalog::classify(identity_from(c.args()[0]), catalog::parse_overrides(c.args()[1]));
        CHECK(golden::equal(c.result<Json>(), kind_json(kind)));
    });
}

TEST_CASE("golden app_name", "[golden]") {
    golden::for_each(kModule, "app_name", [](const golden::Case& c) {
        CHECK(catalog::app_name(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden pretty_name", "[golden]") {
    golden::for_each(kModule, "pretty_name", [](const golden::Case& c) {
        CHECK(catalog::pretty_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden matches", "[golden]") {
    golden::for_each(kModule, "matches", [](const golden::Case& c) {
        CHECK(catalog::matches(identity_from(c.args()[0]), c.arg<std::string>(1)) ==
              c.result<bool>());
    });
}

TEST_CASE("golden parse_overrides", "[golden]") {
    golden::for_each(kModule, "parse_overrides", [](const golden::Case& c) {
        Json pairs = Json::array();
        for (const auto& [match, category] : catalog::parse_overrides(c.args()[0])) {
            pairs.push_back(Json::array({match, category}));
        }
        CHECK(golden::equal(c.result<Json>(), pairs));
    });
}

TEST_CASE("golden lookup", "[golden]") {
    golden::for_each(kModule, "lookup", [](const golden::Case& c) {
        const auto* found = catalog::lookup(c.arg<std::string>(0));
        Json actual = nullptr;
        if (found != nullptr) {
            actual = {{"name", found->name},
                      {"category", found->category},
                      {"surface", found->surface},
                      {"ids", found->ids},
                      {"browser", found->browser}};
        }
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden app_keys", "[golden]") {
    golden::for_each(kModule, "app_keys", [](const golden::Case& c) {
        const auto keys = catalog::app_keys(*identity_from(c.args()[0]));
        CHECK(std::vector<std::string>(keys.begin(), keys.end()) ==
              c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden is_browser", "[golden]") {
    golden::for_each(kModule, "is_browser", [](const golden::Case& c) {
        CHECK(catalog::is_browser(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("golden catalog is_terminal", "[golden]") {
    golden::for_each(kModule, "is_terminal", [](const golden::Case& c) {
        CHECK(catalog::is_terminal(identity_from(c.args()[0])) == c.result<bool>());
    });
}

TEST_CASE("golden known_names", "[golden]") {
    golden::for_each(kModule, "known_names", [](const golden::Case& c) {
        CHECK(catalog::known_names() == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden builtin_category", "[golden]") {
    golden::for_each(kModule, "builtin_category", [](const golden::Case& c) {
        CHECK(catalog::builtin_category(c.arg<std::string>(0)) ==
              c.result<std::optional<std::string>>());
    });
}
