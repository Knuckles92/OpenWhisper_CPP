// The Qt-free copy of the Personalize pages, replayed from the golden vectors of
// ui_qt.dialogs.settings_styles, settings_dictionary, settings_commands, settings_snippets,
// settings_cleanup_rules, ui_qt.widgets.command_settings and snippets_panel.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/json.h"
#include "focus/catalog.h"
#include "golden/golden.h"
#include "text/personalize_copy.h"
#include "text/snippets.h"

using fv::Json;
using fv::text::EngineInfo;
using fv::text::ProviderInfo;
namespace text = fv::text;
namespace golden = fv::golden;
namespace styles_page = fv::text::styles_page;
namespace dictionary_page = fv::text::dictionary_page;

namespace {

constexpr std::string_view kStyles = "ui_qt.dialogs.settings_styles";
constexpr std::string_view kDictionary = "ui_qt.dialogs.settings_dictionary";

// The engine a dictionary vector was recorded with, from its resolved endpoint and backend.
EngineInfo engine_of(const golden::Case& c) {
    const Json result = c.result<Json>();
    std::optional<dictionary_page::Endpoint> endpoint;
    if (!result.at("endpoint").is_null()) {
        endpoint = dictionary_page::Endpoint{result.at("endpoint").at("name").get<std::string>(),
                                             result.at("endpoint").at("local").get<bool>()};
    }
    std::optional<dictionary_page::RemoteBackend> remote;
    if (!result.at("remote").is_null()) {
        const Json& fields = result.at("remote");
        remote = dictionary_page::RemoteBackend{fields.at("support").get<std::string>(),
                                                fields.at("name").get<std::string>(),
                                                fields.at("host_name").get<std::string>()};
    }
    const EngineInfo engine = dictionary_page::engine_info(c.args().at(0), endpoint, remote);
    const Json& expected = result.at("engine");
    CHECK(engine.support == expected.at(0).get<std::string>());
    CHECK(engine.name == expected.at(1).get<std::string>());
    CHECK(engine.destination == expected.at(2).get<std::string>());
    return engine;
}

} // namespace

TEST_CASE("the privacy note names the product and falls back to your provider") {
    CHECK(styles_page::privacy_note(Json::object(), false, ProviderInfo{"", false}) ==
          "Works on Windows for now. Here, " + std::string(fv::identity().product) +
              " only knows which app you're in.");
    CHECK(styles_page::privacy_note(Json{{"transcript_cleanup_enabled", true}}, true,
                                    ProviderInfo{"", false}) ==
          "Sent only to your AI cleanup provider (your provider) with the dictation. Never "
          "saved, and never sent to a speech engine.");
}

TEST_CASE("an engine only a disconnected host could describe says nothing") {
    const EngineInfo engine = dictionary_page::engine_info(Json{{"selected_model", "remote"}});
    CHECK(engine.support.empty());
    CHECK(engine.name.empty());
    CHECK(dictionary_page::engine_line(Json::object(), engine) ==
          "Engines that take hints also listen for your words. Every engine fixes spellings "
          "after recognition.");
    CHECK_FALSE(dictionary_page::sounds_like_only(Json::object(), engine));
}

// --- golden vectors: settings_styles --------------------------------------------------------

TEST_CASE("golden settings_styles cleanup_on", "[golden]") {
    golden::for_each(kStyles, "cleanup_on", [](const golden::Case& c) {
        CHECK(styles_page::cleanup_on(c.args().at(0)) == c.result<bool>());
    });
}

TEST_CASE("golden settings_styles reads_text_near_cursor", "[golden]") {
    golden::for_each(kStyles, "reads_text_near_cursor", [](const golden::Case& c) {
        CHECK(styles_page::reads_text_near_cursor(c.args().at(0), c.arg<bool>(1)) ==
              c.result<bool>());
    });
}

