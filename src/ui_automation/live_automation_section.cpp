#include "ui_automation/live_automation_section.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <tuple>
#include <typeinfo>

#include <QCoreApplication>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "agentapi/mcp_runtime.h"
#include "agentapi/setup.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/event_bus.h"
#include "automation/permissions.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_mcp"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// One row of the Live API card: the permission's settings key, and what it says.
struct PermissionSpec {
    std::string_view key;
    const char* title;
    const char* description;
    const char* icon;
};

const std::vector<PermissionSpec>& permissionSpecs() {
    namespace permissions = automation::permissions;
    static const std::vector<PermissionSpec> specs{
        {permissions::LIVE_EVENTS, "Follow live events",
         "A program can read your finished dictations, the actions you run and meeting lines as "
         "they happen, like reading history.",
         "file-music-blue.svg"},
        {permissions::RUN_ACTIONS, "Run actions",
         "Start your saved actions with text a program sends, and show its notices.",
         "bolt-green.svg"},
        {permissions::CONTROL_RECORDING, "Start recordings",
         "Turn on the microphone to dictate, and stop or cancel a recording.",
         "microphone-blue.svg"},
        {permissions::TEXT_FILTERS, "Change dictated text",
         "Let a program on this computer rewrite or hold back words before they're pasted or sent "
         "to an action. Words for coding agents also need the switch below.",
         "filter-purple.svg"},
        {permissions::AGENT_PROMPTS, "Send words to my coding agents",
         "Lets a program run your Agent hotkey and Claude Code session actions with its own "
         "words. Needs Run actions too.",
         "send-purple.svg"},
    };
    return specs;
}

} // namespace

namespace live_section {

std::string plural(std::size_t count, std::string_view noun) {
    return std::to_string(count) + " " + std::string(noun) + (count == 1 ? "" : "s");
}

} // namespace live_section

// Shared with the clearing task, which runs on another thread: whether the section still
// exists, published under a lock its destructor takes, so a task never posts to a destroyed one.
struct LiveAutomationSection::Lifeline {
    std::mutex mutex;
    LiveAutomationSection* section = nullptr; // null once its destruction has begun

    // Queues fn(section) to the section's thread while it exists. The lock is held across the
    // post, and the destructor takes it before ~QObject removes the section's posted events, so
    // a post that saw the section alive is either removed with it or runs while it still exists.
    template <class Fn> void deliver(const std::shared_ptr<Lifeline>& self, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (section == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            section,
            [self, fn = std::forward<Fn>(fn)]() mutable {
                if (self->section != nullptr) {
                    fn(*self->section);
                }
            },
            Qt::QueuedConnection);
    }
};

