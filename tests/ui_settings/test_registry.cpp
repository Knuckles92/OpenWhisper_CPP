// The Settings shell's page registry: the rail's chapters, page modules built on first use and
// their hooks (tests/test_personalize_foundation_ui_settings.py). Every destination registers a
// stand-in, so the rail is OpenWhisper's whole rail.
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include <QSignalSpy>
#include <QStringList>

#include "settings_support.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::qs;
using fvtest::RecordingPage;
using fvtest::SettingsRig;
namespace dest = fv::destinations;

namespace {

using Calls = std::vector<std::string>;

// The fake page module of the Dictionary slot (fake_page): it records its hooks, says
// "3 words" on the rail, and names fake_page_tile.
struct FakePageRig {
    explicit FakePageRig(bool broken = false) {
        rig.registerStandIns([this, broken](fv::SettingsPageSpec& spec) {
            if (spec.destination != dest::dictionary) {
                return;
            }
            spec.title = QStringLiteral("Fake words");
            spec.subtitle = QStringLiteral("Words for the tests.");
            spec.controls = {"fake_page_tile"};
            spec.searchFields = {{"fake_page_tile", QStringLiteral("Fake words tile"),
                                  QStringLiteral("zebra quokka")}};
            spec.railValue = [this](const fv::Json&) {
                if (railBroken) {
                    throw std::runtime_error("broken page");
                }
                return QStringLiteral("3 words");
            };
            spec.create = [this, broken](QWidget* parent) -> QWidget* {
                page = new RecordingPage(calls, parent);
                page->broken = broken;
                return page;
            };
        });
    }

