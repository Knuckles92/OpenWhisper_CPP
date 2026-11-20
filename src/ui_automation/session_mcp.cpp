#include "ui_automation/session_mcp.h"

#include <QApplication>
#include <QMessageBox>
#include <QStringList>

#include "agentapi/agent_controls.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "meeting/runtime.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_prompts.h"
#include "ui_history/history_feature.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"

namespace fv {
namespace {
QString grants(const Json& settings) {
    QStringList result;
    if (keys::mcp_retitle_transcriptions.read(settings))
        result << QStringLiteral("rename dictations");
    if (keys::mcp_retitle_meetings.read(settings))
        result << QStringLiteral("rename finished meetings");
    if (keys::mcp_settings_access.read(settings)) {
        result << QStringLiteral("read app preferences");
        for (const auto& name : agentapi::writable_settings(settings))
            result << QStringLiteral("change %1").arg(qt::toQString(name));
    }
    return result.isEmpty() ? QString()
                            : QStringLiteral("\nAdditional access: %1.").arg(result.join(", "));
}

struct DesktopChanges {
    SettingsStore& settings;
    std::weak_ptr<HistoryPanel> history;
    std::weak_ptr<SettingsPagesSignals> pages;
    std::weak_ptr<SettingsWindow> window;
    std::weak_ptr<CleanupSettings> cleanup;

