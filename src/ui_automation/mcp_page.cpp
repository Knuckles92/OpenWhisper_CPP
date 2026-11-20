#include "ui_automation/mcp_page.h"

#include <array>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <typeinfo>

#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "agentapi/agent_controls.h"
#include "agentapi/mcp_events.h"
#include "agentapi/setup.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "remote/control.h"
#include "remote/errors.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_automation/session_mcp.h"
#include "ui_automation/settings_page_host.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_mcp"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// One optional permission of the Connection's neighbour card.
struct PermissionSpec {
    std::string_view key;
    const char* title;
    const char* description;
    const char* icon;
};

const std::array<PermissionSpec, 3>& permissionSpecs() {
    static const std::array<PermissionSpec, 3> specs{{
        {keys::mcp_retitle_transcriptions.key(), "Rename dictations",
         "Keep the original text and source filename.", "typography-blue.svg"},
        {keys::mcp_retitle_meetings.key(), "Rename finished meetings",
         "Change titles of local saved meetings.", "box-blue.svg"},
        {keys::mcp_settings_access.key(), "Read app preferences",
         "Read supported preferences. Allow individual changes below.", "layout-grid-blue.svg"},
    }};
    return specs;
}

// What stands in for the listener while the context has none: off, and every start fails.
// Affinity: thread-safe.
class UnavailableMcpControl final : public agentapi::McpServerControl {
public:
    agentapi::ServerStatus status() override {
        agentapi::ServerStatus status;
        status.state = "error";
        status.message = "MCP is not available in this session.";
        return status;
    }
    std::string token() override { return {}; }
    void start(int, bool, std::function<bool()>) override {}
    void stop(bool) override {}
};

agentapi::McpServerControl& unavailableControl() {
    static UnavailableMcpControl control;
    return control;
}

// preferences.get(key) is True: a stored `true`, not merely a truthy value.
bool isTrue(const Json& settings, std::string_view key) {
    if (!settings.is_object()) {
        return false;
    }
    const auto found = settings.find(std::string(key));
    return found != settings.end() && found->is_boolean() && found->get<bool>();
}

} // namespace

