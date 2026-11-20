// The controller's populate_language_menu hands its menu to the language menu module
// (ui_controller.py populate_language_menu -> language_menu.populate(menu, self)): the tray's
// Language submenu is filled with the module's rows over the app's saved languages.
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QMenu>

#include "controller_support.h"
#include "ui_controller/ui_controller.h"

// from
// tests/test_personalize_foundation_ui_shell.py::TestFeatureDelegation::test_language_menu_is_filled_by_its_module
TEST_CASE("the controller's language menu is filled by the language menu module", "[widget]") {
    fvtest::RigOptions options;
    options.settings["selected_model"] = "parakeet";
    options.settings["dictation_languages"] = fv::Json::array({"en", "es"});
    options.settings["dictation_active_language"] = "es";
    fvtest::ControllerRig rig(options);
    QMenu menu;

    rig.ui().populateLanguageMenu(&menu);

    QStringList texts;
    QStringList checked;
    for (const QAction* action : menu.actions()) {
        texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
        if (action->isChecked()) {
            checked << action->text();
        }
    }
    CHECK(texts == QStringList{QStringLiteral("English"), QStringLiteral("Spanish"),
                               QStringLiteral("---"), QStringLiteral(u"Choose languages…")});
    CHECK(checked == QStringList{QStringLiteral("Spanish")});
    // A null menu (the tray's submenu already gone) is left alone.
    rig.ui().populateLanguageMenu(nullptr);
}
