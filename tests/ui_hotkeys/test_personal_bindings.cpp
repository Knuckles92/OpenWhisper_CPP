// The Hotkeys page's profile and transform rows over the real fv_text sources
// (ProfileBindingSource, TransformBindingSource): the OpenWhisper tests that need a saved profile
// or transform library. The global capture is hotkeys::FakeCapture and a row's own field is driven
// by key events; nothing here hooks the keyboard or the mouse. The action rows are M5's: a stand-in
// source stands for the saved actions where a test needs them in the shortcut space.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QLabel>
#include <QPushButton>
#include <QTest>

#include "hotkeys/defaults.h"
#include "hotkeys_test_support.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_hotkeys/personal_binding_sources.h"

using fv::Json;
using namespace fvtest;

namespace {

// A Hotkeys page whose profile and transform rows are the real sources.
struct PersonalScene : PageScene {
    explicit PersonalScene(const Json& values = Json::object()) : PageScene(values) {
        fv::registerPersonalBindingSources(*page, {[this] { ++profilesSaved; },
                                                   [this] {
                                                       ++transformsSaved;
                                                   }});
        page->loadSettings(*store->snapshot());
    }

    std::string transformHotkey(const std::string& name) const {
        for (const auto& transform : fv::text::load_transforms(*store->snapshot())) {
            if (transform.name == name) {
                return transform.hotkey;
            }
        }
        return "<missing>";
    }
    std::string profileHotkey(const std::string& name) const {
        for (const auto& profile : fv::text::load_cleanup_profiles(*store->snapshot())) {
            if (profile.name == name) {
                return profile.hotkey;
            }
        }
        return "<missing>";
    }

    int profilesSaved = 0;
    int transformsSaved = 0;
};

// The saved actions, as the action source M5 registers will list them.
class ActionsStandIn final : public fv::BindingSource {
public:
    std::vector<fv::hotkey_bindings::Binding> list(const Json&) const override {
        return {{"action", "ask", "Ask the docs", hotkey, "Opens a link.", true}};
    }
    bool save(Json&, const std::string&, const std::string&) override { return true; }

    std::string hotkey = "ctrl+alt+d";
};

} // namespace

// from
// tests/test_hotkey_directory.py::test_a_transform_shortcut_set_on_hotkeys_saves_and_registers
TEST_CASE("test_a_transform_shortcut_set_on_hotkeys_saves_and_registers",
          "[test_hotkey_directory]") {
    PersonalScene scene;
    auto& page = *scene.page;
    page.show();
    auto* field = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(field != nullptr);
    QTest::mouseClick(field, Qt::LeftButton);
    CHECK(field->isCapturing());
    CHECK(scene.host->suspends.back() == true);

    QTest::keyClick(field, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);

    CHECK(scene.transformHotkey("Polish") == "ctrl+alt+p");
    CHECK(scene.host->suspends.back() == false);
    CHECK(scene.transformsSaved == 1);
    CHECK(scene.profilesSaved == 0);
    CHECK(scene.host->message() ==
          QStringLiteral("Polish shortcut set to ") + display("ctrl+alt+p") + QStringLiteral("."));
    CHECK(bindingClear(field)->isEnabled());
}

// from
// tests/test_hotkey_directory.py::test_a_profile_shortcut_set_on_hotkeys_registers_like_one_saved_on_profiles
TEST_CASE("test_a_profile_shortcut_set_on_hotkeys_registers_like_one_saved_on_profiles",
          "[test_hotkey_directory]") {
    PersonalScene scene;
    auto* field = bindingField(*scene.page, QStringLiteral("Email"));
    REQUIRE(field != nullptr);

    Q_EMIT field->captured(QStringLiteral("ctrl+alt+e"));

    CHECK(scene.profileHotkey("Email") == "ctrl+alt+e");
    CHECK(scene.profilesSaved == 1);
    CHECK(scene.transformsSaved == 0);
}

