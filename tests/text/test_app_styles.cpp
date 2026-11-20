// tests/test_personalize_s1_styles.py: tones, categories, the prompt block and the mutators (the
// pipeline tests are M4.9's); plus the services.app_styles golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "prompt_test_support.h"
#include "pyport/errors.h"
#include "settings/install.h"
#include "text/app_styles.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::focus::FocusSnapshot;
using fv::prompt_test::identity;
using fv::prompt_test::snapshot;
using fv::prompt_test::snapshot_from;
using fv::text::app_styles::AppStyle;
namespace styles = fv::text::app_styles;
namespace golden = fv::golden;

namespace {

const auto kOutlook = identity("outlook.exe", "Outlook");
const auto kSlack = identity("slack.exe", "Slack");
const auto kWhatsApp = identity("whatsapp.exe", "WhatsApp");
const auto kNotion = identity("notion.exe", "Notion");
const auto kTerminal = identity("windowsterminal.exe", "Windows Terminal");
const auto kVsCode = identity("code.exe", "VS Code");
const auto kGmail = identity("chrome.exe", "Chrome", "Gmail");
const auto kSelf = identity("openwhisper", "Ours", "", true);

Json new_install() {
    Json tones = Json::object();
    for (const auto& [category, tone] : styles::NEW_INSTALL_TONES) {
        tones[std::string(category)] = tone;
    }
    return Json{{"app_style_tones", tones}};
}

Json with(Json settings, const Json& extra) {
    for (const auto& [key, value] : extra.items()) {
        settings[key] = value;
    }
    return settings;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

Json plain(const std::optional<AppStyle>& style) {
    if (!style) {
        return nullptr;
    }
    return Json{{"category", style->category},
                {"tone", style->tone},
                {"app_name", style->app_name},
                {"surface", style->surface}};
}

} // namespace

// --- tones and categories ----------------------------------------------------------

TEST_CASE("test_existing_installs_read_as_formal_everywhere", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_existing_installs_read_as_formal_everywhere
    CHECK(
        styles::resolve_tones(Json::object()) ==
        Json{{"email", "formal"}, {"work", "formal"}, {"personal", "formal"}, {"other", "formal"}});
}

TEST_CASE("test_style_for_new_install_tones", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_style_for_new_install_tones
    const std::vector<std::tuple<fv::focus::AppIdentity, std::string, std::string, std::string>>
        rows{
            {kOutlook, "email", "formal", "text"},      {kGmail, "email", "formal", "text"},
            {kSlack, "work", "casual", "text"},         {kWhatsApp, "personal", "casual", "text"},
            {kNotion, "other", "formal", "text"},       {kVsCode, "other", "formal", "code"},
            {kTerminal, "other", "formal", "terminal"},
        };
    for (const auto& [app, category, tone, surface] : rows) {
        INFO(app.app_id);
        const auto style = styles::style_for(snapshot(app), new_install());
        REQUIRE(style.has_value());
        CHECK(style->category == category);
        CHECK(style->tone == tone);
        CHECK(style->surface == surface);
    }
}

TEST_CASE("test_no_style_when_off_unknown_or_ourselves", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_no_style_when_off_unknown_or_ourselves
    const Json off = with(new_install(), Json{{"app_styles_enabled", false}});
    CHECK_FALSE(styles::style_for(snapshot(kSlack), off).has_value());
    CHECK_FALSE(styles::style_for(std::nullopt, new_install()).has_value());
    CHECK_FALSE(styles::style_for(FocusSnapshot{}, new_install()).has_value());
    CHECK_FALSE(styles::style_for(snapshot(kSelf), new_install()).has_value());
}

TEST_CASE("test_category_for_works_with_styles_off", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_category_for_works_with_styles_off
    const Json off = Json::parse(R"({"app_styles_enabled": false,
        "app_style_overrides": [{"match": "Notion", "category": "work"}]})");
    CHECK(styles::category_for(snapshot(kSlack), off) == "work");
    CHECK(styles::category_for(snapshot(kNotion), off) == "work");
    CHECK(styles::category_for(FocusSnapshot{}, off).empty());
    CHECK(styles::category_for(std::nullopt, off).empty());
    CHECK(styles::category_for(snapshot(kSelf), off).empty());
}

TEST_CASE("test_overrides_move_apps_between_styles", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_overrides_move_apps_between_styles
    const Json settings = with(new_install(), Json::parse(R"({"app_style_overrides": [
        {"match": "notion", "category": "work"}, {"match": "Gmail", "category": "personal"}]})"));
    CHECK(styles::style_for(snapshot(kNotion), settings)->tone == styles::CASUAL);
    const auto gmail = styles::style_for(snapshot(kGmail), settings);
    REQUIRE(gmail.has_value());
    CHECK(gmail->category == "personal");
    CHECK(gmail->app_name == "Gmail");
}

// --- prompt block ----------------------------------------------------------------------

TEST_CASE("test_formal_text_adds_nothing", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_formal_text_adds_nothing
    CHECK(styles::prompt_block(std::nullopt).empty());
    CHECK(styles::prompt_block(AppStyle{"email", "formal", "Outlook", "text"}).empty());
}

