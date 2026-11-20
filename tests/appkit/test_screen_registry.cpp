// ScreenRegistry: MainWindow's lazy screens and update_screen / update_transcription_tabs replay
// (ui_qt/main_window.py:710-787).

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QObject>
#include <QString>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include "appkit/order.h"
#include "appkit/registries.h"

namespace {

// Stands in for a transcription tab: records every setter call it receives.
class FakeScreen : public QWidget {
public:
    explicit FakeScreen(QWidget* parent) : QWidget(parent) {}

    void set_model_downloading(const std::string& model, bool downloading) {
        calls.push_back("set_model_downloading " + model + (downloading ? " True" : " False"));
        if (downloading) {
            model_downloads.insert(model);
        } else {
            model_downloads.erase(model);
        }
    }

    std::vector<std::string> calls;
    std::set<std::string> model_downloads;
    fv::Json meeting_state;
};

FakeScreen* fake(QWidget* widget) {
    return static_cast<FakeScreen*>(widget);
}

// A setter call kept by the registry, as update_screen(key, method, value).
fv::ScreenRegistry::Apply call(std::string method, std::string value = {}) {
    return [method = std::move(method), value = std::move(value)](QWidget* widget) {
        fake(widget)->calls.push_back(value.empty() ? method : method + " " + value);
    };
}

void downloading(fv::ScreenRegistry& registry, const std::string& model, bool active) {
    registry.update(
        "upload", "set_model_downloading",
        [model, active](QWidget* widget) { fake(widget)->set_model_downloading(model, active); },
        model);
}

struct Fixture {
    QWidget window;
    fv::ScreenRegistry registry;
    int created = 0;

    Fixture() {
        const auto factory = [this](QWidget* parent) -> QWidget* {
            ++created;
            return new FakeScreen(parent);
        };
        registry.define("quick_record", fv::order::tab::quick_record,
                        QStringLiteral("Quick Record"), factory, "transcription");
        registry.define("upload", fv::order::tab::upload_file, QStringLiteral("Upload File"),
                        factory, "transcription");
        registry.define("meeting", fv::order::tab::meeting_mode, QStringLiteral("Meeting Mode"),
                        factory);
    }
};

} // namespace

TEST_CASE("inactive screens stay unbuilt and keep the latest call per method",
          "[widget][test_deferred_screens]") {
    // The update_screen part of tests/test_deferred_screens.py::
    // test_controller_startup_keeps_inactive_views_unbuilt
    Fixture f;
    f.registry.update("upload", "set_engine_busy", call("set_engine_busy", "True"));
    f.registry.updateGroup("transcription", "set_device_info", call("set_device_info", "CPU"));
    downloading(f.registry, "tiny", true);
    downloading(f.registry, "base", true);
    downloading(f.registry, "tiny", false);
    f.registry.update("meeting", "set_dashboard_available", call("set_dashboard_available"));
    CHECK(f.created == 0);
    CHECK(f.registry.existing("upload") == nullptr);

    auto* upload = fake(f.registry.ensure("upload", &f.window));
    CHECK(upload->parentWidget() == &f.window);
    CHECK(upload->model_downloads == std::set<std::string>{"base"});
    // Group calls first, then the screen's own in the order of each one's latest call: the
    // repeated "tiny" moved behind "base".
    CHECK(upload->calls == std::vector<std::string>{"set_device_info CPU", "set_engine_busy True",
                                                    "set_model_downloading base True",
                                                    "set_model_downloading tiny False"});
    CHECK(f.created == 1);
    CHECK(f.registry.existing("upload") == upload);
    CHECK(f.registry.existing("meeting") == nullptr);
    CHECK(f.registry.ensure("upload", &f.window) == upload);
    CHECK(f.created == 1);

    // Once built, calls go straight to the screen.
    upload->calls.clear();
    f.registry.update("upload", "set_engine_busy", call("set_engine_busy", "False"));
    CHECK(upload->calls == std::vector<std::string>{"set_engine_busy False"});
}

TEST_CASE("replay keeps the order of each method's latest call", "[widget]") {
    Fixture f;
    f.registry.update("meeting", "a", call("a", "1"));
    f.registry.update("meeting", "b", call("b", "1"));
    f.registry.update("meeting", "c", call("c", "1"));
    f.registry.update("meeting", "a", call("a", "2"));
    auto* meeting = fake(f.registry.ensure("meeting", &f.window));
    CHECK(meeting->calls == std::vector<std::string>{"b 1", "c 1", "a 2"});
}

TEST_CASE("set_meeting_state merges dict updates", "[widget]") {
    Fixture f;
    const auto apply = [](QWidget* widget, const fv::Json& state) {
        fake(widget)->meeting_state = state;
        fake(widget)->calls.push_back("set_meeting_state " + state.dump());
    };
    CHECK(f.registry.mergedState("meeting", "set_meeting_state") == nullptr);
    f.registry.updateMerged("meeting", "set_meeting_state",
                            fv::Json{{"active", false}, {"status", "idle"}}, apply);
    f.registry.update("meeting", "set_dashboard_available", call("set_dashboard_available"));
    f.registry.updateMerged("meeting", "set_meeting_state",
                            fv::Json{{"active", false}, {"dashboard_available", false}}, apply);
    const fv::Json* kept = f.registry.mergedState("meeting", "set_meeting_state");
    REQUIRE(kept != nullptr);
    CHECK(kept->dump() == R"({"active":false,"status":"idle","dashboard_available":false})");

    auto* meeting = fake(f.registry.ensure("meeting", &f.window));
    CHECK(meeting->calls ==
          std::vector<std::string>{
              "set_dashboard_available",
              R"(set_meeting_state {"active":false,"status":"idle","dashboard_available":false})"});

    meeting->calls.clear();
    f.registry.updateMerged("meeting", "set_meeting_state", fv::Json{{"active", true}}, apply);
    CHECK(meeting->meeting_state.dump() ==
          R"({"active":true,"status":"idle","dashboard_available":false})");
    CHECK(f.registry.mergedState("meeting", "set_meeting_state")->at("active") == true);

    CHECK_THROWS_AS(
        f.registry.updateMerged("meeting", "set_meeting_state", fv::Json::array(), apply),
        std::invalid_argument);
}

