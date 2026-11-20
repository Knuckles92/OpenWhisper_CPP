#include "ui_remote/host_dashboard.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include <QBoxLayout>
#include <QClipboard>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "agentapi/mcp_runtime.h"
#include "agentapi/setup.h"
#include "base/clock.h"
#include "base/log.h"
#include "remote/protocol.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_remote/client_history_dialog.h"
#include "ui_remote/host_beacon.h"
#include "ui_remote/remote_text.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/remote_link_glyph.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.host_dashboard"};

constexpr int kCopyFeedbackMs = 2000;

QString q(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string utf8(const QString& text) {
    return text.toStdString();
}

QString jstr(const Json& object, const char* name) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? q(found->get_ref<const std::string&>())
                                                       : QString();
}

double number(const Json& object, const char* name) {
    if (!object.is_object()) {
        return 0.0;
    }
    const auto found = object.find(name);
    if (found == object.end()) {
        return 0.0;
    }
    if (found->is_number()) {
        return found->get<double>();
    }
    return found->is_boolean() && found->get<bool>() ? 1.0 : 0.0;
}

bool flag(const Json& object, const char* name) {
    if (!object.is_object()) {
        return false;
    }
    const auto found = object.find(name);
    if (found == object.end() || found->is_null()) {
        return false;
    }
    if (found->is_boolean()) {
        return found->get<bool>();
    }
    if (found->is_number()) {
        return found->get<double>() != 0.0;
    }
    if (found->is_string()) {
        return !found->get_ref<const std::string&>().empty();
    }
    return !found->empty();
}

const Json& member(const Json& object, const char* name) {
    static const Json kNull;
    if (!object.is_object()) {
        return kNull;
    }
    const auto found = object.find(name);
    return found != object.end() ? *found : kNull;
}

WrappedLabel* muted(const QString& text = {}, const char* name = "hostMuted") {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QString::fromLatin1(name));
    return label;
}

QPushButton* link(const QString& text, const char* name) {
    auto* button = new QPushButton(text);
    button->setObjectName(QString::fromLatin1(name));
    button->setCursor(Qt::PointingHandCursor);
    button->setFlat(true);
    button->setAutoDefault(false);
    return button;
}

QLabel* dot(const QString& tone) {
    auto* made = new QLabel;
    made->setObjectName(QStringLiteral("hostStatDot"));
    ui::setStyleProperty(made, ui::prop::tone, tone);
    made->setFixedSize(8, 8);
    return made;
}

// Empties `layout`, hiding each widget at once (deletion comes later).
void clearLayout(QLayout& layout) {
    while (QLayoutItem* item = layout.takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

// The page-local sheet (host_dashboard.py's _STYLE). Where OpenWhisper selects on properties the
// theme does not declare (busy, connected, hostLink), this selects on prop::active and on the
// links' own names.
const QString kStyle = QStringLiteral(R"(
    QScrollArea#hostScroll, QWidget#hostContent {
        background-color: @bg;
        border: none;
    }
    QFrame#hostHero {
        background-color: @surface;
        border: 1px solid @border;
        border-radius: 16px;
    }
    QFrame#hostHero[state="on"] {
        background-color: rgba(@success-rgb, 0.08);
        border: 1px solid rgba(@success-rgb, 0.40);
    }
    QFrame#hostHero[state="error"] {
        background-color: @warning-tint;
        border: 1px solid @warning-tint-border;
    }
    QLabel#hostHeroTitle {
        color: @text-heading;
        font-size: 20px;
        font-weight: 700;
        background: transparent;
    }
    QLabel#hostHeroDetail {
        color: @text-body;
        font-size: 13px;
        background: transparent;
    }
    QLabel#hostHeroIdentity {
        color: @text-secondary;
        font-size: 12px;
        background: transparent;
    }
    QFrame#hostStat {
        background-color: @surface;
        border: 1px solid @border;
        border-radius: 12px;
    }
    QLabel#hostStatCaption {
        color: @text-secondary;
        font-size: 12px;
        font-weight: 600;
        background: transparent;
    }
    QLabel#hostStatValue {
        color: @text-heading;
        font-size: 24px;
        font-weight: 700;
        background: transparent;
    }
    QLabel#hostStatDetail {
        color: @text-secondary;
        font-size: 12px;
        background: transparent;
    }
    QLabel#hostStatDot { border-radius: 4px; background-color: @text-muted; }
    QLabel#hostStatDot[tone="success"] { background-color: @success; }
    QLabel#hostStatDot[tone="accent"] { background-color: @accent; }
    QLabel#hostStatDot[tone="accent-cyan"] { background-color: @accent-cyan; }
    QLabel#hostStatDot[tone="purple"] { background-color: @purple; }
    QLabel#hostStatDot[tone="warning"] { background-color: @warning; }
    QLabel#hostStatDot[tone="danger"] { background-color: @danger; }
    QFrame#hostCard {
        background-color: @surface;
        border: 1px solid @border;
        border-radius: 14px;
    }
    QLabel#hostCardTitle {
        color: @text-heading;
        font-size: 15px;
        font-weight: 700;
        background: transparent;
    }
    QLabel#hostCardBadge {
        color: @text-secondary;
        background-color: @surface-hover;
        border-radius: 9px;
        padding: 1px 8px;
        font-size: 12px;
        font-weight: 600;
    }
    QLabel#hostEngineName {
        color: @text-heading;
        font-size: 17px;
        font-weight: 600;
        background: transparent;
    }
    QLabel#hostMuted, QLabel#hostEventText {
        color: @text-secondary;
        font-size: 13px;
        background: transparent;
    }
    QLabel#hostEventText { color: @text-body; }
    QLabel#hostMcpStatus {
        color: @text-heading;
        font-size: 14px;
        font-weight: 600;
        background: transparent;
    }
    QLabel#hostWarning {
        color: @warning-text;
        font-size: 13px;
        background: transparent;
    }
    QLabel#hostFieldLabel {
        color: @text-secondary;
        font-size: 13px;
        font-weight: 600;
        background: transparent;
    }
    QFrame#hostClientRow, QFrame#hostDeviceRow {
        background-color: @surface-sunken;
        border: 1px solid @border-subtle;
        border-radius: 10px;
    }
    QLabel#hostClientName, QLabel#hostDeviceName {
        color: @text-heading;
        font-size: 14px;
        font-weight: 600;
        background: transparent;
    }
    QLabel#hostClientAddress, QLabel#hostClientStats, QLabel#hostDeviceDetail {
        color: @text-secondary;
        font-size: 12px;
        background: transparent;
    }
    QLabel#hostClientState {
        color: @text-secondary;
        font-size: 12px;
        background: transparent;
    }
    QLabel#hostClientState[active="true"] {
        color: @accent-cyan;
        font-weight: 600;
    }
    QLabel#hostDeviceStatus {
        color: @text-secondary;
        font-size: 12px;
        background: transparent;
    }
    QLabel#hostDeviceStatus[active="true"] {
        color: @success-text;
        font-weight: 600;
    }
    QFrame#hostEventRow { background: transparent; }
    QLabel#hostEventTime {
        color: @text-muted;
        font-size: 12px;
        background: transparent;
    }
    QFrame#hostPairingBox {
        background-color: rgba(@accent-rgb, 0.10);
        border: 1px solid @accent-tint-border;
        border-radius: 12px;
    }
    QLabel#hostPairingCode {
        color: @text-heading;
        font-size: 28px;
        font-weight: 700;
        letter-spacing: 4px;
        background: transparent;
    }
    QPushButton#hostCardButton {
        background-color: @surface-hover;
    }
    QPushButton#hostCardButton:hover {
        background-color: @surface-active;
    }
    QPushButton#hostModelsLink, QPushButton#hostManageLink, QPushButton#hostMcpLink {
        background: transparent;
        border: none;
        color: @accent;
        font-size: 13px;
        font-weight: 600;
        padding: 2px 0px;
        text-align: left;
    }
    QPushButton#hostModelsLink:hover, QPushButton#hostManageLink:hover,
    QPushButton#hostMcpLink:hover {
        color: @accent-soft;
        text-decoration: underline;
    }
)");

