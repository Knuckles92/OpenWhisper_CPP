#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QThread>

#include "agentapi/testing/fake_mcp_control.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_automation/mcp_page.h"
#include "ui_automation/session_mcp.h"
#include "ui_history/history_feature.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_settings/app_pages.h"
#include "ui_widgets/settings_switch.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {

struct SettingsNotifications {
    fv::MemorySettingsStore settings;
    fv::qt::QtUiExecutor ui{qApp};
    fv::AppContext context{fv::AppServices{.settings = &settings, .ui = &ui}};
    std::shared_ptr<fv::SettingsPagesSignals> pages = std::make_shared<fv::SettingsPagesSignals>();
    std::shared_ptr<fv::HistoryPanel> history = std::make_shared<fv::HistoryPanel>();
    std::shared_ptr<fv::CleanupGates> gates = std::make_shared<fv::CleanupGates>(settings);
    std::shared_ptr<fv::CleanupSettings> cleanup =
        std::make_shared<fv::CleanupSettings>(context, *gates);

    SettingsNotifications() {
        context.provide(pages);
        context.provide(history);
        context.provide(gates);
        context.provide(cleanup);
    }
};

struct DesktopMode {
    const std::string key = std::string(fv::identity().env_prefix) + "UI";
    const std::optional<std::string> previous = fv::testkit::get_env(key);

    explicit DesktopMode(const std::string& mode) { fv::testkit::set_env(key, mode); }
    ~DesktopMode() { fv::testkit::set_env(key, previous); }
};

} // namespace

TEST_CASE("MCP settings changes apply on the GUI thread using the latest saved values",
          "[widget]") {
    SettingsNotifications fixture;
    std::vector<std::pair<QString, QThread*>> themes;
    QObject::connect(
        fixture.pages.get(), &fv::SettingsPagesSignals::uiThemeChanged, fixture.pages.get(),
        [&](const QString& value) { themes.emplace_back(value, QThread::currentThread()); });
    const auto notify = fv::sessionMcpDesktopChanges(fixture.context);
    fixture.settings.save_setting("ui_theme", "dark");
    std::thread worker([&] { notify("settings", fv::Json{{"ui_theme", "light"}}); });
    worker.join();
    CHECK(themes.empty());
    QApplication::processEvents();
    CHECK(themes ==
          std::vector<std::pair<QString, QThread*>>{{QStringLiteral("dark"), qApp->thread()}});
}

TEST_CASE("MCP theme changes resolve the latest inherited theme after reset", "[widget]") {
    const std::string mode = GENERATE(std::string("classic"), std::string("omarchy"));
    DesktopMode desktop_mode(mode);
    SettingsNotifications fixture;
    QStringList themes;
    QObject::connect(fixture.pages.get(), &fv::SettingsPagesSignals::uiThemeChanged,
                     fixture.pages.get(), [&](const QString& value) { themes.append(value); });
    fixture.settings.save_setting("ui_theme", "light");
    const auto notify = fv::sessionMcpDesktopChanges(fixture.context);
    std::thread worker([&] { notify("settings", fv::Json{{"ui_theme", "light"}}); });
    worker.join();
    fixture.settings.update(fv::Json::object(), {"ui_theme"});
    QApplication::processEvents();
    CHECK(themes ==
          QStringList{mode == "omarchy" ? QStringLiteral("omarchy") : QStringLiteral("dark")});
}