namespace mcp_page {

const std::vector<std::string>& control_names() {
    static const std::vector<std::string> names{"mcp_page", "mcp_view"};
    return names;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#x27;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace mcp_page

// ---- McpSettingsPage ----

McpSettingsPage::McpSettingsPage(SettingsStore& settings, agentapi::McpServerControl& server,
                                 QWidget* parent)
    : QWidget(parent), settings_(settings), server_(server) {
    setObjectName(QStringLiteral("mcpSettingsPage"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    const std::shared_ptr<const Json> saved = settings_.snapshot();
    const QString product = q(identity().product);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    enableTile_ = new QFrame;
    enableTile_->setObjectName(QStringLiteral("mcpServerBar"));
    enableTile_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* statusRow = new QHBoxLayout(enableTile_);
    statusRow->setContentsMargins(18, 14, 18, 14);
    statusRow->setSpacing(12);
    statusDot_ = new StatusDot(nullptr, 10);
    statusRow->addWidget(statusDot_);
    statusLabel_ = mcpLabel(QString(), "mcpStatusText");
    statusLabel_->setTextFormat(Qt::RichText);
    statusLabel_->setAccessibleName(QStringLiteral("MCP server status"));
    statusRow->addWidget(statusLabel_, 1);
    retry_ = new McpActionButton(QStringLiteral("Retry"));
    retry_->setBaseMinimumSize(70, 34);
    connect(retry_, &QPushButton::clicked, this, [this] { start(); });
    statusRow->addWidget(retry_);
    enabled_ = new SettingsSwitch;
    enabled_->setAccessibleName(QStringLiteral("Enable MCP"));
    enabled_->setToolTip(
        (server_.session_only()
             ? QStringLiteral("Review access to start a local session. Quitting %1 stops sharing.")
             : QStringLiteral("Starts with %1 when enabled. Quitting the app stops access."))
            .arg(product));
    connect(enabled_, &QAbstractButton::toggled, this, [this](bool checked) { toggle(checked); });
    statusRow->addWidget(enabled_);
    layout->addWidget(enableTile_);

    columns_ = new QGridLayout;
    columns_->setContentsMargins(0, 0, 0, 0);
    columns_->setSpacing(14);
    layout->addLayout(columns_);
    McpCard connectionCard = makeMcpCard(
        QStringLiteral("Connection"), QStringLiteral("Connect your assistant in two simple steps."),
        QStringLiteral("server-blue.svg"));
    connectionCard_ = connectionCard.frame;
    McpCard permissionsCard = makeMcpCard(
        QStringLiteral("Permissions"),
        QStringLiteral("Read-only by default. Optional changes are off until you allow them."),
        QStringLiteral("key-blue.svg"));
    permissionsCard_ = permissionsCard.frame;

    connection_ = new QWidget;
    connection_->setObjectName(QStringLiteral("mcpConnection"));
    auto* setupLayout = new QVBoxLayout(connection_);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->setSpacing(8);
    setupLayout->addWidget(
        mcpLabel(QStringLiteral("1. Where is your assistant?"), "mcpSectionLabel"));
    agentLocation_ = new SegmentedBar({{QStringLiteral("This computer"), QString()},
                                       {QStringLiteral("Another computer"), QString()}});
    agentLocation_->setAccessibleName(QStringLiteral("Agent computer"));
    const bool tailscaleOn = !server_.session_only() && keys::mcp_tailscale_enabled.read(*saved);
    agentLocation_->setVisible(!server_.session_only());
    agentLocation_->setCurrentIndex(tailscaleOn ? remoteIndex_ : localIndex_);
    connect(agentLocation_, &SegmentedBar::currentIndexChanged, this,
            [this](int) { renderSetup(); });
    setupLayout->addWidget(agentLocation_);
    auto* addressLayout = new QVBoxLayout;
    addressLayout->setSpacing(4);
    urlLabel_ = mcpLabel(QStringLiteral("MCP server address"), "mcpFieldLabel");
    addressLayout->addWidget(urlLabel_);
    url_ = new QLineEdit;
    url_->setReadOnly(true);
    url_->setAccessibleName(QStringLiteral("MCP server address"));
    copyUrl_ = copyRow(addressLayout, url_, QStringLiteral("Copy address"),
                       [this] { return url_->text(); });
    copyUrl_->setAccessibleName(QStringLiteral("Copy MCP server address"));
    setupLayout->addLayout(addressLayout);
    setupLayout->addSpacing(8);
    setupLayout->addWidget(mcpLabel(QStringLiteral("2. Set up your assistant"), "mcpSectionLabel"));
    setupKind_ = new SegmentedBar({{QStringLiteral("Setup prompt"), QString()},
                                   {QStringLiteral("Claude Code"), QString()},
                                   {QStringLiteral("Cursor"), QString()},
                                   {QStringLiteral("ChatGPT"), QString()}});
    setupKind_->setAccessibleName(QStringLiteral("Agent setup method"));
    connect(setupKind_, &SegmentedBar::currentIndexChanged, this, [this](int) { renderSetup(); });
    setupLayout->addWidget(setupKind_);
    setupHint_ = mcpDetail(QString());
    setupLayout->addWidget(setupHint_);
    copySetup_ = new McpActionButton(QStringLiteral("Copy setup prompt"));
    copySetup_->setObjectName(QStringLiteral("primaryButton"));
    copySetup_->setBaseMinimumSize(0, 38);
    copySetup_->setToolTip(
        QStringLiteral("Includes the access token, which grants saved-history access and the "
                       "permissions you choose. Share only with trusted assistants."));
    connect(copySetup_, &QPushButton::clicked, this, [this] { copySetupText(); });
    preview_ = new McpDisclosure(QStringLiteral("Preview prompt"), true);
    copyActions_ = new McpAdaptiveRow({{copySetup_, 0}, {nullptr, 0}, {preview_->toggle(), 0}}, 10);
    setupLayout->addWidget(copyActions_);
    setupText_ = new QPlainTextEdit;
    setupText_->setReadOnly(true);
    setupText_->setMinimumHeight(160);
    setupText_->setMaximumHeight(210);
    setupText_->setAccessibleName(QStringLiteral("MCP setup instructions"));
    preview_->bodyLayout()->addWidget(setupText_);
    setupLayout->addWidget(preview_);
    setupLayout->addSpacing(8);
    auto* tokenLayout = new QVBoxLayout;
    tokenLayout->setSpacing(4);
    auto* tokenLabel = mcpLabel(
        QStringLiteral("<b>Access token</b> · already included when you copy the setup above"),
        "mcpFieldLabel");
    tokenLabel->setTextFormat(Qt::RichText);
    tokenLayout->addWidget(tokenLabel);
    token_ = new QLineEdit;
    token_->setReadOnly(true);
    token_->setEchoMode(QLineEdit::Password);
    token_->setAccessibleName(QStringLiteral("MCP access token"));
    token_->setPlaceholderText(QStringLiteral("Available when MCP is running"));
    token_->setToolTip(QStringLiteral(
        "Saved in your system credential store. Copy it here if an assistant asks for it."));
    copyToken_ = copyRow(tokenLayout, token_, QStringLiteral("Copy token"),
                         [this] { return q(server_.token()); });
    copyToken_->setAccessibleName(QStringLiteral("Copy access token"));
    copyToken_->setToolTip(
        QStringLiteral("Grants saved-history access and the permissions you choose. Share only "
                       "with trusted assistants."));
    setupLayout->addLayout(tokenLayout);
    connectionCard.layout->addWidget(connection_);

    advanced_ = new McpDisclosure(QStringLiteral("Advanced connection"));
    advanced_->bodyLayout()->addWidget(mcpDetail(QStringLiteral(
        "Turn MCP off to change these settings. Remote assistants must be on the same Tailscale "
        "network.")));
    port_ = new NoWheelSpinBox;
    port_->setAccessibleName(QStringLiteral("MCP local port"));
    port_->setRange(1, 65535);
    port_->setKeyboardTracking(false);
    port_->setValue(static_cast<int>(keys::mcp_port.read(*saved)));
    connect(port_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int value) { portChanged(value); });
    advanced_->bodyLayout()->addWidget(
        new FieldTile(QStringLiteral("Local port"), QString(), port_, QIcon(), true));
    tailscaleTile_ = new SettingTile(
        QStringLiteral("Allow agents over Tailscale"),
        QStringLiteral("Connect from another computer on the same Tailscale network. Local "
                       "access stays available."),
        ui::designIcon(QStringLiteral("world-blue.svg")));
    tailscaleEnabled()->setAccessibleName(QStringLiteral("Allow agents over Tailscale"));
    connect(tailscaleEnabled(), &QAbstractButton::toggled, this,
            [this](bool checked) { saveTailscale(checked); });
    advanced_->bodyLayout()->addWidget(tailscaleTile_);
    connectionCard.layout->addWidget(advanced_);
    connectionCard.layout->addStretch();

    auto* baseline = new McpPermissionRow(
        QStringLiteral("Read saved text"),
        QStringLiteral("Search saved dictations, transcripts and meeting insights whenever MCP is "
                       "on."),
        QStringLiteral("file-music-blue.svg"));
    baseline->checkbox()->hide();
    baseline->setCursor(Qt::ArrowCursor);
    baseline->checkbox()->setEnabled(false);
    auto* included = new QLabel(QStringLiteral("Included"));
    included->setObjectName(QStringLiteral("mcpIncluded"));
    baseline->rowLayout()->addWidget(included, 0, Qt::AlignVCenter);
    permissionsCard.layout->addWidget(baseline);
    for (const PermissionSpec& spec : permissionSpecs()) {
        auto* row = new McpPermissionRow(q(spec.title), q(spec.description), q(spec.icon));
        const std::string key(spec.key);
        connect(row->checkbox(), &QAbstractButton::toggled, this,
                [this, key](bool checked) { savePermission(key, checked); });
        permissionChecks_.emplace_back(key, row->checkbox());
        permissionsCard.layout->addWidget(row);
    }

    permissionSummary_ = new QLabel;
    permissionSummary_->setObjectName(QStringLiteral("settingsTileDescription"));
    preferences_ = new McpDisclosure(QStringLiteral("Choose individual preferences"), false,
                                     permissionSummary_);
    preferences_->bodyLayout()->addWidget(mcpDetail(QStringLiteral(
        "Each checkbox allows changes to that preference. Permissions apply to every assistant "
        "using this token; turning one off blocks future changes immediately.")));
    settingsPermissions_ = new QWidget;
    settingsPermissions_->setObjectName(QStringLiteral("mcpSettingsPermissions"));
    auto* preferencesLayout = new QVBoxLayout(settingsPermissions_);
    preferencesLayout->setContentsMargins(0, 0, 0, 0);
    preferencesLayout->setSpacing(10);
    for (const agentapi::SettingControl& control : agentapi::SETTING_CONTROLS()) {
        const std::string group(control.group);
        McpDisclosure* disclosure = nullptr;
        for (const auto& [name, existing] : settingGroups_) {
            if (name == group) {
                disclosure = existing;
            }
        }
        if (disclosure == nullptr) {
            disclosure = new McpDisclosure(q(group));
            ui::setStyleProperty(disclosure->toggle(), ui::prop::group, true);
            settingGroups_.emplace_back(group, disclosure);
            preferencesLayout->addWidget(disclosure);
        }
        auto* check = new QCheckBox(q(control.label));
        check->setAccessibleName(QStringLiteral("Allow agent changes: %1").arg(q(control.label)));
        check->setToolTip(q(control.effect));
        const std::string key(control.key);
        connect(check, &QCheckBox::toggled, this,
                [this, key](bool checked) { saveSettingPermission(key, checked); });
        disclosure->bodyLayout()->addWidget(check);
        settingChecks_.emplace_back(key, check);
    }
    preferences_->bodyLayout()->addWidget(settingsPermissions_);
    permissionsCard.layout->addWidget(preferences_);
    WrappedLabel* limits = mcpDetail(QStringLiteral(
        "Never allowed: deletion, transcript edits, audio access or permission changes. A "
        "cloud-powered assistant may send saved text to its provider."));
    limits->setObjectName(QStringLiteral("mcpNote"));
    permissionsCard.layout->addWidget(limits);
    permissionsCard.layout->addStretch();

    notice_ = mcpLabel(QString(), "mcpNotice");
    notice_->setTextFormat(Qt::PlainText);
    layout->addWidget(notice_);
    layout->addStretch();
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    arrangeColumns();
    refresh();
}

McpSettingsPage::~McpSettingsPage() = default;

SettingsSwitch* McpSettingsPage::tailscaleEnabled() const {
    return tailscaleTile_->checkbox();
}

SettingsSwitch* McpSettingsPage::permissionCheck(std::string_view key) const {
    for (const auto& [name, check] : permissionChecks_) {
        if (name == key) {
            return check;
        }
    }
    return nullptr;
}

QCheckBox* McpSettingsPage::settingCheck(std::string_view key) const {
    for (const auto& [name, check] : settingChecks_) {
        if (name == key) {
            return check;
        }
    }
    return nullptr;
}

McpActionButton* McpSettingsPage::copyRow(QLayout* layout, QWidget* field,
                                          const QString& buttonText,
                                          std::function<QString()> value) {
    auto* button = new McpActionButton(buttonText);
    button->setObjectName(QStringLiteral("mcpCopyButton"));
    button->setBaseMinimumSize(0, 38);
    button->setAccessibleName(buttonText);
    connect(button, &QPushButton::clicked, this,
            [this, value = std::move(value)] { copyText(value()); });
    layout->addWidget(new McpCopyField(field, button));
    return button;
}

void McpSettingsPage::arrangeColumns() {
    const bool wide = width() >= ui::scaled(740);
    if (wide_ == wide) {
        return;
    }
    wide_ = wide;
    columns_->removeWidget(connectionCard_);
    columns_->removeWidget(permissionsCard_);
    constexpr Qt::Alignment top = Qt::AlignTop;
    columns_->addWidget(connectionCard_, 0, 0, top);
    columns_->addWidget(permissionsCard_, wide ? 0 : 1, wide ? 1 : 0, top);
    columns_->setColumnStretch(0, wide ? 6 : 1);
    columns_->setColumnStretch(1, wide ? 5 : 0);
}

void McpSettingsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrangeColumns();
}

void McpSettingsPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        permissionsCard_ != nullptr) {
        arrangeColumns();
    }
}

void McpSettingsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    timer_->start();
}

void McpSettingsPage::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

// ---- actions ----

void McpSettingsPage::copyText(const QString& text) {
    if (server_.status().state != "running" || text.isEmpty()) {
        return;
    }
    action_prompts::copyText()(text);
    noticeText_ = QStringLiteral("Copied to clipboard.");
    notice_->setText(noticeText_);
    notice_->show();
}

void McpSettingsPage::toggle(bool checked) {
    try {
        if (!server_.session_only())
            settings_.save_setting(keys::mcp_enabled.key(), checked);
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the MCP setting ({})", type_display_name(typeid(failure)));
        noticeText_ = QStringLiteral("Could not save the MCP setting. Try again.");
        refresh();
        return;
    }
    noticeText_.clear();
    if (checked) {
        start();
        return;
    }
    server_.stop();
    refresh();
}

void McpSettingsPage::start() {
    const bool tailscale =
        !server_.session_only() && keys::mcp_tailscale_enabled.read(*settings_.snapshot());
    try {
        server_.start(port_->value(), tailscale);
    } catch (const std::exception& failure) {
        kLog.warning("Could not start MCP ({})", type_display_name(typeid(failure)));
        noticeText_ = q(failure.what());
    }
    refresh();
}

void McpSettingsPage::saveTailscale(bool checked) {
    try {
        settings_.save_setting(keys::mcp_tailscale_enabled.key(), checked);
        noticeText_.clear();
        agentLocation_->setCurrentIndex(checked ? remoteIndex_ : localIndex_);
    } catch (const std::exception& failure) {
        kLog.warning("Could not save MCP Tailscale access ({})",
                     type_display_name(typeid(failure)));
        noticeText_ = QStringLiteral("Could not save Tailscale access. Try again.");
    }
    refresh();
}