// from tests/test_hotkey_directory.py::test_a_taken_shortcut_is_refused_with_its_editors_reason
TEST_CASE("test_a_taken_shortcut_is_refused_with_its_editors_reason", "[test_hotkey_directory]") {
    PersonalScene scene;
    auto& page = *scene.page;
    const std::string cancel = page.currentHotkeys()["cancel"].get<std::string>();
    auto* field = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(field != nullptr);

    Q_EMIT field->captured(QString::fromStdString(cancel));

    CHECK(scene.transformHotkey("Polish").empty());
    CHECK(field->text().isEmpty());
    CHECK(bindingNote(field) == "That shortcut is already used by Cancel. Choose another.");
    CHECK(scene.transformsSaved == 0);
}

// from
// tests/test_personalize_s6_settings.py::test_profiles_and_transforms_block_a_standard_shortcut
TEST_CASE("test_profiles_and_transforms_block_a_standard_shortcut",
          "[test_personalize_s6_settings]") {
    PersonalScene scene(
        Json{{"transcript_cleanup_profiles", Json::array({{{"id", "ticket"},
                                                           {"name", "Ticket"},
                                                           {"instructions", "Ticket."},
                                                           {"hotkey", "ctrl+alt+t"}}})},
             {"text_transforms", Json::array({{{"id", "polish"},
                                               {"name", "Polish"},
                                               {"instruction", "Polish it."},
                                               {"hotkey", "ctrl+alt+p"}}})}});
    auto& page = *scene.page;
    page.show();
    auto* field = page.hotkeyInput("cycle_language");
    QTest::mouseClick(field, Qt::LeftButton);
    scene.capture()->capture("alt+ctrl+t");
    CHECK(scene.note("cycle_language") == "That shortcut is already used by Ticket.");
    QTest::mouseClick(field, Qt::LeftButton);
    scene.capture()->capture("ctrl+alt+p");
    CHECK(scene.note("cycle_language") == "That shortcut is already used by Polish.");

    // Re-saving an action's own shortcut is not a conflict.
    const std::string record = page.currentHotkeys()["record_toggle"].get<std::string>();
    QTest::mouseClick(page.hotkeyInput("record_toggle"), Qt::LeftButton);
    scene.capture()->capture(record);
    CHECK(scene.note("record_toggle").empty());
    CHECK(scene.saved("cycle_language").empty());
}

// from tests/test_cleanup_profiles.py::test_standard_hotkey_editor_rejects_profile_collision
TEST_CASE("test_standard_hotkey_editor_rejects_profile_collision", "[test_cleanup_profiles]") {
    // OpenWhisper hands the dialog a standard map that takes the profile's shortcut. The
    // page's fields refuse such a shortcut earlier, so the map that reaches the save is one
    // where a hand-edited profile already sits on a default: Reset puts the defaults back and
    // is refused for the profile, with its name.
    const std::string cancel = fv::hotkeys::default_hotkeys().at(1).second;
    REQUIRE_FALSE(cancel.empty());
    PersonalScene scene(
        Json{{"transcript_cleanup_profiles", Json::array({{{"id", "ticket"},
                                                           {"name", "Support ticket"},
                                                           {"instructions", "A short ticket."},
                                                           {"hotkey", cancel}}})}});
    auto& page = *scene.page;
    const std::string record = page.currentHotkeys()["record_toggle"].get<std::string>();
    auto* reset = page.findChild<QPushButton*>(QStringLiteral("hotkeyResetButton"));
    REQUIRE(reset != nullptr);

    reset->click();

    CHECK(scene.host->message().startsWith(QStringLiteral("Couldn't save hotkeys: ")));
    CHECK(scene.host->message().contains(QStringLiteral("Support ticket")));
    CHECK(scene.host->message().endsWith(QStringLiteral(". Choose another.")));
    CHECK(scene.host->applied.empty());
    CHECK(page.currentHotkeys()["record_toggle"].get<std::string>() == record);
}

