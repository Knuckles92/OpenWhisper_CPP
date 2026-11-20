// The live strip under the learned-rule composer and the new-rule glow. Time is a FakeClock the
// test moves; what the strip does on a real timer (the reveal delay, the slide) is waited for.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "base/testing/fake_clock.h"
#include "personalize_support.h"
#include "ui_personalize/rule_activity.h"
#include "ui_widgets/eliding_label.h"
#include "uimath/loudness.h"

namespace {

using Catch::Approx;
using fv::LevelTrace;
using fv::RuleActivityState;
using fv::RuleActivityStrip;
using std::chrono::milliseconds;
using std::chrono::seconds;

// The strip in a window, as the composer holds it.
struct StripRig {
    StripRig() {
        auto* layout = new QVBoxLayout(&host);
        strip = new RuleActivityStrip(60, nullptr, &clock);
        layout->addWidget(strip);
        host.resize(640, 120);
        host.show();
        fvtest::processEvents();
    }

    fv::FakeClock clock;
    QWidget host;
    RuleActivityStrip* strip = nullptr;
};

} // namespace

// from
// tests/test_rule_composer.py::TestLevelTrace::test_level_scale_is_logarithmic_between_floor_and_ceiling
TEST_CASE("test_level_scale_is_logarithmic_between_floor_and_ceiling",
          "[test_rule_composer][unit]") {
    CHECK(fv::level_to_height(0.0) == 0.0);
    CHECK(fv::level_to_height(std::pow(10.0, -60.0 / 20)) == 0.0);
    CHECK(fv::level_to_height(std::pow(10.0, -36.0 / 20)) == Approx(0.5));
    CHECK(fv::level_to_height(std::pow(10.0, -12.0 / 20)) == 1.0);
    CHECK(fv::level_to_height(1.0) == 1.0);
    double previous = -1.0;
    for (const double level : {0.002, 0.01, 0.05, 0.2}) {
        const double height = fv::level_to_height(level);
        CHECK(height >= previous);
        previous = height;
    }
}

// from tests/test_rule_composer.py::TestLevelTrace::test_each_step_banks_the_loudest_level_heard
TEST_CASE("test_each_step_banks_the_loudest_level_heard", "[test_rule_composer][widget]") {
    fv::FakeClock clock;
    LevelTrace trace(nullptr, &clock);
    trace.startListening();
    trace.pushLevel(0.02);
    trace.pushLevel(0.1);
    trace.pushLevel(0.05);
    clock.advance(milliseconds(60));
    trace.advance();
    CHECK(trace.barHeights() == std::vector<double>{fv::level_to_height(0.1)});
    // A step with no level from the microphone is silence, not a repeat.
    clock.advance(milliseconds(60));
    trace.advance();
    CHECK(trace.barHeights() == std::vector<double>{fv::level_to_height(0.1), 0.0});
}

// from tests/test_rule_composer.py::TestLevelTrace::test_holding_stops_the_trace_from_taking_levels
TEST_CASE("test_holding_stops_the_trace_from_taking_levels", "[test_rule_composer][widget]") {
    fv::FakeClock clock;
    LevelTrace trace(nullptr, &clock);
    trace.startListening();
    trace.pushLevel(0.1);
    clock.advance(milliseconds(60));
    trace.advance();
    trace.hold(RuleActivityState::Transcribing);
    trace.pushLevel(0.3);
    clock.advance(milliseconds(540));
    trace.advance();
    CHECK(trace.barHeights().size() == 1);
}

// from
// tests/test_rule_composer.py::TestRuleActivityStrip::test_listening_opens_at_once_and_finish_closes
TEST_CASE("test_listening_opens_at_once_and_finish_closes", "[test_rule_composer][widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    CHECK(strip.isHidden());
    strip.showListening(QStringLiteral("USB microphone"));
    CHECK_FALSE(strip.isHidden());
    CHECK(strip.state() == RuleActivityState::Listening);
    CHECK(strip.titleLabel()->text() == QStringLiteral("Listening"));
    CHECK(strip.detailLabel()->text() == QStringLiteral("USB microphone"));
    CHECK(strip.timeLabel()->text() == QStringLiteral("0:00 / 1:00"));
    strip.finish(false);
    CHECK(strip.isHidden());
    CHECK(strip.state() == RuleActivityState::Idle);
}