LiveAutomationSection::LiveAutomationSection(SettingsStore& settings,
                                             agentapi::McpServerControl& server,
                                             LiveServices services, QWidget* parent)
    : QWidget(parent), settings_(settings), server_(server), services_(std::move(services)),
      lifeline_(std::make_shared<Lifeline>()) {
    lifeline_->section = this;
    setObjectName(QStringLiteral("mcpLiveSection"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(14);
    auto* heading = new QLabel(QStringLiteral("Live events and automation"));
    heading->setObjectName(QStringLiteral("settingsTileGroupTitle"));
    layout->addWidget(heading);
    columns_ = new QGridLayout;
    columns_->setContentsMargins(0, 0, 0, 0);
    columns_->setSpacing(14);
    layout->addLayout(columns_);
    buildLiveApi();
    buildMcpEvents();
    buildChannel();
    notice_ = mcpLabel(QString(), "mcpNotice");
    notice_->setTextFormat(Qt::PlainText);
    notice_->hide();
    layout->addWidget(notice_);
    arrangeColumns();
    refresh();
}

LiveAutomationSection::~LiveAutomationSection() {
    std::lock_guard lock(lifeline_->mutex);
    lifeline_->section = nullptr;
}

SettingsSwitch* LiveAutomationSection::apiPermission(std::string_view key) const {
    for (const auto& [name, check] : apiChecks_) {
        if (name == key) {
            return check;
        }
    }
    return nullptr;
}

// ---- building ----

McpCard LiveAutomationSection::card(const QString& title, const QString& description,
                                    const QString& icon, const QString& chip) {
    McpCard made = makeMcpCard(title, description, icon);
    if (!chip.isEmpty()) {
        auto* badge = new QLabel(chip);
        badge->setObjectName(QStringLiteral("mcpPreviewChip"));
        made.header->addWidget(badge, 0, Qt::AlignTop);
    }
    return made;
}

std::pair<StatusDot*, WrappedLabel*>
LiveAutomationSection::statusLine(QLayout* layout, const QString& accessibleName) {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("mcpLiveRow"));
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(8);
    auto* dot = new StatusDot(nullptr, 8);
    line->addWidget(dot, 0, Qt::AlignVCenter);
    WrappedLabel* label = mcpLabel(QString(), "mcpLiveStatus");
    label->setAccessibleName(accessibleName);
    line->addWidget(label, 1);
    layout->addWidget(row);
    return {dot, label};
}

McpActionButton* LiveAutomationSection::copyButton(const QString& text) {
    auto* button = new McpActionButton(text);
    button->setObjectName(QStringLiteral("mcpCopyButton"));
    button->setBaseMinimumSize(0, 38);
    button->setAccessibleName(text);
    return button;
}

void LiveAutomationSection::buildLiveApi() {
    McpCard made = card(
        QStringLiteral("Live API"),
        QStringLiteral("Your own programs can follow along as you dictate and, if you allow it, "
                       "act for you."),
        QStringLiteral("broadcast-green.svg"));
    liveApiCard_ = made.frame;
    QVBoxLayout* layout = made.layout;
    std::tie(listenersDot_, listenersLabel_) =
        statusLine(layout, QStringLiteral("Programs listening to live events"));
    for (const PermissionSpec& spec : permissionSpecs()) {
        auto* row = new McpPermissionRow(q(spec.title), q(spec.description), q(spec.icon));
        const std::string key(spec.key);
        connect(row->checkbox(), &QAbstractButton::toggled, this,
                [this, key](bool checked) { save(key, checked); });
        apiChecks_.emplace_back(key, row->checkbox());
        layout->addWidget(row);
    }
    layout->addWidget(mcpDetail(QStringLiteral(
        "These also apply to assistants connected over MCP. Each is checked on every request.")));
    auto* address = new QVBoxLayout;
    address->setSpacing(4);
    address->addWidget(mcpLabel(QStringLiteral("Live API address"), "mcpFieldLabel"));
    apiUrl_ = new QLineEdit;
    apiUrl_->setReadOnly(true);
    apiUrl_->setAccessibleName(QStringLiteral("Live API address"));
    copyApiUrl_ = copyButton(QStringLiteral("Copy address"));
    connect(copyApiUrl_, &QPushButton::clicked, this,
            [this] { copy(apiUrl_->text(), QStringLiteral("Address copied.")); });
    address->addWidget(new McpCopyField(apiUrl_, copyApiUrl_));
    layout->addLayout(address);

    endpoints_ = new McpDisclosure(QStringLiteral("Endpoints"));
    endpoints_->bodyLayout()->addWidget(mcpDetail(QStringLiteral(
        "Send the MCP access token as a Bearer header. Click an address to copy it.")));
    auto* gridHost = new QWidget;
    gridHost->setObjectName(QStringLiteral("mcpEndpoints"));
    auto* grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(4);
    int rowIndex = 0;
    for (const live_section::Endpoint& endpoint : live_section::LIVE_ENDPOINTS) {
        auto* button =
            new QPushButton(q(endpoint.method) + QStringLiteral("  ") + q(endpoint.path));
        button->setObjectName(QStringLiteral("mcpEndpoint"));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringLiteral("Copy this address"));
        button->setAccessibleName(QStringLiteral("Copy the %1 address").arg(q(endpoint.path)));
        const std::string_view path = endpoint.path;
        connect(button, &QPushButton::clicked, this, [this, path] { copyEndpoint(path); });
        grid->addWidget(button, rowIndex, 0, Qt::AlignLeft);
        grid->addWidget(mcpDetail(q(endpoint.what)), rowIndex, 1);
        endpointButtons_.push_back(button);
        ++rowIndex;
    }
    grid->setColumnStretch(1, 1);
    endpoints_->bodyLayout()->addWidget(gridHost);
    copyExample_ = copyButton(QStringLiteral("Copy a curl example"));
    copyExample_->setToolTip(QStringLiteral(
        "Includes the access token. Paste it into a terminal to watch events live."));
    connect(copyExample_, &QPushButton::clicked, this, [this] { copyExampleCommand(); });
    endpoints_->bodyLayout()->addWidget(copyExample_, 0, Qt::AlignLeft);
    layout->addWidget(endpoints_);
    layout->addStretch();
}