void McpSettingsPage::savePermission(const std::string& key, bool checked) {
    try {
        if (server_.session_only())
            server_.stop();
        settings_.save_setting(key, checked);
        noticeText_.clear();
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the MCP agent permission ({})",
                     type_display_name(typeid(failure)));
        noticeText_ = QStringLiteral("Could not save the agent permission. Try again.");
    }
    refresh();
}

void McpSettingsPage::saveSettingPermission(const std::string& key, bool checked) {
    try {
        if (server_.session_only())
            server_.stop();
        settings_.mutate([&](Json& saved) {
            Json permissions = Json::object();
            const auto found = saved.find(std::string(keys::mcp_writable_settings.key()));
            if (found != saved.end() && found->is_object()) {
                permissions = *found;
            }
            permissions[key] = checked;
            saved[std::string(keys::mcp_writable_settings.key())] = std::move(permissions);
        });
        noticeText_.clear();
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the MCP setting permission ({})",
                     type_display_name(typeid(failure)));
        noticeText_ = QStringLiteral("Could not save the setting permission. Try again.");
    }
    refresh();
}

void McpSettingsPage::portChanged(int value) {
    try {
        settings_.save_setting(keys::mcp_port.key(), value);
        noticeText_.clear();
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the MCP port ({})", type_display_name(typeid(failure)));
        noticeText_ = QStringLiteral("Could not save the MCP port. Try again.");
        const QSignalBlocker blocker(port_);
        port_->setValue(static_cast<int>(keys::mcp_port.read(*settings_.snapshot())));
    }
    refresh();
}

