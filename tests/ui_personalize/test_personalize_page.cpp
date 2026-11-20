// The page base every Personalize page builds on, its registration in the Settings pages, the
// host that forwards to the Settings window, the fake host, ShowWatcher and the feature.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QLabel>
#include <QMetaMethod>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/destinations.h"
#include "personalize_support.h"
#include "pyport/errors.h"
#include "ui_personalize/cleanup_prompt_dialog.h"
#include "ui_personalize/cleanup_rule_dialog.h"
#include "ui_personalize/personalize_feature.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/personalize_page.h"
#include "ui_personalize/rule_activity.h"
#include "ui_personalize/show_watcher.h"
#include "ui_personalize/testing/fake_personalize_host.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"

using fvtest::qs;
using fvtest::SettingsRig;
namespace dest = fv::destinations;

namespace {

// A page with one switch that is saved as "flag" and shown from it, counting what the base
// calls: the shape of a Snippets or Commands page.
class SwitchPage : public fv::PersonalizePage {
public:
    SwitchPage(fv::PersonalizeHost& host, fv::SettingsPage& container)
        : PersonalizePage(host, container) {
        check = new QCheckBox(QStringLiteral("Expand snippets"), this);
        pageLayout()->addWidget(check);
        exposeControl("flag_check", check);
        connect(check, &QCheckBox::toggled, this, [this](bool checked) {
            if (loading()) {
                ++ignoredToggles;
                return;
            }
            save("flag", checked);
        });
    }

    int loads = 0;
    int syncs = 0;
    int ignoredToggles = 0;
    QCheckBox* check = nullptr;

protected:
    void load(const fv::Json& settings) override {
        ++loads;
        check->setChecked(settings.value("flag", false));
    }
    void sync(const fv::Json& settings) override {
        ++syncs;
        check->setChecked(settings.value("flag", false));
    }
};

// A page that only shows what it was loaded with, so the default sync is load.
class PlainPage : public fv::PersonalizePage {
public:
    using PersonalizePage::PersonalizePage;
    int loads = 0;

protected:
    void load(const fv::Json&) override { ++loads; }
};

fv::PageDescriptor switchDescriptor(SwitchPage** made = nullptr) {
    fv::PageDescriptor descriptor;
    descriptor.destination = dest::snippets;
    descriptor.title = QStringLiteral("Snippets");
    descriptor.subtitle = QStringLiteral("Say a short phrase to insert text.");
    descriptor.create = [made](fv::PersonalizeHost& host, fv::SettingsPage& container) {
        auto* page = new SwitchPage(host, container);
        if (made != nullptr) {
            *made = page;
        }
        return page;
    };
    descriptor.railValue = [](const fv::Json& settings) {
        return settings.value("flag", false) ? QStringLiteral("On") : QStringLiteral("Off");
    };
    descriptor.searchFields = {{"flag_check", QStringLiteral("Expand snippets as you dictate"),
                                QStringLiteral("Say a trigger phrase, alone or mid-sentence.")}};
    descriptor.helpTexts = {QStringLiteral("Triggers ignore capitals and punctuation.")};
    descriptor.controls = {"flag_check"};
    return descriptor;
}

} // namespace

TEST_CASE("PersonalizePage re-syncs on every show and never writes while loading", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    auto* page = new SwitchPage(host, container);
    container.pageLayout()->addWidget(page);
    host.saved = fv::Json{{"flag", true}};

    // Showing the page reads the settings again, and what that does to the switch is not saved.
    container.resize(400, 200);
    container.show();
    CHECK(page->syncs == 1);
    CHECK(page->check->isChecked());
    CHECK(page->ignoredToggles == 1);
    CHECK(host.callsTo("persist").empty());
    CHECK(host.callsTo("settings()").size() == 1);

    // Hidden while another page changes the setting, then shown again.
    container.hide();
    host.saved["flag"] = false;
    container.show();
    CHECK(page->syncs == 2);
    CHECK_FALSE(page->check->isChecked());
    CHECK(page->ignoredToggles == 2);
    CHECK(host.callsTo("persist").empty());

    // The user's own click is saved.
    page->check->click();
    CHECK(host.callsTo("persist").size() == 1);
    CHECK(host.callsTo("persist").front() == "persist(flag, true)");
    CHECK(host.saved.at("flag") == true);
}

TEST_CASE("loadSettings holds the loading flag and calls load once", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    auto* page = new SwitchPage(host, container);
    CHECK_FALSE(page->loading());
    page->loadSettings(fv::Json{{"flag", true}});
    CHECK(page->loads == 1);
    CHECK(page->syncs == 0);
    CHECK(page->check->isChecked());
    CHECK(page->ignoredToggles == 1);
    CHECK_FALSE(page->loading());
    CHECK(host.callsTo("persist").empty());
}