void LiveAutomationSection::buildMcpEvents() {
    McpCard made = card(
        QStringLiteral("MCP Events"),
        QStringLiteral("Agents subscribe to events over MCP, and %1 sends them signed webhooks.")
            .arg(q(identity().product)),
        QStringLiteral("bell-amber.svg"), QStringLiteral("Preview"));
    mcpEventsCard_ = made.frame;
    QVBoxLayout* layout = made.layout;
    auto* row = new McpPermissionRow(
        QStringLiteral("Let agents subscribe"),
        QStringLiteral("An assistant over MCP chooses the web address, and it receives what you "
                       "dictate (the events it subscribes to)."),
        QStringLiteral("key-blue.svg"));
    mcpEventsSwitch_ = row->checkbox();
    connect(mcpEventsSwitch_, &QAbstractButton::toggled, this,
            [this](bool checked) { saveMcpEvents(checked); });
    layout->addWidget(row);
    auto* line = new QWidget;
    line->setObjectName(QStringLiteral("mcpLiveRow"));
    auto* countRow = new QHBoxLayout(line);
    countRow->setContentsMargins(0, 0, 0, 0);
    countRow->setSpacing(10);
    subscriptionsLabel_ = mcpLabel(QString(), "mcpLiveStatus");
    subscriptionsLabel_->setAccessibleName(QStringLiteral("MCP event subscriptions"));
    countRow->addWidget(subscriptionsLabel_, 1);
    clearSubscriptions_ = new McpActionButton(QStringLiteral("Clear"));
    clearSubscriptions_->setObjectName(QStringLiteral("mcpCopyButton"));
    clearSubscriptions_->setBaseMinimumSize(0, 34);
    clearSubscriptions_->setAccessibleName(QStringLiteral("Clear MCP event subscriptions"));
    clearSubscriptions_->setToolTip(
        QStringLiteral("Forget every subscription and its signing secret"));
    connect(clearSubscriptions_, &QPushButton::clicked, this, [this] { clearAllSubscriptions(); });
    countRow->addWidget(clearSubscriptions_, 0, Qt::AlignVCenter);
    layout->addWidget(line);
    layout->addStretch();
}