TEST_CASE("profiles and transforms are listed with their shortcuts", "[test_hotkey_directory]") {
    // The starters while nothing was saved; the Actions group is M5's.
    PersonalScene scene(
        Json{{"text_transforms", Json::array({{{"id", "polish"},
                                               {"name", "Polish"},
                                               {"instruction", "  Polish\nthe text.  "},
                                               {"hotkey", "ctrl+alt+p"}},
                                              {{"id", "short"},
                                               {"name", "Shorter"},
                                               {"instruction", std::string(120, 'x')},
                                               {"hotkey", ""}}})}});
    auto& page = *scene.page;

    for (const char* starter : {"Support ticket", "Email"}) {
        INFO(starter);
        auto* field = bindingField(page, QString::fromLatin1(starter));
        REQUIRE(field != nullptr);
        CHECK(field->text().isEmpty());
        CHECK_FALSE(bindingClear(field)->isEnabled());
    }
    auto* polish = bindingField(page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);
    CHECK(polish->text() == display("ctrl+alt+p"));
    CHECK(bindingClear(polish)->isEnabled());
    CHECK(cardOf(polish)->findChild<QLabel*>(QStringLiteral("hotkeyShortcutDescription"))->text() ==
          QStringLiteral("Polish the text."));
    auto* shorter = bindingField(page, QStringLiteral("Shorter"));
    REQUIRE(shorter != nullptr);
    CHECK(
        cardOf(shorter)->findChild<QLabel*>(QStringLiteral("hotkeyShortcutDescription"))->text() ==
        QString::fromStdString(std::string(79, 'x')) + QStringLiteral("…"));
    // Saved transforms replace the starters.
    CHECK(bindingField(page, QStringLiteral("Make concise")) == nullptr);
}

TEST_CASE("a row says when a standard shortcut makes its own do nothing",
          "[test_hotkey_directory]") {
    const std::string record = fv::hotkeys::default_hotkeys().front().second;
    PersonalScene scene(Json{{"text_transforms", Json::array({{{"id", "polish"},
                                                               {"name", "Polish"},
                                                               {"instruction", "Polish it."},
                                                               {"hotkey", record}}})}});
    auto* polish = bindingField(*scene.page, QStringLiteral("Polish"));
    REQUIRE(polish != nullptr);

    CHECK(bindingNote(polish) == "Recording uses this shortcut too, so it does nothing here.");
}

TEST_CASE("a clear on a profile row saves an empty shortcut and tells the profile's page",
          "[test_hotkey_directory]") {
    PersonalScene scene(
        Json{{"transcript_cleanup_profiles", Json::array({{{"id", "email"},
                                                           {"name", "Email"},
                                                           {"instructions", "Write an email."},
                                                           {"hotkey", "ctrl+alt+e"}}})}});
    auto* field = bindingField(*scene.page, QStringLiteral("Email"));
    REQUIRE(field != nullptr);
    REQUIRE(field->text() == display("ctrl+alt+e"));

    bindingClear(field)->click();

    CHECK(scene.profileHotkey("Email").empty());
    CHECK(field->text().isEmpty());
    CHECK(scene.host->message() == QStringLiteral("Email shortcut cleared."));
    CHECK(scene.profilesSaved == 1);
}

TEST_CASE("a shortcut an action uses is refused on a transform's row", "[test_hotkey_directory]") {
    // The saved actions share the shortcut space; the validation counts them through the page's
    // registered sources.
    PersonalScene scene;
    scene.page->registerBindingSource("action", std::make_shared<ActionsStandIn>());
    scene.page->loadSettings(*scene.store->snapshot());
    auto* field = bindingField(*scene.page, QStringLiteral("Polish"));
    REQUIRE(field != nullptr);

    Q_EMIT field->captured(QStringLiteral("alt+ctrl+d"));

    CHECK(scene.transformHotkey("Polish").empty());
    CHECK(bindingNote(field) == "That shortcut is already used by Ask the docs. Choose another.");
    CHECK(scene.transformsSaved == 0);
}

TEST_CASE("a row whose profile was deleted meanwhile says so", "[test_hotkey_directory]") {
    PersonalScene scene;
    auto* field = bindingField(*scene.page, QStringLiteral("Email"));
    REQUIRE(field != nullptr);
    scene.store->save_setting("transcript_cleanup_profiles", Json::array());

    Q_EMIT field->captured(QStringLiteral("ctrl+alt+e"));

    CHECK(bindingNote(field) == "Email no longer exists.");
    CHECK(scene.profilesSaved == 0);
}

TEST_CASE("the sources list the starters until a library is saved", "[test_hotkey_directory]") {
    PageScene scene;
    CHECK(fv::ProfileBindingSource().list(*scene.store->snapshot()).size() == 2);
    CHECK(fv::TransformBindingSource().list(*scene.store->snapshot()).size() == 4);
}
