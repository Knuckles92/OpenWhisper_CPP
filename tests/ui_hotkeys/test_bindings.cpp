// Ported from tests/test_hotkey_directory.py: every shortcut on Settings > Hotkeys. The profile,
// transform and action modules arrive with M4 and M5; here stand-in sources keep profiles and
// transforms in the settings the way those modules do.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>

#include <QPushButton>

#include "hotkeys/conflicts.h"
#include "hotkeys/defaults.h"
#include "hotkeys_test_support.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_widgets/buttons.h"

using fv::Json;
using namespace fvtest;

namespace {

Json polishTransform(const std::string& hotkey = "") {
    return Json::array({{{"id", "polish"},
                         {"name", "Polish"},
                         {"instruction", "Polish it."},
                         {"hotkey", hotkey}}});
}

std::string transformHotkey(const PageScene& scene, const std::string& name) {
    for (const auto& item : (*scene.store->snapshot())["text_transforms"]) {
        if (item["name"] == name) {
            return item["hotkey"].get<std::string>();
        }
    }
    return "<missing>";
}

} // namespace

// from tests/test_hotkey_directory.py::test_every_standard_shortcut_has_a_row_on_hotkeys
TEST_CASE("every standard shortcut has a row on hotkeys", "[test_hotkey_directory]") {
    PageScene scene;
    auto& page = *scene.page;
    std::set<std::string> rows;
    for (const auto& item : fv::standard_hotkeys::items()) {
        if (item.kind == fv::standard_hotkeys::Item::Kind::Row) {
            rows.insert(item.key);
        }
    }
    std::set<std::string> defaults;
    for (const auto& [action, hotkey] : fv::hotkeys::default_hotkeys()) {
        defaults.insert(action);
    }
    std::set<std::string> labelled;
    for (const auto& [action, label] : fv::hotkeys::standard_labels()) {
        labelled.insert(std::string(action));
    }
    CHECK(rows == defaults);
    CHECK(defaults == labelled);
    for (const auto& key : rows) {
        INFO(key);
        auto* field = page.hotkeyInput(key);
        REQUIRE(field != nullptr);
        CHECK(page.isAncestorOf(field));
    }
}

// from tests/test_hotkey_directory.py::test_a_binding_capture_stops_a_standard_one
TEST_CASE("a binding capture stops a standard one", "[test_hotkey_directory]") {
    PageScene scene(Json{{"text_transforms", polishTransform()}});
    scene.addListSources();
    auto& page = *scene.page;
    page.show();
    QTest::mouseClick(page.hotkeyInput("cancel"), Qt::LeftButton);
    auto* capture = scene.capture();
    CHECK(page.capturingKey() == "cancel");

    auto* polish = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);
    polish->beginCapture();

    CHECK_FALSE(page.capturingKey().has_value());
    CHECK(capture->stopped());
    // Global hotkeys stay paused for the binding's own capture.
    CHECK(scene.host->suspends == std::vector<bool>{true, false, true});
}

// from tests/test_hotkey_directory.py::test_reset_leaves_your_own_shortcuts_alone
TEST_CASE("reset leaves your own shortcuts alone", "[test_hotkey_directory]") {
    PageScene scene(Json{{"hotkeys", {{"scratchpad_toggle", "ctrl+alt+n"}}},
                         {"text_transforms", polishTransform()}});
    scene.addListSources();
    auto& page = *scene.page;
    auto* polish = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);
    Q_EMIT polish->captured(QStringLiteral("ctrl+alt+j"));
    CHECK(transformHotkey(scene, "Polish") == "ctrl+alt+j");
    CHECK(scene.transforms->savedCount == 1);
    CHECK(scene.host->message() ==
          QStringLiteral("Polish shortcut set to ") + display("ctrl+alt+j") + QStringLiteral("."));

    auto* reset = page.findChild<QPushButton*>(QStringLiteral("hotkeyResetButton"));
    REQUIRE(reset != nullptr);
    reset->click();

    CHECK(scene.saved("scratchpad_toggle").empty());
    CHECK(transformHotkey(scene, "Polish") == "ctrl+alt+j");
}

// from tests/test_hotkey_directory.py::test_each_group_opens_its_own_page
TEST_CASE("each group opens its own page", "[test_hotkey_directory]") {
    PageScene scene;
    std::map<QString, QPushButton*> links;
    for (auto* button : scene.page->bindingShortcuts()->findChildren<QPushButton*>(
             QStringLiteral("settingsPageLink"))) {
        links[button->text()] = button;
    }
    REQUIRE(links.size() == 3);

    links.at(QStringLiteral("Open Commands"))->click();
    links.at(QStringLiteral("Open Profiles"))->click();
    links.at(QStringLiteral("Open Actions"))->click();

    CHECK(scene.host->destinations == std::vector<QString>{QStringLiteral("commands"),
                                                           QStringLiteral("cleanup_profiles"),
                                                           QStringLiteral("actions")});
}