    void apply(std::string_view kind, const Json& changed) {
        if (kind == "meeting" || kind == "transcription") {
            if (const auto panel = history.lock()) {
                if (kind == "meeting") {
                    panel->refreshPastMeetings();
                } else {
                    panel->refresh();
                }
            }
            return;
        }
        if (kind != "settings" || !changed.is_object()) {
            return;
        }
        const auto latest = settings.snapshot();
        if (const auto signals = pages.lock()) {
            if (changed.contains(keys::ui_theme.key())) {
                const auto* theme = agentapi::setting_control(keys::ui_theme.key());
                Q_EMIT signals->uiThemeChanged(
                    qt::toQString(theme->resolve(*latest).get<std::string>()));
            }
            if (changed.contains(keys::ui_font_scale.key())) {
                Q_EMIT signals->uiFontScaleChanged(
                    static_cast<int>(keys::ui_font_scale.read(*latest)));
            }
            if (changed.contains(keys::streaming_enabled.key())) {
                Q_EMIT signals->streamingSettingsChanged();
            }
            if (changed.contains(keys::streaming_overlay_font_size.key())) {
                Q_EMIT signals->streamingFontChanged(
                    static_cast<int>(keys::streaming_overlay_font_size.read(*latest)));
            }
            if (changed.contains(keys::streaming_overlay_layout.key())) {
                Q_EMIT signals->streamingLayoutChanged(
                    qt::toQString(keys::streaming_overlay_layout.read(*latest)));
            }
            if (changed.contains(keys::recording_trigger_mode.key())) {
                Q_EMIT signals->recordingTriggerModeChanged(
                    qt::toQString(keys::recording_trigger_mode.read(*latest)));
            }
            if (changed.contains(keys::recording_overlay_look.key()) ||
                changed.contains(keys::recording_overlay_dot.key()) ||
                changed.contains(keys::recording_overlay_text.key()) ||
                changed.contains(keys::recording_overlay_clock.key())) {
                Q_EMIT signals->recordingLookChanged(
                    qt::toQString(keys::recording_overlay_look.read(*latest)));
            }
        }
        if (changed.contains(keys::transcript_cleanup_enabled.key()) ||
            changed.contains(keys::transcript_cleanup_level.key())) {
            if (const auto controls = cleanup.lock()) {
                controls->updateCleanupUi();
                Q_EMIT controls->cleanupChanged();
            }
        }
        if (const auto settings_window = window.lock()) {
            if (auto* dialog = settings_window->dialog()) {
                dialog->refresh();
            }
        }
    }
};

class SessionControl final : public agentapi::McpServerControl {
public:
    SessionControl(std::shared_ptr<agentapi::McpServerControl> listener, SettingsStore& settings,
                   std::function<bool(const QString&)> confirm)
        : listener_(std::move(listener)), settings_(settings), confirm_(std::move(confirm)) {}
    agentapi::ServerStatus status() override { return listener_->status(); }
    std::string token() override { return listener_->token(); }
    void stop(bool wait) override { listener_->stop(wait); }
    bool session_only() const override { return true; }
    void start(int port, bool tailscale, std::function<bool()> access) override {
        // The saved Tailscale preference has no authority to bind another network interface.
        // A separate remote-access workflow is needed for that listener.
        if (tailscale)
            throw std::invalid_argument("Turn off Tailscale access to start a local MCP session.");
        const auto permissions = grants(*settings_.snapshot());
        const auto message =
            QStringLiteral("Apps on this computer with your MCP token will be able to search "
                           "saved dictations, transcripts and meeting insights through "
                           "127.0.0.1:%1.%2\n\nStart this local server for this session?")
                .arg(port)
                .arg(permissions);
        if (!confirm_ || !confirm_(message))
            return;
        if (permissions != grants(*settings_.snapshot()))
            throw std::runtime_error("MCP permissions changed. Review them and start again.");
        auto revoked = std::make_shared<std::atomic<bool>>(false);
        listener_->start(port, false,
                         [store = &settings_, permissions, revoked, access = std::move(access)] {
                             if (permissions != grants(*store->snapshot()) || (access && !access()))
                                 revoked->store(true);
                             return !revoked->load();
                         });
    }

private:
    std::shared_ptr<agentapi::McpServerControl> listener_;
    SettingsStore& settings_;
    std::function<bool(const QString&)> confirm_;
};
} // namespace

std::shared_ptr<agentapi::McpServerControl>
sessionMcpControl(std::shared_ptr<agentapi::McpServerControl> listener, SettingsStore& settings,
                  std::function<bool(const QString&)> confirm) {
    return std::make_shared<SessionControl>(std::move(listener), settings, std::move(confirm));
}

std::function<void(std::string, Json)> sessionMcpDesktopChanges(AppContext& context) {
    auto target = std::make_shared<DesktopChanges>(DesktopChanges{
        context.settings(), context.find_shared<HistoryPanel>(),
        context.find_shared<SettingsPagesSignals>(), context.find_shared<SettingsWindow>(),
        context.find_shared<CleanupSettings>()});
    return [target, ui = &context.ui()](std::string kind, Json changes) {
        ui->post(
            [weak = std::weak_ptr(target), kind = std::move(kind), changes = std::move(changes)] {
                if (const auto receiver = weak.lock()) {
                    receiver->apply(kind, changes);
                }
            });
    };
}

void provideSessionMcp(AppContext& context) {
    if (context.find<agentapi::McpServerControl>())
        return;
    try {
        agentapi::McpRuntimeDeps deps;
        deps.database = context.paths().user_file(identity().database_file);
        deps.allow_live_tools = false;
        deps.reactor = &context.net();
        deps.settings = &context.settings();
        deps.credentials = &context.credentials();
        deps.io = &context.io();
        deps.clock = &context.clock();
        auto listener = std::make_shared<agentapi::McpRuntime>(deps);
        std::function<void(std::string, std::string)> rename_meeting;
        if (const auto runtime = context.find_shared<meeting::MeetingRuntime>()) {
            rename_meeting = [weak = std::weak_ptr(runtime)](std::string id, std::string title) {
                const auto service = weak.lock();
                if (!service) {
                    throw std::runtime_error("The meeting service has already closed.");
                }
                service->retitle_saved_meeting(id, title);
            };
        }
        listener->configure_controls(sessionMcpDesktopChanges(context), std::move(rename_meeting));
        context.provide<agentapi::McpServerControl>(
            sessionMcpControl(std::move(listener), context.settings(), [](const QString& message) {
                return action_prompts::ask(QApplication::activeWindow(),
                                           QStringLiteral("Start local MCP server?"),
                                           message.toHtmlEscaped().replace("\n", "<br>"),
                                           QMessageBox::Yes | QMessageBox::Cancel,
                                           QMessageBox::Cancel) == QMessageBox::Yes;
            }));
    } catch (const ServiceUnavailable&) {
        // Minimal application graphs can still show setup instructions.
    }
}
} // namespace fv