bool askToStop(QWidget* parent, const QString& text, const QString& informative) {
    QMessageBox box(parent != nullptr ? parent->window() : nullptr);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Stop sharing"));
    box.setText(text);
    box.setInformativeText(informative);
    auto* stop = box.addButton(QObject::tr("Stop sharing"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == stop;
}

} // namespace

// ---- pieces ----

namespace host_dashboard {

StatTile::StatTile(const QString& captionText, const QString& tone, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("hostStat"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(2);
    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(7);
    caption = new QLabel(captionText);
    caption->setObjectName(QStringLiteral("hostStatCaption"));
    head->addWidget(dot(tone), 0, Qt::AlignVCenter);
    head->addWidget(caption, 1);
    layout->addLayout(head);
    value = new QLabel(QStringLiteral("—"));
    value->setObjectName(QStringLiteral("hostStatValue"));
    layout->addWidget(value);
    detail = new ElidingLabel;
    detail->setObjectName(QStringLiteral("hostStatDetail"));
    layout->addWidget(detail);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void StatTile::set(const QString& valueText, const QString& detailText) {
    value->setText(valueText);
    detail->setText(detailText);
}

Card::Card(const QString& titleText, QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("hostCard"));
    setAccessibleName(titleText);
    body = new QVBoxLayout(this);
    body->setContentsMargins(18, 14, 18, 16);
    body->setSpacing(10);
    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(8);
    title = new QLabel(titleText);
    title->setObjectName(QStringLiteral("hostCardTitle"));
    head->addWidget(title);
    badge = new QLabel;
    badge->setObjectName(QStringLiteral("hostCardBadge"));
    badge->hide();
    head->addWidget(badge, 0, Qt::AlignVCenter);
    head->addStretch(1);
    body->addLayout(head);
}

void Card::setBadge(const QString& text) {
    badge->setText(text);
    badge->setVisible(!text.isEmpty());
}

ClientRow::ClientRow(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("hostClientRow"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(12);
    glyph = new RemoteLinkGlyph(nullptr, true);
    glyph->setToolTip(tr("This computer on the left, the paired computer on the right"));
    layout->addWidget(glyph, 0, Qt::AlignVCenter);
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    name = new ElidingLabel;
    name->setObjectName(QStringLiteral("hostClientName"));
    address = new ElidingLabel;
    address->setObjectName(QStringLiteral("hostClientAddress"));
    top->addWidget(name);
    top->addWidget(address, 1);
    column->addLayout(top);
    stats = new ElidingLabel;
    stats->setObjectName(QStringLiteral("hostClientStats"));
    column->addWidget(stats);
    layout->addLayout(column, 1);
    state = new QLabel;
    state->setObjectName(QStringLiteral("hostClientState"));
    layout->addWidget(state, 0, Qt::AlignVCenter);
}

void ClientRow::showClient(const host_text::GroupedClient& client, const Json& served, double now,
                           const std::chrono::time_zone& zone) {
    name->setText(q(client.name));
    address->setText(q(client.address));
    const auto count = static_cast<long long>(number(served, "transcriptions"));
    glyph->setLink(QStringLiteral("connected"), client.busy, count);
    QStringList parts;
    if (count != 0) {
        parts.append(q(host_text::count(count, "transcription", "transcriptions")));
        parts.append(tr("%1 of audio").arg(q(host_text::span(number(served, "audio_s")))));
    } else {
        parts.append(tr("Nothing transcribed yet"));
    }
    const auto last = host_text::ago(number(served, "last_at"), now, zone);
    if (count != 0 && !last.empty()) {
        parts.append(tr("last %1").arg(q(last)));
    }
    if (client.connections > 1) {
        parts.append(tr("%1 connections").arg(client.connections));
    }
    stats->setText(parts.join(QStringLiteral(" · ")));
    if (client.busy) {
        state->setText(tr("Transcribing…"));
    } else if (client.since != 0.0 && now - client.since >= 60) {
        state->setText(tr("connected for %1").arg(q(host_text::span(now - client.since))));
    } else {
        state->setText(client.since != 0.0 ? tr("just connected") : tr("connected"));
    }
    ui::setStyleProperty(state, ui::prop::active, client.busy);
}

EventRow::EventRow(const Json& event, bool fresh, double now, const std::chrono::time_zone& zone,
                   QWidget* parent)
    : QFrame(parent), serial_(static_cast<long long>(number(event, "serial"))) {
    setObjectName(QStringLiteral("hostEventRow"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 5, 8, 5);
    layout->setSpacing(10);
    const double at = number(event, "at");
    auto* when = new QLabel(q(host_text::clock(at != 0.0 ? at : now, zone)));
    when->setObjectName(QStringLiteral("hostEventTime"));
    when->setMinimumWidth(62);
    layout->addWidget(when, 0, Qt::AlignTop);
    layout->addWidget(dot(q(host_text::event_tone(utf8(jstr(event, "kind"))))), 0,
                      Qt::AlignVCenter);
    text = new WrappedLabel(q(host_text::event_text(event)));
    text->setObjectName(QStringLiteral("hostEventText"));
    layout->addWidget(text, 1);
    if (fresh) {
        auto* animation = new QVariantAnimation(this);
        animation->setStartValue(1.0);
        animation->setEndValue(0.0);
        animation->setDuration(1600);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            glow_ = value.toDouble();
            update();
        });
        glow_ = 1.0;
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void EventRow::paintEvent(QPaintEvent* event) {
    if (glow_ > 0) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor("accent", static_cast<int>(46 * glow_)));
        painter.drawRoundedRect(QRectF(rect()), 8, 8);
    }
    QFrame::paintEvent(event);
}

} // namespace host_dashboard

using host_dashboard::Card;
using host_dashboard::ClientRow;
using host_dashboard::EventRow;
using host_dashboard::StatTile;

// What one background read brought back; each part is set only when it was asked for.
struct HostDashboard::Read {
    std::optional<remote::HostState> state;
    std::optional<std::map<std::string, Json>> stored;
    std::optional<Json> engine;
    std::optional<std::vector<remote::ConnectedClient>> clients;
    std::optional<Json> activity;
    bool all = false;
};

// ---- construction ----

HostDashboard::HostDashboard(remote::RemoteEngineControl* service, Executor& io, Executor& ui,
                             QWidget* parent)
    : QWidget(parent), io_(io), ui_(ui) {
    setObjectName(QStringLiteral("hostDashboard"));
    ui::setLocalStyleSheet(this, kStyle);
    writeClipboard_ = [](const QString& value) {
        if (QClipboard* clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(value);
        }
    };
    confirmStop_ = [this](const QString& text, const QString& informative) {
        return askToStop(this, text, informative);
    };
    flushTimer_ = new QTimer(this);
    flushTimer_->setSingleShot(true);
    flushTimer_->setInterval(host_text::COALESCE_MS);
    connect(flushTimer_, &QTimer::timeout, this, [this] { flushPending(false); });
    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(host_text::CLOCK_INTERVAL_MS);
    connect(clockTimer_, &QTimer::timeout, this, [this] { schedule({"live"}); });
    countdown_ = new QTimer(this);
    countdown_->setInterval(1000);
    connect(countdown_, &QTimer::timeout, this, &HostDashboard::showPairing);
    mcpTimer_ = new QTimer(this);
    mcpTimer_->setInterval(host_text::MCP_POLL_MS);
    connect(mcpTimer_, &QTimer::timeout, this, &HostDashboard::refreshMcp);
    build();
    showUnbound();
    bind(service);
}

HostDashboard::~HostDashboard() = default;

void HostDashboard::build() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    parts_.scroll = new QScrollArea;
    parts_.scroll->setObjectName(QStringLiteral("hostScroll"));
    parts_.scroll->setWidgetResizable(true);
    parts_.scroll->setFrameShape(QFrame::NoFrame);
    parts_.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("hostContent"));
    page_ = new QVBoxLayout(content);
    page_->setContentsMargins(20, 18, 20, 20);
    page_->setSpacing(14);
    buildHero(*page_);
    buildStats(*page_);
    buildEngine(*page_);
    buildClients(*page_);
    buildDevices(*page_);
    buildMcp(*page_);
    buildActivity(*page_);
    page_->addStretch(1);
    parts_.scroll->setWidget(content);
    outer->addWidget(parts_.scroll);
}