// from
// tests/test_rule_composer.py::TestRuleActivityStrip::test_opened_strip_grows_to_its_full_height
TEST_CASE("test_opened_strip_grows_to_its_full_height", "[test_rule_composer][widget]") {
    StripRig rig;
    rig.strip->showListening(QStringLiteral("USB microphone"));
    CHECK(fvtest::pumpUntil([&] { return rig.strip->height() == rig.strip->sizeHint().height(); }));
    // Unbounded once open, so a larger font can still grow the panel.
    CHECK(fvtest::pumpUntil([&] { return rig.strip->maximumHeight() == 16777215; }));
}

// from
// tests/test_rule_composer.py::TestRuleActivityStrip::test_work_that_ends_at_once_never_opens_the_strip
TEST_CASE("test_work_that_ends_at_once_never_opens_the_strip", "[test_rule_composer][widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showPolishing(QStringLiteral("OpenRouter · model"));
    CHECK(strip.isHidden());
    strip.finish();
    fvtest::wait(250);
    CHECK(strip.isHidden());
    CHECK(strip.state() == RuleActivityState::Idle);
}

// from tests/test_rule_composer.py::TestRuleActivityStrip::test_slow_work_opens_after_a_short_delay
TEST_CASE("test_slow_work_opens_after_a_short_delay", "[test_rule_composer][widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showPolishing(QStringLiteral("OpenRouter · model"));
    CHECK(fvtest::pumpUntil([&] { return !strip.isHidden(); }, 1000));
    CHECK(strip.state() == RuleActivityState::Polishing);
    CHECK(strip.titleLabel()->text() == QStringLiteral("Polishing with AI"));
}

// from
// tests/test_rule_composer.py::TestRuleActivityStrip::test_transcribing_follows_listening_without_closing
TEST_CASE("test_transcribing_follows_listening_without_closing", "[test_rule_composer][widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showListening(QStringLiteral("USB microphone"));
    strip.showTranscribing(QStringLiteral("On this computer · Parakeet"));
    CHECK_FALSE(strip.isHidden());
    CHECK(strip.state() == RuleActivityState::Transcribing);
    CHECK(strip.timeLabel()->isHidden());
}

// from
// tests/test_rule_composer.py::TestRuleActivityStrip::test_clock_turns_amber_near_the_automatic_stop
TEST_CASE("test_clock_turns_amber_near_the_automatic_stop", "[test_rule_composer][widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showListening(QStringLiteral("USB microphone"));
    CHECK_FALSE(strip.timeLabel()->property("nearCap").toBool());
    rig.clock.advance(seconds(51));
    strip.updateClock();
    CHECK(strip.timeLabel()->text() == QStringLiteral("0:51 / 1:00"));
    CHECK(strip.timeLabel()->property("nearCap").toBool());
}

// from tests/test_rule_composer.py::TestItemGlow::test_flash_washes_the_row_then_fades_away
TEST_CASE("test_flash_washes_the_row_then_fades_away", "[test_rule_composer][widget]") {
    QListWidget view;
    view.addItems({QStringLiteral("First rule"), QStringLiteral("Second rule")});
    view.resize(320, 200);
    view.show();
    fv::ItemGlow glow(&view);
    glow.animation()->setDuration(60);
    glow.flash(view.item(1));
    CHECK(glow.isVisible());
    CHECK(glow.strength() == 1.0);
    CHECK(fvtest::pumpUntil([&] { return glow.isHidden(); }, 2000));
}

TEST_CASE("the trace takes levels only while listening and keeps a ring of 480 bars", "[widget]") {
    fv::FakeClock clock;
    LevelTrace trace(nullptr, &clock);
    trace.pushLevel(0.5);
    CHECK(trace.peak() == 0.0);
    trace.startListening();
    trace.pushLevel(0.5);
    CHECK(trace.peak() == 0.5);
    for (int step = 0; step < 500; ++step) {
        clock.advance(milliseconds(60));
        trace.advance();
    }
    CHECK(trace.barHeights().size() == LevelTrace::kMaxBars);
    // The first step held the loudest level; everything after it was silence.
    CHECK(trace.barHeights().back() == 0.0);
}

TEST_CASE("the bars ease 0.35 of the way to their target on every tick", "[widget]") {
    fv::FakeClock clock;
    LevelTrace trace(nullptr, &clock);
    trace.startListening();
    trace.pushLevel(0.2);
    clock.advance(milliseconds(60));
    trace.advance();
    const double target = fv::level_to_height(0.2);
    CHECK(trace.shownHeights().front() == Approx(target * 0.35));
    trace.advance();
    CHECK(trace.shownHeights().front() == Approx(target * 0.35 + (target - target * 0.35) * 0.35));
    // Holding takes the bars where they are.
    trace.hold(RuleActivityState::Polishing);
    CHECK(trace.barHeights() == trace.shownHeights());
    trace.clear();
    CHECK(trace.barHeights().empty());
    CHECK(trace.state() == RuleActivityState::Idle);
}