void LiveAutomationSection::buildChannel() {
    McpCard made =
        card(QStringLiteral("Claude Code channel"),
             QStringLiteral("Send words into a running Claude Code session with a “Tell my Claude "
                            "Code session” action."),
             QStringLiteral("send-purple.svg"), QStringLiteral("Research preview"));
    channelCard_ = made.frame;
    QVBoxLayout* layout = made.layout;
    std::tie(sessionsDot_, sessionsLabel_) =
        statusLine(layout, QStringLiteral("Claude Code sessions connected to the channel"));
    layout->addWidget(
        mcpLabel(QStringLiteral("1. Add %1 to a project's .mcp.json").arg(q(identity().product)),
                 "mcpSectionLabel"));
    // A session that loads the entry without the channel flag still connects (and counts) but
    // Claude Code drops what it is sent.
    channelScope_ = mcpDetail(QStringLiteral(
        "Only in projects you start with the command below, not in your user-wide settings. A "
        "session started without it connects but can't receive messages."));
    layout->addWidget(channelScope_);

    std::optional<Paths> owned;
    if (services_.paths == nullptr) {
        owned = Paths::resolve(identity(), system_env());
    }
    const Paths& paths = services_.paths != nullptr ? *services_.paths : *owned;
    const std::filesystem::path executable =
        services_.executable.empty()
            ? base_utf8::path_from_utf8(qt::toUtf8(QCoreApplication::applicationFilePath()))
            : services_.executable;
    channelConfig_ = new QPlainTextEdit;
    channelConfig_->setObjectName(QStringLiteral("mcpChannelConfig"));
    channelConfig_->setReadOnly(true);
    channelConfig_->setAccessibleName(QStringLiteral("Claude Code channel .mcp.json entry"));
    channelConfig_->setPlainText(q(agentapi::claude_channel_config(executable, paths)));
    channelConfig_->setMinimumHeight(160);
    channelConfig_->setMaximumHeight(230);
    copyChannelConfig_ = copyButton(QStringLiteral("Copy .mcp.json entry"));
    connect(copyChannelConfig_, &QPushButton::clicked, this, [this] {
        copy(channelConfig_->toPlainText(),
             QStringLiteral("Entry copied. Merge it into that project's .mcp.json."));
    });
    channelPreview_ = new McpDisclosure(QStringLiteral("Preview entry"), true);
    channelPreview_->bodyLayout()->addWidget(channelConfig_);
    layout->addWidget(new McpAdaptiveRow(
        {{copyChannelConfig_, 0}, {nullptr, 0}, {channelPreview_->toggle(), 0}}, 10));
    layout->addWidget(channelPreview_);
    layout->addWidget(
        mcpLabel(QStringLiteral("2. Start Claude Code with the channel"), "mcpSectionLabel"));
    channelCommand_ =
        new WrappedLabel(q(agentapi::claude_channel_command()), nullptr, /*breakLongWords=*/true);
    channelCommand_->setObjectName(QStringLiteral("mcpCommandBox"));
    channelCommand_->setTextFormat(Qt::PlainText);
    channelCommand_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    channelCommand_->setAccessibleName(QStringLiteral("Claude Code channel command"));
    copyChannelCommand_ = copyButton(QStringLiteral("Copy command"));
    connect(copyChannelCommand_, &QPushButton::clicked, this, [this] {
        copy(channelCommand_->text(),
             QStringLiteral("Command copied. Run it in that project's folder."));
    });
    // Full width, so the command wraps between words rather than inside them.
    layout->addWidget(channelCommand_);
    layout->addWidget(copyChannelCommand_, 0, Qt::AlignLeft);
    channelPrograms_ = mcpDetail(QStringLiteral(
        "A program can send words here too once you allow Send words to my coding agents. Claude "
        "sees they came from a program, not from you."));
    layout->addWidget(channelPrograms_);
    layout->addStretch();
}

// ---- layout ----

void LiveAutomationSection::arrangeColumns() {
    const bool wide = width() >= ui::scaled(740);
    if (wide_ == wide) {
        return;
    }
    wide_ = wide;
    for (QFrame* card : {liveApiCard_, mcpEventsCard_, channelCard_}) {
        columns_->removeWidget(card);
    }
    constexpr Qt::Alignment top = Qt::AlignTop;
    if (wide) {
        columns_->addWidget(liveApiCard_, 0, 0, 2, 1, top);
        columns_->addWidget(mcpEventsCard_, 0, 1, top);
        columns_->addWidget(channelCard_, 1, 1, top);
        columns_->setRowStretch(1, 1);
    } else {
        int row = 0;
        for (QFrame* card : {liveApiCard_, mcpEventsCard_, channelCard_}) {
            columns_->addWidget(card, row++, 0, top);
        }
        columns_->setRowStretch(1, 0);
    }
    columns_->setColumnStretch(0, wide ? 6 : 1);
    columns_->setColumnStretch(1, wide ? 5 : 0);
}

