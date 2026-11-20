// The golden vectors of ui_qt.widgets.rule_activity: the clock text, the colour blend and the
// level trace after a scripted stream (tools/parity/cases/ui_qt.widgets.rule_activity.py).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include <QColor>

#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "personalize_support.h"
#include "ui_personalize/rule_activity.h"

namespace {

using fv::Json;
using fv::RuleActivityState;
using std::chrono::milliseconds;

RuleActivityState stateNamed(std::string_view name) {
    for (const RuleActivityState state :
         {RuleActivityState::Idle, RuleActivityState::Listening, RuleActivityState::Transcribing,
          RuleActivityState::Polishing}) {
        if (fv::ruleActivityStateName(state) == name) {
            return state;
        }
    }
    FAIL("no state " << name);
    return RuleActivityState::Idle;
}

// Runs the script the case file describes on a trace over a FakeClock. The Python stand-in
// clock reads the written milliseconds after each start (listen, hold), so the script's times
// are offsets from the last start.
Json runStream(const Json& ops) {
    fv::FakeClock clock;
    fv::LevelTrace trace(nullptr, &clock);
    auto base = clock.steady();
    for (const Json& op : ops) {
        const std::string name = op.at(0).get<std::string>();
        if (name == "listen") {
            trace.startListening();
            base = clock.steady();
        } else if (name == "push") {
            trace.pushLevel(op.at(1).get<double>());
        } else if (name == "at") {
            const auto target = base + milliseconds(op.at(1).get<long long>());
            const auto now = clock.steady();
            REQUIRE(target >= now);
            clock.advance(target - now);
            trace.advance();
        } else if (name == "advance") {
            trace.advance();
        } else if (name == "hold") {
            trace.hold(stateNamed(op.at(1).get<std::string>()));
            base = clock.steady();
        } else if (name == "clear") {
            trace.clear();
        } else {
            FAIL("unknown op " << name);
        }
    }
    Json result = Json::object();
    result["bars"] = trace.barHeights();
    result["shown"] = trace.shownHeights();
    result["peak"] = trace.peak();
    result["state"] = std::string(fv::ruleActivityStateName(trace.state()));
    return result;
}

} // namespace

TEST_CASE("_format_clock replays the golden vectors", "[golden][rule_activity]") {
    fv::golden::for_each(
        "ui_qt.widgets.rule_activity", "_format_clock", [](const fv::golden::Case& c) {
            CHECK(fv::formatRuleClock(c.arg<int>(0)).toStdString() == c.result<std::string>());
        });
}

TEST_CASE("_mix replays the golden vectors", "[golden][rule_activity]") {
    fv::golden::for_each("ui_qt.widgets.rule_activity", "_mix", [](const fv::golden::Case& c) {
        const QColor start(c.arg<int>(0), c.arg<int>(1), c.arg<int>(2));
        const QColor end(c.arg<int>(3), c.arg<int>(4), c.arg<int>(5));
        const QColor mixed = fv::mixColors(start, end, c.arg<double>(6), c.arg<double>(7));
        const auto expected = c.result<std::vector<int>>();
        REQUIRE(expected.size() == 4);
        CHECK(mixed.red() == expected[0]);
        CHECK(mixed.green() == expected[1]);
        CHECK(mixed.blue() == expected[2]);
        CHECK(mixed.alpha() == expected[3]);
    });
}

TEST_CASE("LevelTrace.bar_heights replays the golden vectors", "[golden][rule_activity][widget]") {
    fv::golden::for_each(
        "ui_qt.widgets.rule_activity", "bar_heights", [](const fv::golden::Case& c) {
            const Json got = runStream(c.arg<Json>(0));
            CHECK(fv::golden::matches(c.raw().at("result"), got, c.float_tolerance()));
        });
}
