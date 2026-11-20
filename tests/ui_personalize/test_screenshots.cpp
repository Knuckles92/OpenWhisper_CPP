// The rule activity strip and the two dialogs against the references OpenWhisper rendered for them
// (tools/parity/builders/a13.py, tests/reference/<widget>/<variant>/<theme>-windows11-dpr1-s<n>):
// the widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The strip is the builder's: 400x40, 30 bars of the scripted level stream, one 60 ms step each,
// eased once per step; the pulse and the orbit at phase 0; the clock at 0:12 while listening and
// the sweep at phase 0 while polishing. The dialogs are shown once and hidden again to learn the
// size they open at, as the builder does.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QDialog>
#include <QList>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QSize>

#include "base/testing/fake_clock.h"
#include "medium_prompt.h"
#include "personalize_support.h"
#include "screenshot/screenshot.h"
#include "ui_personalize/cleanup_prompt_dialog.h"
#include "ui_personalize/cleanup_rule_dialog.h"
#include "ui_personalize/rule_activity.h"
#include "ui_widgets/collapse_animation.h"

namespace {

using std::chrono::milliseconds;

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start. Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes (as tests/ui_downloads and tests/ui_history do). A bar already sized is left
// alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
    if (auto* self = qobject_cast<QAbstractScrollArea*>(&root)) {
        areas.prepend(self);
    }
    for (QAbstractScrollArea* area : areas) {
        for (QScrollBar* bar : {area->horizontalScrollBar(), area->verticalScrollBar()}) {
            QWidget* container = bar->parentWidget();
            if (container == nullptr || container->size() != kUnsized || bar->size() != kUnsized) {
                continue;
            }
            const QSize hint = bar->sizeHint();
            const QSize sized = bar->orientation() == Qt::Horizontal
                                    ? QSize(kUnsized.width(), hint.height())
                                    : QSize(hint.width(), kUnsized.height());
            container->resize(sized);
            bar->resize(sized);
        }
    }
}

// One level per 60 ms step: quiet, a word, a pause, a louder word.
constexpr std::array<double, 30> kLevels{
    0.0,  0.004, 0.02, 0.08, 0.15,  0.06, 0.01, 0.0,  0.03, 0.12, 0.2,  0.09,  0.02, 0.005, 0.0,
    0.05, 0.1,   0.18, 0.07, 0.015, 0.0,  0.02, 0.06, 0.14, 0.11, 0.04, 0.008, 0.0,  0.03,  0.09,
};

std::string variantOf(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

void stripShot(const std::string& theme, int scale, bool polishing) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fv::FakeClock clock;
    fv::RuleActivityStrip strip(60, nullptr, &clock);
    strip.showListening(QStringLiteral("USB Microphone"));
    for (const double level : kLevels) {
        strip.pushLevel(level);
        clock.advance(milliseconds(60));
        strip.trace()->advance();
    }
    if (polishing) {
        strip.showPolishing(QStringLiteral("OpenRouter · openrouter/free"));
    } else {
        // 9 pulse periods in all: the live dot is at phase 0 and "0:12" is on the clock.
        clock.advance(milliseconds(12600 - 60 * static_cast<int>(kLevels.size())));
        strip.updateClock();
    }
    // The builder runs no event loop, so the slide that opened the strip has not moved.
    strip.heightAnimation()->stop();
    strip.setMaximumHeight(fv::kUnlimitedHeight);

    const char* name =
        polishing ? "rule_activity_strip/polishing" : "rule_activity_strip/listening";
    fv::screenshot::Options options;
    // The glyph is _ActivityGlyph in OpenWhisper, an unnamed helper here.
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(strip, name, variantOf(theme, scale), options);
    strip.finish(false);
}

// As the builder does for a dialog: shown once and hidden again to learn the size it opens at,
// which is the size recorded.
void dialogShot(const std::string& theme, int scale, const char* name,
                const std::function<std::unique_ptr<QDialog>()>& make) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    const std::unique_ptr<QDialog> dialog = make();
    dialog->show();
    QCoreApplication::processEvents();
    dialog->hide();
    sizeHiddenScrollBarsAsQt611(*dialog);
    fv::screenshot::expect_matches(*dialog, name, variantOf(theme, scale));
}

std::unique_ptr<QDialog> confirmDialog() {
    return std::make_unique<fv::CleanupRuleDialog>(
        QStringLiteral("Always spell my name \"Alex Rivera\"."),
        QStringLiteral("always spell my name alex rivera"), QString(), true);
}

std::unique_ptr<QDialog> editDialog() {
    return std::make_unique<fv::CleanupRuleDialog>(
        QStringLiteral("Always spell my name \"Alex Rivera\"."));
}

std::unique_ptr<QDialog> promptDialog() {
    return std::make_unique<fv::CleanupPromptDialog>(QString::fromUtf8(fvtest::kMediumPrompt));
}

} // namespace

TEST_CASE("rule activity strip listening matches OpenWhisper, dark", "[screenshot]") {
    stripShot("dark", 100, false);
}
TEST_CASE("rule activity strip listening matches OpenWhisper, light", "[screenshot]") {
    stripShot("light", 100, false);
}
TEST_CASE("rule activity strip listening matches OpenWhisper, dark at 130 %", "[screenshot]") {
    stripShot("dark", 130, false);
}

TEST_CASE("rule activity strip polishing matches OpenWhisper, dark", "[screenshot]") {
    stripShot("dark", 100, true);
}
TEST_CASE("rule activity strip polishing matches OpenWhisper, light", "[screenshot]") {
    stripShot("light", 100, true);
}
TEST_CASE("rule activity strip polishing matches OpenWhisper, dark at 130 %", "[screenshot]") {
    stripShot("dark", 130, true);
}

TEST_CASE("cleanup rule dialog confirm matches OpenWhisper, dark", "[screenshot]") {
    dialogShot("dark", 100, "cleanup_rule_dialog/confirm", confirmDialog);
}
TEST_CASE("cleanup rule dialog confirm matches OpenWhisper, light", "[screenshot]") {
    dialogShot("light", 100, "cleanup_rule_dialog/confirm", confirmDialog);
}
TEST_CASE("cleanup rule dialog confirm matches OpenWhisper, dark at 130 %", "[screenshot]") {
    dialogShot("dark", 130, "cleanup_rule_dialog/confirm", confirmDialog);
}

TEST_CASE("cleanup rule dialog edit matches OpenWhisper, dark", "[screenshot]") {
    dialogShot("dark", 100, "cleanup_rule_dialog/edit", editDialog);
}
TEST_CASE("cleanup rule dialog edit matches OpenWhisper, light", "[screenshot]") {
    dialogShot("light", 100, "cleanup_rule_dialog/edit", editDialog);
}
TEST_CASE("cleanup rule dialog edit matches OpenWhisper, dark at 130 %", "[screenshot]") {
    dialogShot("dark", 130, "cleanup_rule_dialog/edit", editDialog);
}

TEST_CASE("cleanup prompt dialog default matches OpenWhisper, dark", "[screenshot]") {
    dialogShot("dark", 100, "cleanup_prompt_dialog/default", promptDialog);
}
TEST_CASE("cleanup prompt dialog default matches OpenWhisper, light", "[screenshot]") {
    dialogShot("light", 100, "cleanup_prompt_dialog/default", promptDialog);
}
TEST_CASE("cleanup prompt dialog default matches OpenWhisper, dark at 130 %", "[screenshot]") {
    dialogShot("dark", 130, "cleanup_prompt_dialog/default", promptDialog);
}
