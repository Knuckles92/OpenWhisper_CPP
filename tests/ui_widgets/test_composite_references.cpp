// Every composite widget against the reference OpenWhisper rendered for it (tools/parity/
// builders/a02.py, tests/reference/<widget>/<variant>/<theme>-windows11-dpr1-s<scale>.{png,json}):
// the widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// OpenWhisper's private _RailItem and _ChapterRow are RailItem and ChapterRow here (a leading
// underscore and a capital is a name C++ reserves); checkClasses compares the classes on their
// own with those mapped, and the tree comparison leaves them out.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QAbstractScrollArea>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QScrollBar>
#include <QSize>

#include "app_theme.h"
#include "screenshot/screenshot.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/field_help.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace {

// What a builder of tools/parity/builders/a02.py returns: the widget, whatever must outlive it,
// and the pinned focus.
struct Built {
    std::unique_ptr<QWidget> widget;
    std::unique_ptr<QWidget> owner;
    QWidget* focus = nullptr;
    // The widget to compare, when it is not widget itself.
    QWidget* shown = nullptr;
    // Tree keys left out of the comparison, each with its reason at the builder.
    std::vector<std::string> ignore;
};

std::string mapped(std::string name) {
    if (name == "_RailItem") {
        return "RailItem";
    }
    if (name == "_ChapterRow") {
        return "ChapterRow";
    }
    return name;
}

bool hasPrivateClass(const fv::Json& tree) {
    if (tree.value("class", std::string()).starts_with('_')) {
        return true;
    }
    if (tree.contains("children")) {
        for (const fv::Json& child : tree["children"]) {
            if (hasPrivateClass(child)) {
                return true;
            }
        }
    }
    return false;
}

// The class of every widget in the tree against the reference's.
void checkClasses(const fv::Json& expected, const fv::Json& actual, const std::string& path) {
    const std::string wanted = mapped(expected.value("class", std::string()));
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const fv::Json empty = fv::Json::array();
    const fv::Json& want = expected.contains("children") ? expected["children"] : empty;
    const fv::Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start: 12 px. Qt 6.10 leaves them at
// QWidget's default 100 x 30. They are never drawn, so only the trees differ, and here they are
// given 6.11's sizes (architecture amendments 17 and 21; tests/ui_history does the same). A bar
// already sized is left alone.
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

void check(const std::string& widget, const std::string& theme, int scale,
           const std::function<Built()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    Built built = build();
    QWidget& shown = built.shown != nullptr ? *built.shown : *built.widget;
    sizeHiddenScrollBarsAsQt611(shown);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    fv::screenshot::Options options;
    options.focus = built.focus;
    options.ignore_keys = built.ignore;
    const fv::Json reference = fv::screenshot::load_reference(widget, variant).at("tree");
    if (hasPrivateClass(reference)) {
        // Checked before expect_matches, which ends the test case when it skips the pixels.
        shown.ensurePolished();
        checkClasses(reference, fv::screenshot::dump_tree(shown), widget);
        options.ignore_keys.push_back("class");
    }
    fv::screenshot::expect_matches(shown, widget, variant, options);
}

#define REFERENCE_CASE(name, ...)                                                                  \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", 100, __VA_ARGS__);                                                     \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", 100, __VA_ARGS__);                                                    \
    }

#define REFERENCE_CASE_130(name, ...)                                                              \
    REFERENCE_CASE(name, __VA_ARGS__)                                                              \
    TEST_CASE("reference screenshot " name " dark 130", "[screenshot]") {                          \
        check(name, "dark", 130, __VA_ARGS__);                                                     \
    }