// ---- the setup text ----

QString McpSettingsPage::setupUrl(const agentapi::ServerStatus& status) const {
    if (agentLocation_->currentIndex() == remoteIndex_) {
        return q(status.remote_url);
    }
    return q(status.url());
}

QString McpSettingsPage::setupFor(const QString& url, const std::string& token) const {
    const std::string address = qt::toUtf8(url);
    const std::string_view withPlaceholder =
        token.empty() ? agentapi::TOKEN_PLACEHOLDER : std::string_view(token);
    switch (setupKind_->currentIndex()) {
    case 0:
        return q(agentapi::agent_prompt(address, std::nullopt, token));
    case 1:
        return q(agentapi::claude_command(address, withPlaceholder));
    case 2:
        return q(agentapi::client_config(address, withPlaceholder));
    default:
        return q(agentapi::chatgpt_config(address, withPlaceholder));
    }
}

void McpSettingsPage::copySetupText() {
    // Built at click time: the preview only ever holds the masked token.
    const QString address = setupUrl(server_.status());
    if (!address.isEmpty()) {
        copyText(setupFor(address, server_.token()));
    }
}

void McpSettingsPage::renderSetup() {
    const agentapi::ServerStatus status = server_.status();
    const QString address = setupUrl(status);
    const bool remote = agentLocation_->currentIndex() == remoteIndex_;
    urlLabel_->setText(remote ? QStringLiteral("Tailscale MCP address")
                              : QStringLiteral("Local MCP address"));
    const bool ready = status.state == "running" && !address.isEmpty();
    url_->setToolTip(ready ? address
                           : QStringLiteral("The address appears when this connection is "
                                            "available."));
    copySetup_->setEnabled(ready);
    copyUrl_->setEnabled(ready);
    url_->setEnabled(ready);
    static const std::array<const char*, 4> kinds{"setup prompt", "Claude Code command",
                                                  "Cursor JSON", "ChatGPT config"};
    static const std::array<const char*, 4> previews{"prompt", "command", "JSON", "config"};
    const int kind = setupKind_->currentIndex();
    const QString kindName = QString::fromLatin1(kinds.at(static_cast<std::size_t>(kind)));
    copySetup_->setText(QStringLiteral("Copy %1").arg(kindName));
    preview_->toggle()->setText(
        QStringLiteral("Preview %1")
            .arg(QString::fromLatin1(previews.at(static_cast<std::size_t>(kind)))));
    preview_->toggle()->setAccessibleName(QStringLiteral("Preview %1").arg(kindName));
    preview_->toggle()->setToolTip(QStringLiteral("Preview %1").arg(kindName));
    copyActions_->reflow();
    const bool hasToken = !server_.token().empty();
    static const std::array<const char*, 4> hints{
        "Paste the setup prompt into your assistant.",
        "Paste the command into your terminal.",
        "Merge this JSON into Cursor's MCP configuration.",
        "For the desktop app, merge this into ~/.codex/config.toml.",
    };
    QString hint = QString::fromLatin1(hints.at(static_cast<std::size_t>(kind)));
    if (hasToken) {
        hint += QStringLiteral(" Your access token is already filled in.");
    } else if (kind == 0) {
        hint += QStringLiteral(" Copy the access token separately when asked.");
    } else {
        hint += QStringLiteral(" Replace %1 with the access token.")
                    .arg(q(agentapi::TOKEN_PLACEHOLDER));
    }
    setupHint_->setText(hint);
    if (!ready) {
        url_->setText(status.state == "running"
                          ? QStringLiteral("Tailscale access is not enabled")
                          : QStringLiteral("Enable MCP to get your connection "
                                           "URL"));
        setupText_->setPlainText(
            status.state == "running"
                ? QStringLiteral("To connect an agent on another computer, turn MCP off, enable "
                                 "Allow agents over Tailscale, then turn MCP on again.")
                : QStringLiteral("Connection instructions appear when MCP is running."));
        setupHint_->setText(setupText_->toPlainText());
        return;
    }
    if (url_->text() != address) {
        url_->setText(address);
        url_->setCursorPosition(0);
    }
    const QString text =
        setupFor(address, hasToken ? std::string(mcp_page::HIDDEN_TOKEN) : std::string());
    if (setupText_->toPlainText() != text) {
        setupText_->setPlainText(text);
    }
}