TEST_CASE("a page's sync is its load unless it says otherwise", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    PlainPage page(host, container);
    container.show();
    CHECK(page.loads == 1);
    container.hide();
    container.show();
    CHECK(page.loads == 2);
}

TEST_CASE("a page that loads throws its flag back", "[widget]") {
    class Throwing : public fv::PersonalizePage {
    public:
        using PersonalizePage::PersonalizePage;

    protected:
        void load(const fv::Json&) override { throw std::runtime_error("broken page"); }
    };
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    Throwing page(host, container);
    CHECK_THROWS_AS(page.loadSettings(fv::Json::object()), std::runtime_error);
    CHECK_FALSE(page.loading());
    // A show that fails is logged and swallowed, never thrown into the event loop.
    container.show();
    CHECK(page.isVisible());
}

TEST_CASE("save refuses while loading and passes through otherwise", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    SwitchPage page(host, container);
    CHECK(page.save("a", 1));
    CHECK(page.saveMany(fv::Json{{"b", 2}}, {"a"}));
    CHECK(host.saved == fv::Json{{"b", 2}});
    {
        fv::PersonalizePage::LoadingScope scope(page);
        CHECK(page.loading());
        CHECK_FALSE(page.save("a", 3));
        CHECK_FALSE(page.saveMany(fv::Json{{"c", 3}}));
    }
    CHECK_FALSE(page.loading());
    host.loading = true;
    CHECK(page.loading());
    CHECK_FALSE(page.save("a", 4));
    host.loading = false;
    CHECK(page.save("a", 4));
    CHECK(host.callsTo("persist(a, 3)").empty());
}

TEST_CASE("an exposed control is found by name", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    SwitchPage page(host, container);
    CHECK(page.namedControl("flag_check") == page.check);
    CHECK(page.namedControl("nothing") == nullptr);
    // A control that is gone is not handed out.
    auto* temporary = new QLabel(&page);
    page.exposeControl("temporary", temporary);
    CHECK(page.namedControl("temporary") == temporary);
    delete temporary;
    CHECK(page.namedControl("temporary") == nullptr);
}

TEST_CASE("addTileGroup puts a caption, tiles and space on the page's column", "[widget]") {
    fv::FakePersonalizeHost host;
    fv::SettingsPage container;
    PlainPage page(host, container);
    auto* tile = new QLabel(QStringLiteral("a tile"));
    const auto group =
        page.addTileGroup(QStringLiteral("Words"), {tile}, 1, QStringLiteral("An intro line."));
    REQUIRE(group.caption != nullptr);
    CHECK(group.caption->text() == QStringLiteral("Words"));
    REQUIRE(group.intro != nullptr);
    CHECK(group.grid != nullptr);
    CHECK(tile->parentWidget() != nullptr);
    CHECK(page.pageLayout()->count() >= 3);
    CHECK(container.tileColumns().size() == 1);
}

TEST_CASE("a page registered through registerPersonalizePage appears in the registry", "[widget]") {
    SettingsRig rig;
    fv::registerPersonalizePage(rig.context(), switchDescriptor());
    const fv::SettingsPageSpec* spec = rig.registry().page(dest::snippets);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Snippets"));
    CHECK(spec->subtitle == QStringLiteral("Say a short phrase to insert text."));
    CHECK(spec->scroll);
    REQUIRE(spec->railValue);
    CHECK(spec->railValue(fv::Json{{"flag", true}}) == QStringLiteral("On"));
    CHECK(spec->railValue(fv::Json::object()) == QStringLiteral("Off"));
    REQUIRE(spec->searchFields.size() == 1);
    CHECK(spec->searchFields[0].target == "flag_check");
    CHECK(spec->searchFields[0].label == QStringLiteral("Expand snippets as you dictate"));
    CHECK(spec->helpTexts.size() == 1);
    CHECK(spec->controls == std::vector<std::string>{"flag_check"});
    CHECK(rig.registry().controlDestination("flag_check") == dest::snippets);
    // Only the registration happened: nothing is built until the page is selected.
    CHECK_THROWS_AS(fv::registerPersonalizePage(rig.context(), switchDescriptor()),
                    std::invalid_argument);
}

// Home, so that the window opens on it and builds no other page until asked.
void registerHome(SettingsRig& rig) {
    rig.registry().addPage(fvtest::standInSpec(fvtest::headings().front()));
}

