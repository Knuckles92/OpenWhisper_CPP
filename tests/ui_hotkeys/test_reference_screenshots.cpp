// The Hotkeys page and a standard row's card against the references OpenWhisper rendered for
// them (tools/parity/builders/a06.py, tests/reference/<widget>/<theme>-windows11-dpr1-s<scale>):
// the widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, HotkeysPage here.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QCoreApplication>

#include "hotkeys_test_support.h"
#include "screenshot/screenshot.h"

using fv::Json;
using namespace fvtest;

namespace {

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "HotkeysPage";
    }
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

void check(QWidget& widget, const std::string& name, const std::string& variant) {
    widget.ensurePolished();
    checkClasses(fv::screenshot::load_reference(name, variant).at("tree"),
                 fv::screenshot::dump_tree(widget), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(widget, name, variant, options);
}

std::string variantName(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

// As the builder measures its height: shown at its width, events processed, hidden again. The
// wrapped labels take their heights from that first layout. render_reference.py then dumps the
// tree after grabbing the pixels, and the grab sends the hidden latch row its pending resize, so
// the row's children are laid out at its own default size; the grab here does the same.
void settle(QWidget& widget) {
    widget.resize(640, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

void hotkeysPage(const std::string& theme, int scale, bool pushHold) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    // No bindings: the profile, transform and action groups show their empty texts.
    PageScene scene(pushHold ? Json{{"recording_trigger_mode", "push_hold"}} : Json::object());
    settle(*scene.page);
    check(*scene.page, pushHold ? "hotkeys_page/push-hold" : "hotkeys_page/defaults",
          variantName(theme, scale));
}

void conflictCard(const std::string& theme, int scale) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    PageScene scene;
    auto& page = *scene.page;
    auto* field = page.hotkeyInput("command_mode");
    // A capture of the record shortcut on Command Mode's row, refused.
    Q_EMIT field->captureRequested();
    scene.capture()->capture(page.currentHotkeys()["record_toggle"].get<std::string>());
    REQUIRE(scene.note("command_mode") == "That shortcut is already used by Recording.");
    // Laid out on the page at the builder's width, then on its own.
    page.resize(640, 4000);
    page.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    page.hide();
    fv::ReflowCard* card = cardOf(field);
    REQUIRE(card != nullptr);
    card->setParent(nullptr);
    std::unique_ptr<QWidget> owned(card);
    check(*card, "hotkey_shortcut_card/conflict", variantName(theme, scale));
}

} // namespace

TEST_CASE("reference screenshot hotkeys_page defaults dark", "[screenshot]") {
    hotkeysPage("dark", 100, false);
}

TEST_CASE("reference screenshot hotkeys_page defaults light", "[screenshot]") {
    hotkeysPage("light", 100, false);
}

TEST_CASE("reference screenshot hotkeys_page defaults dark 130", "[screenshot]") {
    hotkeysPage("dark", 130, false);
}

TEST_CASE("reference screenshot hotkeys_page push-hold dark", "[screenshot]") {
    hotkeysPage("dark", 100, true);
}

TEST_CASE("reference screenshot hotkeys_page push-hold light", "[screenshot]") {
    hotkeysPage("light", 100, true);
}

TEST_CASE("reference screenshot hotkeys_page push-hold dark 130", "[screenshot]") {
    hotkeysPage("dark", 130, true);
}

TEST_CASE("reference screenshot hotkey_shortcut_card conflict dark", "[screenshot]") {
    conflictCard("dark", 100);
}

TEST_CASE("reference screenshot hotkey_shortcut_card conflict light", "[screenshot]") {
    conflictCard("light", 100);
}

TEST_CASE("reference screenshot hotkey_shortcut_card conflict dark 130", "[screenshot]") {
    conflictCard("dark", 130);
}