// ---- state ----

void McpSettingsPage::refresh() {
    const agentapi::ServerStatus status = server_.status();
    const std::shared_ptr<const Json> snapshot = settings_.snapshot();
    const Json& preferences = *snapshot;
    const bool selected = server_.session_only()
                              ? status.state == "starting" || status.state == "running"
                              : keys::mcp_enabled.read(preferences);
    for (const auto& [key, check] : permissionChecks_) {
        const QSignalBlocker blocker(check);
        check->setChecked(isTrue(preferences, key));
    }
    const std::set<std::string> granted = agentapi::writable_settings(preferences);
    for (const auto& [key, check] : settingChecks_) {
        const QSignalBlocker blocker(check);
        check->setChecked(granted.contains(key));
    }
    settingsPermissions_->setEnabled(keys::mcp_settings_access.read(preferences));
    const bool access = settingsPermissions_->isEnabled();
    permissionSummary_->setText(access ? q(std::to_string(granted.size()) + " allowed to change")
                                       : QStringLiteral("Needs read access"));
    {
        const QSignalBlocker blocker(enabled_);
        enabled_->setChecked(selected);
    }
    {
        const QSignalBlocker blocker(tailscaleEnabled());
        tailscaleEnabled()->setChecked(!server_.session_only() &&
                                       keys::mcp_tailscale_enabled.read(preferences));
    }
    const bool stoppedOrError = status.state == "stopped" || status.state == "error";
    tailscaleTile_->setVisible(!server_.session_only());
    tailscaleTile_->setEnabled(stoppedOrError && !selected);
    enableTile_->setEnabled(status.state != "stopping");
    port_->setEnabled(stoppedOrError && !selected);
    retry_->setVisible(status.state == "error" && (selected || server_.session_only()));
    static const std::map<std::string, std::string, std::less<>> labels{
        {"stopped", "Off"},       {"starting", "Starting"},     {"running", "Running"},
        {"stopping", "Stopping"}, {"error", "Could not start"},
    };
    std::string detail = status.message;
    if (status.state == "running") {
        detail = "Keep " + std::string(identity().product) + " open.";
    } else if (status.state == "stopped") {
        detail = "Turn on to let an assistant search your saved dictations and meetings.";
    }
    const auto label = labels.find(status.state);
    statusLabel_->setText(q("<b>" + (label != labels.end() ? label->second : status.state) +
                            "</b> · " + mcp_page::html_escape(detail)));
    statusLabel_->setToolTip(q(status.message));
    ui::setStyleProperty(enableTile_, ui::prop::state,
                         status.state == "running" ? QStringLiteral("running")
                         : status.state == "error" ? QStringLiteral("error")
                                                   : QStringLiteral("off"));
    statusDot_->setStatus(status.state == "running" ? EngineStatus::Ready
                          : status.state == "error" ? EngineStatus::Attention
                                                    : EngineStatus::Unknown);
    statusDot_->setBusy(status.state == "starting" || status.state == "stopping");
    const bool ready = status.state == "running";
    connection_->setEnabled(ready);
    renderSetup();
    token_->setText(ready ? q(server_.token()) : QString());
    if (!ready) {
        setupText_->setPlainText(
            QStringLiteral("Connection instructions appear when MCP is running."));
    }
    notice_->setText(noticeText_);
    notice_->setVisible(!noticeText_.isEmpty());
}

// ---- McpSettingsView ----

