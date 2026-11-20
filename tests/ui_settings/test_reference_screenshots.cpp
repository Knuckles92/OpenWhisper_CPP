// The Settings shell against the references OpenWhisper rendered for it
// (tools/parity/builders/a11.py, tests/reference/settings_dialog/<variant>/): a stand-in page for
// every destination, Recording selected, at 1315x850 and folded into the page menu at 780 px.
// The widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The trees are compared without class names: OpenWhisper's page container and rail rows are
// private classes (_SettingsPage, _RailItem, _ChapterRow), names C++ reserves. checkClasses
// compares the classes on their own with those mapped.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QList>
#include <QRect>
#include <QScrollBar>
#include <QSize>

#include "screenshot/screenshot.h"
#include "settings_support.h"

using fvtest::SettingsRig;

namespace {

constexpr int kScreenMargin = 32;

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes (architecture amendment 17, as tests/ui_history does). A bar already sized is left
// alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
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

// The class of every widget in the tree against the reference's, where OpenWhisper's private
// classes (_SettingsPage, and the rail's _RailItem and _ChapterRow) drop the underscore here.
void checkClasses(const fv::Json& expected, const fv::Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted.starts_with('_')) {
        wanted.erase(0, 1);
    }
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

// The builder's dialog: every destination a stand-in, Recording selected, on the builder's
// screen, so the first-show fit sizes it as it sized OpenWhisper's.
void settingsDialog(const std::string& theme, int scale, const char* name, QRect screen) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    SettingsRig rig;
    rig.registerStandIns();
    fv::SettingsDialog dialog(rig.context());
    dialog.availableScreenRect = [screen] {
        return screen;
    };
    dialog.selectDestination(fvtest::qs(fv::destinations::recording));
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    // Checked first, since expect_matches ends the test case when it skips the pixels; shown
    // as expect_matches shows it, because showing raises the size grip to the end of the
    // children.
    const fv::Json pins = fv::screenshot::load_reference(name, variant);
    dialog.setFixedSize(pins.at("logical_size")[0].get<int>(),
                        pins.at("logical_size")[1].get<int>());
    dialog.show();
    QCoreApplication::processEvents();
    // render_reference.py dumps the tree after its grab, and a grab lays out the hidden pages and
    // rail rows the stack and the rail hold (pending resize events are sent to every child); the
    // tree compared here is taken after one as well.
    (void)dialog.grab();
    // After the show: polishing the dialog before it would lay out a rail the narrow window
    // hides before OpenWhisper's ever laid it out.
    sizeHiddenScrollBarsAsQt611(dialog);
    checkClasses(pins.at("tree"), fv::screenshot::dump_tree(dialog), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(dialog, name, variant, options);
}

void shell(const std::string& theme, int scale) {
    settingsDialog(theme, scale, "settings_dialog/shell", QRect(0, 0, 1920, 1080));
}

void narrow() {
    // The screen's room after the fit's margins is the window itself, which lowers the 940 px
    // minimum to 780.
    settingsDialog("dark", 100, "settings_dialog/narrow",
                   QRect(0, 0, 780 + 2 * kScreenMargin, 850 + 2 * kScreenMargin));
}

} // namespace

TEST_CASE("reference screenshot settings_dialog shell dark", "[screenshot]") {
    shell("dark", 100);
}

TEST_CASE("reference screenshot settings_dialog shell light", "[screenshot]") {
    shell("light", 100);
}

TEST_CASE("reference screenshot settings_dialog shell dark 130", "[screenshot]") {
    shell("dark", 130);
}

TEST_CASE("reference screenshot settings_dialog narrow dark", "[screenshot]") {
    narrow();
}
