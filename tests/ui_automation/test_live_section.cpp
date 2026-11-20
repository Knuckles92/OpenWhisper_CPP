// Settings > Automate > API & MCP, the live API, MCP Events and Claude Code channel cards
// (tests/test_automation_ui_api_page.py), and the page as a Settings window builds it. The two
// tests of the paired host's tab (test_host_tab_hides_this_computers_live_cards_and_says_where_
// they_are, test_the_host_page_keeps_its_own_controls_without_live_cards) go with the remote
// engine's host management.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPoint>
#include <QScrollBar>
#include <QSignalSpy>

#include "agentapi/setup.h"
#include "automation/permissions.h"
#include "automation_support.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "mcp_support.h"
#include "settings/keys.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::agentapi::ServerStatus;
using fv::automation::permissions::ALL;
using namespace fvtest;
namespace permissions = fv::automation::permissions;
namespace live = fv::live_section;

namespace {

const char* const kApi = "[test_automation_ui_api_page][widget]";

const std::vector<std::string> kPermissions{"api_live_events", "api_run_actions",
                                            "api_control_recording", "api_text_filters",
                                            "api_agent_prompts"};
constexpr const char* kAgent = "api_agent_prompts";

bool has(const QString& haystack, const char* needle) {
    return haystack.contains(QString::fromUtf8(needle));
}

// running(): the listener on `port`.
void running(McpRig& rig, fv::LiveAutomationSection& section, int port = 8767) {
    rig.server.current = ServerStatus{"running", "Ready", port, ""};
    section.refreshLive();
}

// QTest.mouseClick(row, Left, pos=QPoint(4, 4)): the press a row toggles on.
void pressRow(QWidget* row, QPoint at = QPoint(4, 4)) {
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(at), QPointF(row->mapToGlobal(at)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(row, &event);
}

// The channel entry as the section shows it: this process's executable and folders.
std::string channelEntry() {
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    return fv::agentapi::claude_channel_config(
        std::filesystem::path(QApplication::applicationFilePath().toStdWString()), paths);
}

} // namespace

TEST_CASE("TestLiveApi::test_permissions_default_to_reading_only_and_persist", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const auto& checks = section->apiPermissionChecks();
    std::vector<std::string> order;
    for (const auto& [key, check] : checks) {
        order.push_back(key);
    }
    CHECK(order == kPermissions);
    CHECK(section->apiPermission("api_live_events")->isChecked());
    for (std::size_t i = 1; i < kPermissions.size(); ++i) {
        CHECK_FALSE(section->apiPermission(kPermissions[i])->isChecked());
    }
    for (std::size_t i = 1; i < kPermissions.size(); ++i) {
        section->apiPermission(kPermissions[i])->click();
        CHECK(rig.saved(kPermissions[i]) == Json(true));
    }
    section->apiPermission("api_live_events")->click();
    CHECK(rig.saved("api_live_events") == Json(false));
    CHECK_FALSE(section->notice()->isVisible());
}

TEST_CASE("TestLiveApi::test_clicking_a_permission_row_toggles_it", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    fv::SettingsSwitch* check = section->apiPermission("api_control_recording");
    pressRow(check->parentWidget());
    CHECK(rig.saved("api_control_recording") == Json(true));
}

TEST_CASE("TestLiveApi::test_a_failed_save_says_so_and_shows_what_is_saved", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    fv::SettingsSwitch* check = section->apiPermission("api_text_filters");
    rig.store.file().fail_next_replace("read only");
    check->click();
    CHECK_FALSE(check->isChecked());
    CHECK(text(section->notice()->text()) == "Couldn't save that permission. Try again.");
}

TEST_CASE("TestLiveApi::test_saved_choices_are_reread_when_shown", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    rig.store.save_setting("api_run_actions", true);
    section->refresh();
    CHECK(section->apiPermission("api_run_actions")->isChecked());
}

TEST_CASE("TestLiveApi::test_address_and_endpoints_need_mcp_running", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    CHECK_FALSE(section->apiUrl()->isEnabled());
    CHECK(has(section->apiUrl()->text(), "Turn on MCP"));
    for (const QPushButton* button : section->endpointButtons()) {
        CHECK_FALSE(button->isEnabled());
    }
    CHECK(text(section->listenersLabel()->text()) == "Off while MCP is off");
    running(rig, *section, 9123);
    CHECK((section->apiUrl()->isEnabled() &&
           text(section->apiUrl()->text()) == "http://127.0.0.1:9123"));
    section->copyApiUrl()->click();
    CHECK(text(rig.clipboard.last()) == "http://127.0.0.1:9123");
    std::vector<std::string> paths;
    for (const live::Endpoint& endpoint : live::LIVE_ENDPOINTS) {
        paths.emplace_back(endpoint.path);
    }
    for (const char* wanted : {"/v1/events", "/v1/actions", "/v1/recording/start", "/v1/filters"}) {
        CHECK(std::find(paths.begin(), paths.end(), wanted) != paths.end());
    }
    const auto events = std::find(paths.begin(), paths.end(), "/v1/events") - paths.begin();
    section->endpointButtons()[static_cast<std::size_t>(events)]->click();
    CHECK(text(rig.clipboard.last()) == "http://127.0.0.1:9123/v1/events");
    section->copyExample()->click();
    CHECK(text(rig.clipboard.last()) ==
          "curl -N -H \"Authorization: Bearer " + kMcpToken + "\" http://127.0.0.1:9123/v1/events");
    // The token is copied, never shown.
    CHECK(section->apiUrl()->text().contains(QString::fromStdString(kMcpToken)) == false);
    for (const QPushButton* button : section->endpointButtons()) {
        CHECK_FALSE(button->text().contains(QString::fromStdString(kMcpToken)));
    }
}

TEST_CASE("TestLiveApi::test_listening_programs_are_counted_live_from_the_page_timer", kApi) {
    McpRig rig;
    auto view = rig.makeView();
    fv::LiveAutomationSection& section = *view->live();
    running(rig, section);
    CHECK(text(section.listenersLabel()->text()) == "No programs listening yet");
    CHECK(section.listenersDot()->status() == fv::EngineStatus::Unknown);
    auto first = rig.bus.listen(std::string(live::SSE_LISTENER_LABEL));
    auto second = rig.bus.listen(std::string(live::SSE_LISTENER_LABEL));
    auto own = rig.bus.listen("webhooks"); // the app's own delivery
    QMetaObject::invokeMethod(view->local()->timer(), "timeout");
    CHECK(text(section.listenersLabel()->text()) == "2 programs listening");
    CHECK(section.listenersDot()->status() == fv::EngineStatus::Ready);
    first->close();
    QMetaObject::invokeMethod(view->local()->timer(), "timeout");
    CHECK(text(section.listenersLabel()->text()) == "1 program listening");
}

TEST_CASE("TestMcpEvents::test_switch_persists_and_tells_the_app", kApi) {
    McpRig rig;
    auto view = rig.makeView();
    QSignalSpy changed(view.get(), &fv::McpSettingsView::settingsChanged);
    fv::SettingsSwitch* toggle = view->live()->mcpEventsSwitch();
    CHECK_FALSE(toggle->isChecked());
    CHECK(text(view->live()->subscriptionsLabel()->text()) == "Off. Agents can't subscribe.");
    toggle->click();
    CHECK(rig.saved("mcp_events_enabled") == Json(true));
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == "mcp_events");
    CHECK(text(view->live()->subscriptionsLabel()->text()) == "No subscriptions yet");
    toggle->click();
    CHECK(rig.saved("mcp_events_enabled") == Json(false));
    CHECK(changed.count() == 2);
    CHECK(changed.at(1).at(0).toString() == "mcp_events");
}

