#include "ui_automation/automation_feature.h"

#include <memory>

#include <QClipboard>
#include <QGuiApplication>
#include <QObject>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/action_runtime.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "base/log.h"
#include "ui_automation/action_bindings.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/action_relay.h"
#include "ui_automation/actions_page.h"
#include "ui_automation/hooks_page.h"
#include "ui_automation/mcp_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_actions"};

// Test buttons work without subscribing saved hooks to app events. The bus and controls have
// context lifetime; neither engine's start() is called here.
struct ManualHookBus {
    automation::EventBus bus;
};

class ManualActionHost final : public automation::ActionHost {
public:
    void set_status(std::string) override {}
    void show_notice(std::string, bool) override {}
    bool show_in_scratchpad(std::string) override { return false; }
    bool copy_to_clipboard(std::string text, std::string) override {
        auto* clipboard = QGuiApplication::clipboard();
        if (!clipboard)
            return false;
        clipboard->setText(qt::toQString(text));
        return true;
    }
    bool paste_text_now(std::string, const dictation::DictationJob*) override { return false; }
    dictation::PasteTarget paste_target(const dictation::DictationJob&) override {
        return dictation::PasteTarget::Unknown;
    }
    void discard_clipboard_prefetch() override {}
};

class ManualActionTester final : public automation::ActionTester {
public:
    ManualActionTester(SettingsStore& settings, CredentialStore& credentials, EventSink& events,
                       Executor& ui)
        : runtime_(host_, settings, credentials, events, ui) {}
    void test(const automation::Action& action, std::string sample,
              std::function<void(automation::ActionResult)> done) override {
        if (action.kind != automation::ActionKind::CLIPBOARD) {
            QString destination = qt::toQString(action.name);
            const auto add = [&](std::string_view key) {
                const auto value = action.param(key);
                if (value.is_string() && !value.get<std::string>().empty())
                    destination += QStringLiteral("\n") + qt::toQString(value.get<std::string>());
            };
            if (action.kind == automation::ActionKind::WEBHOOK)
                add("url");
            else if (action.kind == automation::ActionKind::OPEN_LINK)
                add("template");
            else if (action.kind == automation::ActionKind::PROGRAM)
                add("program");
            else if (action.kind == automation::ActionKind::AGENT) {
                const auto value = action.param("agent");
                if (value.is_string()) {
                    const auto agent = value.get<std::string>();
                    destination +=
                        QStringLiteral("\n") +
                        qt::toQString(automation::agent_display_name(agent).value_or(agent));
                }
            } else if (action.kind == automation::ActionKind::CHANNEL)
                destination += QStringLiteral("\nConnected Claude Code sessions");
            else if (action.kind == automation::ActionKind::SCRIPTS)
                destination += QStringLiteral("\nConnected scripts");
            const QString message =
                QStringLiteral("<p>Run this action and send the following text?</p>"
                               "<p><b>Destination:</b><br>%1</p>"
                               "<p><b>Text:</b></p><pre>%2</pre>")
                    .arg(destination.toHtmlEscaped(), qt::toQString(sample).toHtmlEscaped());
            if (action_prompts::ask(nullptr, QStringLiteral("Confirm action"), message,
                                    QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel) != QMessageBox::Yes) {
                if (done)
                    done({false, "Canceled", "", true});
                return;
            }
        }
        runtime_.test(action, std::move(sample), std::move(done));
    }
    int cancel_tests() override { return runtime_.cancel_tests(); }

private:
    ManualActionHost host_;
    automation::ActionRuntime runtime_;
};

void provideManualControls(AppContext& context) {
    try {
        auto* holder = context.find<ManualHookBus>();
        if (!holder && (!context.find<automation::ProgramControl>() ||
                        !context.find<automation::WebhookControl>())) {
            auto made = std::make_shared<ManualHookBus>();
            holder = made.get();
            context.provide<ManualHookBus>(std::move(made));
        }
        if (!context.find<automation::ProgramControl>()) {
            const auto* options = context.find<automation::ProgramOptions>();
            context.provide<automation::ProgramControl>(std::make_shared<automation::ProgramEngine>(
                context.settings(), holder->bus,
                options ? *options : automation::ProgramOptions{}));
        }
        if (!context.find<automation::WebhookControl>()) {
            auto http = std::shared_ptr<http::Client>(&context.http(), [](http::Client*) {});
            context.provide<automation::WebhookControl>(std::make_shared<automation::WebhookEngine>(
                context.settings(), context.credentials(), std::move(http), holder->bus));
        }
    } catch (const ServiceUnavailable&) {
        kLog.debug("Manual hook tests are unavailable in this context");
    }
    if (context.find<automation::ActionTester>())
        return;
    try {
        // Resolve all required references before publishing either service.
        auto& settings = context.settings();
        auto& credentials = context.credentials();
        EventSink* events = nullptr;
        try {
            events = &context.events();
        } catch (const ServiceUnavailable&) {
            events = &automation::bus();
        }
        auto& ui = context.ui();
        context.provide<automation::ActionTester>(
            std::make_shared<ManualActionTester>(settings, credentials, *events, ui));
    } catch (const ServiceUnavailable&) {
        kLog.debug("Manual action tests are unavailable in this context");
    }
}

} // namespace

void AutomationFeature::provide(AppContext& context) {
    provideManualControls(context);
    // The session's one relay: the Actions page's card and editor share it (as OpenWhisper's
    // Walkthrough tab and Settings write-up share action_relay.relay()).
    if (context.find<ActionRelay>() != nullptr) {
        return;
    }
    try {
        Executor& io = context.io();
        CredentialStore& store = context.credentials();
        context.provide<ActionRelay>(std::make_shared<ActionRelay>(io, sessionRelayWork(store)));
    } catch (const ServiceUnavailable&) {
        // A context without blocking work or a credential store (a test's) has no relay; the
        // Actions page then makes its own when it is built.
        kLog.debug("No action relay: the context has no io executor or credential store");
    }
}

void AutomationFeature::install(AppContext& context) {
    registerActionsPage(context);
    registerHooksPage(context);
    registerMcpPage(context);
    // The Hotkeys page lists the actions' shortcuts beside the profiles' and transforms'.
    if (auto* window = context.find<SettingsWindow>()) {
        if (SettingsDialog* dialog = window->dialog()) {
            new ActionBindingsWiring(*dialog);
        }
        QObject::connect(window, &SettingsWindow::dialogCreated, window,
                         [](SettingsDialog* dialog) {
                             if (dialog != nullptr) {
                                 new ActionBindingsWiring(*dialog);
                             }
                         });
    }
}

} // namespace fv
