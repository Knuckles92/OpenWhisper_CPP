// The Apps & styles, Commands and Profiles pages against the references OpenWhisper rendered for
// them (tools/parity/builders/a13.py, tests/reference/settings_styles/, settings_commands/ and
// settings_cleanup_profiles/): each page body built inside the Settings window over the builder's
// settings, with the builder's answers from the computer (no app seen this session, text near
// the cursor readable, a cleanup model ready), laid out at the builder's width, then taken out of
// the window as the builder takes OpenWhisper's out of its dialog. The widget tree first, then
// the pixels. Class names are not compared: the body is OpenWhisper's private _SettingsPage, the
// page's own class here.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QListWidget>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include "screenshot/screenshot.h"
#include "styles_commands_support.h"
#include "text/transforms.h"
#include "ui_settings/settings_page.h"

using fv::Json;
using namespace fvtest;

namespace {

// builders/a11.py's PAGE_WIDTH, which a13's page builders reuse.
constexpr int kPageWidth = 900;

Json stylesSettings() {
    return Json{
        {"transcript_cleanup_enabled", true},
        {"app_context_enabled", true},
        {"app_context_read_text", false},
        {"app_style_tones",
         Json{
             {"email", "formal"}, {"work", "casual"}, {"personal", "formal"}, {"other", "formal"}}},
        {"app_style_overrides", Json::array({Json{{"match", "Notion"}, {"category", "work"}}})},
        {"app_context_excluded_apps", Json::array({"1Password"})}};
}

Json commandsSettings() {
    Json transforms = Json::array();
    for (fv::text::Transform transform : fv::text::starter_transforms()) {
        if (transforms.empty()) {
            transform.hotkey = "ctrl+alt+p";
        }
        transforms.push_back(fv::text::transform_to_json(transform));
    }
    return Json{{"hotkeys", Json{{"command_mode", "ctrl+alt+space"}}},
                {"text_transforms", transforms}};
}

// As test_page_screenshots.cpp: Qt 6.11, the kit the references were rendered with, gives a
// scroll area's hidden scroll bars their thickness from the start; Qt 6.10 leaves them at
// QWidget's default 100 x 30. They are never drawn, so only the trees differ.
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
              const Json& settings, const std::function<void(PersonalizeRig&)>& prepare = {}) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    PersonalizeRig rig(settings);
    fv::SettingsDialog& dialog = rig.window();
    fv::SettingsPage* page = dialog.ensurePage(key);
    REQUIRE(page != nullptr);
    if (prepare) {
        prepare(rig);
    }
    PersonalizeRig::answers();
    // The page's tile grids take their columns from the page's width, so the page leaves the
    // window's scroll area first (as the builder takes it out).
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

void stylesDefault(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::styles, "settings_styles/default", stylesSettings());
}

// "Polish" selected, as the builder selects it.
void commandsTransforms(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::commands, "settings_commands/transforms", commandsSettings(),
             [](PersonalizeRig& rig) { rig.transforms().transformList()->setCurrentRow(0); });
}

// "Support ticket" selected, as the builder selects it.
void profilesStarters(const std::string& theme, int scale) {
    pageShot(theme, scale, dest::cleanup_profiles, "settings_cleanup_profiles/starters",
             Json::object(),
             [](PersonalizeRig& rig) { rig.profilesPanel().profileList()->setCurrentRow(0); });
}

} // namespace

TEST_CASE("reference screenshot settings_styles default dark", "[screenshot]") {
    stylesDefault("dark", 100);
}

TEST_CASE("reference screenshot settings_styles default light", "[screenshot]") {
    stylesDefault("light", 100);
}

TEST_CASE("reference screenshot settings_styles default dark 130", "[screenshot]") {
    stylesDefault("dark", 130);
}

TEST_CASE("reference screenshot settings_commands transforms dark", "[screenshot]") {
    commandsTransforms("dark", 100);
}

TEST_CASE("reference screenshot settings_commands transforms light", "[screenshot]") {
    commandsTransforms("light", 100);
}

TEST_CASE("reference screenshot settings_commands transforms dark 130", "[screenshot]") {
    commandsTransforms("dark", 130);
}

TEST_CASE("reference screenshot settings_cleanup_profiles starters dark", "[screenshot]") {
    profilesStarters("dark", 100);
}

TEST_CASE("reference screenshot settings_cleanup_profiles starters light", "[screenshot]") {
    profilesStarters("light", 100);
}

TEST_CASE("reference screenshot settings_cleanup_profiles starters dark 130", "[screenshot]") {
    profilesStarters("dark", 130);
}