TEST_CASE("TestMcpEvents::test_a_failed_save_does_not_tell_the_app", kApi) {
    McpRig rig;
    auto view = rig.makeView();
    QSignalSpy changed(view.get(), &fv::McpSettingsView::settingsChanged);
    rig.store.file().fail_next_replace("read only");
    view->live()->mcpEventsSwitch()->click();
    CHECK_FALSE(view->live()->mcpEventsSwitch()->isChecked());
    CHECK(changed.count() == 0);
    CHECK(has(view->live()->notice()->text(), "Couldn't save MCP Events"));
}

TEST_CASE("TestMcpEvents::test_subscription_count_and_clear", kApi) {
    McpRig rig;
    rig.events.count = 3;
    auto section = rig.makeSection();
    section->refreshLive();
    CHECK(text(section->subscriptionsLabel()->text()) == "3 subscriptions");
    CHECK_FALSE(section->clearSubscriptions()->isHidden());
    section->clearSubscriptions()->click();
    CHECK_FALSE(section->clearSubscriptions()->isEnabled());
    REQUIRE(waitFor([&] { return text(section->notice()->text()) == "Cleared 3 subscriptions."; }));
    CHECK(rig.events.clears == 1);
    CHECK(section->clearSubscriptions()->isHidden());
}

TEST_CASE("TestMcpEvents::test_clear_errors_are_reported", kApi) {
    McpRig rig;
    rig.events.count = 1;
    rig.events.onClear = []() -> std::size_t {
        throw std::runtime_error("store locked");
    };
    auto section = rig.makeSection();
    section->refreshLive();
    section->clearSubscriptions()->click();
    REQUIRE(waitFor([&] { return has(section->notice()->text(), "store locked"); }));
    CHECK(section->clearSubscriptions()->isEnabled());
}

