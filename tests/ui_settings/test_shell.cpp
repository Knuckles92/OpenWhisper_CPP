// The Settings shell: routing, the chapter rail, the page menu, lazy pages, saving, the rail-value
// batches, keys, reveal and the backup lock (tests/test_settings_unified.py,
// tests/test_settings_general_layout.py, tests/test_desktop_performance.py). Every destination
// registers a stand-in page.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidgetItem>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalSpy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QTest>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "settings_support.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::qs;
using fvtest::SettingsRig;
namespace dest = fv::destinations;

namespace {

// The rail value every stand-in shows, so each row has a value label to click.
void withRailValues(fv::SettingsPageSpec& spec) {
    if (spec.destination != dest::overview) {
        spec.railValue = [](const fv::Json&) {
            return QStringLiteral("A value");
        };
    }
}

} // namespace

// from
// tests/test_settings_unified.py::TestRouting::test_page_transitions_keep_sidebar_rows_over_their_click_targets
TEST_CASE("test_page_transitions_keep_sidebar_rows_over_their_click_targets",
          "[test_settings_unified][widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"), 115);
    SettingsRig rig;
    rig.registerStandIns(withRailValues);
    auto dialog = rig.dialog();
    dialog->show();
    fvtest::wait(50);
    for (std::string_view key :
         {dest::advanced, dest::voice_model, dest::meeting_dashboard, dest::overview,
          dest::cleanup_rules, dest::backup, dest::recording, dest::voice_model,
          dest::meeting_dashboard, dest::meeting_after, dest::api_keys, dest::overview}) {
        dialog->selectDestination(qs(key));
        fvtest::wait(50);
        fv::NavRail* rail = dialog->rail();
        for (const QString& destination : rail->keys()) {
            QListWidgetItem* item = rail->destinationItem(destination);
            if (item->isHidden()) {
                continue;
            }
            const QRect expected = rail->visualItemRect(item);
            const QRect row = rail->destinationRow(destination)->geometry();
            INFO(std::string(key) << " " << destination.toStdString());
            CHECK(row.top() == expected.top());
            CHECK(row.bottom() == expected.bottom());
        }
    }
    dialog->close();
}

