// MenuRegistry: the title bar's File, View and Help menus (ui_qt/main_window.py:959-1026) and the
// tray menu (ui_qt/system_tray.py:59-112), built from items features register in any order.

#include <stdexcept>
#include <vector>

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QString>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include "appkit/order.h"
#include "appkit/registries.h"

namespace {

using fv::Menu;
namespace tray = fv::order::tray;

// What a menu shows: action texts, "---" for separators, ">" before a submenu's title.
std::vector<QString> entries(const QMenu& menu) {
    std::vector<QString> found;
    for (const QAction* action : menu.actions()) {
        if (action->isSeparator()) {
            found.push_back(QStringLiteral("---"));
        } else if (action->menu() != nullptr) {
            found.push_back(QStringLiteral(">") + action->text());
        } else {
            found.push_back(action->text());
        }
    }
    return found;
}

fv::MenuItemSpec text_item(Menu menu, int order, QString text) {
    return fv::MenuItemSpec{menu, order, [text](QObject* parent) {
                                return new QAction(text, parent);
                            }};
}

} // namespace

TEST_CASE("menu population follows order.h whatever the registration order", "[widget]") {
    fv::MenuRegistry registry;
    // Registered out of order, as features install by id.
    registry.add({"tray.exit", Menu::Tray, tray::exit, QStringLiteral("Exit")});
    registry.addSeparator(Menu::Tray, tray::separator_before_exit);
    registry.add({"tray.settings", Menu::Tray, tray::settings, QStringLiteral("Settings")});
    registry.addItem(fv::MenuItemSpec{Menu::Tray, tray::language,
                                      [](QObject* parent) {
                                          // populate() passes the QMenu being filled.
                                          auto* menu = new QMenu(QStringLiteral("Language"),
                                                                 qobject_cast<QWidget*>(parent));
                                          return menu->menuAction();
                                      },
                                      "tray.language"});
    registry.add(
        {"tray.toggle", Menu::Tray, tray::toggle_recording, QStringLiteral("Start Recording")});
    registry.addSeparator(Menu::Tray, tray::separator_before_recording);
    registry.add({"tray.hide", Menu::Tray, tray::hide, QStringLiteral("Hide")});
    registry.add({"tray.show", Menu::Tray, tray::show, QStringLiteral("Show")});
    registry.add(
        {"file.settings", Menu::File, fv::order::file_menu::settings, QStringLiteral("Settings")});

    QMenu menu;
    registry.populate(Menu::Tray, &menu);
    CHECK(entries(menu) ==
          std::vector<QString>{QStringLiteral("Show"), QStringLiteral("Hide"),
                               QStringLiteral("---"), QStringLiteral("Start Recording"),
                               QStringLiteral(">Language"), QStringLiteral("Settings"),
                               QStringLiteral("---"), QStringLiteral("Exit")});
    CHECK(registry.action("tray.language") != nullptr);
    CHECK(registry.action("tray.language")->menu() != nullptr);
    CHECK(registry.items(Menu::Tray).size() == 8);
    CHECK(registry.items(Menu::Help).empty());
}

TEST_CASE("items of equal order keep their registration order", "[widget]") {
    fv::MenuRegistry registry;
    registry.addItem(text_item(Menu::View, 100, QStringLiteral("first")));
    registry.addItem(text_item(Menu::View, 50, QStringLiteral("before")));
    registry.addItem(text_item(Menu::View, 100, QStringLiteral("second")));
    QMenu menu;
    registry.populate(Menu::View, &menu);
    CHECK(entries(menu) == std::vector<QString>{QStringLiteral("before"), QStringLiteral("first"),
                                                QStringLiteral("second")});
}

TEST_CASE("add builds the action with OpenWhisper's setters", "[widget]") {
    fv::MenuRegistry registry;
    QAction* exit = registry.add({"file.exit", Menu::File, fv::order::file_menu::exit,
                                  QStringLiteral("Exit"), QKeySequence(QStringLiteral("Ctrl+Q"))});
    QAction* host = registry.add(
        {"view.host_mode", Menu::View, fv::order::view_menu::host_mode, QStringLiteral("Host Mode"),
         QKeySequence(QStringLiteral("Ctrl+Shift+H")), QAction::NoRole, true});
    QAction* minimize = registry.add({"file.minimize",
                                      Menu::File,
                                      fv::order::file_menu::minimize_to_tray,
                                      QStringLiteral("Minimize to Tray"),
                                      {},
                                      QAction::NoRole,
                                      false,
                                      true});
    CHECK(exit->text() == QStringLiteral("Exit"));
    CHECK(exit->shortcut() == QKeySequence(QStringLiteral("Ctrl+Q")));
    CHECK(exit->menuRole() == QAction::NoRole);
    CHECK_FALSE(exit->isCheckable());
    CHECK(host->isCheckable());
    CHECK(minimize->shortcut().isEmpty());
    CHECK(exit->parent() == &registry);
    CHECK(registry.action("file.exit") == exit);
    CHECK(registry.action("missing") == nullptr);

    QMenu menu;
    registry.populate(Menu::File, &menu);
    CHECK(entries(menu) == std::vector<QString>{QStringLiteral("---"),
                                                QStringLiteral("Minimize to Tray"),
                                                QStringLiteral("Exit")});
    CHECK(menu.actions().at(1) == minimize);

    CHECK_THROWS_MATCHES(
        registry.add({"file.exit", Menu::File, 1, QStringLiteral("Again")}), std::invalid_argument,
        Catch::Matchers::Message("A menu item with the id 'file.exit' is already registered."));
    CHECK_THROWS_AS(registry.add({"", Menu::File, 1, QStringLiteral("No id")}),
                    std::invalid_argument);
}

TEST_CASE("an item whose create returns null is left out", "[widget]") {
    fv::MenuRegistry registry;
    registry.addItem(fv::MenuItemSpec{Menu::Help, fv::order::help_menu::check_for_updates,
                                      [](QObject*) -> QAction* { return nullptr; },
                                      "help.updates"});
    registry.addItem(text_item(Menu::Help, fv::order::help_menu::about, QStringLiteral("About")));
    QMenu menu;
    registry.populate(Menu::Help, &menu);
    CHECK(entries(menu) == std::vector<QString>{QStringLiteral("About")});
    CHECK(registry.action("help.updates") == nullptr);
    registry.populate(Menu::Help, nullptr);
}