void HostDashboard::buildHero(QVBoxLayout& page) {
    parts_.hero = new QFrame;
    parts_.hero->setObjectName(QStringLiteral("hostHero"));
    ui::setStyleProperty(parts_.hero, ui::prop::state, QStringLiteral("off"));
    auto* row = new QHBoxLayout(parts_.hero);
    row->setContentsMargins(18, 16, 18, 16);
    row->setSpacing(16);
    parts_.beacon = new HostBeacon;
    row->addWidget(parts_.beacon, 0, Qt::AlignVCenter);
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    parts_.heroTitle = new WrappedLabel;
    parts_.heroTitle->setObjectName(QStringLiteral("hostHeroTitle"));
    parts_.heroDetail = new WrappedLabel;
    parts_.heroDetail->setObjectName(QStringLiteral("hostHeroDetail"));
    parts_.heroDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    parts_.heroIdentity = new WrappedLabel;
    parts_.heroIdentity->setObjectName(QStringLiteral("hostHeroIdentity"));
    parts_.heroIdentity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(parts_.heroTitle);
    column->addWidget(parts_.heroDetail);
    column->addWidget(parts_.heroIdentity);
    row->addLayout(column, 1);
    parts_.startButton = new PrimaryButton(tr("Start sharing"));
    parts_.startButton->setBaseMinimumSize(140, 40);
    connect(parts_.startButton, &QPushButton::clicked, this, [this] { setSharing(true); });
    parts_.stopButton = new Button(tr("Stop sharing"));
    parts_.stopButton->setBaseMinimumSize(130, 40);
    connect(parts_.stopButton, &QPushButton::clicked, this, [this] { setSharing(false); });
    row->addWidget(parts_.startButton, 0, Qt::AlignVCenter);
    row->addWidget(parts_.stopButton, 0, Qt::AlignVCenter);
    page.addWidget(parts_.hero);
}

void HostDashboard::buildStats(QVBoxLayout& page) {
    parts_.stats = new QWidget;
    parts_.stats->setObjectName(QStringLiteral("hostStats"));
    statsGrid_ = new QGridLayout(parts_.stats);
    statsGrid_->setContentsMargins(0, 0, 0, 0);
    statsGrid_->setHorizontalSpacing(12);
    statsGrid_->setVerticalSpacing(12);
    parts_.statConnected = new StatTile(tr("Connected"), QStringLiteral("success"));
    parts_.statTranscriptions = new StatTile(tr("Transcriptions"), QStringLiteral("accent"));
    parts_.statAudio = new StatTile(tr("Audio transcribed"), QStringLiteral("purple"));
    parts_.statSpeed = new StatTile(tr("Speed"), QStringLiteral("warning"));
    arrangeStats(4);
    page.addWidget(parts_.stats);
}

void HostDashboard::arrangeStats(int columns) {
    if (columns == statColumns_) {
        return;
    }
    statColumns_ = columns;
    const std::array<StatTile*, 4> tiles = {parts_.statConnected, parts_.statTranscriptions,
                                            parts_.statAudio, parts_.statSpeed};
    for (auto* tile : tiles) {
        statsGrid_->removeWidget(tile);
    }
    for (int index = 0; index < 4; ++index) {
        statsGrid_->addWidget(tiles.at(static_cast<std::size_t>(index)), index / columns,
                              index % columns);
    }
    for (int column = 0; column < 4; ++column) {
        statsGrid_->setColumnStretch(column, column < columns ? 1 : 0);
    }
}

void HostDashboard::buildEngine(QVBoxLayout& page) {
    auto* card = parts_.engineCard = new Card(tr("Engine"));
    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);
    parts_.engineDot = new StatusDot(nullptr, 10);
    head->addWidget(parts_.engineDot, 0, Qt::AlignVCenter);
    parts_.engineName = new QLabel;
    parts_.engineName->setObjectName(QStringLiteral("hostEngineName"));
    head->addWidget(parts_.engineName, 1);
    card->body->addLayout(head);
    parts_.engineDetail = muted();
    card->body->addWidget(parts_.engineDetail);
    parts_.engineStatus = muted({}, "hostWarning");
    card->body->addWidget(parts_.engineStatus);
    auto* picker = new QHBoxLayout;
    picker->setContentsMargins(0, 4, 0, 0);
    picker->setSpacing(10);
    auto* serve = new QLabel(tr("Serve"));
    serve->setObjectName(QStringLiteral("hostFieldLabel"));
    picker->addWidget(serve);
    parts_.modelCombo = new NoWheelComboBox;
    parts_.modelCombo->setObjectName(QStringLiteral("hostModelCombo"));
    parts_.modelCombo->setMinimumHeight(36);
    parts_.modelCombo->setMinimumWidth(220);
    parts_.modelCombo->setToolTip(
        tr("The model paired computers use, from the ones ready on this computer"));
    connect(parts_.modelCombo, &QComboBox::activated, this, &HostDashboard::onModelActivated);
    picker->addWidget(parts_.modelCombo, 1);
    card->body->addLayout(picker);
    parts_.engineHint = muted();
    card->body->addWidget(parts_.engineHint);
    parts_.modelsLink = link(tr("Download models in Settings"), "hostModelsLink");
    connect(parts_.modelsLink, &QPushButton::clicked, this,
            [this] { Q_EMIT settingsRequested(q(host_text::MODELS_DESTINATION)); });
    card->body->addWidget(parts_.modelsLink, 0, Qt::AlignLeft);
    page.addWidget(card);
}

void HostDashboard::buildClients(QVBoxLayout& page) {
    auto* card = parts_.clientsCard = new Card(tr("Connected now"));
    parts_.clientsLayout = new QVBoxLayout;
    parts_.clientsLayout->setContentsMargins(0, 0, 0, 0);
    parts_.clientsLayout->setSpacing(8);
    card->body->addLayout(parts_.clientsLayout);
    parts_.clientsEmpty = muted();
    card->body->addWidget(parts_.clientsEmpty);
    page.addWidget(card);
}

void HostDashboard::buildDevices(QVBoxLayout& page) {
    auto* card = parts_.devicesCard = new Card(tr("Paired computers"));
    parts_.pairingBox = new QFrame;
    parts_.pairingBox->setObjectName(QStringLiteral("hostPairingBox"));
    auto* box = new QHBoxLayout(parts_.pairingBox);
    box->setContentsMargins(16, 12, 12, 12);
    box->setSpacing(14);
    parts_.pairingCode = new QLabel;
    parts_.pairingCode->setObjectName(QStringLiteral("hostPairingCode"));
    parts_.pairingCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->addWidget(parts_.pairingCode);
    parts_.pairingExpiry = muted();
    box->addWidget(parts_.pairingExpiry, 1);
    parts_.cancelPairingButton = new Button(tr("Cancel"));
    parts_.cancelPairingButton->setBaseMinimumSize(90, 36);
    connect(parts_.cancelPairingButton, &QPushButton::clicked, this,
            [this] { act([](auto& service) { service.cancel_pairing(); }); });
    box->addWidget(parts_.cancelPairingButton, 0, Qt::AlignVCenter);
    parts_.pairingBox->hide();
    card->body->addWidget(parts_.pairingBox);
    auto* pairRow = new QHBoxLayout;
    pairRow->setContentsMargins(0, 0, 0, 0);
    pairRow->setSpacing(12);
    parts_.pairButton = new PrimaryButton(tr("Pair a computer"));
    parts_.pairButton->setBaseMinimumSize(150, 38);
    connect(parts_.pairButton, &QPushButton::clicked, this,
            [this] { act([](auto& service) { (void)service.open_pairing(); }); });
    pairRow->addWidget(parts_.pairButton, 0, Qt::AlignVCenter);
    parts_.pairNote = muted();
    pairRow->addWidget(parts_.pairNote, 1);
    card->body->addLayout(pairRow);
    parts_.devicesLayout = new QVBoxLayout;
    parts_.devicesLayout->setContentsMargins(0, 0, 0, 0);
    parts_.devicesLayout->setSpacing(6);
    card->body->addLayout(parts_.devicesLayout);
    parts_.manageLink =
        link(tr("Manage sharing and paired computers in Settings"), "hostManageLink");
    connect(parts_.manageLink, &QPushButton::clicked, this,
            [this] { Q_EMIT settingsRequested(q(host_text::REMOTE_ENGINE_DESTINATION)); });
    card->body->addWidget(parts_.manageLink, 0, Qt::AlignLeft);
    page.addWidget(card);
}