TEST_CASE("the Settings window builds the page over itself and saves through it", "[widget]") {
    SettingsRig rig(fv::Json{{"flag", true}});
    registerHome(rig);
    SwitchPage* page = nullptr;
    fv::registerPersonalizePage(rig.context(), switchDescriptor(&page));
    const auto dialog = rig.dialog();
    CHECK_FALSE(dialog->isBuilt(dest::snippets));
    dialog->ensurePage(dest::snippets);
    REQUIRE(page != nullptr);
    CHECK(dialog->isBuilt(dest::snippets));
    CHECK(dialog->pageContent(dest::snippets) == page);
    // The saved value was loaded, and loading saved nothing.
    CHECK(page->loads == 1);
    CHECK(page->check->isChecked());
    CHECK(page->ignoredToggles == 1);
    CHECK(rig.settings().snapshot()->at("flag") == true);

    // A click is saved through the dialog and shows in the rail.
    page->check->click();
    CHECK(rig.settings().snapshot()->at("flag") == false);
    CHECK(dialog->control("flag_check") == page->check);

    // The window's own refresh loads every built page from one snapshot.
    rig.settings().save_setting("flag", true);
    dialog->refresh();
    CHECK(page->loads == 2);
    CHECK(page->check->isChecked());
    CHECK(rig.settings().snapshot()->at("flag") == true);
}

TEST_CASE("the page's host outlives every widget torn down with the page", "[widget]") {
    SettingsRig rig;
    registerHome(rig);
    SwitchPage* page = nullptr;
    fv::registerPersonalizePage(rig.context(), switchDescriptor(&page));
    auto dialog = rig.dialog();
    dialog->ensurePage(dest::snippets);
    REQUIRE(page != nullptr);
    const QPointer<fv::DialogPersonalizeHost> host =
        dynamic_cast<fv::DialogPersonalizeHost*>(&page->host());
    REQUIRE(host != nullptr);
    CHECK(host->dialog() == dialog.get());

    // A control going down with the page (a capture field resuming hotkeys, a draft saved on
    // teardown) still reaches a live host and, through it, the window, which refuses the save.
    bool hostAlive = false;
    bool refused = false;
    QObject::connect(page->check, &QObject::destroyed, [&host, &hostAlive, &refused] {
        hostAlive = !host.isNull() && host->dialog() != nullptr;
        refused = hostAlive && host->isLoading() && !host->persist("late", 1);
    });
    dialog.reset();
    CHECK(hostAlive);
    CHECK(refused);
    CHECK_FALSE(rig.settings().snapshot()->contains("late"));
    // And the host goes with the window.
    CHECK(host.isNull());
}

TEST_CASE("a page built without a Settings window throws", "[widget]") {
    SettingsRig rig;
    fv::registerPersonalizePage(rig.context(), switchDescriptor());
    const fv::SettingsPageSpec* spec = rig.registry().page(dest::snippets);
    REQUIRE(spec != nullptr);
    QWidget plain;
    CHECK_THROWS_AS(spec->create(&plain), std::logic_error);
    fv::SettingsPage orphan;
    CHECK_THROWS_AS(spec->create(&orphan), std::logic_error);
}

TEST_CASE("a builder that makes no page is refused", "[widget]") {
    SettingsRig rig;
    fv::PageDescriptor descriptor = switchDescriptor();
    descriptor.create = [](fv::PersonalizeHost&, fv::SettingsPage&) -> fv::PersonalizePage* {
        return nullptr;
    };
    registerHome(rig);
    fv::registerPersonalizePage(rig.context(), std::move(descriptor));
    const auto dialog = rig.dialog();
    CHECK_THROWS_AS(dialog->ensurePage(dest::snippets), std::logic_error);
    CHECK_FALSE(dialog->isBuilt(dest::snippets));
}