TEST_CASE("TestChannel::test_setup_snippets_copy_exactly", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const std::string entry = channelEntry();
    CHECK(Json::parse(text(section->channelConfig()->toPlainText())) == Json::parse(entry));
    section->copyChannelConfig()->click();
    CHECK(text(rig.clipboard.last()) == entry);
    section->copyChannelCommand()->click();
    CHECK(text(rig.clipboard.last()) == fv::agentapi::claude_channel_command());
    CHECK(text(section->channelCommand()->text()) == fv::agentapi::claude_channel_command());
    // The entry is a preview that opens on request.
    CHECK(section->channelPreview()->body()->isHidden());
    section->channelPreview()->toggle()->click();
    CHECK_FALSE(section->channelPreview()->body()->isHidden());
}

TEST_CASE("TestChannel::test_the_card_keeps_the_entry_to_projects_started_with_the_flag", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const QString scope = section->channelScope()->text();
    CHECK((has(scope, "user-wide") && has(scope, "can't receive messages")));
    CHECK(fv::agentapi::claude_channel_command().find("--dangerously-load-development-channels") !=
          std::string::npos);
}

TEST_CASE("TestChannel::test_connected_sessions_are_counted", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    CHECK(text(section->sessionsLabel()->text()) == "Needs MCP on");
    running(rig, *section);
    CHECK(text(section->sessionsLabel()->text()) == "No sessions connected");
    auto session = rig.bus.listen(std::string(live::CHANNEL_LISTENER_LABEL));
    section->refreshLive();
    // "Connected", not "listening": one started without the flag counts too.
    CHECK(text(section->sessionsLabel()->text()) == "1 session connected");
    CHECK(section->sessionsDot()->status() == fv::EngineStatus::Ready);
    // A channel session is not a plain event-stream program.
    CHECK(text(section->listenersLabel()->text()) == "No programs listening yet");
}

TEST_CASE("test_live_cards_fit_and_split_into_columns_below_the_dialog_width", kApi) {
    // Python also runs this with OPENWHISPER_UI=omarchy, a mode that is not ported.
    for (const int fontPercent : {100, 130}) {
        DYNAMIC_SECTION("font " << fontPercent << " %") {
            AppTheme theme("dark", fontPercent);
            McpRig rig;
            rig.server.current = ServerStatus{"running", "Ready", 8767, ""};
            auto view = rig.makeView();
            view->live()->endpoints()->toggle()->click();
            view->live()->channelPreview()->toggle()->click();
            McpColumn column(*view, true);
            column.scroll->show();
            fv::LiveAutomationSection& live = *view->live();
            // The Settings dialog's page is about 983 px wide at its default size.
            for (const int width : {360, 480, 720, 983, 1300}) {
                column.scroll->resize(width, 900);
                McpColumn::settle();
                INFO("width " << width);
                CHECK(column.scroll->horizontalScrollBar()->maximum() == 0);
                const bool wide =
                    live.width() >= static_cast<int>(std::lround(740.0 * fontPercent / 100));
                if (width >= 983) {
                    CHECK(wide);
                }
                if (wide) {
                    CHECK(live.mcpEventsCard()->x() > live.liveApiCard()->x());
                    CHECK(live.channelCard()->y() > live.mcpEventsCard()->y());
                } else {
                    CHECK(live.mcpEventsCard()->y() > live.liveApiCard()->y());
                    CHECK(live.channelCard()->y() > live.mcpEventsCard()->y());
                }
                for (QAbstractButton* button : live.findChildren<QAbstractButton*>()) {
                    if (!button->isVisible()) {
                        continue;
                    }
                    INFO(text(button->text()));
                    const QPoint position = button->mapTo(&live, QPoint(0, 0));
                    CHECK(position.x() >= 0);
                    CHECK(position.x() + button->width() <= live.width() + 1);
                    CHECK(button->height() >= std::min(button->sizeHint().height(), 34) - 1);
                }
            }
        }
    }
}

TEST_CASE("test_settings_page_wires_mcp_events_to_the_app", kApi) {
    ActionsRig rig;
    ScriptedMcpControl server;
    ScopedBus bus;
    rig.context().provide<fv::agentapi::McpServerControl>(
        std::shared_ptr<fv::agentapi::McpServerControl>(&server, [](auto*) {}));
    fv::SettingsDialog& dialog = rig.dialog(false);
    CHECK_FALSE(dialog.isBuilt(dest::mcp));
    dialog.selectDestination(qs(dest::mcp));
    CHECK(dialog.isBuilt(dest::mcp));
    CHECK(dialog.rail()->name(qs(dest::mcp)) == QStringLiteral("API & MCP"));
    auto* view = dynamic_cast<fv::McpSettingsView*>(dialog.pageContent(dest::mcp));
    REQUIRE(view != nullptr);
    view->live()->mcpEventsSwitch()->click();
    CHECK(rig.settings().value("mcp_events_enabled") == Json(true));
    CHECK(rig.changes == QStringList{QStringLiteral("mcp_events")});
    view->live()->apiPermission("api_run_actions")->click();
    CHECK(rig.settings().value("api_run_actions") == Json(true));
    CHECK(rig.changes == QStringList{QStringLiteral("mcp_events")});
}