void HostDashboard::buildMcp(QVBoxLayout& page) {
    auto* card = parts_.mcpCard = new Card(tr("MCP"));
    parts_.mcpIntro = muted();
    card->body->addWidget(parts_.mcpIntro);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    parts_.mcpDot = new StatusDot(nullptr, 10);
    row->addWidget(parts_.mcpDot, 0, Qt::AlignVCenter);
    parts_.mcpStatus = new WrappedLabel;
    parts_.mcpStatus->setObjectName(QStringLiteral("hostMcpStatus"));
    parts_.mcpStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(parts_.mcpStatus, 1);
    parts_.mcpButton = new Button(tr("Turn on"));
    parts_.mcpButton->setObjectName(QStringLiteral("hostCardButton"));
    parts_.mcpButton->setBaseMinimumSize(110, 36);
    connect(parts_.mcpButton, &QPushButton::clicked, this, &HostDashboard::toggleMcp);
    row->addWidget(parts_.mcpButton, 0, Qt::AlignVCenter);
    card->body->addLayout(row);
    parts_.mcpDetail = muted({}, "hostWarning");
    card->body->addWidget(parts_.mcpDetail);
    auto* actions = parts_.mcpActions = new QBoxLayout(QBoxLayout::TopToBottom);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(10);
    parts_.mcpLink = link(tr("Connect an agent in Settings"), "hostMcpLink");
    connect(parts_.mcpLink, &QPushButton::clicked, this,
            [this] { Q_EMIT settingsRequested(q(host_text::MCP_DESTINATION)); });
    actions->addWidget(parts_.mcpLink, 0, Qt::AlignLeft | Qt::AlignVCenter);
    parts_.mcpCopyToken = new Button(tr("Copy access token"));
    parts_.mcpCopyToken->setAccessibleName(tr("Copy MCP access token"));
    parts_.mcpCopyToken->setObjectName(QStringLiteral("hostCardButton"));
    parts_.mcpCopyToken->setBaseMinimumSize(110, 36);
    parts_.mcpCopyToken->setEnabled(false);
    connect(parts_.mcpCopyToken, &QPushButton::clicked, this, &HostDashboard::copyMcpAccessToken);
    actions->addWidget(parts_.mcpCopyToken, 0, Qt::AlignLeft | Qt::AlignVCenter);
    parts_.mcpCopyPrompt = new Button(tr("Copy agent install prompt"));
    parts_.mcpCopyPrompt->setAccessibleName(tr("Copy agent install prompt"));
    parts_.mcpCopyPrompt->setObjectName(QStringLiteral("hostCardButton"));
    parts_.mcpCopyPrompt->setBaseMinimumSize(110, 36);
    parts_.mcpCopyPrompt->setEnabled(false);
    connect(parts_.mcpCopyPrompt, &QPushButton::clicked, this, &HostDashboard::copyMcpAgentPrompt);
    actions->addWidget(parts_.mcpCopyPrompt, 0, Qt::AlignLeft | Qt::AlignVCenter);
    actions->addStretch(1);
    card->body->addLayout(actions);
    promptFeedback_ = new QTimer(this);
    promptFeedback_->setSingleShot(true);
    promptFeedback_->setInterval(kCopyFeedbackMs);
    connect(promptFeedback_, &QTimer::timeout, this,
            [this] { parts_.mcpCopyPrompt->setText(tr("Copy agent install prompt")); });
    tokenFeedback_ = new QTimer(this);
    tokenFeedback_->setSingleShot(true);
    tokenFeedback_->setInterval(kCopyFeedbackMs);
    connect(tokenFeedback_, &QTimer::timeout, this,
            [this] { parts_.mcpCopyToken->setText(tr("Copy access token")); });
    card->hide();
    page.addWidget(card);
}

void HostDashboard::buildActivity(QVBoxLayout& page) {
    auto* card = parts_.activityCard = new Card(tr("Recent activity"));
    parts_.activityLayout = new QVBoxLayout;
    parts_.activityLayout->setContentsMargins(0, 0, 0, 0);
    parts_.activityLayout->setSpacing(2);
    card->body->addLayout(parts_.activityLayout);
    parts_.activityEmpty =
        muted(tr("Nothing yet. Each request from a paired computer shows up here as it happens."));
    card->body->addWidget(parts_.activityEmpty);
    page.addWidget(card);
}

// ---- binding and refresh ----

void HostDashboard::bind(remote::RemoteEngineControl* service) {
    if (service == service_ && (service == nullptr || events_.connected())) {
        return;
    }
    events_ = {};
    service_ = service;
    if (service_ != nullptr) {
        QPointer<HostDashboard> guard(this);
        events_ = service_->changed.connect(ui_, [guard](const std::string& kind) {
            if (guard) {
                guard->onServiceEvent(kind);
            }
        });
    }
    // Read when shown: host_state() asks Tailscale, which a window that never shows this page
    // shouldn't do at every launch.
    schedule({"all", "models"});
}

void HostDashboard::bindMcp(agentapi::McpServerControl* server) {
    mcpBound_ = true;
    mcp_ = server;
    parts_.mcpCard->setVisible(server != nullptr);
    if (server != nullptr && isVisible()) {
        mcpTimer_->start();
    } else if (server == nullptr) {
        mcpTimer_->stop();
    }
    refreshMcp();
}

void HostDashboard::setMcpLookup(std::function<agentapi::McpServerControl*()> lookup) {
    mcpLookup_ = std::move(lookup);
}

void HostDashboard::setSettings(SettingsStore* settings) {
    settings_ = settings;
}

void HostDashboard::setClock(const Clock& clock) {
    clock_ = &clock;
}

void HostDashboard::setClipboardWriter(std::function<void(const QString&)> write) {
    writeClipboard_ = std::move(write);
}

void HostDashboard::setStopConfirmation(
    std::function<bool(const QString&, const QString&)> confirm) {
    confirmStop_ = std::move(confirm);
}

void HostDashboard::setClientHistorySource(remote::ClientHistorySource& source) {
    if (history_ != nullptr) {
        return;
    }
    history_ = new QPushButton(tr("Browse shared history"));
    history_->setObjectName(QStringLiteral("hostBrowseSharedHistory"));
    history_->setAutoDefault(false);
    connect(history_, &QPushButton::clicked, this,
            [this, &source] { openClientHistory(*this, source, io_, ui_); });
    parts_.devicesCard->body->insertWidget(parts_.devicesCard->body->indexOf(parts_.manageLink),
                                           history_, 0, Qt::AlignLeft);
}

void HostDashboard::refresh() {
    pending_.insert({"all", "models"});
    flushPending(true);
}

void HostDashboard::flush() {
    flushPending(true);
}

void HostDashboard::setEngineBusy(bool busy) {
    engineBusy_ = busy;
    if (!busy) {
        switching_.reset();
    }
    parts_.engineDot->setBusy(engineBusy_);
    schedule({"engine", "models"});
}

