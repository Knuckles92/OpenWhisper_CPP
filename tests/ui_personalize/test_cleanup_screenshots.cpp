// AI cleanup, its Cleanup model card and Learned rules against the references OpenWhisper
// rendered for them (tools/parity/builders/a11.py settings_cleanup, a15.py cleanup_model_section,
// a13.py settings_cleanup_rules): each page body built inside the Settings window over the
// builder's settings, laid out at the builder's width, then taken out of the window, as the
// builders take OpenWhisper's out of its dialog. OpenRouter's key is found, read off the GUI
// thread as in the app, and the test waits for its line. The widget tree first, then the pixels,
// with the application styled as OpenWhisper styles it. Class names are not compared: the page
// body is OpenWhisper's private _SettingsPage, the page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include "cleanup_support.h"
#include "screenshot/screenshot.h"
#include "ui_personalize/cleanup_model_section.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/agent_picker.h"

using fv::Json;
using namespace fvtest;

namespace {

// builders/a11.py's PAGE_WIDTH and a15.py's SECTION_WIDTH.
constexpr int kPageWidth = 900;
constexpr int kSectionWidth = 680;

// builders/a11.py's CLEANUP_VALUES.
Json cleanupValues() {
    return Json{{"selected_model", "parakeet"},
                {"transcript_cleanup_enabled", true},
                {"transcript_cleanup_level", "medium"},
                {"transcript_cleanup_provider", "openrouter"},
                {"transcript_cleanup_model", "openrouter/free"},
                {"transcript_cleanup_reasoning", "off"}};
}

// Qt 6.11 gives a scroll area's hidden scroll bars their thickness from the start, Qt 6.10 leaves
// them at QWidget's default 100 x 30; they are never drawn, so only the trees differ, and here
// they get 6.11's sizes (as test_page_screenshots.cpp does).
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    for (QAbstractScrollArea* area : root.findChildren<QAbstractScrollArea*>()) {
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

// The builder's measurement: shown at its width, events processed, hidden again.
void settle(QWidget& widget, int width) {
    widget.resize(width, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

// The page body out of the window at kPageWidth, as builders/a11.py's _app_page takes it.
std::unique_ptr<QWidget> takePage(CleanupRig& rig, std::string_view key) {
    fv::SettingsDialog& dialog = rig.window();
    fv::SettingsPage* page = dialog.ensurePage(key);
    REQUIRE(page != nullptr);
    QScrollArea* scroll = dialog.pageScroll(key);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(key));
    REQUIRE(body != nullptr);
    body->setParent(nullptr);
    return body;
}

void expectShot(QWidget& widget, int width, const char* name, const std::string& theme, int scale) {
    settle(widget, width);
    widget.ensurePolished();
    sizeHiddenScrollBarsAsQt611(widget);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(widget, name,
                                   theme + "-windows11-dpr1-s" + std::to_string(scale), options);
}

// OpenRouter's key line, which the page reads off the GUI thread.
void waitForKey(CleanupRig& rig) {
    QLabel* status = rig.picker().requirementLabel();
    REQUIRE(pumpUntil(
        [status] { return status->text() == QStringLiteral("OpenRouter API key found"); }));
}

// builders/a11.py's _without_agents takes OpenWhisper's installed-agent picker (above the card)
// and its model card (inside it) out of the references' trees; both are hidden while an endpoint
// runs cleanup. The port's runner controls and agent model card leave its tree the same way:
// moved under a holder the Settings window deletes, so the section's pointers stay good.
void withoutAgents(CleanupRig& rig) {
    fv::CleanupModelSection& section = rig.model();
    auto* holder = new QWidget;
    section.runnerControls()->setParent(holder);
    section.agentPicker()->modelCard()->setParent(holder);
    QObject::connect(&rig.window(), &QObject::destroyed, [holder] { delete holder; });
}

void cleanupShot(const std::string& theme, int scale, const char* name, const Json& values) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    CleanupRig rig(values);
    rig.key("OPENROUTER_API_KEY", "sk-or-test");
    rig.builtCleanupPage();
    waitForKey(rig);
    withoutAgents(rig);
    std::unique_ptr<QWidget> body = takePage(rig, dest::cleanup);
    expectShot(*body, kPageWidth, name, theme, scale);
}

void mediumShot(const std::string& theme, int scale) {
    cleanupShot(theme, scale, "settings_cleanup/medium", cleanupValues());
}

void customShot(const std::string& theme, int scale) {
    Json values = cleanupValues();
    values["transcript_cleanup_prompt"] = "Rewrite it as a friendly Slack message.";
    cleanupShot(theme, scale, "settings_cleanup/custom", values);
}

void rulesShot(const std::string& theme, int scale) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    CleanupRig rig({{"selected_model", "parakeet"},
                    {"transcript_cleanup_enabled", true},
                    {"transcript_cleanup_level", "medium"},
                    {"transcript_cleanup_rules",
                     Json::array({"Always spell my name \"Alex Rivera\".", "Use British spelling.",
                                  "Keep acronyms uppercase."})}});
    rig.builtRulesPage();
    std::unique_ptr<QWidget> body = takePage(rig, dest::cleanup_rules);
    expectShot(*body, kPageWidth, "settings_cleanup_rules/library", theme, scale);
}

} // namespace

TEST_CASE("reference screenshot settings_cleanup medium dark", "[screenshot]") {
    mediumShot("dark", 100);
}

TEST_CASE("reference screenshot settings_cleanup medium light", "[screenshot]") {
    mediumShot("light", 100);
}

TEST_CASE("reference screenshot settings_cleanup medium dark 130", "[screenshot]") {
    mediumShot("dark", 130);
}

TEST_CASE("reference screenshot settings_cleanup custom dark", "[screenshot]") {
    customShot("dark", 100);
}

TEST_CASE("reference screenshot settings_cleanup custom light", "[screenshot]") {
    customShot("light", 100);
}

TEST_CASE("reference screenshot settings_cleanup custom dark 130", "[screenshot]") {
    customShot("dark", 130);
}

TEST_CASE("reference screenshot cleanup_model_section openrouter dark", "[screenshot]") {
    AppTheme appTheme(QStringLiteral("dark"), 100);
    CleanupRig rig(cleanupValues());
    rig.key("OPENROUTER_API_KEY", "sk-or-test");
    fv::CleanupPage& page = rig.builtCleanupPage();
    waitForKey(rig);
    withoutAgents(rig);
    std::unique_ptr<QWidget> section(page.modelSection());
    section->setParent(nullptr);
    expectShot(*section, kSectionWidth, "cleanup_model_section/openrouter", "dark", 100);
}

TEST_CASE("reference screenshot settings_cleanup_rules library dark", "[screenshot]") {
    rulesShot("dark", 100);
}

TEST_CASE("reference screenshot settings_cleanup_rules library light", "[screenshot]") {
    rulesShot("light", 100);
}

TEST_CASE("reference screenshot settings_cleanup_rules library dark 130", "[screenshot]") {
    rulesShot("dark", 130);
}