TEST_CASE("golden settings_styles styles_blocker", "[golden]") {
    golden::for_each(kStyles, "styles_blocker", [](const golden::Case& c) {
        CHECK(styles_page::styles_blocker(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden settings_styles rail_value", "[golden]") {
    golden::for_each(kStyles, "rail_value", [](const golden::Case& c) {
        CHECK(styles_page::rail_value(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden settings_styles home_copy", "[golden]") {
    golden::for_each(kStyles, "home_copy", [](const golden::Case& c) {
        CHECK(styles_page::home_copy(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden settings_styles apps_summary", "[golden]") {
    golden::for_each(kStyles, "apps_summary", [](const golden::Case& c) {
        std::vector<fv::focus::catalog::Override> overrides;
        for (const Json& pair : c.args().at(1)) {
            overrides.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
        }
        CHECK(styles_page::apps_summary(c.arg<std::string>(0), overrides) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden settings_styles privacy_note", "[golden]") {
    golden::for_each(kStyles, "privacy_note", [](const golden::Case& c) {
        const Json result = c.result<Json>();
        const ProviderInfo provider{result.at("provider").at("name").get<std::string>(),
                                    result.at("provider").at("local").get<bool>()};
        CHECK(golden::equal_rebranded(
            result.at("result").get<std::string>(),
            styles_page::privacy_note(c.args().at(0), c.arg<bool>(1), provider)));
    });
}

TEST_CASE("golden settings_styles _excluded", "[golden]") {
    golden::for_each(kStyles, "_excluded", [](const golden::Case& c) {
        CHECK(styles_page::excluded(c.args().at(0)) == c.result<std::vector<std::string>>());
    });
}

// --- golden vectors: settings_dictionary ----------------------------------------------------

TEST_CASE("golden settings_dictionary cannot_steer", "[golden]") {
    golden::for_each(kDictionary, "cannot_steer", [](const golden::Case& c) {
        CHECK(dictionary_page::cannot_steer(engine_of(c)) ==
              c.result<Json>().at("result").get<bool>());
    });
}

TEST_CASE("golden settings_dictionary sounds_like_only", "[golden]") {
    golden::for_each(kDictionary, "sounds_like_only", [](const golden::Case& c) {
        CHECK(dictionary_page::sounds_like_only(c.args().at(0), engine_of(c)) ==
              c.result<Json>().at("result").get<bool>());
    });
}

TEST_CASE("golden settings_dictionary engine_line", "[golden]") {
    golden::for_each(kDictionary, "engine_line", [](const golden::Case& c) {
        CHECK(dictionary_page::engine_line(c.args().at(0), engine_of(c)) ==
              c.result<Json>().at("result").get<std::string>());
    });
}

TEST_CASE("golden settings_dictionary home_detail", "[golden]") {
    golden::for_each(kDictionary, "home_detail", [](const golden::Case& c) {
        CHECK(dictionary_page::home_detail(c.args().at(0), engine_of(c)) ==
              c.result<Json>().at("result").get<std::string>());
    });
}

TEST_CASE("golden settings_dictionary rail_value", "[golden]") {
    golden::for_each(kDictionary, "rail_value", [](const golden::Case& c) {
        CHECK(dictionary_page::rail_value(c.args().at(0)) == c.result<std::string>());
    });
}

// --- golden vectors: commands, snippets, rules ------------------------------------------------

TEST_CASE("golden settings_commands rail_value", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_commands", "rail_value", [](const golden::Case& c) {
        CHECK(text::commands_page::rail_value(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden command_settings command_mode_hint", "[golden]") {
    golden::for_each("ui_qt.widgets.command_settings", "command_mode_hint",
                     [](const golden::Case& c) {
                         CHECK(text::commands_page::command_mode_hint(c.args().at(0)) ==
                               c.result<std::string>());
                     });
}

TEST_CASE("golden settings_snippets", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_snippets", "_count", [](const golden::Case& c) {
        CHECK(text::snippets_page::count(c.args().at(0)) == c.result<std::string>());
    });
    golden::for_each("ui_qt.dialogs.settings_snippets", "rail_value", [](const golden::Case& c) {
        CHECK(text::snippets_page::rail_value(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden snippets_panel preview", "[golden]") {
    golden::for_each("ui_qt.widgets.snippets_panel", "_update_hints", [](const golden::Case& c) {
        const Json& fields = c.args().at(0);
        const text::Snippet draft{
            fields.at("id").get<std::string>(), fields.at("trigger").get<std::string>(),
            fields.at("text").get<std::string>(), fields.at("formatted").get<bool>()};
        CHECK(text::snippets_page::preview(draft) == c.result<std::string>());
    });
}

TEST_CASE("golden settings_cleanup_rules rule_count_label", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_cleanup_rules", "rule_count_label",
                     [](const golden::Case& c) {
                         CHECK(text::cleanup_rules_page::rule_count_label(c.arg<std::int64_t>(0)) ==
                               c.result<std::string>());
                     });
}
