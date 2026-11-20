// The Fast judgments page against the references OpenWhisper rendered for it
// (tools/parity/builders/a31.py, tests/reference/settings_fast_judgments_page/<variant>/): the
// page body built inside the Settings window, laid out at the builder's width, then taken out of
// the window, as the builder takes OpenWhisper's out of its dialog. The widget tree first, then
// the pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage, the page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>

#include "fast_judgments_support.h"
#include "screenshot/screenshot.h"
#include "ui_settings/settings_page.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "FastJudgmentsPage";
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

void pageShot(const std::string& theme, int scale, const char* name, const Json& settings) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    FastJudgmentsRig scene(settings);
    fv::SettingsDialog& dialog = scene.dialog();
    fv::SettingsPage* page = dialog.ensurePage(dest::meeting_fast);
    // What the page reads on the worker has arrived.
    auto* judgments = dynamic_cast<fv::FastJudgmentsPage*>(dialog.pageContent(dest::meeting_fast));
    REQUIRE(judgments != nullptr);
    scene.settle(*judgments);
    // The page's tile grids take their columns from the page's width, so the page leaves the
    // window's scroll area first (as the builder takes it out), where a pending layout would
    // give it the viewport's width again.
    QScrollArea* scroll = dialog.pageScroll(dest::meeting_fast);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(dest::meeting_fast));
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

// TypeSafe, Fast judgments off, no key set.
void offNoKey(const std::string& theme, int scale) {
    pageShot(theme, scale, "settings_fast_judgments_page/off-no-key-fiorivoce", Json::object());
}

// "Your text model" on LM Studio, a local server that needs no key, with qwen3-4b, Live checks on.
void textModel(const std::string& theme, int scale) {
    pageShot(theme, scale, "settings_fast_judgments_page/text-model",
             Json{{"typesafe_enabled", true},
                  {"typesafe_provider", "text_model"},
                  {"meeting_llm_provider", "lm_studio"},
                  {"meeting_llm_model", "qwen3-4b"}});
}

} // namespace

TEST_CASE("reference screenshot settings_fast_judgments_page off-no-key dark 100", "[screenshot]") {
    offNoKey("dark", 100);
}

TEST_CASE("reference screenshot settings_fast_judgments_page off-no-key light 100",
          "[screenshot]") {
    offNoKey("light", 100);
}

TEST_CASE("reference screenshot settings_fast_judgments_page off-no-key dark 130", "[screenshot]") {
    offNoKey("dark", 130);
}

TEST_CASE("reference screenshot settings_fast_judgments_page text-model dark 100", "[screenshot]") {
    textModel("dark", 100);
}

TEST_CASE("reference screenshot settings_fast_judgments_page text-model light 100",
          "[screenshot]") {
    textModel("light", 100);
}

TEST_CASE("reference screenshot settings_fast_judgments_page text-model dark 130", "[screenshot]") {
    textModel("dark", 130);
}