// from
// tests/test_settings_unified.py::TestRouting::test_scrolled_sidebar_clicks_open_the_visible_destination
TEST_CASE("test_scrolled_sidebar_clicks_open_the_visible_destination",
          "[test_settings_unified][widget]") {
    const int scale = GENERATE(100, 115, 130);
    const char* target =
        GENERATE("modelManagerRailName", "modelManagerRailValue", "modelManagerRailIcon", "row");
    INFO("scale " << scale << ", " << target);
    fvtest::AppTheme theme(QStringLiteral("dark"), scale);
    SettingsRig rig;
    rig.registerStandIns(withRailValues);
    auto dialog = rig.dialog();
    dialog->show();
    dialog->resize(1430, 850);
    // The App chapter's pages show once one of them is current.
    dialog->selectDestination(qs(dest::general));
    QApplication::processEvents();
    fv::NavRail* rail = dialog->rail();
    const QPoint centre = rail->viewport()->rect().center();
    // One wheel event scrolls at most a page, and at large fonts the rail is more than one page
    // taller than its viewport.
    for (int step = 0; step < 3; ++step) {
        QWheelEvent wheel(QPointF(centre), QPointF(rail->viewport()->mapToGlobal(centre)), QPoint(),
                          QPoint(0, -2400), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(rail->viewport(), &wheel);
        QApplication::processEvents();
    }
    for (std::string_view key : {dest::hotkeys, dest::api_keys, dest::advanced}) {
        QWidget* row = rail->destinationRow(qs(key));
        REQUIRE(row != nullptr);
        QWidget* label = row;
        if (std::string_view(target) != "row") {
            label = row->findChild<QWidget*>(QString::fromLatin1(target));
            REQUIRE(label != nullptr);
        }
        const QPoint point = label->mapTo(rail->viewport(), label->rect().center());
        CHECK(rail->viewport()->rect().contains(point));
        CHECK(rail->itemAt(point) == rail->destinationItem(qs(key)));
        QTest::mouseClick(dialog->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          label->mapTo(dialog.get(), label->rect().center()));
        QApplication::processEvents();
        CHECK(rail->currentKey() == qs(key));
        CHECK(dialog->stack()->currentWidget() == dialog->pageScroll(key));
        CHECK(dialog->pageTitleLabel()->text() == dialog->pageTitle(key));
    }
    dialog->close();
}

// from tests/test_settings_unified.py::TestRouting::test_window_opens_on_home
TEST_CASE("test_window_opens_on_home", "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    CHECK(dialog->rail()->currentKey() == qs(dest::overview));
    CHECK(dialog->pageTitleLabel()->text() == QStringLiteral("Home"));
    CHECK(dialog->rail()->name(qs(dest::overview)) == QStringLiteral("Home"));
    CHECK(dialog->rail()->value(qs(dest::overview)).isEmpty());
}

TEST_CASE("without Home the window opens on the first registered page", "[widget]") {
    SettingsRig rig;
    for (const fvtest::Heading& heading : fvtest::headings()) {
        if (heading.key == dest::recording || heading.key == dest::general) {
            rig.registry().addPage(fvtest::standInSpec(heading));
        }
    }
    auto dialog = rig.dialog();
    CHECK(dialog->rail()->currentKey() == qs(dest::recording));
    CHECK(dialog->rail()->chapters() ==
          QStringList{QStringLiteral("Dictation"), QStringLiteral("App")});
    CHECK(dialog->pageKeys() == std::vector<std::string>{"recording", "general"});
    dialog->selectDestination(qs(dest::cleanup));
    CHECK(dialog->rail()->currentKey() == qs(dest::recording));
}

TEST_CASE("show_home returns to the top of Home", "[test_settings_unified][widget]") {
    // from tests/test_settings_unified.py::TestRouting::test_show_home_returns_to_the_top_of_home
    SettingsRig rig;
    rig.registerStandIns([](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::overview) {
            spec.create = [](QWidget* parent) {
                return fvtest::fillStandIn(parent, 30);
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->show();
    QApplication::processEvents();
    QScrollBar* bar = dialog->pageScroll(dest::overview)->verticalScrollBar();
    bar->setValue(bar->maximum());
    dialog->selectDestination(qs(dest::mcp));
    dialog->showHome();
    CHECK(dialog->rail()->currentKey() == qs(dest::overview));
    CHECK(bar->value() == 0);
    dialog->close();
}

// from
// tests/test_settings_unified.py::TestRouting::test_legacy_model_manager_names_land_on_a_destination
TEST_CASE("test_legacy_model_manager_names_land_on_a_destination",
          "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    const std::vector<std::pair<std::string_view, std::string_view>> aliases{
        {"ondemand", dest::voice_model},  {"text", dest::cleanup},
        {"meeting", dest::meeting_voice}, {"runtime", dest::runtime},
        {"downloads", dest::downloads},   {"library", dest::downloads},
        {"voice", dest::downloads},       {"engine_downloads", dest::downloads},
        {dest::general, dest::general},
    };
    for (const auto& [alias, destination] : aliases) {
        INFO(alias);
        dialog->selectDestination(qs(dest::overview));
        CHECK(dialog->rail()->currentKey() == qs(dest::overview));
        CHECK(dest::resolve_destination(alias) == destination);
        dialog->selectDestination(qs(alias));
        CHECK(dialog->rail()->currentKey() == qs(destination));
    }
}

TEST_CASE("unknown names are ignored and Home's sections reveal its cards", "[widget]") {
    SettingsRig rig;
    QPointer<fv::InfoTile> card;
    rig.registerStandIns([&card](fv::SettingsPageSpec& spec) {
        if (spec.destination != dest::overview) {
            return;
        }
        spec.create = [&card](QWidget* parent) -> QWidget* {
            struct Home : QWidget, fv::SettingsPageModule {
                explicit Home(QWidget* parent) : QWidget(parent) {
                    auto* layout = new QVBoxLayout(this);
                    tile = new fv::InfoTile(QStringLiteral("Dictation"), QString());
                    layout->addWidget(tile);
                }
                QWidget* namedControl(std::string_view name) override {
                    return name == dest::basic_dictation ? tile : nullptr;
                }
                fv::InfoTile* tile;
            };
            auto* home = new Home(parent);
            card = home->tile;
            return home;
        };
    });
    auto dialog = rig.dialog();
    dialog->selectDestination(qs(dest::general));
    dialog->selectDestination(QStringLiteral("no_such_page"));
    CHECK(dialog->rail()->currentKey() == qs(dest::general));

    dialog->selectDestination(qs(dest::basic_dictation));
    CHECK(dialog->rail()->currentKey() == qs(dest::overview));
    QApplication::processEvents();
    REQUIRE(card != nullptr);
    CHECK(card->property("searchHit").toBool());
}

// from
// tests/test_settings_unified.py::TestChapters::test_going_to_a_page_opens_its_chapter_and_marks_it
TEST_CASE("test_going_to_a_page_opens_its_chapter_and_marks_it",
          "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    fv::NavRail* rail = dialog->rail();
    CHECK(rail->activeChapter().isEmpty());
    dialog->selectDestination(qs(dest::cleanup));
    CHECK(rail->activeChapter() == QStringLiteral("Dictation"));
    CHECK_FALSE(rail->destinationItem(qs(dest::cleanup))->isHidden());
    CHECK(rail->destinationItem(qs(dest::mcp))->isHidden());
    dialog->selectDestination(qs(dest::mcp));
    CHECK(rail->activeChapter() == QStringLiteral("Automate"));
    // The chapter you came from stays open, so nothing above shifts.
    CHECK_FALSE(rail->destinationItem(qs(dest::cleanup))->isHidden());
}

// from tests/test_settings_unified.py::TestChapters::test_the_crumb_names_the_chapter
TEST_CASE("test_the_crumb_names_the_chapter", "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    CHECK(dialog->pageCrumb()->text() == QStringLiteral("SETTINGS"));
    dialog->selectDestination(qs(dest::meeting_fast));
    CHECK(dialog->pageCrumb()->text() == QStringLiteral("MEETING MODE"));
    CHECK(dialog->pageTitleLabel()->text() == QStringLiteral("Fast judgments"));
    CHECK(dialog->pageSubtitleLabel()->text() ==
          QStringLiteral("Optional checks by Jev or your own text model. Each feature describes "
                         "the text it shares."));
}

// from tests/test_settings_unified.py::TestChapters::test_every_page_is_in_exactly_one_place
TEST_CASE("test_every_page_is_in_exactly_one_place", "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    fv::NavRail* rail = dialog->rail();
    const QStringList keys = rail->keys();
    REQUIRE_FALSE(keys.isEmpty());
    CHECK(keys.first() == qs(dest::overview));
    CHECK(rail->chapter(qs(dest::overview)).isEmpty());
    QStringList chaptered;
    for (const QString& chapter : rail->chapters()) {
        chaptered += rail->chapterKeys(chapter);
    }
    QStringList rest = keys.mid(1);
    chaptered.sort();
    rest.sort();
    CHECK(chaptered == rest);
    for (const fvtest::Heading& heading : fvtest::headings()) {
        CHECK(dialog->isRegistered(heading.key));
        CHECK(keys.contains(qs(heading.key)));
    }
    CHECK(static_cast<std::size_t>(keys.size()) == fvtest::headings().size());
}

// from tests/test_settings_unified.py::TestChapters::test_the_page_menu_lists_every_page_by_chapter
TEST_CASE("test_the_page_menu_lists_every_page_by_chapter", "[test_settings_unified][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    dialog->selectDestination(qs(dest::hooks));
    QMenu* menu = dialog->showPageMenu();
    REQUIRE(menu == dialog->pageMenu());
    CHECK(menu->objectName() == QStringLiteral("settingsPageMenuList"));
    QStringList texts;
    QStringList checked;
    std::vector<QAction*> headings;
    for (QAction* action : menu->actions()) {
        if (action->isSeparator()) {
            continue;
        }
        texts.append(action->text());
        if (action->isChecked()) {
            checked.append(action->text());
        }
        if (dialog->rail()->chapters().contains(action->text())) {
            headings.push_back(action);
        }
    }
    REQUIRE_FALSE(texts.isEmpty());
    CHECK(texts.first() == QStringLiteral("Home"));
    const qsizetype automate = texts.indexOf(QStringLiteral("Automate"));
    CHECK(texts.mid(automate + 1, 3) == QStringList{QStringLiteral("Actions"),
                                                    QStringLiteral("Hooks"),
                                                    QStringLiteral("API & MCP")});
    CHECK(checked == QStringList{QStringLiteral("Hooks")});
    REQUIRE_FALSE(headings.empty());
    for (QAction* heading : headings) {
        CHECK_FALSE(heading->isEnabled());
    }
    for (QAction* action : menu->actions()) {
        if (action->text() == QStringLiteral("Runtime")) {
            action->trigger();
        }
    }
    CHECK(dialog->rail()->currentKey() == qs(dest::runtime));
    menu->close();
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_uses_rail_and_independently_scrollable_pages
TEST_CASE("test_uses_rail_and_independently_scrollable_pages",
          "[test_settings_general_layout][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    dialog->resize(fv::SettingsDialog::kDefaultSize);
    dialog->ensureAllPages();
    // Chapters by feature, under the Home landing page.
    CHECK(dialog->pageKeys() == std::vector<std::string>{"overview",
                                                         "recording",
                                                         "voice_model",
                                                         "cleanup",
                                                         "remote_engine",
                                                         "walkthroughs",
                                                         "dictionary",
                                                         "snippets",
                                                         "styles",
                                                         "cleanup_rules",
                                                         "cleanup_profiles",
                                                         "commands",
                                                         "actions",
                                                         "hooks",
                                                         "mcp",
                                                         "meeting_voice",
                                                         "meeting_intelligence",
                                                         "meeting_fast",
                                                         "meeting_after",
                                                         "meeting_dashboard",
                                                         "downloads",
                                                         "runtime",
                                                         "backup",
                                                         "general",
                                                         "appearance",
                                                         "hotkeys",
                                                         "api_keys",
                                                         "advanced"});
    CHECK(dialog->rail()->currentKey() == qs(dest::overview));
    for (const std::string& key : dialog->pageKeys()) {
        INFO(key);
        if (key == dest::downloads) {
            // Its catalog list is the page's own scroller.
            CHECK(dialog->pageScroll(key) == nullptr);
            CHECK(dialog->page(key)->parentWidget() == dialog->stack());
            continue;
        }
        QScrollArea* scroll = dialog->pageScroll(key);
        REQUIRE(scroll != nullptr);
        CHECK(scroll->widget() == dialog->page(key));
        CHECK(scroll->widgetResizable());
        CHECK(scroll->objectName() == QStringLiteral("settingsPageScroll"));
        CHECK(scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
        CHECK(dialog->page(key)->objectName() == QStringLiteral("settingsPage_%1").arg(qs(key)));
    }
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_pages_scroll_without_forcing_the_window_taller
TEST_CASE("test_pages_scroll_without_forcing_the_window_taller",
          "[test_settings_general_layout][widget]") {
    fvtest::AppTheme theme;
    SettingsRig rig;
    // General is long enough to scroll at the smaller sizes.
    rig.registerStandIns([](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.create = [](QWidget* parent) {
                return fvtest::fillStandIn(parent, 10);
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->ensureAllPages();
    dialog->show();
    fvtest::processEvents(8);
    CHECK(dialog->size() == fv::SettingsDialog::kDefaultSize);
    QPushButton* close = dialog->closeButton();
    for (const QSize size : {QSize(980, 560), fv::SettingsDialog::kMinimumSize}) {
        dialog->resize(size);
        for (const QString& key : dialog->rail()->keys()) {
            dialog->rail()->select(key);
            fvtest::processEvents(8);
            INFO(size.width() << "x" << size.height() << " " << key.toStdString());
            CHECK(dialog->size() == size);
            CHECK(dialog->minimumSize() == fv::SettingsDialog::kMinimumSize);
            fv::SettingsPage* page = dialog->page(fv::qt::toUtf8(key));
            QScrollArea* scroll = dialog->pageScroll(fv::qt::toUtf8(key));
            if (scroll == nullptr) {
                // Downloads fills the stack and scrolls its list.
                CHECK(page->height() >= page->minimumSizeHint().height());
                CHECK(page->geometry().bottom() <= dialog->stack()->height());
                continue;
            }
            CHECK(page->width() == scroll->viewport()->width());
            CHECK(page->height() >= page->minimumSizeHint().height());
            CHECK(dialog->rect().contains(close->mapTo(dialog.get(), close->rect().bottomRight())));
            QScrollBar* bar = scroll->verticalScrollBar();
            bar->setValue(bar->maximum());
            QApplication::processEvents();
            CHECK(page->mapTo(scroll->viewport(), page->rect().bottomRight()).y() <=
                  scroll->viewport()->height());
        }
    }
    CHECK(dialog->pageScroll(dest::general)->verticalScrollBar()->maximum() > 0);
    // Only the current chapter's pages show, but a short window can still scroll the rail: the
    // current page stays in view.
    const QRect item = dialog->rail()->visualItemRect(dialog->rail()->currentItem());
    CHECK(dialog->rail()->viewport()->rect().contains(item));
    dialog->close();
}

// from tests/test_settings_general_layout.py::test_settings_chevrons_use_the_drawing_for_the_theme
TEST_CASE("test_settings_chevrons_use_the_drawing_for_the_theme",
          "[test_settings_general_layout][widget]") {
    const QString themeName = GENERATE(QStringLiteral("dark"), QStringLiteral("light"));
    fvtest::AppTheme theme(themeName);
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    const QString suffix = fv::ui::currentPalette().qCss(u"icon-suffix");
    const QImage wanted = fv::ui::designIcon(QStringLiteral("chevron-down-slate%1.svg").arg(suffix))
                              .pixmap(16, 16)
                              .toImage();
    CHECK(dialog->pageMenuButton()->icon().pixmap(16, 16).toImage() == wanted);
    const QString other =
        fv::ui::paletteFor(themeName == QStringLiteral("dark") ? QStringLiteral("light")
                                                               : QStringLiteral("dark"))
            .qCss(u"icon-suffix");
    if (other != suffix) {
        CHECK(dialog->pageMenuButton()->icon().pixmap(16, 16).toImage() !=
              fv::ui::designIcon(QStringLiteral("chevron-down-slate%1.svg").arg(other))
                  .pixmap(16, 16)
                  .toImage());
    }
}

// from
// tests/test_desktop_performance.py::test_settings_read_cache_isolated_nested_values_and_external_changes
TEST_CASE("test_settings_read_cache_isolated_nested_values_and_external_changes",
          "[test_desktop_performance][widget]") {
    SettingsRig rig(fv::Json{{"nested", {{"items", {1}}}}});
    std::vector<fv::Json> loads;
    rig.registerStandIns([&loads](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.railValue = [&loads](const fv::Json& settings) {
                loads.push_back(settings);
                return QStringLiteral("Manual");
            };
        }
    });
    auto dialog = rig.dialog();
    // One read of the file per refresh, however many readers the snapshot serves.
    rig.settings().set_contents(fv::Json{{"nested", {{"items", {1}}}}});
    const std::size_t reads = rig.settings().file().reads();
    loads.clear();
    dialog->refresh();
    CHECK(rig.settings().file().reads() == reads + 1);
    REQUIRE(loads.size() == 1);
    // The copy a page reads is its own.
    fv::Json copy = loads.front();
    copy["nested"]["items"].push_back(2);
    CHECK(*dialog->settingsSnapshot() == fv::Json{{"nested", {{"items", {1}}}}});
    // An edit from outside is seen by the next refresh.
    rig.settings().set_contents(fv::Json{{"external", true}});
    loads.clear();
    dialog->refresh();
    REQUIRE(loads.size() == 1);
    CHECK(loads.front() == fv::Json{{"external", true}});
}

// from
// tests/test_settings_unified.py::TestWorkers::test_a_window_closed_mid_work_is_not_held_by_the_worker
TEST_CASE("test_a_window_closed_mid_work_is_not_held_by_the_worker",
          "[test_settings_unified][widget]") {
    // The shell's stand-in for a page's worker (a key check, a rule polish): it delivers to the
    // page through qt::post, never holding the window.
    SettingsRig rig;
    std::atomic_bool release{false};
    std::atomic_bool delivered{false};
    std::thread worker;
    // Released here, on the GUI thread, after the worker is done with it.
    std::shared_ptr<fv::qt::QtUiExecutor> delivery;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination != dest::api_keys) {
            return;
        }
        spec.create = [&](QWidget* parent) -> QWidget* {
            QWidget* page = fvtest::fillStandIn(parent);
            // Delivery goes through an executor on the page: it drops a task whose page is gone.
            delivery = std::make_shared<fv::qt::QtUiExecutor>(page);
            worker = std::thread([&release, &delivered, delivery] {
                while (!release) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                delivery->post([&delivered] { delivered = true; });
            });
            return page;
        };
    });
    auto dialog = rig.dialog();
    dialog->focusApiKeys(QStringLiteral("OPENAI_API_KEY"));
    REQUIRE(worker.joinable());
    QPointer<fv::SettingsDialog> watched = dialog.get();
    dialog.reset();
    CHECK(watched == nullptr);
    release = true;
    worker.join();
    fvtest::processEvents(3);
    CHECK_FALSE(delivered);
}

// --- Saving ---

TEST_CASE("persist saves, skips an equal stored value and reports a failure", "[widget]") {
    SettingsRig rig(fv::Json{{"auto_paste", 1}});
    int railReads = 0;
    rig.registerStandIns([&railReads](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.railValue = [&railReads](const fv::Json&) {
                ++railReads;
                return QString();
            };
        }
    });
    auto dialog = rig.dialog();
    const std::size_t writes = rig.settings().file().writes();

    // Python's ==: True equals the stored 1, so only the rail is redrawn.
    int before = railReads;
    CHECK(dialog->persist("auto_paste", true));
    CHECK(rig.settings().file().writes() == writes);
    CHECK(railReads == before + 1);

    CHECK(dialog->persist("copy_clipboard", false, QStringLiteral("Saved.")));
    CHECK(rig.settings().contents()["copy_clipboard"] == false);
    CHECK(dialog->messageText() == QStringLiteral("Saved."));

    rig.settings().file().fail_next_replace("Disk full");
    CHECK_FALSE(dialog->persist("copy_clipboard", true));
    CHECK(dialog->messageText() == QStringLiteral("Couldn't save setting: Disk full"));
    CHECK(rig.settings().contents()["copy_clipboard"] == false);

    rig.settings().file().fail_next_replace("Disk full");
    CHECK_FALSE(dialog->persistMany(fv::Json{{"copy_clipboard", true}}, {"auto_paste"}));
    CHECK(dialog->messageText() == QStringLiteral("Couldn't save settings: Disk full"));
    CHECK(dialog->persistMany(fv::Json{{"copy_clipboard", true}}, {"auto_paste"}));
    CHECK(dialog->messageText().isEmpty());
    CHECK(rig.settings().contents() == fv::Json{{"copy_clipboard", true}});
}

TEST_CASE("nothing a page does while it loads is saved", "[widget]") {
    SettingsRig rig(fv::Json{{"auto_paste", false}});
    QPointer<fv::SettingTile> tile;
    bool savedWhileLoading = true;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination != dest::general) {
            return;
        }
        spec.create = [&](QWidget* parent) -> QWidget* {
            auto* page = qobject_cast<fv::SettingsPage*>(parent);
            tile = new fv::SettingTile(QStringLiteral("Auto-paste"), QString());
            page->pageLayout()->addWidget(tile);
            page->dialog()->bindings().checkbox(
                tile->checkbox(), "auto_paste",
                [](const fv::Json& settings) { return settings.value("auto_paste", true); });
            savedWhileLoading = page->dialog()->persist("auto_paste", true);
            return page;
        };
    });
    auto dialog = rig.dialog();
    const std::size_t writes = rig.settings().file().writes();
    dialog->selectDestination(qs(dest::general));
    CHECK_FALSE(savedWhileLoading);
    REQUIRE(tile != nullptr);
    CHECK_FALSE(tile->checkbox()->isChecked());
    CHECK(rig.settings().file().writes() == writes);
    CHECK(dialog->isPageLoading(dest::general));
    CHECK_FALSE(dialog->isPageLoading(dest::recording));
}

TEST_CASE("a page built while another builds loads only its own bindings", "[widget]") {
    // ensure_page's re-entrancy: the outer page's create builds the inner page, which loads only
    // the bindings it adds; the outer page's bindings load once, when it is done.
    SettingsRig rig;
    std::vector<std::string> loads;
    rig.registerStandIns([&loads](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.create = [&loads](QWidget* parent) -> QWidget* {
                auto* page = qobject_cast<fv::SettingsPage*>(parent);
                fv::SettingsDialog* dialog = page->dialog();
                dialog->bindings().custom(
                    [&loads](const fv::Json&) {
                        loads.emplace_back("general");
                        return fv::Json();
                    },
                    [](const fv::Json&) {});
                CHECK(dialog->ensurePage(dest::general) == page); // being built: no rebuild
                dialog->ensurePage(dest::appearance);
                CHECK(dialog->isLoading());
                return page;
            };
        }
        if (spec.destination == dest::appearance) {
            spec.create = [&loads](QWidget* parent) -> QWidget* {
                auto* page = qobject_cast<fv::SettingsPage*>(parent);
                page->dialog()->bindings().custom(
                    [&loads](const fv::Json&) {
                        loads.emplace_back("appearance");
                        return fv::Json();
                    },
                    [](const fv::Json&) {});
                return page;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->ensurePage(dest::general);
    CHECK(loads == std::vector<std::string>{"appearance", "general", "appearance"});
    CHECK_FALSE(dialog->isLoading());
    CHECK(dialog->isBuilt(dest::appearance));
    loads.clear();
    dialog->refresh();
    CHECK(loads == std::vector<std::string>{"general", "appearance"});
}

TEST_CASE("a page that cannot be built is shown empty from the rail and the page menu",
          "[widget]") {
    SettingsRig rig;
    int builds = 0;
    rig.registerStandIns([&builds](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.create = [&builds](QWidget*) -> QWidget* {
                ++builds;
                throw std::runtime_error("broken page");
            };
        }
    });
    auto dialog = rig.dialog();
    // Asked directly, the error is the caller's.
    CHECK_THROWS_AS(dialog->selectDestination(qs(dest::general)), std::runtime_error);
    CHECK(dialog->currentDestination() == qs(dest::overview));
    // The rail and the page menu are Qt slots: the page shows empty and nothing escapes.
    dialog->rail()->select(qs(dest::general));
    CHECK(dialog->currentDestination() == qs(dest::general));
    CHECK(dialog->pageTitleLabel()->text() == QStringLiteral("General"));
    CHECK_FALSE(dialog->isBuilt(dest::general));
    dialog->selectDestination(qs(dest::appearance));
    QMenu* menu = dialog->showPageMenu();
    for (QAction* action : menu->actions()) {
        if (action->text() == QStringLiteral("General")) {
            action->trigger();
        }
    }
    menu->close();
    CHECK(builds == 3);
    CHECK(dialog->currentDestination() == qs(dest::appearance));
    // The escape override is a key handler's too.
    dialog->setEscapeOverride([] { throw std::runtime_error("broken capture"); });
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &escape);
    CHECK(escape.isAccepted());
}

TEST_CASE("a page's content already placed in a row of the page stays there", "[widget]") {
    SettingsRig rig;
    QPointer<QWidget> content;
    QPointer<QHBoxLayout> row;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.create = [&](QWidget* parent) -> QWidget* {
                auto* page = qobject_cast<fv::SettingsPage*>(parent);
                row = new QHBoxLayout;
                page->pageLayout()->addLayout(row);
                content = new QWidget;
                row->addWidget(content);
                return content;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->selectDestination(qs(dest::general));
    REQUIRE(content != nullptr);
    REQUIRE(row != nullptr);
    CHECK(row->indexOf(content) == 0);
    CHECK(dialog->page(dest::general)->pageLayout()->indexOf(content) < 0);
    CHECK(dialog->pageContent(dest::general) == content);
}

TEST_CASE("an unregistered page cannot be built", "[widget]") {
    SettingsRig rig;
    auto dialog = rig.dialog();
    CHECK_THROWS_AS(dialog->ensurePage(dest::general), std::invalid_argument);
}

// --- Rail values ---

TEST_CASE("rail values are drawn once per batch", "[widget]") {
    SettingsRig rig;
    int railReads = 0;
    rig.registerStandIns([&railReads](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.railValue = [&railReads](const fv::Json& settings) {
                ++railReads;
                return settings.value("auto_paste", true) ? QStringLiteral("Auto-paste on")
                                                          : QStringLiteral("Manual");
            };
        }
    });
    auto dialog = rig.dialog();
    CHECK(dialog->rail()->value(qs(dest::general)) == QStringLiteral("Auto-paste on"));
    const int before = railReads;
    dialog->beginRailBatch();
    dialog->beginRailBatch();
    dialog->persist("auto_paste", false);
    dialog->refreshRailValues();
    dialog->endRailBatch();
    CHECK(railReads == before);
    dialog->endRailBatch();
    CHECK(railReads == before + 1);
    CHECK(dialog->rail()->value(qs(dest::general)) == QStringLiteral("Manual"));

    railReads = 0;
    dialog->refresh();
    CHECK(railReads == 1);
}

TEST_CASE("Home is refreshed once per refresh, while it is shown", "[widget]") {
    SettingsRig rig;
    int homeRefreshes = 0;
    rig.registerStandIns([&homeRefreshes](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::overview) {
            spec.create = [&homeRefreshes](QWidget* parent) -> QWidget* {
                struct Home : QWidget, fv::SettingsPageModule {
                    Home(QWidget* parent, int& count) : QWidget(parent), count_(count) {}
                    void refreshPage() override { ++count_; }
                    int& count_;
                };
                return new Home(parent, homeRefreshes);
            };
        }
    });
    auto dialog = rig.dialog();
    // from tests/test_settings_unified.py::TestRouting::test_refresh_rebuilds_home_once
    homeRefreshes = 0;
    dialog->refresh();
    CHECK(homeRefreshes == 1);
    dialog->selectDestination(qs(dest::general));
    homeRefreshes = 0;
    dialog->refresh();
    CHECK(homeRefreshes == 0);
}

// --- Keys and closing ---

TEST_CASE("Enter is swallowed and Esc closes unless a capture is held", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    dialog->show();
    QApplication::processEvents();
    QSignalSpy closed(dialog.get(), &fv::SettingsDialog::closed);
    QSignalSpy rejected(dialog.get(), &QDialog::rejected);

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &enter);
    CHECK(enter.isAccepted());
    CHECK(dialog->isVisible());

    int cancelled = 0;
    dialog->setEscapeOverride([&cancelled] { ++cancelled; });
    QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &override);
    CHECK(override.isAccepted());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &escape);
    CHECK(cancelled == 1);
    CHECK(dialog->isVisible());
    CHECK(rejected.isEmpty());

    dialog->setEscapeOverride({});
    QKeyEvent second(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog.get(), &second);
    CHECK_FALSE(dialog->isVisible());
    CHECK(rejected.count() == 1);
    CHECK(closed.count() == 1);

    dialog->show();
    QApplication::processEvents();
    dialog->closeButton()->click();
    CHECK_FALSE(dialog->isVisible());
    CHECK(closed.count() == 2);
}