const std::vector<std::pair<QString, QString>> kLooks{
    {QStringLiteral("Ribbon"), QStringLiteral("A glowing trace of your voice")},
    {QStringLiteral("Bars"), QStringLiteral("A scrolling level meter")},
    {QStringLiteral("Mirror wave"), QStringLiteral("Your voice's outline, mirrored")},
    {QStringLiteral("Aurora"), QStringLiteral("Flowing waves")},
    {QStringLiteral("Pulse"), QStringLiteral("A ring that swells and ripples")},
    {QStringLiteral("Dots"), QStringLiteral("Dots that ripple as you speak")},
    {QStringLiteral("Bait ball"), QStringLiteral("A spinning school lifts the swell")},
    {QStringLiteral("Real fish"), QStringLiteral("Fish school under the waves")},
    {QStringLiteral("Deep-sea glow"), QStringLiteral("Glowing fish in the deep")},
    {QStringLiteral("Fizz"), QStringLiteral("Glasses filling with fizz")},
    {QStringLiteral("Classic"), QStringLiteral("The original particles")},
};

} // namespace

REFERENCE_CASE("field_help_card/open", [] {
    auto help = std::make_unique<fv::FieldHelp>(
        QStringLiteral("Model"), QStringLiteral("Model"),
        QStringLiteral("The speech model dictation uses."),
        std::vector<std::pair<QString, QString>>{
            {QStringLiteral(u"Open Settings → Downloads"), QStringLiteral("downloads")}});
    // The caption is its own window here, so showHelp hosts the card on it.
    help->show();
    help->showHelp();
    QWidget* card = help->card();
    // OpenWhisper sets the card's sheet while the card already inherits the caption's
    // stylesheet proxy, so no StyleChange reaches WidgetStyleFilter: its @tokens stay unresolved,
    // Qt drops the colour declarations, and the description takes the application's QLabel
    // colour instead of @text-secondary. Here the sheet is resolved, as it was written to be (the
    // card's sheet and its source properties differ, and the description's pixels:
    // <variant>.tolerance.json).
    return Built{nullptr, std::move(help), nullptr, card, {"properties", "styleSheet"}};
})

REFERENCE_CASE("segmented_bar/looks", [] {
    auto bar = std::make_unique<fv::SegmentedBar>(kLooks);
    bar->setCurrentIndex(1);
    return Built{std::move(bar)};
})

REFERENCE_CASE("segmented_bar/compact", [] {
    return Built{std::make_unique<fv::SegmentedBar>(
        std::vector<std::pair<QString, QString>>{{QStringLiteral("Header"), QString()},
                                                 {QStringLiteral("Footer"), QString()}},
        true)};
})

REFERENCE_CASE_130("setting_tile/toggle", [] {
    auto tile = std::make_unique<fv::SettingTile>(
        QStringLiteral("Paste into the active window"),
        QStringLiteral("Types the transcript where your cursor is once dictation finishes."),
        fv::ui::designIcon(QStringLiteral("bolt-green.svg")));
    tile->checkbox()->setChecked(true);
    return Built{std::move(tile)};
})

REFERENCE_CASE_130("setting_tile/field", [] {
    auto* combo = new fv::NoWheelComboBox;
    combo->addItems({QStringLiteral("Default (100%)"), QStringLiteral("Large (115%)"),
                     QStringLiteral("Extra large (130%)")});
    return Built{std::make_unique<fv::FieldTile>(
        QStringLiteral("Text size"), QStringLiteral("How large text is across the app."), combo,
        fv::ui::designIcon(QStringLiteral("typography-blue.svg")))};
})

REFERENCE_CASE_130("setting_tile/notice", [] {
    auto tile = std::make_unique<fv::InfoTile>(
        QStringLiteral("Microphone access is off"),
        QStringLiteral("Turn on microphone access for desktop apps to record."),
        fv::ui::designIcon(QStringLiteral("microphone-blue.svg")));
    fv::ui::setStyleProperty(tile.get(), fv::ui::prop::kind, QStringLiteral("notice"));
    tile->addActions({new fv::Button(QStringLiteral("Open privacy settings"))});
    return Built{std::move(tile)};
})

REFERENCE_CASE_130("setting_tile/intro", [] {
    auto tile = std::make_unique<fv::InfoTile>(
        QStringLiteral("What's new"),
        QStringLiteral("Live preview shows your words while you speak, and long dictations "
                       "paste as fast as short ones."),
        fv::ui::designIcon(QStringLiteral("wand-purple.svg")));
    fv::ui::setStyleProperty(tile.get(), fv::ui::prop::kind, QStringLiteral("intro"));
    tile->addTrailing(fv::neutralButton(new fv::Button(QStringLiteral("Got it"))));
    return Built{std::move(tile)};
})

