// The Walkthroughs page against the references OpenWhisper rendered for it
// (tools/parity/builders/a11.py, tests/reference/settings_walkthroughs_page/<variant>/): the page
// body built inside the Settings window, laid out at the builder's width, then taken out of the
// window, as the builder takes OpenWhisper's out of its dialog. The widget tree first, then the
// pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, the page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>

#include "screenshot/screenshot.h"
#include "ui_settings/settings_page.h"
#include "walkthroughs_support.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "WalkthroughsPage";
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

// The builder's measurement: shown at its width, events processed, hidden again; a grab lays out
// what the tree dump then reads, as render_reference.py's grab does.
void settle(QWidget& widget) {
    widget.resize(kPageWidth, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

void pageShot(const std::string& theme, int scale, const char* name, const Json& settings,
              fv::agents::ScanResult scan) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    WalkthroughsRig scene(settings, std::move(scan));
    fv::SettingsDialog& dialog = scene.dialog();
    fv::SettingsPage* page = dialog.ensurePage(dest::walkthroughs);
    scene.settle();
    // The page's tile grids take their columns from the page's width, so the page leaves the
    // window's scroll area first (as the builder takes it out), where a pending layout would
    // give it the viewport's width again.
    QScrollArea* scroll = dialog.pageScroll(dest::walkthroughs);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(dest::walkthroughs));
    REQUIRE(body != nullptr);
    body->setParent(nullptr);
    settle(*body);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    body->ensurePolished();
    const Json pins = fv::screenshot::load_reference(name, variant);
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(*body), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*body, name, variant, options);
}

// Every setting at its default, every agent installed.
void defaults(const std::string& theme, int scale) {
    pageShot(theme, scale, "settings_walkthroughs_page/defaults", Json::object(), everyAgent());
}

// The AI write-up on with Codex chosen, which the scan did not find.
void writeUpCodex(const std::string& theme, int scale) {
    pageShot(theme, scale, "settings_walkthroughs_page/write-up-codex",
             Json{{"walkthrough_write_up", true}, {"walkthrough_write_up_agent", "codex"}},
             withoutCodex());
}

} // namespace

TEST_CASE("reference screenshot settings_walkthroughs_page defaults dark 100", "[screenshot]") {
    defaults("dark", 100);
}

TEST_CASE("reference screenshot settings_walkthroughs_page defaults light 100", "[screenshot]") {
    defaults("light", 100);
}

TEST_CASE("reference screenshot settings_walkthroughs_page defaults dark 130", "[screenshot]") {
    defaults("dark", 130);
}

TEST_CASE("reference screenshot settings_walkthroughs_page write-up-codex dark 100",
          "[screenshot]") {
    writeUpCodex("dark", 100);
}

TEST_CASE("reference screenshot settings_walkthroughs_page write-up-codex light 100",
          "[screenshot]") {
    writeUpCodex("light", 100);
}

TEST_CASE("reference screenshot settings_walkthroughs_page write-up-codex dark 130",
          "[screenshot]") {
    writeUpCodex("dark", 130);
}