void HostDashboard::followEngineBusy(Signal<bool>& busy) {
    QPointer<HostDashboard> guard(this);
    engineBusyEvents_ = busy.connect(ui_, [guard](bool value) {
        if (guard) {
            guard->setEngineBusy(value);
        }
    });
}

void HostDashboard::setDeviceInfo(const QString&, bool) {
    schedule({"engine"});
}

void HostDashboard::showEngineError(const QString& message) {
    switching_.reset();
    engineError_ = message;
    renderEngine();
}

void HostDashboard::onServiceEvent(const std::string& kind) {
    if (kind == "activity" || kind == "clients") {
        schedule({"live"});
    } else if (kind == "engine") {
        schedule({"engine", "models", "live"});
    } else if (kind == "models" || kind == "components") {
        schedule({"models"});
    } else if (kind == "records") {
        // A paired computer stored a record: only the stored counts on the device rows change,
        // not the host's state or its engine.
        schedule({"devices"});
    } else {
        schedule({"all"});
    }
}

void HostDashboard::schedule(std::initializer_list<const char*> parts) {
    for (const char* part : parts) {
        pending_.insert(part);
    }
    if (isVisible() && !flushTimer_->isActive()) {
        flushTimer_->start();
    }
}

void HostDashboard::flushPending(bool force) {
    if (!force && !isVisible()) {
        return; // Kept pending until the page is shown.
    }
    if (service_ == nullptr) {
        pending_.clear();
        showUnbound();
        return;
    }
    if (reading_) {
        return; // Taken up when the read in flight comes back.
    }
    auto pending = std::exchange(pending_, {});
    if (pending.empty()) {
        return;
    }
    Read wanted;
    wanted.all = pending.contains("all");
    const bool host = wanted.all || (pending.contains("devices") && !state_);
    const bool devices = !host && pending.contains("devices");
    const bool engine = wanted.all || pending.contains("engine");
    const bool live = wanted.all || pending.contains("live");
    if (pending.contains("models")) {
        loadModels();
    }
    if (wanted.all) {
        if (!mcpBound_) {
            bindMcp(mcpLookup_ ? mcpLookup_() : nullptr);
        } else {
            refreshMcp();
        }
    }
    if (!host && !devices && !engine && !live) {
        return;
    }
    std::vector<std::string> known;
    if (devices) {
        for (const auto& device : state_->devices) {
            known.push_back(device.id);
        }
    }
    reading_ = true;
    QPointer<HostDashboard> guard(this);
    io_.post([guard, service = service_, ui = &ui_, read = std::move(wanted), host, devices, engine,
              live, known = std::move(known)]() mutable {
        const auto counted = [service](const std::vector<std::string>& ids) {
            std::map<std::string, Json> stored;
            for (const auto& id : ids) {
                if (id.empty()) {
                    continue;
                }
                try {
                    stored[id] = service->records_summary(id);
                } catch (const std::exception& error) {
                    kLog.debug("Could not count a paired computer's records: {}", error.what());
                }
            }
            return stored;
        };
        try {
            if (host) {
                read.state = service->host_state();
                std::vector<std::string> ids;
                for (const auto& device : read.state->devices) {
                    ids.push_back(device.id);
                }
                read.stored = counted(ids);
            } else if (devices) {
                read.stored = counted(known);
            }
            if (engine) {
                read.engine = service->engine_state();
            }
            if (live) {
                read.clients = service->connected_clients();
                read.activity = service->host_activity(host_text::ACTIVITY_ROWS);
            }
        } catch (const std::exception& error) {
            kLog.warning("Host dashboard refresh failed: {}", error.what());
        }
        ui->post([guard, read = std::move(read)]() mutable {
            if (guard) {
                guard->apply(std::move(read));
            }
        });
    });
}

void HostDashboard::apply(Read read) {
    reading_ = false;
    if (read.state) {
        state_ = std::move(*read.state);
        for (auto* card :
             {parts_.engineCard, parts_.clientsCard, parts_.devicesCard, parts_.activityCard}) {
            card->setEnabled(true);
        }
        renderHero(*state_);
        renderPairing(*state_);
    }
    if (read.stored) {
        stored_ = std::move(*read.stored);
    }
    if (state_ && (read.state || read.stored)) {
        renderDevices(*state_, stored_);
    }
    if (read.engine) {
        engine_ = read.engine->is_object() ? std::move(*read.engine) : Json::object();
        if (switching_) {
            const auto& [label, family, model] = *switching_;
            if (jstr(engine_, "family") == family && jstr(engine_, "model") == model &&
                flag(engine_, "available") && !engineBusy_) {
                switching_.reset();
            }
        }
        renderEngine();
    }
    if (read.clients || read.activity) {
        refreshLive(read.clients.value_or(std::vector<remote::ConnectedClient>{}),
                    read.activity.value_or(Json::object()));
    }
    if (read.all && mcpBound_) {
        refreshMcp();
    }
    if (!pending_.empty()) {
        if (isVisible()) {
            flushTimer_->start();
        }
    }
}

void HostDashboard::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pending_.insert({"all", "models"});
    flushPending(false);
    clockTimer_->start();
    if (mcp_ != nullptr) {
        mcpTimer_->start();
    }
}

void HostDashboard::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    flushTimer_->stop();
    clockTimer_->stop();
    countdown_->stop();
    mcpTimer_->stop();
}

void HostDashboard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrangeStats(event->size().width() >= host_text::FOUR_STATS_WIDTH ? 4 : 2);
    arrangeMcpActions(event->size().width());
}

void HostDashboard::arrangeMcpActions(int width) {
    const auto outer = page_->contentsMargins();
    const auto card = parts_.mcpCard->body->contentsMargins();
    const int available = width - outer.left() - outer.right() - card.left() - card.right() -
                          parts_.scroll->verticalScrollBar()->sizeHint().width();
    int needed = parts_.mcpLink->sizeHint().width() + parts_.mcpActions->spacing() * 2;
    const std::array<std::pair<Button*, QString>, 2> buttons = {
        std::pair{parts_.mcpCopyToken, tr("Copy access token")},
        std::pair{parts_.mcpCopyPrompt, tr("Copy agent install prompt")}};
    for (const auto& [button, label] : buttons) {
        needed += std::max({button->minimumWidth(), button->sizeHint().width(),
                            button->fontMetrics().horizontalAdvance(label) + 40});
    }
    parts_.mcpActions->setDirection(available >= needed ? QBoxLayout::LeftToRight
                                                        : QBoxLayout::TopToBottom);
}

void HostDashboard::showUnbound() {
    ui::setStyleProperty(parts_.hero, ui::prop::state, QStringLiteral("off"));
    parts_.beacon->setState(QStringLiteral("off"));
    parts_.heroTitle->setText(tr("Host Mode isn't available here"));
    parts_.heroDetail->setText(tr("The remote engine hasn't started in this window."));
    parts_.heroDetail->show();
    parts_.heroIdentity->hide();
    parts_.startButton->hide();
    parts_.stopButton->hide();
    for (auto* card :
         {parts_.engineCard, parts_.clientsCard, parts_.devicesCard, parts_.activityCard}) {
        card->setEnabled(false);
    }
}

// ---- sharing ----

QString HostDashboard::where(const remote::HostState& state) const {
    const QString host = state.host_name.empty() ? tr("This computer") : q(state.host_name);
    QStringList lines;
    if (state.address && !state.address->empty()) {
        QString line = tr("%1 at %2").arg(host, q(*state.address));
        if (state.address_kind == std::optional<std::string>("vpn")) {
            line += tr(" (a VPN address; computers on your network may not reach it)");
        }
        lines.append(line);
    } else {
        lines.append(tr("%1 on port %2").arg(host).arg(state.port));
    }
    if (const auto& tailnet = state.tailscale;
        tailnet && tailnet->running() && !tailnet->address.empty()) {
        lines.append(tr("Tailscale: %1 at %2")
                         .arg(q(tailnet->name.empty() ? tailnet->address : tailnet->name),
                              q(tailnet->address)));
    }
    return lines.join(QLatin1Char('\n'));
}