TEST_CASE("MCP desktop changes refresh saved history and existing settings controls", "[widget]") {
    SettingsNotifications fixture;
    int history = 0;
    fixture.history->attachCalendar({}, [&] { ++history; });
    int preview = 0;
    int cleanup = 0;
    int font = 0;
    QString layout;
    QString trigger;
    QObject::connect(fixture.pages.get(), &fv::SettingsPagesSignals::streamingSettingsChanged,
                     fixture.pages.get(), [&] { ++preview; });
    QObject::connect(fixture.pages.get(), &fv::SettingsPagesSignals::streamingFontChanged,
                     fixture.pages.get(), [&](int size) { font = size; });
    QObject::connect(fixture.pages.get(), &fv::SettingsPagesSignals::streamingLayoutChanged,
                     fixture.pages.get(), [&](const QString& value) { layout = value; });
    QObject::connect(fixture.pages.get(), &fv::SettingsPagesSignals::recordingTriggerModeChanged,
                     fixture.pages.get(), [&](const QString& value) { trigger = value; });
    QObject::connect(fixture.cleanup.get(), &fv::CleanupSettings::cleanupChanged,
                     fixture.cleanup.get(), [&] { ++cleanup; });
    const auto notify = fv::sessionMcpDesktopChanges(fixture.context);
    const fv::Json changes{{"streaming_enabled", true},
                           {"streaming_overlay_font_size", 24},
                           {"streaming_overlay_layout", "footer"},
                           {"transcript_cleanup_enabled", false},
                           {"recording_trigger_mode", "push_hold"}};
    fixture.settings.update(changes);
    std::thread worker([&] {
        notify("transcription", fv::Json{{"id", "dictation"}});
        notify("meeting", fv::Json{{"id", "saved-meeting"}});
        notify("settings", changes);
    });
    worker.join();
    CHECK(history == 0);
    CHECK(preview == 0);
    QApplication::processEvents();
    CHECK(history == 2);
    CHECK(preview == 1);
    CHECK(cleanup == 1);
    CHECK(font == 24);
    CHECK(layout == QStringLiteral("footer"));
    CHECK(trigger == QStringLiteral("push_hold"));
}

TEST_CASE("a saved MCP preference needs fresh named local access before the listener starts",
          "[widget]") {
    fv::MemorySettingsStore settings(
        fv::Json{{"mcp_enabled", true}, {"mcp_retitle_meetings", true}});
    auto fake = std::make_shared<fv::agentapi::FakeMcpControl>();
    int prompts = 0;
    bool allow = false;
    bool change = false;
    auto control = fv::sessionMcpControl(fake, settings, [&](const QString& text) {
        ++prompts;
        CHECK(text.contains(QStringLiteral("127.0.0.1:8767")));
        CHECK(text.contains(QStringLiteral("transcripts")));
        CHECK(text.contains(QStringLiteral("rename finished meetings")));
        if (change)
            settings.save_setting("mcp_retitle_meetings", false);
        return allow;
    });
    CHECK(prompts == 0);
    CHECK(control->status().state == "stopped");
    control->start(8767, false);
    CHECK(control->status().state == "stopped");
    CHECK_THROWS(control->start(8767, true));
    allow = true;
    control->start(8767, false);
    CHECK(control->status().state == "running");
    REQUIRE(fake->session_access);
    CHECK(fake->session_access());
    settings.save_setting("mcp_settings_access", true);
    CHECK_FALSE(fake->session_access());
    settings.save_setting("mcp_settings_access", false);
    CHECK_FALSE(fake->session_access());
    control->stop();
    change = true;
    CHECK_THROWS(control->start(8767, false));
    CHECK(control->status().state == "stopped");
    CHECK(prompts == 3);
}

TEST_CASE("local MCP switch can retry after a declined session with saved access", "[widget]") {
    fv::MemorySettingsStore settings(
        fv::Json{{"mcp_enabled", true}, {"mcp_tailscale_enabled", true}});
    auto fake = std::make_shared<fv::agentapi::FakeMcpControl>();
    bool allow = false;
    auto control = fv::sessionMcpControl(fake, settings, [&](const QString&) { return allow; });
    fv::McpSettingsPage page(settings, *control);
    CHECK_FALSE(page.enabled()->isChecked());
    page.enabled()->click();
    CHECK_FALSE(page.enabled()->isChecked());
    allow = true;
    page.enabled()->click();
    CHECK(page.enabled()->isChecked());
    CHECK(fake->calls().back().flag == false);
    page.enabled()->click();
    CHECK_FALSE(page.enabled()->isChecked());
}
