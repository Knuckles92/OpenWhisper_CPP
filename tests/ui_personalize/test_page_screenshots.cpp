// The Dictionary and Snippets pages against the references OpenWhisper rendered for them
// (tools/parity/builders/a13.py, tests/reference/settings_dictionary/ and settings_snippets/): each
// page body built inside the Settings window over the builder's settings, laid out at the
// builder's width, then taken out of the window, as the builder takes OpenWhisper's out of its
// dialog. The widget tree first, then the pixels, with the application styled as OpenWhisper
// styles it. Class names are not compared: the body is OpenWhisper's private _SettingsPage, the
// page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QList>
#include <QListWidget>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include "pages_support.h"
#include "screenshot/screenshot.h"
#include "ui_personalize/snippets_panel.h"
#include "ui_settings/settings_page.h"

using fv::Json;
using namespace fvtest;

namespace {

// builders/a11.py's PAGE_WIDTH, which a13's page builders reuse.
constexpr int kPageWidth = 900;

// The builder's settings: Parakeet, AI cleanup on at Medium, its three words and two snippets.
const Json kParakeet{{"selected_model", "parakeet"}};

Json cleanupOn() {
    Json values = kParakeet;
    values["transcript_cleanup_enabled"] = true;
    values["transcript_cleanup_level"] = "medium";
    return values;
}

Json words() {
    Json values = cleanupOn();
    values["dictation_dictionary"] = Json::array({
        Json{{"id", "ksenia"},
             {"term", "Ksenia"},
             {"starred", true},
             {"heard", Json::array({"Ksenya", "Xenia"})},
             {"learned", false},
             {"new", false}},
        Json{{"id", "kubernetes"},
             {"term", "Kubernetes"},
             {"starred", false},
             {"heard", Json::array()},
             {"learned", true},
             {"new", true}},
        Json{{"id", "alex"},
             {"term", "Alex Rivera"},
             {"starred", false},
             {"heard", Json::array()},
             {"learned", true},
             {"new", false},
             {"protected", true}},
    });
    return values;
}

Json snippets() {
    Json values = cleanupOn();
    values["dictation_snippets"] = Json::array({
        Json{{"id", "cal"},
             {"trigger", "my calendar link"},
             {"text", "https://cal.example.com/dana"},
             {"formatted", false}},
        Json{{"id", "sig"},
             {"trigger", "my sign-off"},
             {"text", "**Dana Lee**\nDesigner"},
             {"formatted", true}},
    });
    return values;
}

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start. Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes (as test_screenshots.cpp does for the dialogs). A bar already sized is left alone.
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

void pageShot(const std::string& theme, int scale, std::string_view key, const char* name,
              const Json& settings, const std::function<void(fv::SettingsDialog&)>& prepare = {}) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    PagesRig rig(settings);
    fv::SettingsDialog& dialog = rig.dialog();
    fv::SettingsPage* page = dialog.ensurePage(key);
    REQUIRE(page != nullptr);
    if (prepare) {
        prepare(dialog);
    }
    // The page's tile grids take their columns from the page's width, so the page leaves the
    // window's scroll area first (as the builder takes it out), where a pending layout would
    // give it the viewport's width again.
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
    settle(*body);
    body->ensurePolished();
    sizeHiddenScrollBarsAsQt611(*body);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*body, name, theme + "-windows11-dpr1-s" + std::to_string(scale),
                                   options);
}

void dictionaryWords(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::dictionary, "settings_dictionary/words", words());
}

// "my sign-off" selected, as the builder selects it.
void snippetsEditor(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::snippets, "settings_snippets/editor", snippets(),
             [](fv::SettingsDialog& dialog) {
                 auto* panel = qobject_cast<fv::SnippetsPanel*>(dialog.control("snippets_panel"));
                 REQUIRE(panel != nullptr);
                 panel->snippetList()->setCurrentRow(1);
             });
}

} // namespace

TEST_CASE("reference screenshot settings_dictionary words dark", "[screenshot]") {
    dictionaryWords("dark", 100);
}

TEST_CASE("reference screenshot settings_dictionary words light", "[screenshot]") {
    dictionaryWords("light", 100);
}

TEST_CASE("reference screenshot settings_dictionary words dark 130", "[screenshot]") {
    dictionaryWords("dark", 130);
}

TEST_CASE("reference screenshot settings_dictionary empty-gated dark", "[screenshot]") {
    pageShot("dark", 100, dest::dictionary, "settings_dictionary/empty-gated", kParakeet);
}

TEST_CASE("reference screenshot settings_snippets editor dark", "[screenshot]") {
    snippetsEditor("dark", 100);
}

TEST_CASE("reference screenshot settings_snippets editor light", "[screenshot]") {
    snippetsEditor("light", 100);
}

TEST_CASE("reference screenshot settings_snippets editor dark 130", "[screenshot]") {
    snippetsEditor("dark", 130);
}

TEST_CASE("reference screenshot settings_snippets empty dark", "[screenshot]") {
    pageShot("dark", 100, dest::snippets, "settings_snippets/empty", cleanupOn());
}
