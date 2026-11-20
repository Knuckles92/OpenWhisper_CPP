// The storyboard and the "Redo with a new prompt" dialog against OpenWhisper's pixels
// (tools/parity/builders/a23.py, tests/reference/walkthrough_review/<state>/ and
// tests/reference/write_up_prompt_dialog/): the builder's copies of
// tests/fixtures/walkthrough/2026-10-20_14-32-05, with no host callbacks and Settings' defaults.
// Class names are not compared: OpenWhisper's private helpers (_Row, _Board, _Preview, ...) are
// ReviewRow, ReviewBoard, ReviewPreview here.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPlainTextEdit>
#include <QString>

#include "review_support.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "ui_walkthrough/write_up_prompt_dialog.h"
#include "walkthrough/styles.h"

namespace fvtest {
namespace {

constexpr int kReviewWidth = 760;
constexpr int kReviewHeight = 720;

// The application styled the way OpenWhisper's render_reference.py styles it.
class AppTheme {
public:
    AppTheme(const QString& theme, int scale) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scale, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

std::filesystem::path fixtures() {
    return fv::testkit::source_root() / "tests" / "fixtures" / "walkthrough";
}

// The builder's _review_folder: a copy of the fixture walkthrough, with write-ups or a draft.
std::filesystem::path reviewFolder(const std::filesystem::path& scratch, bool writeUps,
                                   bool draft) {
    const auto folder = scratch / "2026-10-20_14-32-05";
    std::filesystem::copy(fixtures() / "2026-10-20_14-32-05", folder,
                          std::filesystem::copy_options::recursive);
    if (writeUps) {
        for (const char* name : {"write-up.md", "write-up-1.md"}) {
            std::filesystem::copy_file(fixtures() / "write-ups" / name, folder / name);
            writeFile(std::filesystem::path(folder / name).replace_extension(".pdf"),
                      "%PDF-1.4 stand-in\n");
        }
        writeFile(folder / "write-up.docx", "stand-in\n");
    }
    if (draft) {
        writeFile(folder / "draft.json",
                  R"({"version": 1, "started_at": "2026-10-20T14:32:05", "shots": []})");
        writeFile(folder / "audio.wav", "RIFF");
    }
    return folder;
}

// expect_matches, with the tree compared again after its grab. render_reference.py dumps its tree
// once it has grabbed the picture, and a grab delivers the resize events still pending in hidden
// children (the save bar, the version switch, the editors' scroll bars), whose layouts then place
// what they hold; expect_matches compares before its grab. So its first comparison leaves
// geometry and visibility out, and the second, after the grab, is the reference's own.
void expectMatches(QWidget& widget, const std::string& name, const std::string& variant,
                   QWidget* focus) {
    fv::screenshot::Options options;
    options.ignore_keys = {"class", "geometry", "visible"};
    options.focus = focus;
    fv::screenshot::expect_matches(widget, name, variant, options);
    const auto pins = fv::screenshot::load_reference(name, variant);
    for (const auto& difference : fv::screenshot::compare_trees(
             pins.at("tree"), fv::screenshot::dump_tree(widget), {"class"})) {
        FAIL_CHECK(name << "/" << variant << " tree after the grab: " << difference);
    }
}

enum class State { Spoken, WriteUp, Draft, Dirty };

void expectReview(State state, const char* name, const char* theme, int scale) {
    AppTheme applied(QString::fromLatin1(theme), scale);
    fv::testkit::TempDataRoot scratch;
    const auto folder = reviewFolder(
        scratch.path(), state == State::WriteUp || state == State::Dirty, state == State::Draft);
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WalkthroughReview review(settings, {}, false, nullptr,
                                 [] { return std::chrono::year{2026} / 10 / 21; });
    if (state == State::Dirty) {
        review.showFolder(folder, true);
        review.rows()[0]->editor()->setPlainText(QStringLiteral(
            "On the billing page, Change plan takes three seconds before anything happens."));
    } else {
        review.showFolder(folder);
    }
    review.setFixedSize(kReviewWidth, kReviewHeight);
    expectMatches(review, std::string("walkthrough_review/") + name,
                  std::string(theme) + "-windows11-dpr1-s" + std::to_string(scale), nullptr);
    review.setDirty(false);
    review.hide();
}

void expectDialog(const char* theme) {
    AppTheme applied(QString::fromLatin1(theme), 100);
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WriteUpPromptDialog dialog(
        settings, QString::fromStdString(fv::walkthrough::starter_styles()[1].instructions), true);
    expectMatches(dialog, "write_up_prompt_dialog", std::string(theme) + "-windows11-dpr1-s100",
                  dialog.promptEdit());
    dialog.hide();
}

} // namespace

TEST_CASE("the review dates its versions and keeps the recorded title across years", "[widget]") {
    using namespace std::chrono;
    fv::testkit::TempDataRoot scratch;
    const auto folder = reviewFolder(scratch.path(), true, false);
    const auto recorded = year{2026} / 10 / 21;
    const auto modified = current_zone()->to_sys(local_days{recorded} + hours{14} + minutes{5});
    std::filesystem::last_write_time(folder / "write-up.md", clock_cast<file_clock>(modified));
    auto today = recorded;
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WalkthroughReview review(settings, {}, false, nullptr, [&] { return today; });
    REQUIRE(review.showFolder(folder));
    const QString title = QStringLiteral(u"Tuesday, October 20 · 2:32 PM");
    REQUIRE(review.titleLabel()->text() == title);
    REQUIRE_FALSE(review.versionMenu()->actions().isEmpty());
    CHECK(review.versionMenu()->actions().front()->text() ==
          QStringLiteral(u"Version 2 · 2:05 PM (latest)"));
    today = year{2027} / 1 / 1;
    REQUIRE(review.showFolder(folder));
    CHECK(review.titleLabel()->text() == title);
    REQUIRE_FALSE(review.versionMenu()->actions().isEmpty());
    CHECK(review.versionMenu()->actions().front()->text() ==
          QStringLiteral(u"Version 2 · Oct 21, 2:05 PM (latest)"));
}

TEST_CASE("the storyboard as spoken matches the reference, dark",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Spoken, "spoken", "dark", 100);
}
TEST_CASE("the storyboard as spoken matches the reference, light",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Spoken, "spoken", "light", 100);
}
TEST_CASE("the storyboard as spoken matches the reference, dark at 130 %",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Spoken, "spoken", "dark", 130);
}
TEST_CASE("the storyboard of a write-up matches the reference, dark",
          "[screenshot][walkthrough_review]") {
    expectReview(State::WriteUp, "write-up", "dark", 100);
}
TEST_CASE("the storyboard of a write-up matches the reference, light",
          "[screenshot][walkthrough_review]") {
    expectReview(State::WriteUp, "write-up", "light", 100);
}
TEST_CASE("the storyboard of a write-up matches the reference, dark at 130 %",
          "[screenshot][walkthrough_review]") {
    expectReview(State::WriteUp, "write-up", "dark", 130);
}
TEST_CASE("the storyboard of a draft matches the reference, dark",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Draft, "draft", "dark", 100);
}
TEST_CASE("the storyboard of a draft matches the reference, light",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Draft, "draft", "light", 100);
}
TEST_CASE("the storyboard of a draft matches the reference, dark at 130 %",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Draft, "draft", "dark", 130);
}
TEST_CASE("the storyboard with unsaved edits matches the reference, dark",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Dirty, "dirty", "dark", 100);
}
TEST_CASE("the storyboard with unsaved edits matches the reference, light",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Dirty, "dirty", "light", 100);
}
TEST_CASE("the storyboard with unsaved edits matches the reference, dark at 130 %",
          "[screenshot][walkthrough_review]") {
    expectReview(State::Dirty, "dirty", "dark", 130);
}
TEST_CASE("the redo-with-a-new-prompt dialog matches the reference, dark",
          "[screenshot][write_up_prompt_dialog]") {
    expectDialog("dark");
}
TEST_CASE("the redo-with-a-new-prompt dialog matches the reference, light",
          "[screenshot][write_up_prompt_dialog]") {
    expectDialog("light");
}

} // namespace fvtest
