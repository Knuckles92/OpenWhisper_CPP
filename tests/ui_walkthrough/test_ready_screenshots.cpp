// The ready card against OpenWhisper's pixels (tools/parity/builders/a23.py,
// tests/reference/walkthrough_ready/<state>/): the card presented for the builder's stand-in
// results, in a folder that does not exist, with Settings' defaults.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <QApplication>
#include <QString>

#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_walkthrough/walkthrough_ready.h"

namespace fvtest {
namespace {

// The application styled the way OpenWhisper's render_reference.py styles it.
class AppTheme {
public:
    explicit AppTheme(const QString& theme) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(100, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// The builder's _ready_card result: a folder that does not exist, so the Word items are off.
fv::walkthrough::WalkthroughResult result(int shots, double duration) {
    fv::walkthrough::WalkthroughResult out;
    out.folder = "C:/captures/2026-10-21 14-32-05";
    out.pdf = out.folder / "walkthrough.pdf";
    out.markdown = out.folder / "walkthrough.md";
    out.shots = shots;
    out.duration_s = duration;
    return out;
}

std::filesystem::path shotFile(int number) {
    return fv::testkit::source_root() / "tests" / "fixtures" / "walkthrough" /
           ("shot-" + std::to_string(number) + ".png");
}

void expectCard(const fv::walkthrough::WalkthroughResult& shown, bool copied, const char* name,
                const char* theme) {
    AppTheme applied(QString::fromLatin1(theme));
    fv::WalkthroughReadyCard card;
    card.present(shown, copied, true, "pdf");
    fv::screenshot::expect_matches(card, name, std::string(theme) + "-windows11-dpr1-s100");
    card.hide();
}

void copied(const char* theme) {
    auto shown = result(3, 72);
    shown.written_up = true;
    shown.first_shot = shotFile(1);
    expectCard(shown, true, "walkthrough_ready/copied", theme);
}

void writeUpFailed(const char* theme) {
    auto shown = result(2, 75);
    shown.first_shot = shotFile(2);
    shown.write_up_error =
        "Claude Code is not installed on this computer. Choose another agent in Settings → "
        "Dictation → Walkthroughs.";
    expectCard(shown, true, "walkthrough_ready/write-up-failed", theme);
}

void copyFailed(const char* theme) {
    expectCard(result(1, 11), false, "walkthrough_ready/copy-failed", theme);
}

} // namespace

TEST_CASE("the ready card after a copy matches the reference, dark",
          "[screenshot][walkthrough_ready]") {
    copied("dark");
}
TEST_CASE("the ready card after a copy matches the reference, light",
          "[screenshot][walkthrough_ready]") {
    copied("light");
}
TEST_CASE("the ready card after a failed write-up matches the reference, dark",
          "[screenshot][walkthrough_ready]") {
    writeUpFailed("dark");
}
TEST_CASE("the ready card after a failed write-up matches the reference, light",
          "[screenshot][walkthrough_ready]") {
    writeUpFailed("light");
}
TEST_CASE("the ready card after a failed copy matches the reference, dark",
          "[screenshot][walkthrough_ready]") {
    copyFailed("dark");
}
TEST_CASE("the ready card after a failed copy matches the reference, light",
          "[screenshot][walkthrough_ready]") {
    copyFailed("light");
}

} // namespace fvtest