TEST_CASE("groups without bindings say what they are for", "[test_hotkey_directory]") {
    PageScene scene;
    auto* bindings = scene.page->bindingShortcuts();
    std::vector<QString> shown;
    for (auto* label : bindings->findChildren<fv::WrappedLabel*>()) {
        if (!label->isHidden()) {
            shown.push_back(label->text());
        }
    }
    CHECK(shown == std::vector<QString>{
                       QStringLiteral("No profiles yet. A profile records straight into a "
                                      "format, like an email or a ticket."),
                       QStringLiteral("No transforms yet. A transform rewrites the text you "
                                      "select."),
                       QStringLiteral("No actions yet. An action sends what you say to a chat, "
                                      "your agent or your own tools."),
                   });
}

TEST_CASE("binding rows show, refuse, clear and explain shortcuts", "[test_hotkey_directory]") {
    const std::string record = fv::hotkeys::default_hotkeys().front().second;
    PageScene scene(Json{{"text_transforms", Json::array({{{"id", "polish"},
                                                           {"name", "Polish"},
                                                           {"instruction", "Polish it."},
                                                           {"hotkey", record}},
                                                          {{"id", "short"},
                                                           {"name", "Shorter"},
                                                           {"instruction", "Shorten."},
                                                           {"hotkey", "ctrl+alt+s"}}})}});
    scene.addListSources();
    auto& page = *scene.page;
    auto* polish = bindingField(page, QStringLiteral("Polish"));
    auto* shorter = bindingField(page, QStringLiteral("Shorter"));
    REQUIRE(polish != nullptr);
    REQUIRE(shorter != nullptr);

    // A hand-edited file can hold a shortcut the app then skips.
    CHECK(bindingNote(polish) == "Recording uses this shortcut too, so it does nothing here.");
    CHECK(shorter->text() == display("ctrl+alt+s"));
    CHECK(bindingClear(shorter)->isEnabled());

    // The editor's reason for a taken shortcut.
    const std::string cancel = page.currentHotkeys()["cancel"].get<std::string>();
    Q_EMIT shorter->captured(QString::fromStdString(cancel));
    CHECK(transformHotkey(scene, "Shorter") == "ctrl+alt+s");
    CHECK(shorter->text() == display("ctrl+alt+s"));
    CHECK(bindingNote(shorter) == "That shortcut is already used by Cancel. Choose another.");
    CHECK(scene.transforms->savedCount == 0);

    bindingClear(shorter)->click();
    CHECK(transformHotkey(scene, "Shorter").empty());
    CHECK(shorter->text().isEmpty());
    CHECK_FALSE(bindingClear(shorter)->isEnabled());
    CHECK(scene.host->message() == QStringLiteral("Shorter shortcut cleared."));
    CHECK(scene.transforms->savedCount == 1);
}

TEST_CASE("a binding that is gone says so", "[test_hotkey_directory]") {
    PageScene scene(Json{{"text_transforms", polishTransform("ctrl+alt+p")}});
    scene.addListSources();
    auto& page = *scene.page;
    auto* polish = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);
    scene.store->save_setting("text_transforms", Json::array());
    Q_EMIT polish->captured(QStringLiteral("ctrl+alt+j"));
    CHECK(bindingNote(polish) == "Polish no longer exists.");
    CHECK(polish->text() == display("ctrl+alt+p"));
}

TEST_CASE("binding rows reload when shown unless a field is capturing", "[test_hotkey_directory]") {
    PageScene scene(Json{{"text_transforms", polishTransform("ctrl+alt+p")}});
    scene.addListSources();
    auto& page = *scene.page;
    page.show();
    auto* polish = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);

    scene.store->save_setting("text_transforms", polishTransform("ctrl+alt+j"));
    polish->beginCapture();
    page.bindingShortcuts()->load(*scene.store->snapshot());
    CHECK(polish->text() == QString::fromUtf16(fv::kCaptureHint));

    polish->cancelCapture();
    page.bindingShortcuts()->hide();
    page.bindingShortcuts()->show();
    CHECK(polish->text() == display("ctrl+alt+j"));
}

TEST_CASE("instructions are clipped to eighty characters", "[test_hotkey_directory]") {
    using fv::hotkey_bindings::clip;
    CHECK(clip("  Polish\n the   text. ") == "Polish the text.");
    const std::string eighty(80, 'a');
    CHECK(clip(eighty) == eighty);
    CHECK(clip(eighty + "b") == std::string(79, 'a') + "…");
    CHECK(clip(std::string(78, 'a') + " bbbb") == std::string(78, 'a') + "…");
}
