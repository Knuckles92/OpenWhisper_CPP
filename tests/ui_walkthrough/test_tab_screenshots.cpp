// The Walkthrough tab, the side panel's Walkthroughs page and the New style dialog against
// OpenWhisper's pixels (tools/parity/builders/a23.py: tests/reference/walkthrough_tab/<state>/,
// tests/reference/past_walkthroughs/ and tests/reference/new_style_dialog/). The captures folders
// are the builder's: none; the fixture walkthrough (tests/fixtures/walkthrough/
// 2026-10-20_14-32-05); or it written up, a copy of it as spoken started 2026-10-19 11:02:40, and
// a newer draft that the tab has closed. Today is 2026-10-21, no agent scan has run, and
// Settings hold only the write-up switch and the closed walkthrough. Class names are not
// compared: OpenWhisper's Field, RecentRow and private helpers have other names here.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include <QApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTest>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "review_support.h"
#include "screenshot/screenshot.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_walkthrough/new_style_dialog.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_tab.h"

namespace fvtest {
namespace {

using fv::Json;

constexpr int kTabWidth = 1100;
constexpr int kTabHeight = 800;
constexpr const char* kDraftName = "2026-10-21_09-15-00";

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

enum class Kind { None, One, Three };

// The builder's _captures.
std::filesystem::path captures(const std::filesystem::path& scratch, Kind kind) {
    namespace fs = std::filesystem;
    const auto root = scratch / "captures";
    fs::create_directories(root);
    if (kind == Kind::None) {
        return root;
    }
    const auto written = root / "2026-10-20_14-32-05";
    fs::copy(fixtures() / "2026-10-20_14-32-05", written, fs::copy_options::recursive);
    if (kind == Kind::One) {
        return root;
    }
    fs::copy_file(fixtures() / "write-ups" / "write-up.md", written / "write-up.md");
    writeFile(written / "write-up.pdf", "%PDF-1.4 stand-in\n");
    const auto spoken = root / "2026-10-19_11-02-40";
    fs::copy(fixtures() / "2026-10-20_14-32-05", spoken, fs::copy_options::recursive);
    Json manifest = Json::parse(readFile(spoken / "walkthrough.json"));
    manifest["started_at"] = "2026-10-19T11:02:40";
    writeFile(spoken / "walkthrough.json", manifest.dump(2));
    const auto draft = root / kDraftName;
    fs::create_directories(draft);
    writeFile(draft / "draft.json",
              R"({"version": 1, "started_at": "2026-10-21T09:15:00", "shots": []})");
    return root;
}

// Python marks each form row with a dynamic property, formRow, that the tab reads back to size
// them; here the tab keeps a list of its rows (lint.style_props allows no raw property), so the
// reference's formRow is left out of the comparison and every other property is compared.
void dropFormRow(Json& node) {
    if (const auto it = node.find("properties"); it != node.end() && it->is_object()) {
        it->erase("formRow");
    }
    if (const auto it = node.find("children"); it != node.end()) {
        for (Json& child : *it) {
            dropFormRow(child);
        }
    }
}

// A hidden widget's geometry is what the last layout that showed it left, not what the picture
// shows: the storyboard's hidden scroll bar keeps the height of a layout pass PyQt made before
// the reference was grabbed. Visibility is still compared.
void dropHiddenGeometry(Json& node) {
    if (node.value("visible", true) == false) {
        node.erase("geometry");
    }
    if (const auto it = node.find("children"); it != node.end()) {
        for (Json& child : *it) {
            dropHiddenGeometry(child);
        }
    }
}

// expect_matches compares the pixels and, before its grab, the tree without its properties,
// geometry and visibility; the whole tree is compared again after the grab, as render_reference.py
// dumps it (see test_review_screenshots.cpp).
void expectMatches(QWidget& widget, const std::string& name, const std::string& variant,
                   QWidget* focus) {
    fv::screenshot::Options options;
    options.ignore_keys = {"class", "geometry", "visible", "properties"};
    options.focus = focus;
    fv::screenshot::expect_matches(widget, name, variant, options);
    Json tree = fv::screenshot::load_reference(name, variant).at("tree");
    dropFormRow(tree);
    dropHiddenGeometry(tree);
    Json shown = fv::screenshot::dump_tree(widget);
    dropHiddenGeometry(shown);
    for (const auto& difference : fv::screenshot::compare_trees(tree, shown, {"class"})) {
        FAIL_CHECK(name << "/" << variant << " tree after the grab: " << difference);
    }
}

std::string variantName(const char* theme, int scale) {
    return std::string(theme) + "-windows11-dpr1-s" + std::to_string(scale);
}

enum class State { Empty, Recording, Storyboard, WriteUpOff, Prompt };

void expectTab(State state, const char* name, const char* theme, int scale,
               int height = kTabHeight) {
    AppTheme applied(QString::fromLatin1(theme), scale);
    fv::testkit::TempDataRoot scratch;
    const Kind kind = state == State::Empty                                      ? Kind::Three
                      : state == State::Storyboard || state == State::WriteUpOff ? Kind::One
                                                                                 : Kind::None;
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    settings.save_all(Json{{"walkthrough_write_up", state != State::WriteUpOff},
                           {"walkthrough_closed", state == State::Empty ? kDraftName : ""}});
    fv::ManualExecutor io;
    fv::WalkthroughTabOptions options;
    options.capturesRoot = captures(scratch.path(), kind);
    options.io = &io;
    options.today = [] {
        return QDate(2026, 10, 21);
    };
    options.platform = "win32";
    fv::WalkthroughTab tab(settings, {}, options);
    tab.setHotkeys(QStringLiteral("Ctrl+Alt+W"), QStringLiteral("Ctrl+Alt+S"));
    for (int round = 0; round < 3; ++round) {
        io.run_all();
        QApplication::processEvents();
    }
    REQUIRE_FALSE(tab.isRecentLoading());
    const std::string widget = std::string("walkthrough_tab/") + name;
    if (state == State::Recording) {
        tab.setRecordingState(true, true);
    }
    if (state == State::Prompt) {
        tab.resize(kTabWidth, kTabHeight);
        tab.editPrompt();
        fv::PromptPopover* popover = tab.promptPopover();
        expectMatches(*popover, widget, variantName(theme, scale), tab.promptEdit());
        popover->hide();
    } else {
        tab.setFixedSize(kTabWidth, height);
        expectMatches(tab, widget, variantName(theme, scale), nullptr);
    }
    tab.review()->setDirty(false);
    tab.hide();
}

void expectPast(const char* theme) {
    AppTheme applied(QString::fromLatin1(theme), 100);
    fv::testkit::TempDataRoot scratch;
    fv::ManualExecutor io;
    fv::PastWalkthroughsPanel panel(captures(scratch.path(), Kind::Three),
                                    [] { return QDate(2026, 10, 21); });
    panel.setExecutor(&io);
    panel.refresh();
    io.run_all();
    REQUIRE(QTest::qWaitFor([&panel] { return panel.total().has_value(); }, 5000));
    panel.setFixedSize(380, 760);
    expectMatches(panel, "past_walkthroughs", variantName(theme, 100), nullptr);
    panel.hide();
}

void expectNewStyle(const char* theme) {
    AppTheme applied(QString::fromLatin1(theme), 100);
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::NewStyleDialog dialog(settings);
    dialog.setFixedSize(540, 420);
    expectMatches(dialog, "new_style_dialog", variantName(theme, 100), dialog.nameEdit());
    dialog.hide();
}

} // namespace

TEST_CASE("the tab's empty stage matches the reference, dark", "[screenshot][walkthrough_tab]") {
    expectTab(State::Empty, "empty", "dark", 100);
}
TEST_CASE("the tab's empty stage matches the reference, light", "[screenshot][walkthrough_tab]") {
    expectTab(State::Empty, "empty", "light", 100);
}
TEST_CASE("the tab's empty stage matches the reference, dark at 130 %",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::Empty, "empty", "dark", 130);
}
TEST_CASE("the tab recording matches the reference, dark", "[screenshot][walkthrough_tab]") {
    expectTab(State::Recording, "recording", "dark", 100);
}
TEST_CASE("the tab recording matches the reference, light", "[screenshot][walkthrough_tab]") {
    expectTab(State::Recording, "recording", "light", 100);
}
TEST_CASE("the tab recording matches the reference, dark at 130 %",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::Recording, "recording", "dark", 130);
}
TEST_CASE("the tab's storyboard matches the reference, dark", "[screenshot][walkthrough_tab]") {
    expectTab(State::Storyboard, "storyboard", "dark", 100);
}
TEST_CASE("the tab's storyboard matches the reference, light", "[screenshot][walkthrough_tab]") {
    expectTab(State::Storyboard, "storyboard", "light", 100);
}
TEST_CASE("the tab's storyboard matches the reference, dark at 130 %",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::Storyboard, "storyboard", "dark", 130);
}
TEST_CASE("the storyboard fits the shorter tab at 130 %", "[screenshot][walkthrough_tab]") {
    expectTab(State::Storyboard, "storyboard-760", "dark", 130, 760);
}
TEST_CASE("the tab with the write-up off matches the reference, dark",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::WriteUpOff, "write-up-off", "dark", 100);
}
TEST_CASE("the tab with the write-up off matches the reference, light",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::WriteUpOff, "write-up-off", "light", 100);
}
TEST_CASE("the tab with the write-up off matches the reference, dark at 130 %",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::WriteUpOff, "write-up-off", "dark", 130);
}
TEST_CASE("the prompt popover matches the reference, dark", "[screenshot][walkthrough_tab]") {
    expectTab(State::Prompt, "prompt", "dark", 100);
}
TEST_CASE("the prompt popover matches the reference, light", "[screenshot][walkthrough_tab]") {
    expectTab(State::Prompt, "prompt", "light", 100);
}
TEST_CASE("the prompt popover matches the reference, dark at 130 %",
          "[screenshot][walkthrough_tab]") {
    expectTab(State::Prompt, "prompt", "dark", 130);
}
TEST_CASE("the walkthroughs page matches the reference, dark", "[screenshot][past_walkthroughs]") {
    expectPast("dark");
}
TEST_CASE("the walkthroughs page matches the reference, light", "[screenshot][past_walkthroughs]") {
    expectPast("light");
}
TEST_CASE("the new style dialog matches the reference, dark", "[screenshot][new_style_dialog]") {
    expectNewStyle("dark");
}
TEST_CASE("the new style dialog matches the reference, light", "[screenshot][new_style_dialog]") {
    expectNewStyle("light");
}

} // namespace fvtest