TEST_CASE("the trace paints bars while listening and sweeps them while work runs", "[widget]") {
    fv::FakeClock clock;
    LevelTrace trace(nullptr, &clock);
    trace.resize(240, 22);
    // Idle paints nothing, so any bar changes the picture.
    const QImage idle = trace.grab().toImage();
    trace.startListening();
    for (int step = 0; step < 40; ++step) {
        trace.pushLevel(0.1);
        clock.advance(milliseconds(60));
        trace.advance();
    }
    const QImage listening = trace.grab().toImage();
    CHECK(listening != idle);
    // After capture a highlight sweeps the bars that were taken.
    trace.hold(RuleActivityState::Transcribing);
    const QImage held = trace.grab().toImage();
    clock.advance(milliseconds(650));
    const QImage swept = trace.grab().toImage();
    CHECK(held != listening);
    CHECK(swept != held);
    trace.clear();
    CHECK(trace.grab().toImage() == idle);
}

TEST_CASE("the panel's state drives the theme and the strip is named for assistive tools",
          "[widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    CHECK(strip.objectName() == QStringLiteral("ruleActivityHost"));
    CHECK(strip.panel()->objectName() == QStringLiteral("ruleActivityStrip"));
    CHECK(strip.panel()->property("state").toString() == QStringLiteral("idle"));
    strip.showListening(QStringLiteral("USB microphone"));
    CHECK(strip.panel()->property("state").toString() == QStringLiteral("listening"));
    CHECK(strip.accessibleName() == QStringLiteral("Listening"));
    CHECK(strip.accessibleDescription() == QStringLiteral("USB microphone"));
    strip.showTranscribing(QStringLiteral("On this computer"));
    CHECK(strip.panel()->property("state").toString() == QStringLiteral("transcribing"));
    strip.finish(false);
    CHECK(strip.panel()->property("state").toString() == QStringLiteral("idle"));
    CHECK(strip.trace()->barHeights().empty());
}

TEST_CASE("finishing slides the strip shut, then it is idle and hidden", "[widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showListening(QStringLiteral("USB microphone"));
    REQUIRE(fvtest::pumpUntil([&] { return strip.maximumHeight() == 16777215; }));
    strip.finish();
    CHECK(strip.state() == RuleActivityState::Listening);
    CHECK(fvtest::pumpUntil([&] { return strip.isHidden(); }));
    CHECK(strip.state() == RuleActivityState::Idle);
    CHECK(strip.maximumHeight() == 0);
}

TEST_CASE("work that starts while the strip closes reopens it at once", "[widget]") {
    StripRig rig;
    RuleActivityStrip& strip = *rig.strip;
    strip.showListening(QStringLiteral("USB microphone"));
    REQUIRE(fvtest::pumpUntil([&] { return strip.maximumHeight() == 16777215; }));
    strip.finish();
    strip.showPolishing(QStringLiteral("OpenRouter · openrouter/free"));
    CHECK(strip.state() == RuleActivityState::Polishing);
    CHECK(fvtest::pumpUntil([&] { return strip.maximumHeight() == 16777215; }));
    CHECK_FALSE(strip.isHidden());
}

TEST_CASE("the clock text is capped at the stop and counts whole seconds", "[widget]") {
    fv::FakeClock clock;
    RuleActivityStrip strip(90, nullptr, &clock);
    strip.showListening(QStringLiteral("Mic"));
    CHECK(strip.timeLabel()->text() == QStringLiteral("0:00 / 1:30"));
    clock.advance(milliseconds(12999));
    strip.updateClock();
    CHECK(strip.timeLabel()->text() == QStringLiteral("0:12 / 1:30"));
    CHECK_FALSE(strip.timeLabel()->property("nearCap").toBool());
    clock.advance(seconds(200));
    strip.updateClock();
    CHECK(strip.timeLabel()->text() == QStringLiteral("1:30 / 1:30"));
    CHECK(strip.timeLabel()->property("nearCap").toBool());
    strip.finish(false);
}

TEST_CASE("a cap below one second is one second", "[widget]") {
    fv::FakeClock clock;
    RuleActivityStrip strip(0, nullptr, &clock);
    strip.showListening(QStringLiteral("Mic"));
    CHECK(strip.timeLabel()->text() == QStringLiteral("0:00 / 0:01"));
    strip.finish(false);
}