TEST_CASE("a draft that cannot be saved yet keeps Settings open on its page", "[widget]") {
    SettingsRig rig;
    struct Drafts : QWidget, fv::SettingsPageModule {
        using QWidget::QWidget;
        bool saveDrafts(bool ask) override {
            asked.push_back(ask);
            return !keep;
        }
        bool keep = false;
        std::vector<bool> asked;
    };
    QPointer<Drafts> drafts;
    rig.registerStandIns([&drafts](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::snippets) {
            spec.create = [&drafts](QWidget* parent) -> QWidget* {
                drafts = new Drafts(parent);
                return drafts;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->show();
    dialog->selectDestination(qs(dest::snippets));
    REQUIRE(drafts != nullptr);
    dialog->selectDestination(qs(dest::general));
    CHECK(drafts->asked == std::vector<bool>{false});

    drafts->keep = true;
    dialog->close();
    CHECK(dialog->isVisible());
    CHECK(dialog->rail()->currentKey() == qs(dest::snippets));
    CHECK(drafts->asked.back());

    drafts->keep = false;
    dialog->close();
    CHECK_FALSE(dialog->isVisible());
}

// --- Backup lock, reveal, search ---

TEST_CASE("a running backup keeps Settings on Backup & restore", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    int searches = 0;
    int closes = 0;
    dialog->setSearchOpener([&searches](const QString&) { ++searches; });
    dialog->setSearchCloser([&closes] { ++closes; });
    dialog->openSearch();
    CHECK(searches == 1);

    dialog->setBackupBusy(true);
    CHECK(dialog->rail()->currentKey() == qs(dest::backup));
    CHECK_FALSE(dialog->rail()->isEnabled());
    CHECK_FALSE(dialog->searchButton()->isEnabled());
    CHECK_FALSE(dialog->pageMenuButton()->isEnabled());
    CHECK(closes == 1);
    dialog->selectDestination(qs(dest::general));
    CHECK(dialog->rail()->currentKey() == qs(dest::backup));
    dialog->rail()->select(qs(dest::general));
    CHECK(dialog->rail()->currentKey() == qs(dest::backup));
    dialog->openSearch(QStringLiteral("theme"));
    CHECK(searches == 1);

    dialog->setBackupBusy(false);
    CHECK(dialog->rail()->isEnabled());
    dialog->selectDestination(qs(dest::general));
    CHECK(dialog->rail()->currentKey() == qs(dest::general));
    dialog->searchButton()->click();
    CHECK(searches == 2);
}

TEST_CASE("a reveal tints the control's card for 1.6 s and focuses it", "[widget]") {
    SettingsRig rig;
    QPointer<fv::SettingTile> tile;
    rig.registerStandIns([&tile](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.create = [&tile](QWidget* parent) -> QWidget* {
                fvtest::fillStandIn(parent, 20);
                auto* page = qobject_cast<fv::SettingsPage*>(parent);
                tile = new fv::SettingTile(QStringLiteral("Last"), QStringLiteral("At the end."));
                page->pageLayout()->addWidget(tile);
                return page;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->show();
    dialog->selectDestination(qs(dest::general));
    REQUIRE(tile != nullptr);
    dialog->reveal(tile->checkbox());
    CHECK_FALSE(tile->property("searchHit").toBool()); // once the page is laid out
    QElapsedTimer revealing;
    revealing.start();
    while (!tile->property("searchHit").toBool() && revealing.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    CHECK(tile->property("searchHit").toBool());
    CHECK(dialog->focusWidget() == tile->checkbox());
    QScrollArea* area = dialog->pageScroll(dest::general);
    CHECK(area->verticalScrollBar()->value() > 0);
    QElapsedTimer flashing;
    flashing.start();
    while (tile->property("searchHit").toBool() && flashing.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    CHECK_FALSE(tile->property("searchHit").toBool());
}

TEST_CASE("deep links reach the page they name", "[widget]") {
    SettingsRig rig;
    struct Links : QWidget, fv::SettingsPageModule, fv::SettingsDeepLinks {
        explicit Links(QWidget* parent) : QWidget(parent) {
            auto* layout = new QVBoxLayout(this);
            combo = new QComboBox;
            layout->addWidget(combo);
        }
        void focusCredential(const QString& env) override { calls.append("key " + env); }
        void showBackend(const QString& backend) override { calls.append("backend " + backend); }
        void focusComponent(const QString& id) override { calls.append("component " + id); }
        QWidget* namedControl(std::string_view name) override {
            return name == "hf_policy_combo" ? combo : nullptr;
        }
        QComboBox* combo;
        QStringList calls;
    };
    QPointer<Links> keys;
    QPointer<Links> downloads;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::api_keys) {
            spec.create = [&keys](QWidget* parent) -> QWidget* {
                keys = new Links(parent);
                return keys;
            };
        }
        if (spec.destination == dest::downloads) {
            spec.controls = {"hf_policy_combo"};
            spec.create = [&downloads](QWidget* parent) -> QWidget* {
                downloads = new Links(parent);
                return downloads;
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->show();
    dialog->focusApiKeys(QStringLiteral("GROQ_API_KEY"));
    CHECK(dialog->currentDestination() == qs(dest::api_keys));
    REQUIRE(keys != nullptr);
    CHECK(keys->calls == QStringList{QStringLiteral("key GROQ_API_KEY")});

    dialog->showDownloads(QStringLiteral("parakeet"), QStringLiteral("asr-nvidia-cuda"));
    CHECK(dialog->currentDestination() == qs(dest::downloads));
    REQUIRE(downloads != nullptr);
    CHECK(downloads->calls == QStringList{QStringLiteral("backend parakeet"),
                                          QStringLiteral("component asr-nvidia-cuda")});

    dialog->selectDestination(qs(dest::general));
    dialog->focusHfPolicy();
    CHECK(dialog->currentDestination() == qs(dest::downloads));
    CHECK(dialog->focusWidget() == downloads->combo);
}

TEST_CASE("a narrow window folds the rail into the page menu", "[widget]") {
    const int scale = GENERATE(100, 130);
    fvtest::AppTheme theme(QStringLiteral("dark"), scale);
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    dialog->show();
    // Below the 940 px minimum, as a small screen's first-show fit allows.
    dialog->setMinimumSize(300, 240);
    const auto at = [&dialog](int width) {
        dialog->resize(width, 600);
        QApplication::processEvents();
    };
    const auto px = [scale](int designed) {
        return designed * scale / 100;
    };
    at(px(800));
    CHECK_FALSE(dialog->isNarrow());
    CHECK(dialog->railPane()->isVisible());
    CHECK_FALSE(dialog->pageMenuButton()->isVisible());
    at(px(800) - 1);
    CHECK(dialog->isNarrow());
    CHECK_FALSE(dialog->railPane()->isVisible());
    CHECK(dialog->pageMenuButton()->isVisible());
    CHECK(dialog->pageMenuButton()->text() == QStringLiteral("Home"));
    CHECK(dialog->searchText()->isVisible());
    at(px(720) - 1);
    CHECK_FALSE(dialog->searchText()->isVisible());
    CHECK(dialog->settingsLabel()->isVisible());
    at(px(700) - 1);
    CHECK_FALSE(dialog->settingsLabel()->isVisible());
    CHECK(dialog->brandLabel()->isVisible());
    at(px(560) - 1);
    CHECK_FALSE(dialog->brandLabel()->isVisible());
    CHECK(dialog->brandLabel()->text() == QStringLiteral("FioriVoce"));
    dialog->close();
}

TEST_CASE("Settings is one non-modal window with the shell's names", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    CHECK(dialog->objectName() == QStringLiteral("settingsDialog"));
    CHECK(dialog->windowTitle() == QStringLiteral("Settings"));
    CHECK_FALSE(dialog->isModal());
    CHECK(dialog->isSizeGripEnabled());
    CHECK(dialog->size() == fv::SettingsDialog::kDefaultSize);
    CHECK(dialog->minimumSize() == fv::SettingsDialog::kMinimumSize);
    CHECK(dialog->splitter()->handle(1)->toolTip() ==
          QStringLiteral("Drag to resize the settings sidebar"));
    CHECK(dialog->searchButton()->toolTip() ==
          QStringLiteral("Search every setting, model, and help note"));
    CHECK(dialog->searchButton()->accessibleName() == QStringLiteral("Search settings and models"));
    CHECK(dialog->searchButton()->focusPolicy() == Qt::NoFocus);
    CHECK(dialog->pageMenuButton()->accessibleName() == QStringLiteral("Choose a settings page"));
    CHECK(dialog->closeButton()->text() == QStringLiteral("Done"));
    CHECK(dialog->closeButton()->toolTip() == QStringLiteral("Close Settings (Esc)"));
    auto* hint = dialog->findChild<QLabel*>(QStringLiteral("settingsRailSearchHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->text() ==
          QKeySequence(QStringLiteral("Ctrl+K")).toString(QKeySequence::NativeText));
    auto* saved = dialog->findChild<QLabel*>(QStringLiteral("settingsAutoSaveLabel"));
    REQUIRE(saved != nullptr);
    CHECK(saved->text() == QStringLiteral("Changes save automatically"));
}

TEST_CASE("Ctrl+K and Find open the search", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    QStringList opened;
    dialog->setSearchOpener([&opened](const QString& text) { opened.append(text); });
    dialog->show();
    dialog->activateWindow();
    QApplication::processEvents();
    const QList<QShortcut*> shortcuts = dialog->findChildren<QShortcut*>();
    REQUIRE(shortcuts.size() == 2);
    CHECK(shortcuts[0]->key() == QKeySequence(QStringLiteral("Ctrl+K")));
    CHECK(shortcuts[1]->key() == QKeySequence(QKeySequence::Find));
    for (QShortcut* shortcut : shortcuts) {
        Q_EMIT shortcut->activated();
    }
    CHECK(opened == QStringList{QString(), QString()});
}

// A11-01, A11-02, A11-03, A11-05, A11-07, A11-08: the frame flags and the geometry the shell's
// parts are built with (settings_dialog.py SettingsDialog.__init__, _build_header,
// _build_rail_pane, _build_body).
TEST_CASE("the shell's frame, header, rail pane and body keep OpenWhisper's metrics", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    CHECK(dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    CHECK_FALSE(dialog->windowFlags().testFlag(Qt::MSWindowsFixedSizeDialogHint));

    auto* header = dialog->findChild<QFrame*>(QStringLiteral("settingsViewHeader"));
    REQUIRE(header != nullptr);
    CHECK(header->layout()->contentsMargins() == QMargins(20, 14, 20, 14));
    CHECK(header->layout()->spacing() == 10);
    CHECK(dialog->brandLabel()->objectName() == QStringLiteral("settingsViewBrand"));
    CHECK(dialog->settingsLabel()->objectName() == QStringLiteral("settingsViewLabel"));
    CHECK(dialog->settingsLabel()->text() == QStringLiteral("Settings"));

    QPushButton* search = dialog->searchButton();
    CHECK(search->maximumWidth() == 420);
    CHECK(search->minimumHeight() == 36);
    CHECK(search->maximumHeight() == 36);
    auto* headerRow = qobject_cast<QHBoxLayout*>(header->layout());
    REQUIRE(headerRow != nullptr);
    CHECK(headerRow->stretch(headerRow->indexOf(search)) == 2);
    CHECK(search->layout()->contentsMargins() == QMargins(12, 0, 8, 0));
    CHECK(search->layout()->spacing() == 8);
    CHECK(dialog->searchText()->objectName() == QStringLiteral("settingsRailSearchText"));
    CHECK(dialog->searchText()->text() ==
          QStringLiteral("Search every setting, model and help note"));
    for (QLabel* label : search->findChildren<QLabel*>()) {
        INFO(label->objectName().toStdString());
        CHECK(label->testAttribute(Qt::WA_TransparentForMouseEvents));
    }

    QSplitter* splitter = dialog->splitter();
    CHECK(splitter->orientation() == Qt::Horizontal);
    CHECK(splitter->handleWidth() == 6);
    CHECK_FALSE(splitter->childrenCollapsible());
    CHECK(splitter->handle(1)->cursor().shape() == Qt::SizeHorCursor);
    CHECK(splitter->sizes().first() == 258);
    QWidget* pane = dialog->railPane();
    CHECK(pane->objectName() == QStringLiteral("modelManagerRailPane"));
    CHECK(pane->minimumWidth() == 220);
    CHECK(pane->maximumWidth() == 560);
    CHECK(pane->layout()->contentsMargins() == QMargins(0, 12, 0, 0));

    auto* body = dialog->findChild<QWidget*>(QStringLiteral("modelManagerBody"));
    REQUIRE(body != nullptr);
    CHECK(body->layout()->contentsMargins() == QMargins(28, 16, 28, 16));
    CHECK(body->layout()->spacing() == 14);
    CHECK(dialog->pageCrumb()->objectName() == QStringLiteral("settingsPageCrumb"));
    CHECK(dialog->pageTitleLabel()->objectName() == QStringLiteral("modelManagerTitle"));
    CHECK(dialog->pageSubtitleLabel()->objectName() == QStringLiteral("modelManagerSubtitle"));
    auto* divider = dialog->findChild<QFrame*>(QStringLiteral("settingsPageHeaderDivider"));
    REQUIRE(divider != nullptr);
    CHECK(divider->height() == 1);
    CHECK(dialog->messageLabel()->objectName() == QStringLiteral("modelManagerMessage"));
    CHECK(dialog->closeButton()->objectName() == QStringLiteral("modelManagerCloseButton"));
    CHECK(dialog->closeButton()->minimumWidth() >= 110);
}