TEST_CASE("test_casual_and_very_casual_blocks", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_casual_and_very_casual_blocks
    const std::vector<std::pair<std::string, std::string>> rows{
        {"email", "an email"},
        {"work", "a work chat message"},
        {"personal", "a personal message"},
        {"other", "a note"},
    };
    for (const auto& [category, writing] : rows) {
        INFO(category);
        const std::string casual =
            styles::prompt_block(AppStyle{category, std::string(styles::CASUAL), "Slack", "text"});
        const std::string very = styles::prompt_block(
            AppStyle{category, std::string(styles::VERY_CASUAL), "Slack", "text"});
        CHECK_THAT(casual, StartsWith("Tone: casual"));
        CHECK_THAT(casual, ContainsSubstring(writing));
        CHECK_THAT(casual, ContainsSubstring("contractions are fine"));
        CHECK_THAT(casual, ContainsSubstring("final period"));
        CHECK_THAT(very, StartsWith("Tone: very casual"));
        CHECK_THAT(very, ContainsSubstring("lowercase"));
        CHECK_THAT(very, ContainsSubstring("never end with a period"));
        CHECK_THAT(very, ContainsSubstring("slang"));
        // The provider learns the kind of writing, never the app.
        CHECK((casual + very).find("Slack") == std::string::npos);
    }
}

TEST_CASE("test_surfaces_add_their_rule_whatever_the_tone", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_surfaces_add_their_rule_whatever_the_tone
    const std::string terminal =
        styles::prompt_block(AppStyle{"other", "formal", "Terminal", "terminal"});
    CHECK_THAT(terminal, ContainsSubstring("never insert line breaks"));
    CHECK_THAT(terminal, ContainsSubstring("inline"));
    const auto code =
        lines_of(styles::prompt_block(AppStyle{"other", "casual", "VS Code", "code"}));
    REQUIRE(code.size() == 2);
    CHECK_THAT(code[0], StartsWith("Tone: casual"));
    CHECK_THAT(code[1], ContainsSubstring("snake_case, camelCase"));
}

// --- mutators --------------------------------------------------------------------------

TEST_CASE("test_set_tone_stores_every_category", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_set_tone_stores_every_category
    Json settings = Json::object();
    CHECK(styles::set_tone(settings, "work", styles::VERY_CASUAL));
    CHECK(settings.at("app_style_tones") == Json{{"email", "formal"},
                                                 {"work", "very_casual"},
                                                 {"personal", "formal"},
                                                 {"other", "formal"}});
    CHECK_FALSE(styles::set_tone(settings, "work", styles::VERY_CASUAL));
    CHECK_THROWS_AS(styles::set_tone(settings, "work", "shouty"), fv::py::ValueError);
}

TEST_CASE("test_set_and_remove_overrides", "[test_personalize_s1_styles]") {
    // from tests/test_personalize_s1_styles.py::test_set_and_remove_overrides
    Json settings = Json::object();
    styles::set_override(settings, " Notion ", "work");
    styles::set_override(settings, "Linear", "work");
    styles::set_override(settings, "notion", "personal");
    CHECK(settings.at("app_style_overrides") == Json::parse(R"([
        {"match": "notion", "category": "personal"}, {"match": "Linear", "category": "work"}])"));
    // Back to its built-in category: nothing to remember.
    styles::set_override(settings, "Notion", "other");
    CHECK(settings.at("app_style_overrides") ==
          Json::parse(R"([{"match": "Linear", "category": "work"}])"));
    styles::remove_override(settings, "LINEAR");
    CHECK(settings.at("app_style_overrides") == Json::array());
    CHECK_THROWS_AS(styles::set_override(settings, "  ", "work"), fv::py::ValueError);
}

// --- golden vectors -------------------------------------------------------------------------

TEST_CASE("golden resolve_tones", "[golden]") {
    golden::for_each("services.app_styles", "resolve_tones", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), styles::resolve_tones(c.args().at(0))));
    });
}

TEST_CASE("golden resolve_overrides", "[golden]") {
    golden::for_each("services.app_styles", "resolve_overrides", [](const golden::Case& c) {
        Json got = Json::array();
        for (const auto& [match, category] : styles::resolve_overrides(c.args().at(0))) {
            got.push_back(Json::array({match, category}));
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden category_for", "[golden]") {
    golden::for_each("services.app_styles", "category_for", [](const golden::Case& c) {
        CHECK(styles::category_for(snapshot_from(c.args().at(0)), c.args().at(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden style_for", "[golden]") {
    golden::for_each("services.app_styles", "style_for", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(styles::style_for(snapshot_from(c.args().at(0)),
                                                                      c.args().at(1)))));
    });
}

TEST_CASE("golden app_styles prompt_block", "[golden]") {
    golden::for_each("services.app_styles", "prompt_block", [](const golden::Case& c) {
        const Json& fields = c.args().at(0);
        std::optional<AppStyle> style;
        if (!fields.is_null()) {
            style = AppStyle{
                fields.at("category").get<std::string>(), fields.at("tone").get<std::string>(),
                fields.at("app_name").get<std::string>(), fields.at("surface").get<std::string>()};
        }
        CHECK(styles::prompt_block(style) == c.result<std::string>());
    });
}

TEST_CASE("golden app style mutators", "[golden]") {
    for (const char* name : {"set_tone", "set_override", "remove_override"}) {
        golden::for_each("services.app_styles", name, [&](const golden::Case& c) {
            Json settings = c.args().at(0);
            Json got;
            try {
                const std::string function = name;
                Json result = nullptr;
                if (function == "set_tone") {
                    result =
                        styles::set_tone(settings, c.arg<std::string>(1), c.arg<std::string>(2));
                } else if (function == "set_override") {
                    styles::set_override(settings, c.arg<std::string>(1), c.arg<std::string>(2));
                } else {
                    styles::remove_override(settings, c.arg<std::string>(1));
                }
                got = Json{{"result", result}, {"settings", settings}};
            } catch (const fv::py::ValueError& error) {
                got = Json{{"error", error.what()}};
            }
            CHECK(golden::equal(c.result<Json>(), got));
        });
    }
}