TEST_CASE("DialogPersonalizeHost forwards to a stand-in dialog", "[widget]") {
    SettingsRig rig(fv::Json{{"keep", 1}});
    std::vector<std::string> calls;
    rig.registerStandIns([&](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::dictionary) {
            spec.controls = {"fake_page_tile"};
            spec.create = [&calls](QWidget* parent) -> QWidget* {
                return new fvtest::RecordingPage(calls, parent);
            };
        }
    });
    const auto dialog = rig.dialog();
    fv::DialogPersonalizeHost host(*dialog);
    CHECK(host.dialog() == dialog.get());
    CHECK_FALSE(host.isLoading());

    SECTION("settings and saves") {
        CHECK(host.settings().at("keep") == 1);
        CHECK(host.persist("name", "Ksenia"));
        CHECK(rig.settings().snapshot()->at("name") == "Ksenia");
        CHECK(host.persistMany(fv::Json{{"a", 1}, {"b", 2}}, {"keep"}));
        const auto saved = rig.settings().snapshot();
        CHECK(saved->at("a") == 1);
        CHECK(saved->at("b") == 2);
        CHECK_FALSE(saved->contains("keep"));
        // A value the file already holds is still a success.
        CHECK(host.persist("a", 1));
    }

    SECTION("mutate edits the saved mapping and tells why a refusal failed") {
        QString error = QStringLiteral("stale");
        CHECK(host.mutate([](fv::Json& settings) { settings["terms"] = fv::Json::array({"x"}); },
                          &error));
        CHECK(error.isEmpty());
        CHECK(rig.settings().snapshot()->at("terms") == fv::Json::array({"x"}));

        CHECK_FALSE(host.mutate(
            [](fv::Json& settings) {
                settings["terms"] = fv::Json::array();
                throw fv::py::ValueError("That word is already in your dictionary.");
            },
            &error));
        CHECK(error == QStringLiteral("That word is already in your dictionary."));
        // A refused change writes nothing.
        CHECK(rig.settings().snapshot()->at("terms") == fv::Json::array({"x"}));

        CHECK_FALSE(host.mutate([](fv::Json&) { throw std::runtime_error("disk"); }, &error));
        CHECK(error.isEmpty());
        CHECK_FALSE(host.mutate([](fv::Json&) { throw std::runtime_error("disk"); }));
    }

    SECTION("messages, destinations and notifications") {
        host.showMessage(QStringLiteral("Saved."));
        CHECK(dialog->messageText() == QStringLiteral("Saved."));
        host.showMessage(QString());
        CHECK(dialog->messageText().isEmpty());

        host.selectDestination(qs(dest::general));
        CHECK(dialog->currentDestination() == qs(dest::general));

        QStringList kinds;
        QObject::connect(dialog.get(), &fv::SettingsDialog::settingsChanged,
                         [&kinds](const QString& kind) { kinds << kind; });
        host.notifyChanged(QStringLiteral("dictionary"));
        CHECK(kinds == QStringList{QStringLiteral("dictionary")});

        std::vector<bool> suspended;
        QObject::connect(dialog.get(), &fv::SettingsDialog::hotkeyCaptureSuspended,
                         [&suspended](bool value) { suspended.push_back(value); });
        host.setHotkeyCaptureSuspended(true);
        host.setHotkeyCaptureSuspended(false);
        CHECK(suspended == std::vector<bool>{true, false});
    }

    SECTION("controls come from the pages that expose them") {
        QWidget* tile = host.control("fake_page_tile");
        REQUIRE(tile != nullptr);
        CHECK(dialog->isBuilt(dest::dictionary));
        CHECK(host.control("no_such_control") == nullptr);
        host.reveal(tile);
        fvtest::processEvents(2);
    }

    SECTION("the standard shortcuts are the defaults with the saved ones over them") {
        const fv::Json defaults = host.currentHotkeys();
        CHECK(defaults.contains("record_toggle"));
        CHECK(defaults.contains("cancel"));
        rig.settings().save_setting("hotkeys", fv::Json{{"record_toggle", "f9"}});
        const fv::Json saved = host.currentHotkeys();
        CHECK(saved.at("record_toggle") == "f9");
        CHECK(saved.at("cancel") == defaults.at("cancel"));
    }

    SECTION("a shortcut that cannot be saved comes back with its reason") {
        // No Hotkeys page is registered here, so nothing can save it.
        CHECK_FALSE(
            host.setStandardHotkey(QStringLiteral("cancel"), QStringLiteral("f8")).isEmpty());
    }
}

TEST_CASE("a host used after its window is gone answers with nothing", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    fv::DialogPersonalizeHost host(*dialog);
    dialog.reset();
    CHECK(host.dialog() == nullptr);
    CHECK(host.settings() == fv::Json::object());
    CHECK_FALSE(host.persist("a", 1));
    CHECK_FALSE(host.persistMany(fv::Json{{"a", 1}}));
    CHECK_FALSE(host.mutate([](fv::Json&) {}));
    CHECK_FALSE(host.isLoading());
    CHECK(host.control("anything") == nullptr);
    CHECK(host.currentHotkeys() == fv::Json::object());
    CHECK_FALSE(host.setStandardHotkey(QStringLiteral("cancel"), QStringLiteral("f8")).isEmpty());
    host.showMessage(QStringLiteral("late"));
    host.notifyChanged(QStringLiteral("styles"));
    host.selectDestination(QStringLiteral("general"));
    host.setHotkeyCaptureSuspended(true);
    host.reveal(nullptr);
}