REFERENCE_CASE_130("nav_rail/dictation-open", [] {
    // As the Settings window builds it: a copy of appkit/destinations.h's rail (every page, the
    // Runtime one too), which this test target does not link.
    struct Page {
        const char* key;
        const char* name;
    };
    struct Chapter {
        const char* title;
        const char* icon;
        std::vector<Page> pages;
    };
    const std::vector<Chapter> rail{
        {"Dictation",
         "microphone-blue.svg",
         {{"recording", "Recording"},
          {"voice_model", "Voice model"},
          {"cleanup", "AI cleanup"},
          {"remote_engine", "Remote engine"},
          {"walkthroughs", "Walkthroughs"}}},
        {"Personalize",
         "wand-purple.svg",
         {{"dictionary", "Dictionary"},
          {"snippets", "Snippets"},
          {"styles", "Apps & styles"},
          {"cleanup_rules", "Learned rules"},
          {"cleanup_profiles", "Profiles"},
          {"commands", "Commands"}}},
        {"Automate",
         "bolt-green.svg",
         {{"actions", "Actions"}, {"hooks", "Hooks"}, {"mcp", "API & MCP"}}},
        {"Meeting Mode",
         "users-amber.svg",
         {{"meeting_voice", "Voice & speakers"},
          {"meeting_intelligence", "Intelligence"},
          {"meeting_fast", "Fast judgments"},
          {"meeting_after", "After the meeting"},
          {"meeting_dashboard", "Dashboard"}}},
        {"Models & storage",
         "download-blue.svg",
         {{"downloads", "Downloads"}, {"runtime", "Runtime"}, {"backup", "Backup & restore"}}},
        {"App",
         "adjustments-slate.svg",
         {{"general", "General"},
          {"appearance", "Appearance"},
          {"hotkeys", "Hotkeys"},
          {"api_keys", "API keys"},
          {"advanced", "Developer options"}}},
    };
    auto navRail = std::make_unique<fv::NavRail>();
    navRail->addDestination(QStringLiteral("overview"), QStringLiteral("Home"),
                            fv::ui::designIcon(QStringLiteral("home-blue.svg")));
    for (const Chapter& chapter : rail) {
        navRail->addChapter(QString::fromUtf8(chapter.title),
                            fv::ui::designIcon(QString::fromUtf8(chapter.icon)));
        for (const Page& page : chapter.pages) {
            navRail->addDestination(QString::fromUtf8(page.key), QString::fromUtf8(page.name));
        }
    }
    navRail->setValue(QStringLiteral("recording"), QStringLiteral("Auto-paste on"));
    navRail->setValue(QStringLiteral("appearance"), QStringLiteral(u"Dark · Bars"));
    navRail->select(QStringLiteral("recording"));
    return Built{std::move(navRail)};
})

REFERENCE_CASE("searchable_combo/badge", [] {
    auto combo = std::make_unique<fv::SearchableComboBox>();
    combo->addItems({QStringLiteral("gpt-4o-mini"), QStringLiteral("gpt-4o")});
    combo->setBadge(QStringLiteral("Active"));
    return Built{std::move(combo)};
})

REFERENCE_CASE("button_row/three", [] {
    return Built{std::make_unique<fv::ButtonRow>(
        std::vector<QPushButton*>{new fv::Button(QStringLiteral("Open model folder")),
                                  new fv::Button(QStringLiteral("Copy path")),
                                  new fv::Button(QStringLiteral("Show details"))})};
})

REFERENCE_CASE("reflow/stacked", [] {
    return Built{
        std::make_unique<fv::Reflow>(new QLabel(QStringLiteral("Snippets expand as you type")),
                                     new fv::PrimaryButton(QStringLiteral("Add snippet")))};
})