McpSettingsView::McpSettingsView(SettingsStore& settings, agentapi::McpServerControl& server,
                                 LiveServices services, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("mcpSettingsView"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    local_ = new McpSettingsPage(settings, server);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    layout->addWidget(local_);
    live_ = new LiveAutomationSection(settings, server, std::move(services));
    live_->setVisible(!server.session_only());
    connect(live_, &LiveAutomationSection::settingsChanged, this,
            &McpSettingsView::settingsChanged);
    // Counters refresh on this computer's page timer, which runs while it shows.
    connect(local_->timer(), &QTimer::timeout, live_, &LiveAutomationSection::refreshLive);
    layout->addWidget(live_);
}

namespace {

// What the frame says when a request to the paired host fails: the host's refusal by its code,
// else why it couldn't be reached. OpenWhisper's gate titles, with the host's grant in place of
// its "Allow paired computers to manage MCP" opt-in (a17-paired-mcp).
QString pairedHostProblem(const QString& host, const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const remote::RemoteRequestError& refused) {
        if (refused.code == "forbidden") {
            return McpSettingsView::tr(
                       "%1 hasn't allowed this yet. Its owner can grant this computer a "
                       "15-minute MCP session in Settings → Remote engine on %1.")
                .arg(host);
        }
        if (refused.code == "unsupported") {
            return McpSettingsView::tr(
                       "%1 needs an update. Update and restart %2 on %1, then ask its owner to "
                       "grant this computer an MCP session there.")
                .arg(host, q(identity().product));
        }
        return q(refused.what());
    } catch (const remote::RemoteEngineError& unreachable) {
        return McpSettingsView::tr("Can't reach %1. %2").arg(host, q(unreachable.what()));
    } catch (const std::exception& failure) {
        return q(failure.what());
    } catch (...) {
        return McpSettingsView::tr("Can't reach %1.").arg(host);
    }
}

} // namespace