void LiveAutomationSection::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrangeColumns();
}

void LiveAutomationSection::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        channelCard_ != nullptr) {
        arrangeColumns();
    }
}

void LiveAutomationSection::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

// ---- state ----

QString LiveAutomationSection::baseUrl() const {
    const agentapi::ServerStatus status = server_.status();
    if (status.state != "running") {
        return QString();
    }
    const std::string url = status.url();
    const std::size_t cut = url.rfind("/mcp");
    return q(cut == std::string::npos ? url : url.substr(0, cut));
}

void LiveAutomationSection::refresh() {
    const std::shared_ptr<const Json> snapshot = settings_.snapshot();
    const Json& preferences = *snapshot;
    for (const auto& [key, check] : apiChecks_) {
        const QSignalBlocker blocker(check);
        check->setChecked(automation::permissions::granted(preferences, key));
    }
    {
        const QSignalBlocker blocker(mcpEventsSwitch_);
        mcpEventsSwitch_->setChecked(keys::mcp_events_enabled.read(preferences));
    }
    refreshLive();
}

void LiveAutomationSection::refreshLive() {
    const QString url = baseUrl();
    const bool running = !url.isEmpty();
    const QString shown = running ? url : QStringLiteral("Turn on MCP above to start the live API");
    if (apiUrl_->text() != shown) {
        apiUrl_->setText(shown);
        apiUrl_->setCursorPosition(0);
    }
    for (QWidget* widget : {static_cast<QWidget*>(apiUrl_), static_cast<QWidget*>(copyApiUrl_),
                            static_cast<QWidget*>(copyExample_)}) {
        widget->setEnabled(running);
    }
    for (QPushButton* button : endpointButtons_) {
        button->setEnabled(running);
    }
    automation::EventBus& bus = automation::bus();
    const std::size_t listeners = bus.subscribers(live_section::SSE_LISTENER_LABEL);
    const std::size_t sessions = bus.subscribers(live_section::CHANNEL_LISTENER_LABEL);
    showCount(listenersDot_, listenersLabel_, running, listeners,
              listeners != 0 ? q(live_section::plural(listeners, "program") + " listening")
                             : QStringLiteral("No programs listening yet"),
              QStringLiteral("Off while MCP is off"));
    // "Connected", not "listening": a session started without the channel flag connects too, and
    // can't receive anything.
    showCount(sessionsDot_, sessionsLabel_, running, sessions,
              sessions != 0 ? q(live_section::plural(sessions, "session") + " connected")
                            : QStringLiteral("No sessions connected"),
              QStringLiteral("Needs MCP on"));
    std::size_t count = 0;
    if (services_.subscriptionCount) {
        try {
            count = services_.subscriptionCount();
        } catch (const std::exception&) {
            // Polled every 500 ms; keep it out of the default log.
            kLog.debug("Could not count MCP Events subscriptions");
            count = 0;
        }
    }
    const bool enabled = mcpEventsSwitch_->isChecked();
    const QString text = count != 0 ? q(live_section::plural(count, "subscription"))
                         : enabled  ? QStringLiteral("No subscriptions yet")
                                    : QStringLiteral("Off. Agents can't subscribe.");
    if (subscriptionsLabel_->text() != text) {
        subscriptionsLabel_->setText(text);
    }
    // A Clear with nothing to clear is only noise.
    clearSubscriptions_->setVisible(count > 0);
    clearSubscriptions_->setEnabled(waiting_.empty());
}

