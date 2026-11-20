// Settings > Home against the references OpenWhisper rendered for it (tools/parity/builders/a11.py,
// tests/reference/settings_home/<variant>/): Home built inside the Settings window over the
// builder's fixed sources (Parakeet downloaded, 714 MB of speech models, the CPU runtime installed
// and the CUDA one not, no API key, the note not yet seen), then taken out of the window at the
// builder's width, as the builder takes OpenWhisper's out of its dialog. The widget tree first,
// then the pixels, with the application styled as OpenWhisper styles it.
//
// The search palette is OpenWhisper's over a plain host (settings_search_palette/microphone),
// listing what this Settings window's index finds for "mic".
//
// The trees are compared without class names, then the classes on their own: OpenWhisper's
// microphone combo is its private _MicrophoneCombo, its result row _ResultRow and its note a
// plain InfoTile.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>

#include "home_support.h"
#include "screenshot/screenshot.h"
#include "ui_settings/settings_search_palette.h"

using fv::Json;
using fvtest::HomeRig;

namespace {

// The builder's HOME_WIDE and HOME_NARROW.
constexpr int kWideWidth = 1000;
constexpr int kNarrowWidth = 700;
// The builder's PALETTE_SIZE.
constexpr int kPaletteWidth = 900;
constexpr int kPaletteHeight = 520;

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's default
// 100 x 30. They are never drawn, so only the trees differ, and here they are given 6.11's sizes
// (architecture amendment 17, as test_reference_screenshots.cpp does). A bar already sized is
// left alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
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

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_MicrophoneCombo") {
        wanted = "HomeMicrophoneCombo";
    } else if (wanted == "_ResultRow") {
        wanted = "SearchResultRow";
    } else if (wanted == "InfoTile" &&
               expected.value("properties", Json::object()).value("tileId", "") == "whatsNew") {
        wanted = "WhatsNewTile";
    } else if (wanted.starts_with('_')) {
        wanted.erase(0, 1);
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

void homeShot(const std::string& theme, int scale, const char* name, int width) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    // Home's footer reads model_assignments' runtime_summary for the saved CPU build. The M2
    // pages are registered, so Personalize's and Meeting Mode's are not.
    HomeRig rig(Json{{"fv_speech_runtime", "cpu"}},
                {fv::destinations::dictionary, fv::destinations::meeting_voice});
    rig.storage = fv::models::StorageSummary{1, 3, 714'000'000, {{"Parakeet", 714'000'000}}};
    fv::SettingsDialog& dialog = rig.dialog();
    fv::HomePage& home = rig.home();
    home.refreshHome();
    // Home leaves the window's scroll area first (as the builder takes it out), where a pending
    // layout would give it the viewport's width again.
    QScrollArea* scroll = dialog.pageScroll(fv::destinations::overview);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder != nullptr);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    std::unique_ptr<QWidget> body(&home);
    body->setParent(nullptr);
    // The builder's measurement: shown at its width, events processed, hidden again; a grab lays
    // out what the tree dump then reads, as render_reference.py's grab does.
    body->resize(width, 4000);
    body->show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)body->grab();
    body->hide();
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(name, variant);
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(*body), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*body, name, variant, options);
}

// The search palette over a host the builder's size, its index the Settings window's over the
// test's pages, the query "mic".
void paletteShot(const std::string& theme) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), 100);
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    QWidget host;
    host.resize(kPaletteWidth, kPaletteHeight);
    auto* palette =
        new fv::SearchPalette(&host, [&dialog] { return fv::buildSearchIndex(dialog); });
    palette->open(QStringLiteral("mic"));
    // The builder's entries are what this index lists for the query.
    REQUIRE(palette->currentEntries().size() == 2);
    CHECK(palette->currentEntries()[0].title == "Preferred microphone");
    CHECK(palette->currentEntries()[1].title == "Recording");
    // The first run's rows (the query's textChanged, before open's own run) wait for deletion;
    // the builder lets them go before its dump.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const std::string variant = theme + "-windows11-dpr1-s100";
    const Json pins = fv::screenshot::load_reference("settings_search_palette/microphone", variant);
    host.setFixedSize(kPaletteWidth, kPaletteHeight);
    host.show();
    QCoreApplication::processEvents();
    sizeHiddenScrollBarsAsQt611(host);
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(host), "settings_search_palette");
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    options.focus = palette->input();
    fv::screenshot::expect_matches(host, "settings_search_palette/microphone", variant, options);
}

} // namespace

TEST_CASE("reference screenshot settings_search_palette microphone dark", "[screenshot]") {
    paletteShot("dark");
}

TEST_CASE("reference screenshot settings_search_palette microphone light", "[screenshot]") {
    paletteShot("light");
}

TEST_CASE("reference screenshot settings_home wide dark", "[screenshot]") {
    homeShot("dark", 100, "settings_home/wide", kWideWidth);
}

TEST_CASE("reference screenshot settings_home wide light", "[screenshot]") {
    homeShot("light", 100, "settings_home/wide", kWideWidth);
}

TEST_CASE("reference screenshot settings_home wide dark 130", "[screenshot]") {
    homeShot("dark", 130, "settings_home/wide", kWideWidth);
}

TEST_CASE("reference screenshot settings_home narrow dark", "[screenshot]") {
    homeShot("dark", 100, "settings_home/narrow", kNarrowWidth);
}