TEST_CASE("FakePersonalizeHost records every call and saves like the window", "[unit]") {
    fv::FakePersonalizeHost host;
    host.saved = fv::Json{{"a", 1}};
    CHECK(host.settings() == fv::Json{{"a", 1}});
    CHECK(host.persist("b", 2));
    CHECK(host.persistMany(fv::Json{{"c", 3}}, {"a", "b"}));
    CHECK(host.saved == fv::Json{{"c", 3}});
    host.showMessage(QStringLiteral("Hello"));
    CHECK(host.message == QStringLiteral("Hello"));
    host.notifyChanged(QStringLiteral("snippets"));
    host.selectDestination(QStringLiteral("styles"));
    host.setHotkeyCaptureSuspended(true);
    host.standardHotkeyAnswer = QStringLiteral("clash");
    CHECK(host.setStandardHotkey(QStringLiteral("cancel"), QStringLiteral("f8")) ==
          QStringLiteral("clash"));
    CHECK(host.currentHotkeys() == fv::Json::object());
    CHECK(host.control("x") == nullptr);
    CHECK(host.calls == std::vector<std::string>{
                            "settings()", "persist(b, 2)", "persistMany({\"c\":3}, [a,b])",
                            "showMessage(Hello)", "notifyChanged(snippets)",
                            "selectDestination(styles)", "setHotkeyCaptureSuspended(true)",
                            "setStandardHotkey(cancel, f8)", "currentHotkeys()", "control(x)"});

    // Loading refuses saves; mutate keeps nothing from a change that threw.
    host.loading = true;
    CHECK_FALSE(host.persist("d", 4));
    CHECK_FALSE(host.persistMany(fv::Json{{"d", 4}}));
    host.loading = false;
    QString error;
    CHECK_FALSE(host.mutate(
        [](fv::Json& settings) {
            settings["d"] = 4;
            throw fv::py::ValueError("Refused.");
        },
        &error));
    CHECK(error == QStringLiteral("Refused."));
    CHECK_FALSE(host.saved.contains("d"));
    CHECK(host.mutate([](fv::Json& settings) { settings["e"] = 5; }, &error));
    CHECK(host.saved.at("e") == 5);
    host.failSaves = true;
    host.mutateError = QStringLiteral("Couldn't save.");
    CHECK_FALSE(host.mutate([](fv::Json&) {}, &error));
    CHECK(error == QStringLiteral("Couldn't save."));
}

TEST_CASE("ShowWatcher calls back on every show and survives a callback that throws", "[widget]") {
    QWidget widget;
    int shown = 0;
    new fv::ShowWatcher(&widget, [&shown] { ++shown; });
    widget.show();
    CHECK(shown == 1);
    widget.hide();
    widget.show();
    CHECK(shown == 2);

    QWidget other;
    int failures = 0;
    new fv::ShowWatcher(&other, [&failures] {
        ++failures;
        throw std::runtime_error("callback");
    });
    other.show();
    other.hide();
    other.show();
    CHECK(failures == 2);
    CHECK(other.isVisible());
}

namespace {

// How many signals a class declares itself (not those of its Qt bases).
int declaredSignals(const QMetaObject& meta) {
    int count = 0;
    for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
        if (meta.method(index).methodType() == QMetaMethod::Signal) {
            ++count;
        }
    }
    return count;
}

} // namespace

// "<feature> connects every signal" (architecture 8.5). The foundation declares no signal; the
// pages' own classes and their signals are counted in test_personalize_feature.cpp, and a
// signal added to one of these classes fails the count until it is connected or listed here.
TEST_CASE("PersonalizeFeature connects every signal", "[widget]") {
    SettingsRig rig;
    fv::PersonalizeFeature feature;
    CHECK(feature.id() == "personalize");
    CHECK(feature.required_features() == std::vector<std::string_view>{"settings"});
    feature.provide(rig.context());
    feature.install(rig.context());
    // AI cleanup, Learned rules, Dictionary, Snippets, Apps & styles, Profiles and Commands
    // register themselves; nothing is built yet.
    CHECK(rig.registry().pages().size() == 7);

    for (const QMetaObject* meta :
         {&fv::LevelTrace::staticMetaObject, &fv::RuleActivityStrip::staticMetaObject,
          &fv::ItemGlow::staticMetaObject, &fv::CleanupRuleDialog::staticMetaObject,
          &fv::CleanupPromptDialog::staticMetaObject, &fv::ShowWatcher::staticMetaObject,
          &fv::PersonalizePage::staticMetaObject, &fv::DialogPersonalizeHost::staticMetaObject}) {
        INFO(meta->className());
        CHECK(declaredSignals(*meta) == 0);
    }
}