TEST_CASE("group updates reach every existing member and replay on creation", "[widget]") {
    Fixture f;
    auto* quick = fake(f.registry.ensure("quick_record", &f.window));
    f.registry.updateGroup("transcription", "set_remote_models", call("set_remote_models", "1"));
    f.registry.updateGroup("transcription", "set_remote_link", call("set_remote_link"));
    f.registry.updateGroup("transcription", "set_remote_models", call("set_remote_models", "2"));
    CHECK(quick->calls == std::vector<std::string>{"set_remote_models 1", "set_remote_link",
                                                   "set_remote_models 2"});
    CHECK(f.registry.existingInGroup("transcription") == std::vector<QWidget*>{quick});

    // The collapse sync keeps its place in the replay order and skips its source.
    f.registry.syncGroup("transcription", "set_remote_link", call("set_remote_link", "synced"),
                         quick);
    CHECK(quick->calls.size() == 3);

    auto* upload = fake(f.registry.ensure("upload", &f.window));
    CHECK(upload->calls ==
          std::vector<std::string>{"set_remote_link synced", "set_remote_models 2"});
    CHECK(f.registry.existingInGroup("transcription") == std::vector<QWidget*>{quick, upload});

    upload->calls.clear();
    quick->calls.clear();
    f.registry.syncGroup("transcription", "set_transcription_collapsed",
                         call("set_transcription_collapsed"), upload);
    CHECK(quick->calls == std::vector<std::string>{"set_transcription_collapsed"});
    CHECK(upload->calls.empty());
    CHECK(f.registry.existingInGroup("other").empty());
}

TEST_CASE("creation places the screen before the replay and then signals it", "[widget]") {
    Fixture f;
    std::vector<std::string> events;
    f.registry.update("upload", "set_engine_busy", [&](QWidget*) { events.push_back("replay"); });
    f.registry.setPlacer([&](std::string_view key, QWidget* screen) {
        CHECK(f.registry.existing(key) == screen);
        events.push_back("place " + std::string(key));
    });
    QObject::connect(&f.registry, &fv::ScreenRegistry::screenCreated,
                     [&](const QString& key, QWidget* screen) {
                         CHECK(f.registry.existing(key.toStdString()) == screen);
                         events.push_back("created " + key.toStdString());
                     });
    f.registry.ensure("upload", &f.window);
    CHECK(events == std::vector<std::string>{"place upload", "replay", "created upload"});
}

TEST_CASE("a screen deleted from outside is created again with the kept calls", "[widget]") {
    Fixture f;
    f.registry.update("upload", "set_engine_busy", call("set_engine_busy", "True"));
    delete f.registry.ensure("upload", &f.window);
    CHECK(f.registry.existing("upload") == nullptr);
    auto* again = fake(f.registry.ensure("upload", &f.window));
    CHECK(f.created == 2);
    CHECK(again->calls == std::vector<std::string>{"set_engine_busy True"});
}

TEST_CASE("screen definitions are checked and listed by order", "[widget]") {
    Fixture f;
    CHECK(f.registry.isDefined("upload"));
    CHECK_FALSE(f.registry.isDefined("host"));
    CHECK_THROWS_MATCHES(f.registry.ensure("host", &f.window), std::invalid_argument,
                         Catch::Matchers::Message("No screen is defined with the key 'host'."));
    CHECK_THROWS_MATCHES(
        f.registry.define("upload", 1, QStringLiteral("Again"), [](QWidget*) { return nullptr; }),
        std::invalid_argument,
        Catch::Matchers::Message("A screen with the key 'upload' is already defined."));
    CHECK_THROWS_AS(f.registry.define("", 1, QStringLiteral("No key"), {}), std::invalid_argument);

    f.registry.define("broken", 900, QStringLiteral("Broken"), [](QWidget*) { return nullptr; });
    CHECK_THROWS_MATCHES(
        f.registry.ensure("broken", &f.window), std::logic_error,
        Catch::Matchers::Message("The screen 'broken' was not created: its factory returned "
                                 "nothing."));

    f.registry.addScreen(
        fv::ScreenSpec{fv::order::tab::walkthrough, QStringLiteral("Walkthrough"), {}});
    f.registry.addSidePanel(
        fv::ScreenSpec{fv::order::side_panel::walkthroughs, QStringLiteral("Walkthroughs"), {}});
    f.registry.addSidePanel(
        fv::ScreenSpec{fv::order::side_panel::history, QStringLiteral("History"), {}});
    std::vector<QString> titles;
    for (const auto& spec : f.registry.screens()) {
        titles.push_back(spec.title);
    }
    CHECK(titles == std::vector<QString>{QStringLiteral("Quick Record"),
                                         QStringLiteral("Upload File"),
                                         QStringLiteral("Walkthrough"),
                                         QStringLiteral("Meeting Mode"), QStringLiteral("Broken")});
    titles.clear();
    for (const auto& spec : f.registry.sidePanels()) {
        titles.push_back(spec.title);
    }
    CHECK(titles ==
          std::vector<QString>{QStringLiteral("History"), QStringLiteral("Walkthroughs")});
}
