// What the Fast judgments page says, replayed from OpenWhisper's own answers
// (tools/parity/cases/ui_qt.dialogs.settings_fast_judgments.py): the rail value, the destination
// line, the text-model label, the key notice and the Judge tile's details and descriptions, for
// settings that choose each route and for every combination of which routes have a key.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "llm/judge.h"
#include "settings/keys.h"
#include "ui_settings/fast_judgments_page.h"

namespace golden = fv::golden;
using fv::Json;
namespace fj = fv::fast_judgments;

namespace {

constexpr const char* kModule = "ui_qt.dialogs.settings_fast_judgments";

std::optional<std::string> optionalText(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

// Which routes have a key: {route id: bool}.
std::function<bool(const fv::llm::JudgeRoute&)> keysOf(const Json& present) {
    return [present](const fv::llm::JudgeRoute& route) {
        return present.at(route.id).get<bool>();
    };
}

} // namespace

TEST_CASE("golden ui_qt.dialogs.settings_fast_judgments rail_value", "[golden][widget]") {
    golden::for_each(kModule, "FastJudgmentsSection.rail_value", [](const golden::Case& c) {
        CHECK(fj::rail_value(c.arg<Json>(0), c.arg<bool>(1)) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_fast_judgments _destination_copy", "[golden][widget]") {
    golden::for_each(kModule, "FastJudgmentsSection._destination_copy", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        const fv::llm::JudgeRoute route = fv::llm::route_from_settings(c.arg<Json>(0));
        CHECK(route.id == expected.at("route").get<std::string>());
        CHECK(fj::destination_copy(route) == expected.at("text").get<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_fast_judgments _text_model_label", "[golden][widget]") {
    golden::for_each(kModule, "FastJudgmentsSection._text_model_label", [](const golden::Case& c) {
        CHECK(fj::text_model_label(fv::llm::text_model_route(c.arg<Json>(0))) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_fast_judgments render_key_state", "[golden][widget]") {
    golden::for_each(kModule, "FastJudgmentsSection.render_key_state", [](const golden::Case& c) {
        const Json settings = c.arg<Json>(0);
        const auto present = keysOf(c.arg<Json>(1));
        const Json expected = c.result<Json>();

        const fv::llm::JudgeRoute route = fv::llm::route_from_settings(settings);
        CHECK(route.id == expected.at("route").get<std::string>());
        const std::optional<fv::llm::JudgeRoute> other = fj::other_route(route);
        const fj::KeyNotice notice =
            fj::key_notice(route, present(route), other.has_value() && present(*other),
                           fv::keys::typesafe_enabled.read(settings));

        CHECK(notice.visible == expected.at("notice_visible").get<bool>());
        CHECK(notice.title == optionalText(expected.at("title")));
        CHECK(notice.description == optionalText(expected.at("description")));
        CHECK(notice.switch_visible == expected.at("switch_visible").get<bool>());
        CHECK(notice.switch_text == optionalText(expected.at("switch_text")));
        CHECK(notice.add_tone == expected.at("add_tone").get<std::string>());
        CHECK(notice.add_tooltip == expected.at("add_tooltip").get<std::string>());
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_fast_judgments _render_route", "[golden][widget]") {
    golden::for_each(kModule, "FastJudgmentsSection._render_route", [](const golden::Case& c) {
        const Json settings = c.arg<Json>(0);
        const Json expected = c.result<Json>();
        const fj::RouteView view = fj::route_view(settings, keysOf(c.arg<Json>(1)));

        CHECK(fv::llm::route_from_settings(settings).id == expected.at("route").get<std::string>());
        CHECK(view.route_index == expected.at("route_index").get<int>());
        CHECK(view.route_details == expected.at("route_details").get<std::vector<std::string>>());
        CHECK(view.text_model_visible == expected.at("text_model_visible").get<bool>());
        CHECK(view.text_model_index == expected.at("text_model_index").get<int>());
        CHECK(view.text_model_details ==
              expected.at("text_model_details").get<std::vector<std::string>>());
        CHECK(view.description == expected.at("description").get<std::string>());
        CHECK(view.destination == expected.at("destination").get<std::string>());
    });
}
