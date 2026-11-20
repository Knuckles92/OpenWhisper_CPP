// The Walkthrough tab, its side panel page and the library in the running window
// (tests/test_main_window_compact.py, tests/test_past_walkthroughs.py and
// tests/test_walkthrough_storyboard.py's window cases). Python's MainWindow owns the tab and
// relays its signals; FioriVoce's walkthrough feature registers the tab and connects it, so these
// run over the controller rig with the feature in it.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>

#include "base/utf8.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_geometry.h"
#include "walkthrough/finish.h"
#include "walkthrough/session.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace wt = fv::walkthrough;
using fv::TabbedContentWidget;

namespace {

fvtest::RigOptions walkthroughRig() {
    fvtest::RigOptions options;
    options.walkthrough = true;
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    return options;
}

// A finished walkthrough started at `hour`:`minute` on 2026-10-25, with one screenshot and words.
std::filesystem::path walkthroughAt(const std::filesystem::path& root, int hour, int minute,
                                    const std::string& words) {
    using namespace std::chrono;
    const auto started = local_days{year{2026} / 10 / 25} + hours{hour} + minutes{minute};
    wt::WalkthroughSession::Options options;
    options.started_at = local_seconds(started);
    options.root = root;
    wt::WalkthroughSession session(std::move(options));
    session.add_shot(std::make_shared<wt::FakeShotImage>(120, 80, 0xf84f30), 1.0);
    const auto shots = session.saved_shots();
    // One text per screenshot, then the words after the last.
    const std::vector<std::string> texts{words, ""};
    const auto result =
        wt::build_documents(session, shots, texts, texts, 3.0, {}, fv::Json::object());
    session.end();
    return result.folder;
}

fv::WalkthroughTab* openTab(fvtest::ControllerRig& rig) {
    rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    return qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
}

fv::PastWalkthroughsPanel* pastPanel(fvtest::ControllerRig& rig) {
    auto* sidebar = rig.context().service<fv::HistoryPanel>().sidebar();
    return sidebar != nullptr ? sidebar->findChild<fv::PastWalkthroughsPanel*>() : nullptr;
}

fv::WalkthroughLibraryDialog* shownLibrary(fvtest::ControllerRig& rig) {
    auto* library = rig.window().findChild<fv::WalkthroughLibraryDialog*>();
    return library != nullptr && library->isVisible() ? library : nullptr;
}

QString utf8(const std::filesystem::path& path) {
    return QString::fromStdString(fv::path_to_utf8(path));
}

void settle(int ms = 800) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_the_walkthrough_tab_reaches_the_window
TEST_CASE("the walkthrough tab reaches the window", "[widget]") {
    // Settings needs the HTTP client and the credential store.
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fvtest::RigOptions options = walkthroughRig();
    options.http = &http;
    options.credentials = &credentials;
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    fv::WalkthroughTab* tab = openTab(rig);
    REQUIRE(tab);

    // The side panel's Open library opens the library dialog.
    fv::PastWalkthroughsPanel* past = pastPanel(rig);
    REQUIRE(past);
    past->libraryButton()->click();
    fv::WalkthroughLibraryDialog* library = shownLibrary(rig);
    REQUIRE(library);
    library->close();

    // The settings link opens Settings on the Walkthroughs page.
    tab->settingsLink()->click();
    auto& settings = rig.context().service<fv::SettingsWindow>();
    REQUIRE(settings.dialog());
    CHECK(settings.dialog()->currentDestination() == QStringLiteral("walkthroughs"));
    settings.dialog()->close();

    // Start walkthrough starts one.
    tab->walkthroughButton()->click();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(rig.runtime().recording_walkthrough());
    Q_EMIT tab->cancelRequested();
    CHECK(fvtest::waitUntil([&rig] { return !rig.runtime().is_recording(); }));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_the_walkthrough_tab_lists_walkthroughs_in_the_side_panel
TEST_CASE("the walkthrough tab lists walkthroughs in the side panel", "[widget]") {
    fvtest::ControllerRig rig(walkthroughRig());
    auto* sidebar = rig.context().service<fv::HistoryPanel>().sidebar();
    REQUIRE(sidebar);

    rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    CHECK(sidebar->page() == fv::HistorySidebar::Page::Walkthroughs);
    REQUIRE(rig.window().sidePanelEdgeTab());
    CHECK(rig.window().sidePanelEdgeTab()->toolTip().contains(QStringLiteral("Walkthroughs")));

    rig.window().tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    CHECK(sidebar->page() == fv::HistorySidebar::Page::History);
    CHECK(rig.window().sidePanelEdgeTab()->toolTip().contains(QStringLiteral("History")));
}

// from tests/test_past_walkthroughs.py::test_the_sidebar_shows_walkthroughs_while_their_tab_is_open
TEST_CASE("the sidebar shows walkthroughs while their tab is open", "[widget]") {
    fvtest::ControllerRig rig(walkthroughRig());
    auto* sidebar = rig.context().service<fv::HistoryPanel>().sidebar();
    REQUIRE(sidebar);
    fv::PastWalkthroughsPanel* past = pastPanel(rig);
    REQUIRE(past);

    sidebar->setPage(fv::HistorySidebar::Page::Walkthroughs);
    CHECK(past->isVisibleTo(sidebar));
    past->libraryButton()->click();
    fv::WalkthroughLibraryDialog* library = shownLibrary(rig);
    REQUIRE(library);
    library->close();

    sidebar->setPage(fv::HistorySidebar::Page::History);
    CHECK_FALSE(past->isVisibleTo(sidebar));
}

// from tests/test_past_walkthroughs.py::test_cards_list_newest_first_and_open_in_the_library
// (the cards themselves: tests/ui_walkthrough/test_past_walkthroughs.cpp; here what a card and
// Open library open in the window)
TEST_CASE("past walkthrough cards open in the tab and the library button in the library",
          "[widget]") {
    fvtest::ControllerRig rig(walkthroughRig());
    REQUIRE(rig.startEngine());
    const auto root = wt::captures_root(rig.paths());
    const auto older = walkthroughAt(root, 9, 0, "older one");
    const auto newer = walkthroughAt(root, 10, 30, "newer one");
    fv::PastWalkthroughsPanel* past = pastPanel(rig);
    REQUIRE(past);
    past->refresh();
    REQUIRE(fvtest::waitUntil([past] { return past->cards().size() == 2; }));
    CHECK(past->cards()[0]->summary().folder == newer);
    CHECK(past->cards()[1]->summary().folder == older);

    QTest::mouseClick(past->cards()[1], Qt::LeftButton);
    CHECK(rig.window().tabbedContent()->currentIndex() == TabbedContentWidget::TabWalkthrough);
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    CHECK(fvtest::waitUntil([tab, &older] { return tab->review()->folder() == older; }));
    CHECK_FALSE(shownLibrary(rig));

    QTest::keyClick(past->cards()[0], Qt::Key_Return);
    CHECK(fvtest::waitUntil([tab, &newer] { return tab->review()->folder() == newer; }));

    past->libraryButton()->click();
    CHECK(shownLibrary(rig));
}

// from
// tests/test_walkthrough_storyboard.py::test_a_side_panel_pick_opens_in_the_tab_and_open_library_opens_the_dialog
TEST_CASE("a side panel pick opens in the tab and open library opens the dialog", "[widget]") {
    fvtest::ControllerRig rig(walkthroughRig());
    REQUIRE(rig.startEngine());
    const auto folder = walkthroughAt(wt::captures_root(rig.paths()), 14, 32, "first");
    fv::PastWalkthroughsPanel* past = pastPanel(rig);
    REQUIRE(past);

    Q_EMIT past->walkthroughSelected(utf8(folder));
    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    CHECK(fvtest::waitUntil([tab, &folder] { return tab->review()->folder() == folder; }));
    CHECK_FALSE(shownLibrary(rig));

    Q_EMIT past->walkthroughSelected(QString());
    CHECK(shownLibrary(rig));
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_walkthrough_tab_fits_when_opened_at_minimum_size
TEST_CASE("the walkthrough tab fits when opened at the minimum size", "[widget]") {
    fvtest::ControllerRig rig(walkthroughRig());
    fv::MainWindow& window = rig.window();
    window.maxUsableHeight = [] {
        return 1080;
    };
    window.resize(fv::window_size::kMinWidth, fv::window_size::kMinHeight);
    window.show();
    window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    settle();

    auto* tab = qobject_cast<fv::WalkthroughTab*>(
        rig.context().screens().existing(fv::screen_key::walkthrough));
    REQUIRE(tab);
    // The stage takes the height the user gives the window.
    CHECK(window.isPageElastic(tab));
    QScrollArea* scroll = tab->scrollArea();
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    // The window widens for the column beside the stage.
    CHECK_FALSE(tab->split()->isStacked());
    QWidget* viewport = scroll->viewport();
    const std::vector<QWidget*> controls{
        tab->walkthroughButton(), tab->writeUpSwitch(), tab->agentField(), tab->styleField(),
        tab->copyField(),         tab->settingsLink(),  tab->stage()};
    for (QWidget* widget : controls) {
        const QRect box = widget->rect().translated(widget->mapTo(viewport, QPoint(0, 0)));
        INFO(widget->metaObject()->className());
        CHECK(viewport->rect().contains(box));
    }
}