void HostDashboard::renderHero(const remote::HostState& state) {
    QString tone, title, detail, identity;
    if (!state.error.empty()) {
        tone = QStringLiteral("error");
        title = tr("Sharing couldn't start");
        detail = q(state.error);
    } else if (state.running) {
        tone = QStringLiteral("on");
        const auto label = jstr(state.engine, "label");
        title = flag(state.engine, "available") && !label.isEmpty()
                    ? tr("Sharing %1").arg(label)
                    : tr("Sharing, but no engine is ready");
        detail = where(state);
        if (!state.fingerprint.empty()) {
            identity = tr("Identity %1").arg(q(remote::short_fingerprint(state.fingerprint)));
        }
    } else if (state.enabled) {
        tone = QStringLiteral("starting");
        title = tr("Starting to share…");
    } else {
        tone = QStringLiteral("off");
        title = tr("Sharing is off");
        detail = tr("Start sharing so computers you pair can dictate and record meetings with "
                    "this computer's engine.");
    }
    ui::setStyleProperty(parts_.hero, ui::prop::state, tone);
    parts_.beacon->setState(tone);
    parts_.heroTitle->setText(title);
    parts_.heroDetail->setText(detail);
    parts_.heroDetail->setVisible(!detail.isEmpty());
    parts_.heroIdentity->setText(identity);
    parts_.heroIdentity->setToolTip(
        identity.isEmpty() ? QString()
                           : tr("A computer that pairs with this one shows the same identity."));
    parts_.heroIdentity->setVisible(!identity.isEmpty());
    parts_.startButton->setVisible(!state.enabled);
    parts_.stopButton->setVisible(state.enabled);
    parts_.stopButton->setText(state.error.empty() ? tr("Stop sharing") : tr("Turn off sharing"));
}

void HostDashboard::setSharing(bool enabled) {
    if (service_ == nullptr) {
        return;
    }
    if (!enabled && !clients_.empty()) {
        std::vector<std::string> names;
        for (const auto& client : host_text::group_clients(clients_)) {
            names.push_back(client.name);
        }
        std::ranges::sort(names);
        names.erase(std::ranges::unique(names).begin(), names.end());
        const QString who = names.size() == 1 ? tr("%1 is connected.").arg(q(names.front()))
                                              : tr("%1 computers are connected.").arg(names.size());
        if (!confirmStop_ ||
            !confirmStop_(who, tr("Stopping cuts them off until you share again."))) {
            return;
        }
    }
    // Stopping waits for connected computers' requests; the page reads everything again once
    // the change is made.
    act([enabled](auto& service) { service.set_host_enabled(enabled); });
}

void HostDashboard::act(std::function<void(remote::RemoteEngineControl&)> task) {
    if (service_ == nullptr || changing_) {
        return;
    }
    changing_ = true;
    QPointer<HostDashboard> guard(this);
    io_.post([guard, service = service_, ui = &ui_, task = std::move(task)] {
        try {
            task(*service);
        } catch (const std::exception& error) {
            kLog.warning("Host Mode couldn't change sharing: {}", error.what());
        }
        ui->post([guard] {
            if (guard) {
                guard->changing_ = false;
                guard->refresh();
            }
        });
    });
}

// ---- pairing and paired computers ----

void HostDashboard::renderPairing(const remote::HostState& state) {
    const bool running = state.running;
    if (running && state.pairing) {
        pairingCode_ = q(state.pairing->first);
        const double left = std::isfinite(state.pairing->second) ? state.pairing->second : 0.0;
        pairingDeadline_ =
            (clock_ != nullptr ? clock_->steady() : std::chrono::steady_clock::now()) +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(std::max(0.0, left)));
    } else {
        pairingCode_.clear();
    }
    showPairing();
    std::string owner;
    if (state.tailscale && state.tailscale->running()) {
        owner = state.tailscale->owner;
    }
    QString note;
    if (!running) {
        note = tr("Start sharing to pair a computer.");
    } else if (!owner.empty() && state.tailscale_trust) {
        note = tr("Computers signed in to Tailscale as %1 can pair by picking this one from their "
                  "tailnet, without a code.")
                   .arg(q(owner));
    } else {
        note = tr("On the other computer, open Settings → Remote engine and pick this one; you're "
                  "asked here to allow it. Or enter the code shown here.");
    }
    parts_.pairNote->setText(note);
    parts_.pairButton->setEnabled(running);
    // A code only works while sharing; the note says so instead.
    parts_.pairButton->setVisible(running && pairingCode_.isEmpty());
}

void HostDashboard::showPairing() {
    const auto now = clock_ != nullptr ? clock_->steady() : std::chrono::steady_clock::now();
    const auto left = std::chrono::duration<double>(pairingDeadline_ - now).count();
    if (pairingCode_.isEmpty() || left <= 0) {
        const bool hadCode = !pairingCode_.isEmpty();
        pairingCode_.clear();
        parts_.pairingBox->hide();
        parts_.pairButton->setVisible(state_ && state_->running);
        countdown_->stop();
        if (hadCode) {
            schedule({"all"}); // Expired: the host closed it too.
        }
        return;
    }
    parts_.pairingCode->setText(pairingCode_.left(3) + QLatin1Char(' ') + pairingCode_.mid(3));
    const auto whole = static_cast<int>(left);
    parts_.pairingExpiry->setText(tr("Enter this code on the other computer. Expires in %1:%2.")
                                      .arg(whole / 60)
                                      .arg(whole % 60, 2, 10, QLatin1Char('0')));
    parts_.pairingBox->show();
    parts_.pairButton->hide();
    if (!countdown_->isActive()) {
        countdown_->start();
    }
}