    SettingsRig rig;
    Calls calls;
    RecordingPage* page = nullptr;
    bool railBroken = false;
};

QStringList keysOf(std::initializer_list<std::string_view> keys) {
    QStringList list;
    for (std::string_view key : keys) {
        list.append(qs(key));
    }
    return list;
}

} // namespace

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_personalize_and_automate_sit_between_dictation_and_meeting_mode
TEST_CASE("test_personalize_and_automate_sit_between_dictation_and_meeting_mode",
          "[test_personalize_foundation_ui_settings][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    fv::NavRail* rail = dialog->rail();
    const QStringList personalize =
        keysOf({dest::dictionary, dest::snippets, dest::styles, dest::cleanup_rules,
                dest::cleanup_profiles, dest::commands});
    const QStringList automate = keysOf({dest::actions, dest::hooks, dest::mcp});
    CHECK(rail->chapters().mid(0, 4) ==
          QStringList{QStringLiteral("Dictation"), QStringLiteral("Personalize"),
                      QStringLiteral("Automate"), QStringLiteral("Meeting Mode")});
    CHECK(rail->chapterKeys(QStringLiteral("Personalize")) == personalize);
    CHECK(rail->chapterKeys(QStringLiteral("Automate")) == automate);
    CHECK(rail->chapter(qs(dest::meeting_voice)) == QStringLiteral("Meeting Mode"));
    QStringList automateNames;
    for (const QString& key : automate) {
        automateNames.append(rail->name(key));
    }
    CHECK(automateNames == QStringList{QStringLiteral("Actions"), QStringLiteral("Hooks"),
                                       QStringLiteral("API & MCP")});
    CHECK(rail->chapter(qs(dest::cleanup)) == QStringLiteral("Dictation"));
    QStringList personalizeNames;
    for (const QString& key : personalize) {
        personalizeNames.append(rail->name(key));
    }
    CHECK(personalizeNames == QStringList{QStringLiteral("Dictionary"), QStringLiteral("Snippets"),
                                          QStringLiteral("Apps & styles"),
                                          QStringLiteral("Learned rules"),
                                          QStringLiteral("Profiles"), QStringLiteral("Commands")});
    CHECK(dialog->pageTitle(dest::styles) == QStringLiteral("Apps & styles"));
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_every_sidebar_name_is_its_page_title
TEST_CASE("test_every_sidebar_name_is_its_page_title",
          "[test_personalize_foundation_ui_settings][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    for (const QString& key : dialog->rail()->keys()) {
        INFO(key.toStdString());
        CHECK(dialog->rail()->name(key) == dialog->pageTitle(fv::qt::toUtf8(key)));
    }
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_page_modules_are_registered_without_building_their_pages
TEST_CASE("test_page_modules_are_registered_without_building_their_pages",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake;
    auto dialog = fake.rig.dialog();
    for (const std::string& key : dialog->pageKeys()) {
        INFO(key);
        CHECK(dialog->isBuilt(key) == (key == dest::overview));
    }
    CHECK(fake.calls.empty());
    CHECK(dialog->rail()->value(qs(dest::dictionary)) == QStringLiteral("3 words"));
    CHECK(fake.rig.registry().controlDestination("fake_page_tile") == dest::dictionary);
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPersonalizeRail::test_a_control_attribute_builds_its_page
TEST_CASE("test_a_control_attribute_builds_its_page",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake;
    auto dialog = fake.rig.dialog();
    REQUIRE_FALSE(dialog->isBuilt(dest::dictionary));
    QWidget* tile = dialog->control("fake_page_tile");
    REQUIRE(tile != nullptr);
    CHECK(dialog->isBuilt(dest::dictionary));
    CHECK(dialog->page(dest::dictionary)->isAncestorOf(tile));
    CHECK(dialog->control("no_such_control") == nullptr);
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPageModuleRegistry::test_registry_calls_each_hook_at_its_moment
TEST_CASE("test_registry_calls_each_hook_at_its_moment",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake;
    auto dialog = fake.rig.dialog();
    CHECK(dialog->rail()->value(qs(dest::dictionary)) == QStringLiteral("3 words"));
    CHECK(fake.rig.registry().controlDestination("fake_page_tile") == dest::dictionary);
    CHECK(fake.calls.empty());

    dialog->control("fake_page_tile");
    CHECK(fake.calls == Calls{"build", "load"});

    fake.calls.clear();
    dialog->refresh();
    CHECK(fake.calls == Calls{"load", "refresh"});

    fake.calls.clear();
    dialog->selectDestination(qs(dest::dictionary));
    CHECK(fake.calls.empty());
    dialog->selectDestination(qs(dest::general));
    CHECK(fake.calls == Calls{"cancel"});

    fake.calls.clear();
    dialog->refreshPage(dest::dictionary);
    CHECK(fake.calls == Calls{"refresh"});

    fake.calls.clear();
    dialog->showHome();
    REQUIRE_FALSE(fake.calls.empty());
    CHECK(fake.calls.front() == "cancel");
    fake.calls.clear();
    dialog->close();
    CHECK(fake.calls == Calls{"cancel"});
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPageModuleRegistry::test_unbuilt_pages_are_neither_loaded_refreshed_nor_cancelled
TEST_CASE("test_unbuilt_pages_are_neither_loaded_refreshed_nor_cancelled",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake;
    auto dialog = fake.rig.dialog();
    dialog->refresh();
    dialog->refreshPage(dest::dictionary);
    dialog->selectDestination(qs(dest::general));
    dialog->close();
    CHECK(fake.calls.empty());
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPageModuleRegistry::test_a_failing_rail_value_leaves_the_rail_working
TEST_CASE("test_a_failing_rail_value_leaves_the_rail_working",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake;
    auto dialog = fake.rig.dialog();
    // General's copy_clipboard switch saves through the dialog, as the real page's does.
    auto* tile = new fv::SettingTile(QStringLiteral("Copy to clipboard"), QString());
    dialog->ensurePage(dest::general);
    dialog->page(dest::general)->pageLayout()->addWidget(tile);
    dialog->bindings().checkbox(tile->checkbox(), "copy_clipboard", [](const fv::Json& settings) {
        return settings.value("copy_clipboard", true);
    });

    fake.railBroken = true;
    dialog->refreshRailValues();
    CHECK(dialog->rail()->value(qs(dest::dictionary)).isEmpty());
    dialog->selectDestination(qs(dest::general));
    tile->checkbox()->toggle();
    CHECK(fake.rig.settings().contents().value("copy_clipboard", true) ==
          tile->checkbox()->isChecked());
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestPageModuleRegistry::test_a_failing_page_still_opens_switches_and_closes
TEST_CASE("test_a_failing_page_still_opens_switches_and_closes",
          "[test_personalize_foundation_ui_settings][widget]") {
    FakePageRig fake(true);
    auto dialog = fake.rig.dialog();
    dialog->selectDestination(qs(dest::dictionary));
    CHECK(dialog->rail()->currentKey() == qs(dest::dictionary));
    dialog->refresh();
    dialog->selectDestination(qs(dest::general));
    CHECK(dialog->rail()->currentKey() == qs(dest::general));
    dialog->close();
    CHECK(fake.calls == Calls{"build", "broken", "broken", "broken", "broken", "broken"});
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_notify_changed_reaches_the_injected_callback
TEST_CASE("test_notify_changed_reaches_the_injected_callback",
          "[test_personalize_foundation_ui_settings][widget]") {
    SettingsRig rig;
    int railReads = 0;
    rig.registerStandIns([&railReads](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::general) {
            spec.railValue = [&railReads](const fv::Json&) {
                ++railReads;
                return QStringLiteral("Auto-paste on");
            };
        }
    });
    auto dialog = rig.dialog();
    dialog->notifyChanged(QStringLiteral("dictionary"));
    QSignalSpy changed(dialog.get(), &fv::SettingsDialog::settingsChanged);
    const int before = railReads;
    dialog->notifyChanged(QStringLiteral("transforms"));
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == QStringLiteral("transforms"));
    CHECK(railReads == before + 1);
}

TEST_CASE("capture suspension reaches the injected callback",
          "[test_personalize_foundation_ui_settings][widget]") {
    // from
    // tests/test_personalize_foundation_ui_settings.py::TestDialogHelpers::test_capture_suspension_uses_the_profile_capture_path
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    QSignalSpy suspended(dialog.get(), &fv::SettingsDialog::hotkeyCaptureSuspended);
    dialog->setHotkeyCaptureSuspended(true);
    dialog->setHotkeyCaptureSuspended(false);
    REQUIRE(suspended.count() == 2);
    CHECK(suspended.at(0).at(0).toBool());
    CHECK_FALSE(suspended.at(1).at(0).toBool());
}

TEST_CASE("a standard hotkey goes through the Hotkeys page's handler", "[widget]") {
    SettingsRig rig;
    bool built = false;
    rig.registerStandIns([&built](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::hotkeys) {
            spec.create = [&built](QWidget* parent) {
                built = true;
                auto* page = qobject_cast<fv::SettingsPage*>(parent);
                page->dialog()->setStandardHotkeyHandler(
                    [dialog = page->dialog()](const QString& action, const QString& hotkey) {
                        dialog->showMessage(QStringLiteral("%1 %2").arg(action, hotkey));
                        return QString();
                    });
                return fvtest::fillStandIn(parent);
            };
        }
    });
    auto dialog = rig.dialog();
    CHECK_FALSE(built);
    CHECK(dialog->setStandardHotkey(QStringLiteral("command_mode"), QStringLiteral("ctrl+alt+k"))
              .isEmpty());
    CHECK(built);
    CHECK(dialog->messageText() == QStringLiteral("command_mode ctrl+alt+k"));
}

TEST_CASE("without a Hotkeys page a standard hotkey says it was not saved", "[widget]") {
    SettingsRig rig;
    auto dialog = rig.dialog();
    const QString error =
        dialog->setStandardHotkey(QStringLiteral("command_mode"), QStringLiteral("ctrl+alt+k"));
    CHECK_FALSE(error.isEmpty());
    CHECK(dialog->messageText() == error);
}