void McpSettingsView::bindRemote(remote::RemoteEngineControl* service, Executor* io) {
    if (pairedHost_ || !service || !io) {
        return;
    }
    remote_ = service;
    remoteIo_ = io;
    pairedHost_ = new QFrame(this);
    pairedHost_->setObjectName(QStringLiteral("mcpPairedHost"));
    auto* layout = new QVBoxLayout(pairedHost_);
    hostTitle_ = new QLabel(tr("Paired computer"), pairedHost_);
    hostTitle_->setTextFormat(Qt::PlainText);
    layout->addWidget(hostTitle_);
    checkHost_ = new QPushButton(tr("Check host MCP status"), pairedHost_);
    layout->addWidget(checkHost_, 0, Qt::AlignLeft);
    startHost_ = new QPushButton(tr("Start the authorized host session…"), pairedHost_);
    endHost_ = new QPushButton(tr("End host session"), pairedHost_);
    layout->addWidget(startHost_, 0, Qt::AlignLeft);
    layout->addWidget(endHost_, 0, Qt::AlignLeft);
    hostStatus_ = new QLabel(pairedHost_);
    hostStatus_->setObjectName(QStringLiteral("mcpPairedHostStatus"));
    hostStatus_->setTextFormat(Qt::PlainText);
    hostStatus_->setWordWrap(true);
    layout->addWidget(hostStatus_);
    // Hidden until the pairing has been read: a computer with no host gets no frame.
    pairedHost_->setVisible(false);
    this->layout()->addWidget(pairedHost_);
    remoteUi_ = std::make_shared<qt::QtUiExecutor>(this);
    // The io tasks below touch `this` only inside what they post back to remoteUi_, which drops
    // it once the view is gone; a view closed while a request runs is not held or touched.
    remoteEvents_ = service->changed.connect(*remoteUi_, [this](const std::string& kind) {
        // A rename here ("host_renamed") changes what the frame calls the host.
        if (kind == "client" || kind == "paired" || kind == "host_renamed") {
            readPairing();
        }
    });
    connect(startHost_, &QPushButton::clicked, this, [this] {
        setHostBusy(true);
        remoteIo_->post([ui = remoteUi_, service = remote_, this] {
            std::optional<remote::ClientPairing> peer;
            QString error;
            try {
                peer = service->client_pairing();
            } catch (const std::exception& failure) {
                error = q(failure.what());
            }
            ui->post([this, peer, error] {
                if (!peer) {
                    hostStatus_->setText(
                        error.isEmpty() ? tr("Pair with a host in Remote engine settings first.")
                                        : error);
                    setHostBusy(false);
                    return;
                }
                const QString host = q(peer->host_name);
                const auto answer = action_prompts::question()(
                    this, tr("Start host MCP session?"),
                    tr("Ask %1 to start its authorized read-only MCP session at "
                       "127.0.0.1:8768 on that computer? The host must already have granted "
                       "this computer permission.")
                        .arg(host),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
                if (answer != QMessageBox::Yes) {
                    setHostBusy(false);
                    return;
                }
                hostStatus_->setText(tr("Starting the session on %1…").arg(host));
                remoteIo_->post([ui = remoteUi_, service = remote_, this, peer, host] {
                    QString result;
                    try {
                        const auto reply = service->remote_mcp_request(
                            "mcp_configure", {{"settings", {{"enabled", true}}}}, peer);
                        result = tr("MCP on %1 is %2. Use End host session to disconnect.")
                                     .arg(host, q(reply.value("state", std::string("starting"))));
                    } catch (...) {
                        result = pairedHostProblem(host, std::current_exception());
                    }
                    ui->post([this, result] {
                        hostStatus_->setText(result);
                        setHostBusy(false);
                    });
                });
            });
        });
    });
    connect(endHost_, &QPushButton::clicked, this, [this] {
        remoteIo_->post([ui = remoteUi_, service = remote_, this] {
            service->close_mcp_control();
            ui->post(
                [this] { hostStatus_->setText(tr("Disconnected from the host MCP session.")); });
        });
    });
    connect(checkHost_, &QPushButton::clicked, this, [this] {
        checkHost_->setEnabled(false);
        hostStatus_->setText(tr("Connecting to the paired computer…"));
        remoteIo_->post([ui = remoteUi_, service = remote_, this] {
            QString status;
            try {
                const auto peer = service->client_pairing();
                if (!peer) {
                    status = tr("Pair a computer in Remote engine settings first.");
                } else {
                    const QString host = q(peer->host_name);
                    try {
                        const auto state =
                            service->remote_mcp_request("mcp_state", Json::object(), peer);
                        const auto mode = state.find("state");
                        if (mode == state.end() || !mode->is_string()) {
                            status = tr("%1 returned an unknown MCP status.").arg(host);
                        } else {
                            status = tr("MCP on %1 is %2.")
                                         .arg(host, q(mode->get_ref<const std::string&>()));
                        }
                    } catch (...) {
                        status = pairedHostProblem(host, std::current_exception());
                    }
                }
            } catch (const std::exception& problem) {
                status = q(problem.what());
            }
            ui->post([this, status = std::move(status)] {
                checkHost_->setEnabled(true);
                hostStatus_->setText(status);
            });
        });
    });
    readPairing();
}

void McpSettingsView::readPairing() {
    remoteIo_->post([ui = remoteUi_, service = remote_, this] {
        std::optional<remote::ClientPairing> peer;
        try {
            peer = service->client_pairing();
        } catch (const std::exception& failure) {
            kLog.warning("Couldn't read the paired host: {}", failure.what());
        }
        ui->post([this, peer] {
            if (!peer) {
                pairedHost_->setVisible(false);
                hostStatus_->clear();
                return;
            }
            hostTitle_->setText(tr("Paired computer: %1").arg(q(peer->host_name)));
            pairedHost_->setVisible(true);
        });
    });
}

void McpSettingsView::setHostBusy(bool busy) {
    startHost_->setEnabled(!busy);
    endHost_->setEnabled(!busy);
}

void McpSettingsView::refreshPage() {
    local_->refresh();
    live_->refresh();
}

QWidget* McpSettingsView::namedControl(std::string_view name) {
    if (name == "mcp_page") {
        return local_;
    }
    if (name == "mcp_view") {
        return this;
    }
    return nullptr;
}

// ---- registration ----

void registerMcpPage(AppContext& context) {
    provideSessionMcp(context);
    SettingsPageSpec spec;
    spec.destination = destinations::mcp;
    spec.title = q(mcp_page::TITLE);
    spec.subtitle = q(mcp_page::SUBTITLE);
    spec.controls = mcp_page::control_names();
    spec.create = [&context](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        if (dialog == nullptr) {
            throw std::logic_error("The API & MCP page is built inside the Settings window.");
        }
        agentapi::McpServerControl* server = context.find<agentapi::McpServerControl>();
        if (server == nullptr) {
            kLog.warning("No MCP server control is provided; the page shows MCP as unavailable");
            server = &unavailableControl();
        }
        LiveServices services;
        try {
            services.io = &context.io();
        } catch (const ServiceUnavailable&) {
            // Without blocking work Clear runs on the GUI thread.
        }
        try {
            services.paths = &context.paths();
        } catch (const ServiceUnavailable&) {
            // The channel's entry then takes the folders of this process.
        }
        if (auto* events = context.find<agentapi::McpEvents>()) {
            services.subscriptionCount = [events] {
                return events->subscription_count();
            };
            services.clearSubscriptions = [events] {
                return events->clear_subscriptions();
            };
        }
        auto* io = services.io;
        auto host = std::make_unique<DialogPageHost>(*dialog);
        auto* view = new McpSettingsView(context.settings(), *server, std::move(services));
        if (auto* remoteEngine = context.find<remote::RemoteEngineControl>()) {
            view->bindRemote(remoteEngine, io);
        }
        SettingsPageHost* pageHost = host.get();
        QObject::connect(view, &McpSettingsView::settingsChanged, view,
                         [pageHost](const QString& kind) { pageHost->notifyChanged(kind); });
        // Owned by the window, which deletes its pages first: the host outlives every control
        // torn down with the page.
        host.release()->setParent(dialog);
        return view;
    };
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