void HostDashboard::renderDevices(const remote::HostState& state,
                                  const std::map<std::string, Json>& stored) {
    clearLayout(*parts_.devicesLayout);
    deviceRows_.clear();
    const auto& devices = state.devices;
    parts_.devicesCard->setBadge(devices.empty() ? QString() : QString::number(devices.size()));
    if (devices.empty()) {
        parts_.devicesLayout->addWidget(muted(tr("No computers are paired yet.")));
        return;
    }
    for (const auto& device : devices) {
        auto* row = new QFrame;
        row->setObjectName(QStringLiteral("hostDeviceRow"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(10);
        auto* column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(1);
        auto* name =
            new ElidingLabel(device.name.empty() ? tr("Unnamed computer") : q(device.name));
        name->setObjectName(QStringLiteral("hostDeviceName"));
        column->addWidget(name);
        QStringList details;
        if (const auto paired = remote_text::paired_on(device.paired_at, zone()); !paired.empty()) {
            details.append(
                tr("Paired %1%2")
                    .arg(q(paired), device.via == "tailscale" ? tr(" over Tailscale") : QString()));
        }
        // Counted even while keeping records is off: what's kept stays.
        if (const auto found = stored.find(device.id); found != stored.end()) {
            std::string phrase;
            try {
                phrase = remote_text::stored_phrase(found->second);
            } catch (const std::exception& error) {
                kLog.debug("Could not count a paired computer's records: {}", error.what());
            }
            if (!phrase.empty()) {
                details.append(tr("keeps %1 here").arg(q(phrase)));
            }
        }
        auto* detail = new ElidingLabel(details.join(QStringLiteral(" · ")));
        detail->setObjectName(QStringLiteral("hostDeviceDetail"));
        detail->setVisible(!details.isEmpty());
        column->addWidget(detail);
        layout->addLayout(column, 1);
        auto* status = new QLabel;
        status->setObjectName(QStringLiteral("hostDeviceStatus"));
        layout->addWidget(status, 0, Qt::AlignVCenter);
        parts_.devicesLayout->addWidget(row);
        deviceRows_[device.id] = {row, status, detail, device};
    }
    updateDeviceStatus();
}

void HostDashboard::updateDeviceStatus() {
    std::set<std::string> connected;
    for (const auto& client : clients_) {
        connected.insert(client.device_id);
    }
    const double moment = now();
    for (auto& [id, row] : deviceRows_) {
        if (connected.contains(id)) {
            row.status->setText(tr("Connected"));
            ui::setStyleProperty(row.status, ui::prop::active, true);
            continue;
        }
        ui::setStyleProperty(row.status, ui::prop::active, false);
        const auto seen = host_text::iso_ago(row.device.last_seen, moment, zone());
        row.status->setText(seen.empty() ? QString() : tr("Seen %1").arg(q(seen)));
    }
}

// ---- engine ----

void HostDashboard::renderEngine() {
    const Json& engine = engine_;
    const QString label = jstr(engine, "label");
    const bool available = flag(engine, "available");
    parts_.engineName->setText(label.isEmpty() ? tr("No local engine selected") : label);
    if (available) {
        parts_.engineDot->setStatus(EngineStatus::Ready);
    } else {
        parts_.engineDot->setStatus(!label.isEmpty() || !engine.empty() ? EngineStatus::Attention
                                                                        : EngineStatus::Unknown);
    }
    parts_.engineDot->setBusy(engineBusy_);
    QStringList details;
    if (const auto device = jstr(engine, "device"); !device.isEmpty()) {
        QString line = tr("On %1").arg(q(host_text::device_label(utf8(device))));
        if (const auto compute = jstr(engine, "compute_type"); !compute.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(compute);
        }
        details.append(line);
    }
    if (!label.isEmpty()) {
        details.append(flag(engine, "streaming") ? tr("live preview supported")
                                                 : tr("no live preview"));
    }
    if (switching_) {
        details = QStringList{tr("Switching to %1…").arg(std::get<0>(*switching_))};
    } else if (engineBusy_) {
        details = QStringList{tr("Loading…")};
    }
    parts_.engineDetail->setText(details.join(QStringLiteral(" · ")));
    parts_.engineDetail->setVisible(!details.isEmpty());
    QString status;
    if (!engineError_.isEmpty()) {
        status = engineError_;
    } else if (!available && !engineBusy_ && !switching_) {
        status = jstr(engine, "status");
    }
    parts_.engineStatus->setText(status);
    parts_.engineStatus->setVisible(!status.isEmpty());
    syncModelCombo();
}

void HostDashboard::loadModels() {
    if (service_ == nullptr) {
        return;
    }
    const unsigned token = ++modelsToken_;
    QPointer<HostDashboard> guard(this);
    // The listing holds no page: it hands the models back through the ui executor and a guard.
    io_.post([guard, service = service_, ui = &ui_, token] {
        std::vector<remote::HostModel> models;
        try {
            models = service->host_models();
        } catch (const std::exception& error) {
            kLog.warning("Could not list this computer's ready models: {}", error.what());
        }
        ui->post([guard, token, models = std::move(models)]() mutable {
            if (!guard || token != guard->modelsToken_) {
                return; // A newer listing is on its way.
            }
            guard->models_ = std::move(models);
            guard->syncModelCombo();
        });
    });
}

void HostDashboard::syncModelCombo() {
    auto* combo = parts_.modelCombo;
    const QString family = jstr(engine_, "family");
    const QString model = jstr(engine_, "model");
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    int selected = -1;
    if (models_) {
        for (const auto& entry : *models_) {
            combo->addItem(q(entry.label.empty() ? entry.model : entry.label),
                           QStringList{q(entry.family), q(entry.model)});
            if (q(entry.family) == family && q(entry.model) == model) {
                selected = combo->count() - 1;
            }
        }
    }
    if (selected < 0) {
        const QString placeholder = !models_           ? tr("Looking for models…")
                                    : models_->empty() ? tr("No models are ready")
                                                       : tr("Choose a model to serve");
        combo->insertItem(0, placeholder, QVariant());
        selected = 0;
    }
    combo->setCurrentIndex(selected);
    combo->blockSignals(blocked);
    const bool ready = models_ && !models_->empty();
    combo->setEnabled(ready && !engineBusy_ && !switching_);
    if (models_ && models_->empty()) {
        parts_.engineHint->setText(tr("Download a speech model and its runtime here first."));
    } else if (ready) {
        parts_.engineHint->setText(
            tr("Paired computers use this model, and can switch to any of these themselves."));
    } else {
        parts_.engineHint->setText({});
    }
    parts_.engineHint->setVisible(!parts_.engineHint->text().isEmpty());
    parts_.modelsLink->setVisible(models_ && models_->empty());
}

void HostDashboard::onModelActivated(int index) {
    const auto choice = parts_.modelCombo->itemData(index).toStringList();
    if (choice.size() != 2) {
        return;
    }
    if (jstr(engine_, "family") == choice[0] && jstr(engine_, "model") == choice[1] &&
        flag(engine_, "available")) {
        return;
    }
    switching_ = std::tuple(parts_.modelCombo->itemText(index), choice[0], choice[1]);
    engineError_.clear();
    renderEngine();
    Q_EMIT modelRequested(choice[0], choice[1]);
}

// ---- MCP: agents on this computer searching what it keeps ----

void HostDashboard::refreshMcp() {
    auto* server = mcp_;
    const auto disable = [this] {
        parts_.mcpCopyPrompt->setEnabled(false);
        parts_.mcpCopyToken->setEnabled(false);
    };
    if (server == nullptr) {
        disable();
        return;
    }
    agentapi::ServerStatus status;
    try {
        status = server->status();
    } catch (const std::exception& error) {
        kLog.debug("Could not read the MCP server's status: {}", error.what());
        disable();
        return;
    }
    const bool running = status.state == "running";
    parts_.mcpCopyPrompt->setEnabled(running);
    parts_.mcpCopyToken->setEnabled(running);
    QString intro = (status.remote_url.empty() ? tr("Agents on this computer")
                                               : tr("Agents on this computer and over Tailscale")) +
                    tr(", such as Claude Code or Cursor, can search the dictations and meetings "
                       "saved here. Choose permissions for title and settings changes in "
                       "Settings → MCP.");
    if (state_ && state_->keep_records) {
        intro += tr(" That includes the records paired computers keep here.");
    }
    parts_.mcpIntro->setText(intro);
    QString detail = mcpNotice_;
    QString line;
    auto dot = EngineStatus::Unknown;
    if (running) {
        line = tr("Running at %1")
                   .arg(q(status.remote_url.empty() ? status.url() : status.remote_url));
        dot = EngineStatus::Ready;
        if (status.remote_url.empty() && detail.isEmpty()) {
            detail = tr("For an agent on another computer, enable Allow agents over Tailscale in "
                        "Settings → MCP.");
        }
    } else if (status.state == "starting") {
        line = tr("Starting…");
    } else if (status.state == "stopping") {
        line = tr("Stopping…");
    } else if (status.state == "error") {
        line = tr("Couldn't start");
        dot = EngineStatus::Attention;
        if (detail.isEmpty()) {
            detail = q(status.message);
        }
    } else {
        line = tr("Off");
    }
    parts_.mcpStatus->setText(line);
    parts_.mcpDot->setStatus(dot);
    parts_.mcpDot->setBusy(status.state == "starting" || status.state == "stopping");
    parts_.mcpDetail->setText(detail);
    parts_.mcpDetail->setVisible(!detail.isEmpty());
    parts_.mcpButton->setText(status.state == "error"     ? tr("Try again")
                              : status.state == "stopped" ? tr("Turn on")
                                                          : tr("Turn off"));
    parts_.mcpButton->setEnabled(status.state != "stopping");
}

void HostDashboard::toggleMcp() {
    auto* server = mcp_;
    if (server == nullptr) {
        return;
    }
    bool turnOn = false;
    try {
        const auto state = server->status().state;
        turnOn = state == "stopped" || state == "error";
    } catch (const std::exception& error) {
        kLog.warning("Could not read the MCP server's status: {}", error.what());
        return;
    }
    try {
        // The same setting Settings → MCP saves; the server starts with the app while it is on.
        // The session-only control of the running app keeps no such setting.
        if (settings_ != nullptr && !server->session_only()) {
            settings_->save_setting(keys::mcp_enabled.key(), turnOn);
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not save the MCP setting: {}", error.what());
        mcpNotice_ = tr("Couldn't save the MCP setting. Try again.");
        refreshMcp();
        return;
    }
    mcpNotice_.clear();
    if (turnOn) {
        // Starts on the saved port, as at launch.
        int port = agentapi::DEFAULT_PORT;
        bool tailscale = false;
        if (settings_ != nullptr) {
            const auto snapshot = settings_->snapshot();
            port = keys::mcp_port.read(*snapshot);
            tailscale = !server->session_only() && keys::mcp_tailscale_enabled.read(*snapshot);
        }
        try {
            server->start(port, tailscale);
        } catch (const std::exception& error) {
            kLog.warning("Could not start MCP: {}", error.what());
            mcpNotice_ = q(error.what());
        }
    } else {
        server->stop();
    }
    refreshMcp();
}

void HostDashboard::copyMcpAccessToken() {
    auto* server = mcp_;
    if (server == nullptr) {
        return;
    }
    std::string token;
    try {
        if (server->status().state == "running") {
            token = server->token();
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not read the MCP access token: {}", error.what());
        parts_.mcpCopyToken->setEnabled(false);
        return;
    }
    if (token.empty()) {
        refreshMcp();
        return;
    }
    if (writeClipboard_) {
        writeClipboard_(q(token));
    }
    parts_.mcpCopyToken->setText(tr("Copied"));
    tokenFeedback_->start();
}

void HostDashboard::copyMcpAgentPrompt() {
    auto* server = mcp_;
    if (server == nullptr) {
        return;
    }
    agentapi::ServerStatus status;
    std::string token;
    try {
        status = server->status();
        if (status.state == "running") {
            token = server->token();
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not read the MCP server's status: {}", error.what());
        parts_.mcpCopyPrompt->setEnabled(false);
        return;
    }
    if (status.state != "running") {
        refreshMcp();
        return;
    }
    const auto url = status.remote_url.empty() ? status.url() : status.remote_url;
    if (writeClipboard_) {
        writeClipboard_(q(agentapi::agent_prompt(url, std::nullopt, token)));
    }
    parts_.mcpCopyPrompt->setText(tr("Copied"));
    promptFeedback_->start();
}

// ---- live: who is connected, and what they asked for ----

void HostDashboard::refreshLive(std::vector<remote::ConnectedClient> clients, Json activity) {
    clients_ = std::move(clients);
    if (!activity.is_object()) {
        activity = Json::object();
    }
    const auto grouped = host_text::group_clients(clients_);
    const bool running = state_ && state_->running;
    renderStats(grouped, activity, running);
    renderClients(grouped, activity, now(), running);
    renderActivity(activity);
    updateDeviceStatus();
    parts_.beacon->setBusy(std::ranges::any_of(grouped, &host_text::GroupedClient::busy));
    const auto transcriptions = static_cast<long long>(number(activity, "transcriptions"));
    if (seenTranscriptions_ && transcriptions > *seenTranscriptions_) {
        parts_.beacon->ripple();
    }
    seenTranscriptions_ = transcriptions;
}

void HostDashboard::renderStats(const std::vector<host_text::GroupedClient>& grouped,
                                const Json& activity, bool running) {
    if (!grouped.empty()) {
        QStringList names;
        for (const auto& client : grouped) {
            names.append(q(client.name));
        }
        parts_.statConnected->set(QString::number(grouped.size()),
                                  names.join(QStringLiteral(", ")));
    } else {
        parts_.statConnected->set(QStringLiteral("0"),
                                  running ? tr("none right now") : tr("sharing is off"));
    }
    const auto transcriptions = static_cast<long long>(number(activity, "transcriptions"));
    const double since = number(activity, "since");
    QString detail = since != 0.0 && running
                         ? tr("since %1").arg(q(host_text::clock(since, zone())))
                     : transcriptions != 0 ? tr("last session")
                                           : QString();
    if (const auto errors = static_cast<long long>(number(activity, "errors")); errors != 0) {
        detail = detail.isEmpty() ? tr("%1 failed").arg(errors)
                                  : tr("%1 · %2 failed").arg(detail).arg(errors);
    }
    parts_.statTranscriptions->set(QString::number(transcriptions), detail);
    const double audio = number(activity, "audio_s");
    const double host = number(activity, "host_s");
    if (transcriptions != 0) {
        const auto each = static_cast<double>(transcriptions);
        parts_.statAudio->set(q(host_text::span(audio)),
                              tr("about %1 each").arg(q(host_text::span(audio / each))));
        const auto speed = q(host_text::speed(audio, host));
        parts_.statSpeed->set(speed, tr("%1 per request").arg(q(host_text::quick(host / each))));
        auto ratio = speed;
        while (ratio.endsWith(QChar(0x00D7))) {
            ratio.chop(1);
        }
        parts_.statSpeed->value->setToolTip(
            tr("Each second of decoding here covered %1 seconds of audio.").arg(ratio));
    } else {
        parts_.statAudio->set(QStringLiteral("0 s"));
        parts_.statSpeed->set(QStringLiteral("—"), tr("no requests yet"));
        parts_.statSpeed->value->setToolTip({});
    }
}

void HostDashboard::renderClients(const std::vector<host_text::GroupedClient>& grouped,
                                  const Json& activity, double moment, bool running) {
    const Json& devices = member(activity, "devices");
    std::set<std::string> seen;
    for (int index = 0; index < static_cast<int>(grouped.size()); ++index) {
        const auto& client = grouped.at(static_cast<std::size_t>(index));
        seen.insert(client.device_id);
        auto*& row = clientRows_[client.device_id];
        if (row == nullptr) {
            row = new ClientRow;
        }
        if (parts_.clientsLayout->indexOf(row) != index) {
            parts_.clientsLayout->insertWidget(index, row);
        }
        row->showClient(client, member(devices, client.device_id.c_str()), moment, zone());
        if (!row->isVisibleTo(this) && isVisible()) {
            row->show();
        }
    }
    for (auto it = clientRows_.begin(); it != clientRows_.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        parts_.clientsLayout->removeWidget(it->second);
        it->second->hide();
        it->second->deleteLater();
        it = clientRows_.erase(it);
    }
    parts_.clientsCard->setBadge(grouped.empty() ? QString() : QString::number(grouped.size()));
    if (!grouped.empty()) {
        parts_.clientsEmpty->hide();
    } else {
        parts_.clientsEmpty->setText(
            running ? tr("No computers are connected right now. Paired computers connect when "
                         "they dictate or record a meeting with this engine.")
                    : tr("Nobody can connect while sharing is off."));
        parts_.clientsEmpty->show();
    }
}

void HostDashboard::renderActivity(const Json& activity) {
    const std::pair key{number(activity, "since"),
                        static_cast<long long>(number(activity, "serial"))};
    if (seenActivity_ == key) {
        return;
    }
    const bool first = !seenActivity_;
    const long long lastSeen = seenActivity_ ? seenActivity_->second : 0;
    seenActivity_ = key;
    clearLayout(*parts_.activityLayout);
    const Json& events = member(activity, "events");
    int shown = 0;
    if (events.is_array()) {
        for (const auto& event : events) {
            if (shown == host_text::ACTIVITY_ROWS) {
                break;
            }
            const bool fresh = !first && static_cast<long long>(number(event, "serial")) > lastSeen;
            parts_.activityLayout->addWidget(new EventRow(event, fresh, now(), zone()));
            ++shown;
        }
    }
    parts_.activityEmpty->setVisible(shown == 0);
}

double HostDashboard::now() const {
    const auto utc = clock_ != nullptr ? clock_->utc() : std::chrono::system_clock::now();
    return std::chrono::duration<double>(utc.time_since_epoch()).count();
}

const std::chrono::time_zone& HostDashboard::zone() const {
    if (clock_ != nullptr && clock_->zone() != nullptr) {
        return *clock_->zone();
    }
    return *std::chrono::current_zone();
}

} // namespace fv