void LiveAutomationSection::showCount(StatusDot* dot, WrappedLabel* label, bool running,
                                      std::size_t count, const QString& text,
                                      const QString& offText) {
    const QString shown = running ? text : offText;
    if (label->text() != shown) {
        label->setText(shown);
    }
    const EngineStatus status = running && count != 0 ? EngineStatus::Ready : EngineStatus::Unknown;
    if (dot->status() != status) {
        dot->setStatus(status);
    }
}

// ---- writes ----

void LiveAutomationSection::write(std::string_view key, bool value) {
    settings_.mutate([&](Json& saved) { saved[std::string(key)] = value; });
}

void LiveAutomationSection::save(const std::string& key, bool checked) {
    try {
        write(key, checked);
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the API permission ({})", type_display_name(typeid(failure)));
        showNotice(QStringLiteral("Couldn't save that permission. Try again."));
        refresh();
        return;
    }
    showNotice(QString());
}

void LiveAutomationSection::saveMcpEvents(bool checked) {
    try {
        write(keys::mcp_events_enabled.key(), checked);
    } catch (const std::exception& failure) {
        kLog.warning("Could not save MCP Events ({})", type_display_name(typeid(failure)));
        showNotice(QStringLiteral("Couldn't save MCP Events. Try again."));
        refresh();
        return;
    }
    showNotice(QString());
    Q_EMIT settingsChanged(QStringLiteral("mcp_events"));
    refreshLive();
}

void LiveAutomationSection::clearAllSubscriptions() {
    const int token = ++tokens_;
    waiting_[token] = "clear";
    clearSubscriptions_->setEnabled(false);
    // Forgetting secrets touches the credential store, which can be slow.
    const auto work = services_.clearSubscriptions;
    const std::shared_ptr<Lifeline> lifeline = lifeline_;
    const auto task = [work, lifeline, token] {
        std::size_t cleared = 0;
        std::string error;
        try {
            cleared = work ? work() : 0;
        } catch (const std::exception& failure) {
            kLog.warning("Could not clear MCP Events subscriptions ({})",
                         type_display_name(typeid(failure)));
            error = failure.what();
            if (error.empty()) {
                error = type_display_name(typeid(failure));
            }
        }
        lifeline->deliver(lifeline, [token, cleared, error](LiveAutomationSection& section) {
            section.onAnswer(token, cleared, error);
        });
    };
    if (services_.io != nullptr) {
        services_.io->post(task);
    } else {
        task();
    }
}

void LiveAutomationSection::onAnswer(int token, std::size_t cleared, const std::string& error) {
    const auto found = waiting_.find(token);
    if (found == waiting_.end()) {
        return;
    }
    const bool clearing = found->second == "clear";
    waiting_.erase(found);
    if (!clearing) {
        return;
    }
    if (!error.empty()) {
        showNotice(QStringLiteral("Couldn't clear subscriptions: %1").arg(q(error)));
    } else {
        showNotice(cleared != 0
                       ? q("Cleared " + live_section::plural(cleared, "subscription") + ".")
                       : QStringLiteral("Nothing to clear."));
    }
    refreshLive();
}

// ---- copying ----

void LiveAutomationSection::showNotice(const QString& text) {
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
}

void LiveAutomationSection::copy(const QString& text, const QString& done) {
    if (text.isEmpty()) {
        return;
    }
    action_prompts::copyText()(text);
    showNotice(done);
}

void LiveAutomationSection::copyEndpoint(std::string_view path) {
    const QString base = baseUrl();
    if (!base.isEmpty()) {
        copy(base + q(path), QStringLiteral("Copied %1").arg(base + q(path)));
    }
}

void LiveAutomationSection::copyExampleCommand() {
    const QString base = baseUrl();
    const std::string token = server_.token();
    if (base.isEmpty() || token.empty()) {
        return;
    }
    // Built at click time, so the token is never on screen.
    copy(QStringLiteral("curl -N -H \"Authorization: Bearer %1\" %2/v1/events").arg(q(token), base),
         QStringLiteral("Copied. Paste it into a terminal to watch events live."));
}

} // namespace fv