// ---- the live cards say plainly what each switch exposes ----

TEST_CASE("test_mcp_events_says_the_agent_picks_the_address_and_gets_what_you_dictate", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const QString line = section->mcpEventsSwitch()->accessibleDescription();
    CHECK((has(line, "chooses the web address") && has(line, "what you dictate") &&
           has(line, "subscribes to")));
    // One or two lines on the card, not a paragraph.
    CHECK(line.size() <= 140);
}

TEST_CASE("test_follow_live_events_names_what_it_shares_while_on_by_default", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    fv::SettingsSwitch* check = section->apiPermission("api_live_events");
    CHECK(check->isChecked());
    const QString line = check->accessibleDescription();
    for (const char* shared : {"finished dictations", "actions", "meeting lines"}) {
        CHECK(has(line, shared));
    }
}

TEST_CASE("test_the_channel_card_says_words_a_program_sends_are_marked_and_need_permission", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const QString note = section->channelPrograms()->text();
    CHECK(has(note, "Send words to my coding agents"));
    CHECK((has(note, "program") && has(note, "Claude")));
    CHECK(section->channelCard()->isAncestorOf(section->channelPrograms()));
}

// Review round 2: filters change the words sent to actions too, not only what is pasted, and words
// for coding agents also need "Send words to my coding agents".
TEST_CASE("test_change_dictated_text_names_actions_and_the_agents_switch", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    const QString description =
        section->apiPermission(std::string(permissions::TEXT_FILTERS))->accessibleDescription();
    CHECK(has(description, "pasted or sent to an action"));
    CHECK(has(description, "coding agents also need the switch below"));
}

// ---- the fifth permission: sending a program's words to the user's coding agents ----

TEST_CASE("test_every_permission_has_a_row_in_order", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    std::vector<std::string> order;
    for (const auto& [key, check] : section->apiPermissionChecks()) {
        order.push_back(key);
    }
    std::vector<std::string> all;
    for (const std::string_view key : ALL) {
        all.emplace_back(key);
    }
    CHECK(order == all);
    CHECK(std::string(permissions::AGENT_PROMPTS) == kAgent);
    CHECK(std::string(fv::keys::api_agent_prompts.key()) == kAgent);
}

TEST_CASE("test_sending_words_to_agents_is_off_by_default_and_says_what_it_needs", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    fv::SettingsSwitch* check = section->apiPermission(kAgent);
    CHECK_FALSE(fv::keys::api_agent_prompts.read(Json::object()));
    CHECK_FALSE(check->isChecked());
    CHECK(text(check->accessibleName()) == "Send words to my coding agents");
    const QString description = check->accessibleDescription();
    CHECK((has(description, "Agent hotkey") && has(description, "Claude Code session")));
    CHECK(has(description, "Needs Run actions too."));
    std::vector<std::string> texts;
    for (const fv::WrappedLabel* label : check->parentWidget()->findChildren<fv::WrappedLabel*>()) {
        texts.push_back(text(label->text()));
    }
    CHECK(std::find(texts.begin(), texts.end(), "Send words to my coding agents") != texts.end());
}

TEST_CASE("test_the_switch_and_the_row_save_it", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    fv::SettingsSwitch* check = section->apiPermission(kAgent);
    check->click();
    CHECK(rig.saved(kAgent) == Json(true));
    pressRow(check->parentWidget());
    CHECK(rig.saved(kAgent) == Json(false));
    CHECK_FALSE(section->notice()->isVisible());
}

TEST_CASE("test_a_failed_save_rolls_back_and_says_so", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    rig.store.file().fail_next_replace("read only");
    fv::SettingsSwitch* check = section->apiPermission(kAgent);
    check->click();
    CHECK_FALSE(check->isChecked());
    CHECK(text(section->notice()->text()) == "Couldn't save that permission. Try again.");
}

TEST_CASE("test_saved_choice_is_reread", kApi) {
    McpRig rig;
    auto section = rig.makeSection();
    rig.store.save_setting(kAgent, true);
    section->refresh();
    CHECK(section->apiPermission(kAgent)->isChecked());
    rig.store.save_setting(kAgent, "yes"); // malformed: its default
    section->refresh();
    CHECK_FALSE(section->apiPermission(kAgent)->isChecked());
}
