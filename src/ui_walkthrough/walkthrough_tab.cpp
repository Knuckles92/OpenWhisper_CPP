#include "ui_walkthrough/walkthrough_tab.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "agents/installed_agents.h"
#include "appkit/destinations.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_snap/capture_delay.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_walkthrough/detail/mailbox.h"
#include "ui_walkthrough/new_style_dialog.h"
#include "ui_walkthrough/review_text.h"
#include "ui_walkthrough/walkthrough_copy_tips.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/agent_picker_text.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/feature_badge.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"
#include "walkthrough/write_up_names.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.walkthrough_tab"};

using namespace walkthrough_tab;
namespace review_text = walkthrough_review;

// The New style… item's data: not a style id (hex or a slug) and not "", the user's own.
const QString kNewStyleData = QStringLiteral(":new");

// The context menu's look, opaque: these open over the form and the storyboard, which would
// show through.
const QString kMenuStyleSheet = QStringLiteral(R"(
    QMenu {
        background-color: @surface;
        color: @text;
        border: 1px solid rgba(@overlay-rgb, 0.1);
        border-radius: 10px;
        padding: 6px;
    }
    QMenu::item {
        padding: 8px 28px 8px 14px;
        border-radius: 6px;
        font-size: 13px;
    }
    QMenu::item:selected {
        background-color: @accent;
        color: @on-accent;
    }
    QMenu::separator {
        background-color: rgba(@overlay-rgb, 0.08);
        height: 1px;
        margin: 4px 8px;
    }
    QMenu::item:disabled {
        color: @text-secondary;
    }
)");

QString qs(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString qpath(const std::filesystem::path& path) {
    return qs(path_to_utf8(path));
}

std::string utf8(const QString& value) {
    return value.toStdString();
}

// str.strip() of a widget's text.
std::string stripped(const QString& text) {
    return py::strip(utf8(text));
}

QString tabSheet() {
    QFile file(QStringLiteral(":/ui_walkthrough/walkthrough_tab.qss"));
    if (!file.open(QIODevice::ReadOnly)) {
        kLog.warning("The Walkthrough tab's stylesheet is missing from the resources");
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QLabel* label(const QString& text, const QString& name, bool wrap = false) {
    auto* made = new QLabel(text);
    made->setObjectName(name);
    made->setWordWrap(wrap);
    if (name == QStringLiteral("walkthroughOverline")) {
        QFont font = made->font();
        font.setLetterSpacing(QFont::PercentageSpacing, 108);
        made->setFont(font);
    }
    return made;
}

QWidget* plain() {
    auto* widget = new QWidget();
    widget->setObjectName(QStringLiteral("walkthroughPlain"));
    return widget;
}

QFrame* rule() {
    auto* made = new QFrame();
    made->setObjectName(QStringLiteral("walkthroughRule"));
    made->setFixedHeight(1);
    return made;
}

QLabel* keyChip(const QString& text) {
    auto* chip = new QLabel(text);
    chip->setObjectName(QStringLiteral("walkthroughKey"));
    chip->setAlignment(Qt::AlignCenter);
    return chip;
}

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QFrame* pane() {
    auto* frame = new QFrame();
    frame->setObjectName(QStringLiteral("walkthroughStagePane"));
    return frame;
}

// The agents a walkthrough can be written up by, in the order the menu lists them (AGENTS).
std::vector<std::string> writeUpAgents() {
    std::vector<std::string> agents;
    for (const Json& agent : enums::installed(enums::EnumId::MeetingAgentCore)) {
        agents.push_back(agent.get<std::string>());
    }
    return agents;
}

QString agentName(const std::string& agent) {
    return qs(agents::agent_spec(agent).name);
}

} // namespace

namespace walkthrough_tab {

bool offersAutoMode(std::string_view platform) {
    return platform == "win32" || platform == "darwin";
}

std::string_view currentPlatform() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

} // namespace walkthrough_tab

// ---- Field ----

WalkthroughField::WalkthroughField(MenuBuilder buildMenu, const QString& name)
    : buildMenu_(std::move(buildMenu)), name_(name) {
    setObjectName(QStringLiteral("walkthroughField"));
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, [this] { open(); });
}

void WalkthroughField::setValue(const QString& text, const QString& tip, const QString& tone) {
    value_ = text;
    elide();
    setToolTip(!tip.isEmpty() ? tip : (this->text() != text ? text : QString()));
    setAccessibleName(name_ + QStringLiteral(": ") + text);
    if (tone_ != tone) {
        tone_ = tone;
        ui::setStyleProperty(this, ui::prop::tone, tone);
        restyle(this);
    }
}

void WalkthroughField::elide() {
    // 12 padding on the left, and room for the chevron on the right.
    setText(fontMetrics().elidedText(value_, Qt::ElideRight, std::max(0, width() - 42)));
}

void WalkthroughField::resizeEvent(QResizeEvent* event) {
    QPushButton::resizeEvent(event);
    elide();
}

void WalkthroughField::open() {
    QMenu* menu = buildMenu();
    menu->setMinimumWidth(std::max(menu->sizeHint().width(), width()));
    menu->exec(mapToGlobal(QPoint(0, height() + 4)));
    menu->deleteLater();
}

void WalkthroughField::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(isEnabled() ? 1.0 : 0.4);
    painter.setPen(QPen(ui::tokenColor("text-secondary"), 1.4, Qt::SolidLine, Qt::RoundCap));
    const double x = width() - 17;
    const double y = height() / 2.0;
    // Python's int() of each end: truncation, all of them positive.
    painter.drawLine(static_cast<int>(x - 3), static_cast<int>(y - 2), static_cast<int>(x),
                     static_cast<int>(y + 1));
    painter.drawLine(static_cast<int>(x), static_cast<int>(y + 1), static_cast<int>(x + 3),
                     static_cast<int>(y - 2));
}

// ---- RecentRow ----

WalkthroughRecentRow::WalkthroughRecentRow(const past_walkthroughs::Summary& summary,
                                           std::chrono::year_month_day today, QWidget* parent)
    : QFrame(parent), folder_(summary.folder) {
    setObjectName(QStringLiteral("walkthroughRecent"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 8, 12, 8);
    row->setSpacing(12);
    auto* thumb = new QLabel();
    thumb->setObjectName(QStringLiteral("walkthroughThumb"));
    thumb->setFixedSize(RECENT_THUMB);
    if (!summary.thumb.isNull()) {
        thumb->setPixmap(QPixmap::fromImage(summary.thumb));
    }
    row->addWidget(thumb);
    auto* words = new QVBoxLayout();
    words->setSpacing(2);
    titleLabel_ = new ElidingLabel(qs(summary.title));
    titleLabel_->setObjectName(QStringLiteral("walkthroughTitle"));
    words->addWidget(titleLabel_);
    const std::string meta = summary.when
                                 ? review_text::day(*summary.when, today) + " · " +
                                       review_text::time(*summary.when) + " · " + summary.meta
                                 : summary.meta;
    words->addWidget(label(qs(meta), QStringLiteral("walkthroughHint")));
    row->addLayout(words, 1);
    if (!summary.pill.empty()) {
        QLabel* pill = label(qs(summary.pill), QStringLiteral("walkthroughPill"));
        if (!summary.pill_tone.empty()) {
            ui::setStyleProperty(pill, ui::prop::tone, qs(summary.pill_tone));
        }
        row->addWidget(pill, 0, Qt::AlignVCenter);
    }
    setAccessibleName(QStringLiteral("Open ") + qs(summary.title) + QStringLiteral(". ") +
                      qs(meta));
}

void WalkthroughRecentRow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked(qpath(folder_));
    }
    QFrame::mouseReleaseEvent(event);
}

void WalkthroughRecentRow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        Q_EMIT clicked(qpath(folder_));
        return;
    }
    QFrame::keyPressEvent(event);
}

// ---- PromptPopover ----

PromptPopover::PromptPopover(QWidget* owner, QWidget* content)
    : QWidget(owner, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("walkthroughPopoverFrame"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("walkthroughPopover"));
    outer->addWidget(card);
    auto* body = new QVBoxLayout(card);
    body->setContentsMargins(16, 14, 16, 14);
    body->addWidget(content);
}

void PromptPopover::openBeside(QWidget* column, QWidget* anchor) {
    setFixedWidth(ui::scaled(PROMPT_POPOVER_WIDTH));
    // Content shown or hidden while this was closed is only measured on the second pass, once
    // the first has let it lay out.
    for (int pass = 0; pass < 2; ++pass) {
        layout()->activate();
        adjustSize();
    }
    QPoint at(column->mapToGlobal(QPoint(column->width() + 8, 0)).x(),
              anchor->mapToGlobal(QPoint(0, anchor->height())).y() - height() / 2);
    QScreen* screen =
        column->screen() != nullptr ? column->screen() : QApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    at.setX(std::max(area.left() + 8, std::min(at.x(), area.right() - width() - 8)));
    at.setY(std::max(area.top() + 8, std::min(at.y(), area.bottom() - height() - 8)));
    move(at);
    show();
    activateWindow();
}

void PromptPopover::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    Q_EMIT closed();
}

// ---- WalkthroughTab ----

struct WalkthroughTab::Inbox : ui_walkthrough::detail::Mailbox<WalkthroughTab> {
    using Mailbox::Mailbox;
};

WalkthroughTab::WalkthroughTab(SettingsStore& settings, ReviewHost host, QWidget* parent)
    : WalkthroughTab(settings, std::move(host), WalkthroughTabOptions{}, parent) {}

WalkthroughTab::WalkthroughTab(SettingsStore& settings, ReviewHost host,
                               WalkthroughTabOptions options, QWidget* parent)
    : QWidget(parent), settings_(settings), io_(options.io), today_(std::move(options.today)),
      inbox_(std::make_shared<Inbox>(this)),
      textPrompt_([](QWidget* parent_, const QString& title,
                     const QString& text) -> std::optional<QString> {
          bool ok = false;
          const QString name =
              QInputDialog::getText(parent_, title, text, QLineEdit::Normal, QString(), &ok);
          return ok ? std::optional<QString>(name) : std::nullopt;
      }),
      warning_([](QWidget* parent_, const QString& title, const QString& text) {
          QMessageBox::warning(parent_, title, text);
      }),
      question_([](QWidget* parent_, const QString& title, const QString& text,
                   QMessageBox::StandardButtons buttons, QMessageBox::StandardButton fallback) {
          return QMessageBox::question(parent_, title, text, buttons, fallback);
      }),
      dialogRunner_([](QDialog& dialog) { return dialog.exec(); }) {
    capturesRoot_ = options.capturesRoot
                        ? *options.capturesRoot
                        : walkthrough::captures_root(Paths::resolve(identity(), system_env()));
    // Before the stylesheet: setting it sends a StyleChange (changeEvent).
    setObjectName(QStringLiteral("walkthroughTab"));
    ui::setLocalStyleSheet(this, tabSheet());
    const auto snapshot = settings_.snapshot();
    agent_ = keys::walkthrough_write_up_agent.read(*snapshot);
    styles_ = walkthrough::load_styles(*snapshot);
    promptTimer_ = new QTimer(this);
    promptTimer_->setSingleShot(true);
    promptTimer_->setInterval(PROMPT_SAVE_DELAY_MS);
    connect(promptTimer_, &QTimer::timeout, this, [this] { savePrompt(); });

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    scrollArea_ = new QScrollArea();
    scrollArea_->setObjectName(QStringLiteral("transcriptionTabScrollArea"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scrollArea_);
    auto* content = new QWidget();
    content->setObjectName(QStringLiteral("walkthroughTabContent"));
    scrollArea_->setWidget(content);
    auto* page = new QVBoxLayout(content);
    page->setContentsMargins(24, 6, 24, 16);
    page->setSpacing(0);

    stage_ = new Stage();
    split_ = new StudioSplit(buildColumn(), stage_);
    page->addWidget(split_, 1);
    autoRow_->setVisible(offersAutoMode(options.platform));
    delayRow_->setVisible(offersAutoMode(options.platform));
    buildStage();
    review_->bind(std::move(host));
    promptPopover_ = new PromptPopover(this, buildPromptArea());
    connect(promptPopover_, &PromptPopover::closed, this, [this] { onPromptClosed(); });
    built_ = true;
    applySizes();

    setHotkeys(QString(), QString());
    loadSettings();
    showAgents();
    showWalkthrough();
}

WalkthroughTab::~WalkthroughTab() {
    inbox_->close();
}

void WalkthroughTab::setTextPrompt(TextPrompt prompt) {
    textPrompt_ = std::move(prompt);
}

void WalkthroughTab::setWarning(Warning warning) {
    warning_ = std::move(warning);
}

void WalkthroughTab::setQuestion(WalkthroughReview::Question question) {
    question_ = std::move(question);
}

void WalkthroughTab::setDialogRunner(WalkthroughReview::DialogRunner runner) {
    dialogRunner_ = std::move(runner);
}

std::chrono::year_month_day WalkthroughTab::today() const {
    return past_walkthroughs::toDate(today_ ? today_() : QDate::currentDate());
}

// ---- the control column ----

QFrame* WalkthroughTab::buildColumn() {
    column_ = new QFrame();
    column_->setObjectName(QStringLiteral("walkthroughColumn"));
    auto* body = new QVBoxLayout(column_);
    body->setContentsMargins(16, 16, 16, 12);
    body->setSpacing(2);

    walkthroughButton_ = new SuccessButton(QStringLiteral("Start Walkthrough"));
    ui::setStyleProperty(walkthroughButton_, ui::prop::buttonId, QStringLiteral("walkthrough"));
    walkthroughButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(walkthroughButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT walkthroughRequested(); });
    body->addWidget(walkthroughButton_);

    // One slot under Start holds the shortcuts, or Cancel while a walkthrough records (the stage
    // shows the shortcuts then), so the rows below never shift. Cancel goes under Finish, not
    // beside it: "Finish Walkthrough" needs the column's width.
    keysSlot_ = plain();
    auto* under = new QVBoxLayout(keysSlot_);
    under->setContentsMargins(0, 8, 0, 0);
    under->setSpacing(0);
    cancelButton_ = new WarningButton(QStringLiteral("Cancel"));
    cancelButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(cancelButton_, &QPushButton::clicked, this, [this] { Q_EMIT cancelRequested(); });
    cancelButton_->hide();
    under->addWidget(cancelButton_);
    keysBox_ = plain();
    auto* keys = new QVBoxLayout(keysBox_);
    keys->setContentsMargins(0, 0, 0, 0);
    keys->setSpacing(4);
    walkthroughBadge_ = new FeatureBadge(
        nullptr,
        QStringLiteral("Talk while you take screenshots, then get one document with your words "
                       "and pictures in order. The copy choice below controls how it's "
                       "delivered."),
        column_);
    walkthroughBadge_->setNew(!keys::walkthrough_intro_seen.read(*settings_.snapshot()));
    startKey_ = keyChip(QString());
    snapKey_ = keyChip(QString());
    keys->addWidget(keyRow(QStringLiteral("Start / finish"), startKey_, walkthroughBadge_));
    keys->addWidget(keyRow(QStringLiteral("Screenshot"), snapKey_));
    under->addWidget(keysBox_);
    under->addStretch(1);
    body->addWidget(keysSlot_);

    addSection(body, QStringLiteral("CAPTURE"));
    autoSwitch_ = new SettingsSwitch();
    autoSwitch_->setAccessibleName(QStringLiteral("Auto Mode: capture clicks and drawings"));
    connect(autoSwitch_, &QAbstractButton::toggled, this,
            [this](bool on) { settings_.save_setting(keys::walkthrough_auto_mode.key(), on); });
    autoRow_ = row(QStringLiteral("Auto Mode"), autoSwitch_,
                   QStringLiteral("Save screenshots after clicks and drawings. Use the toolbar at "
                                  "the top to draw."));
    body->addWidget(autoRow_);
    captureDelay_ = new ui_snap::CaptureDelay(nullptr, &settings_);
    delayRow_ = row(QStringLiteral("Capture delay"), captureDelay_,
                    QStringLiteral("Wait before each screenshot, to add marks or let the screen "
                                   "settle."));
    body->addWidget(delayRow_);
    minimizeSwitch_ = new SettingsSwitch();
    minimizeSwitch_->setAccessibleName(
        QStringLiteral("Minimize to tray when a walkthrough starts"));
    connect(minimizeSwitch_, &QAbstractButton::toggled, this, [this](bool on) {
        settings_.save_setting(keys::walkthrough_minimize_to_tray.key(), on);
    });
    body->addWidget(row(QStringLiteral("Minimize to tray"), minimizeSwitch_,
                        QStringLiteral("Gets this window out of the way of what you're showing. "
                                       "Open it again from the tray icon.")));

    addSection(body, QStringLiteral("WRITE-UP"));
    writeUpSwitch_ = new SettingsSwitch();
    writeUpSwitch_->setAccessibleName(QStringLiteral("Write walkthroughs up with an AI agent"));
    connect(writeUpSwitch_, &QAbstractButton::toggled, this,
            [this](bool on) { onWriteUpToggled(on); });
    writeUpRow_ = row(QStringLiteral("AI write-up"), writeUpSwitch_, QString(), &writeUpRowName_);
    body->addWidget(writeUpRow_);
    agentField_ =
        new WalkthroughField([this] { return agentMenu(); }, QStringLiteral("Written by"));
    styleField_ = new WalkthroughField([this] { return styleMenu(); }, QStringLiteral("Style"));
    notesSwitch_ = new SettingsSwitch();
    notesSwitch_->setAccessibleName(QStringLiteral("Follow notes left for the agent"));
    connect(notesSwitch_, &QAbstractButton::toggled, this, [this](bool on) {
        settings_.save_setting(keys::walkthrough_write_up_notes.key(), on);
    });
    promptLink_ = new QPushButton(QStringLiteral("Edit the prompt…"));
    promptLink_->setObjectName(QStringLiteral("walkthroughPromptLink"));
    promptLink_->setCursor(Qt::PointingHandCursor);
    promptLink_->setToolTip(QStringLiteral("See and change exactly what the agent is asked for"));
    connect(promptLink_, &QPushButton::clicked, this, [this] { editPrompt(); });
    QWidget* linkRow = plain();
    auto* links = new QHBoxLayout(linkRow);
    links->setContentsMargins(0, 2, 0, 2);
    links->addStretch(1);
    links->addWidget(promptLink_);
    // Without a write-up these have nobody to act on. Dimmed rather than hidden, so the column
    // keeps its shape.
    writeUpRows_ = {
        row(QStringLiteral("Written by"), agentField_),
        row(QStringLiteral("Style"), styleField_),
        row(QStringLiteral("Follow my notes"), notesSwitch_,
            QStringLiteral("Say \"note for the agent: skip the setup part\" while you talk, and "
                           "the agent follows it instead of writing it into the document.")),
        linkRow,
    };
    for (QWidget* each : writeUpRows_) {
        body->addWidget(each);
    }

    addSection(body, QStringLiteral("COPY"));
    copyField_ = new WalkthroughField([this] { return copyMenu(); },
                                      QStringLiteral("Copy when it finishes"));
    body->addWidget(row(QStringLiteral("On finish, copy"), copyField_));

    body->addStretch(1);
    auto* foot = new QHBoxLayout();
    foot->setContentsMargins(0, 10, 0, 0);
    settingsLink_ = new QPushButton(QStringLiteral("Walkthrough settings  →"));
    settingsLink_->setObjectName(QStringLiteral("walkthroughSettingsLink"));
    settingsLink_->setCursor(Qt::PointingHandCursor);
    settingsLink_->setToolTip(QStringLiteral("Shortcuts and how the document looks"));
    connect(settingsLink_, &QPushButton::clicked, this,
            [this] { Q_EMIT settingsRequested(qs(destinations::walkthroughs)); });
    foot->addWidget(settingsLink_);
    foot->addStretch(1);
    body->addLayout(foot);
    return column_;
}

void WalkthroughTab::addSection(QVBoxLayout* body, const QString& title) {
    body->addSpacing(10);
    body->addWidget(rule());
    body->addSpacing(10);
    body->addWidget(label(title, QStringLiteral("walkthroughOverline")));
    body->addSpacing(2);
}

// A form row: what it is on the left, its control at the right edge.
QWidget* WalkthroughTab::row(const QString& text, QWidget* control, const QString& tip,
                             QLabel** name) {
    QWidget* made = plain();
    formRows_.push_back(made);
    auto* line = new QHBoxLayout(made);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(8);
    QLabel* nameLabel = label(text, QStringLiteral("walkthroughRowName"));
    if (!tip.isEmpty()) {
        nameLabel->setToolTip(tip);
    }
    line->addWidget(nameLabel);
    line->addStretch(1);
    line->addWidget(control);
    if (name != nullptr) {
        *name = nameLabel;
    }
    return made;
}

QWidget* WalkthroughTab::keyRow(const QString& text, QLabel* chip, QWidget* badge) {
    QWidget* made = plain();
    auto* line = new QHBoxLayout(made);
    line->setContentsMargins(0, 2, 0, 2);
    line->setSpacing(6);
    line->addWidget(label(text, QStringLiteral("walkthroughKeyName")));
    if (badge != nullptr) {
        line->addWidget(badge, 0, Qt::AlignVCenter);
    }
    line->addStretch(1);
    line->addWidget(chip);
    return made;
}

void WalkthroughTab::applySizes() {
    if (!built_) {
        return;
    }
    split_->setColumnWidth(ui::scaled(studio::kColumnWidth));
    keysSlot_->setFixedHeight(ui::scaled(KEYS_SLOT_HEIGHT));
    for (WalkthroughField* field : {agentField_, styleField_, copyField_}) {
        field->setFixedSize(ui::scaled(FIELD_WIDTH), ui::scaled(FIELD_HEIGHT));
    }
    captureDelay_->setFixedWidth(ui::scaled(FIELD_WIDTH));
    for (QWidget* each : formRows_) {
        each->setMinimumHeight(ui::scaled(ROW_HEIGHT));
    }
}

// ---- the prompt (in its popover) ----

// What the agent is asked for, and the styles to start from or keep.
QWidget* WalkthroughTab::buildPromptArea() {
    promptArea_ = new QWidget();
    promptArea_->setObjectName(QStringLiteral("walkthroughPlain"));
    auto* area = new QVBoxLayout(promptArea_);
    area->setContentsMargins(0, 0, 0, 0);
    area->setSpacing(10);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Prompt"));
    title->setObjectName(QStringLiteral("walkthroughPromptTitle"));
    head->addWidget(title);
    head->addStretch(1);
    auto* start = new QLabel(QStringLiteral("Start from"));
    start->setObjectName(QStringLiteral("walkthroughKeyName"));
    head->addWidget(start);
    presetCombo_ = new NoWheelComboBox();
    presetCombo_->setObjectName(QStringLiteral("walkthroughPreset"));
    presetCombo_->setAccessibleName(QStringLiteral("Write-up style"));
    connect(presetCombo_, &QComboBox::activated, this,
            [this](int index) { onPresetChosen(index); });
    head->addWidget(presetCombo_);
    area->addLayout(head);

    promptEdit_ = new QPlainTextEdit();
    promptEdit_->setObjectName(QStringLiteral("walkthroughPrompt"));
    promptEdit_->setAccessibleName(QStringLiteral("Write-up prompt"));
    promptEdit_->setPlaceholderText(qs(walkthrough::write_up::DEFAULT_INSTRUCTIONS));
    promptEdit_->setTabChangesFocus(true);
    promptEdit_->setMinimumHeight(150);
    promptEdit_->setMaximumHeight(260);
    connect(promptEdit_, &QPlainTextEdit::textChanged, this, [this] { onPromptEdited(); });
    area->addWidget(promptEdit_);

    auto* options = new QHBoxLayout();
    options->setSpacing(16);
    auto* hint = new QLabel(QStringLiteral("Saved as you type."));
    hint->setObjectName(QStringLiteral("walkthroughKeyName"));
    options->addWidget(hint);
    options->addStretch(1);
    saveStyleButton_ =
        styleLink(QStringLiteral("Save as style…"),
                  QStringLiteral("Keep these words as a style to pick here and in Redo as"),
                  &WalkthroughTab::saveAsStyle);
    options->addWidget(saveStyleButton_);
    deleteStyleButton_ =
        styleLink(QStringLiteral("Delete style"),
                  QStringLiteral("Remove this style from the list; the words stay in the box"),
                  &WalkthroughTab::deleteStyle);
    options->addWidget(deleteStyleButton_);
    area->addLayout(options);
    return promptArea_;
}

QPushButton* WalkthroughTab::styleLink(const QString& text, const QString& tip,
                                       void (WalkthroughTab::*slot)()) {
    auto* button = new QPushButton(text);
    button->setObjectName(QStringLiteral("walkthroughStyleLink"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    connect(button, &QPushButton::clicked, this, [this, slot] { (this->*slot)(); });
    return button;
}

void WalkthroughTab::editPrompt() {
    if (!writeUpSwitch_->isChecked()) {
        writeUpSwitch_->setChecked(true);
    }
    promptPopover_->openBeside(column_, promptLink_);
    promptEdit_->setFocus();
}

void WalkthroughTab::onPromptClosed() {
    // Closing is done typing: save now rather than when the pause ends.
    if (promptTimer_->isActive()) {
        savePrompt();
    }
}

// ---- the stage ----

void WalkthroughTab::buildStage() {
    review_ = new WalkthroughReview(settings_, {}, true, nullptr, [this] { return today(); });
    connect(review_, &WalkthroughReview::changed, this,
            [this](const QString& folder) { onReviewChanged(folder); });
    connect(review_, &WalkthroughReview::closeRequested, this, [this] { closeWalkthrough(); });
    stage_->addPane(review_);
    recordingPane_ = static_cast<QFrame*>(stage_->addPane(buildRecordingPane()));
    emptyPane_ = static_cast<QFrame*>(stage_->addPane(buildEmptyPane()));
}

QFrame* WalkthroughTab::buildRecordingPane() {
    QFrame* made = pane();
    auto* outer = new QVBoxLayout(made);
    outer->setContentsMargins(32, 32, 32, 32);
    outer->addStretch(2);
    auto* head = new QHBoxLayout();
    head->setSpacing(10);
    head->addStretch(1);
    auto* dot = new QLabel();
    dot->setObjectName(QStringLiteral("walkthroughRecordingDot"));
    head->addWidget(dot, 0, Qt::AlignVCenter);
    head->addWidget(
        label(QStringLiteral("Recording your walkthrough"), QStringLiteral("walkthroughHeading")));
    head->addStretch(1);
    outer->addLayout(head);
    outer->addSpacing(10);
    recordingKeys_ = label(QString(), QStringLiteral("walkthroughRecordingKeys"));
    recordingKeys_->setTextFormat(Qt::RichText);
    recordingKeys_->setAlignment(Qt::AlignCenter);
    outer->addWidget(recordingKeys_);
    outer->addSpacing(4);
    QLabel* after = label(QStringLiteral("It opens here as a storyboard when you finish."),
                          QStringLiteral("walkthroughHint"));
    after->setAlignment(Qt::AlignCenter);
    outer->addWidget(after);
    outer->addStretch(3);
    return made;
}

QFrame* WalkthroughTab::buildEmptyPane() {
    QFrame* made = pane();
    auto* outer = new QHBoxLayout(made);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->addStretch(1);
    QWidget* column = plain();
    column->setMaximumWidth(460);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* body = new QVBoxLayout(column);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addStretch(2);
    body->addWidget(
        label(QStringLiteral("No walkthrough open"), QStringLiteral("walkthroughHeading")));
    body->addSpacing(4);
    emptyHint_ =
        label(QStringLiteral("Start one on the left."), QStringLiteral("walkthroughHint"), true);
    body->addWidget(emptyHint_);
    body->addSpacing(22);
    const std::array<std::pair<QString, QString>, 3> steps{{
        {QStringLiteral("Talk it through"),
         QStringLiteral("Press Start and explain what's on screen.")},
        {QStringLiteral("Snap as you go"),
         QStringLiteral("Each screenshot lands beside what you were saying.")},
        {QStringLiteral("Paste the result"),
         QStringLiteral("One document, already on your clipboard.")},
    }};
    int number = 1;
    for (const auto& [title, text] : steps) {
        auto* step = new QHBoxLayout();
        step->setSpacing(12);
        QLabel* badge = label(QString::number(number++), QStringLiteral("walkthroughStep"));
        badge->setAlignment(Qt::AlignCenter);
        step->addWidget(badge, 0, Qt::AlignTop);
        auto* words = new QVBoxLayout();
        words->setSpacing(1);
        words->addWidget(label(title, QStringLiteral("walkthroughTitle")));
        words->addWidget(label(text, QStringLiteral("walkthroughHint"), true));
        step->addLayout(words, 1);
        body->addLayout(step);
        body->addSpacing(14);
    }

    recentBox_ = plain();
    auto* recent = new QVBoxLayout(recentBox_);
    recent->setContentsMargins(0, 10, 0, 0);
    recent->setSpacing(4);
    recent->addWidget(rule());
    recent->addSpacing(12);
    recent->addWidget(label(QStringLiteral("RECENT"), QStringLiteral("walkthroughOverline")));
    recent->addSpacing(2);
    recentLayout_ = new QVBoxLayout();
    recentLayout_->setSpacing(2);
    recent->addLayout(recentLayout_);
    recentBox_->hide();
    body->addWidget(recentBox_);
    body->addStretch(3);
    outer->addWidget(column, 4);
    outer->addStretch(1);
    return made;
}

// Reads the newest few walkthroughs for the empty stage, off the GUI thread.
void WalkthroughTab::loadRecent() {
    const std::uint64_t token = ++recentIssued_;
    recentToken_ = token;
    const std::filesystem::path root = capturesRoot_;
    const auto day = today();
    const double ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    Task load = [inbox = inbox_, root, day, ratio, token] {
        std::vector<std::filesystem::path> folders;
        try {
            folders = walkthrough::entries(root);
        } catch (const std::filesystem::filesystem_error& failure) {
            kLog.debug("The captures folder can't be read: {}", failure.what());
        } catch (const std::exception& failure) {
            // Python's worker dies here and the stage keeps waiting for its list.
            kLog.warning("Recent walkthroughs could not be listed: {}", failure.what());
            return;
        }
        if (folders.size() > static_cast<std::size_t>(RECENT_SHOWN)) {
            folders.resize(static_cast<std::size_t>(RECENT_SHOWN));
        }
        std::vector<past_walkthroughs::Summary> summaries;
        for (const auto& folder : folders) {
            try {
                summaries.push_back(
                    past_walkthroughs::summarize(folder, day, ratio, RECENT_THUMB, 6));
            } catch (const std::exception& failure) {
                kLog.warning("Could not read walkthrough {}: {}", path_to_utf8(folder),
                             failure.what());
            }
        }
        inbox->post([token, summaries = std::move(summaries)](WalkthroughTab& tab) mutable {
            tab.recentLoaded(token, std::move(summaries));
        });
    };
    if (io_ != nullptr) {
        io_->post(std::move(load));
    } else {
        std::thread(std::move(load)).detach();
    }
}

void WalkthroughTab::recentLoaded(std::uint64_t token,
                                  std::vector<past_walkthroughs::Summary> summaries) {
    if (token != recentToken_) {
        return;
    }
    recentToken_ = 0;
    for (WalkthroughRecentRow* each : recentRows_) {
        each->hide();
        each->setParent(nullptr);
        each->deleteLater();
    }
    const auto day = today();
    recentRows_.clear();
    for (const auto& summary : summaries) {
        auto* each = new WalkthroughRecentRow(summary, day);
        connect(each, &WalkthroughRecentRow::clicked, this, [this](const QString& folder) {
            showWalkthrough(path_from_utf8(utf8(folder)), true);
        });
        recentLayout_->addWidget(each);
        recentRows_.push_back(each);
    }
    recentBox_->setVisible(!summaries.empty());
    emptyHint_->setText(summaries.empty()
                            ? QStringLiteral("Start one on the left.")
                            : QStringLiteral("Start one on the left, or open a recent one below."));
}

QString WalkthroughTab::stageState() const {
    return stageState_.value_or(QStringLiteral("empty"));
}

void WalkthroughTab::showStage() {
    // A recording takes the stage even over an open storyboard, which comes back if it is
    // cancelled.
    const QString state = walkthroughActive_  ? QStringLiteral("recording")
                          : review_->folder() ? QStringLiteral("review")
                                              : QStringLiteral("empty");
    if (state == QStringLiteral("empty") && stageState_ != QStringLiteral("empty")) {
        loadRecent();
    }
    stageState_ = state;
    stage_->showPane(state == QStringLiteral("recording") ? static_cast<QWidget*>(recordingPane_)
                     : state == QStringLiteral("review")  ? static_cast<QWidget*>(review_)
                                                          : static_cast<QWidget*>(emptyPane_));
}

// ---- menus ----

QMenu* WalkthroughTab::menu() {
    auto* made = new QMenu(this);
    ui::setLocalStyleSheet(made, kMenuStyleSheet);
    made->setToolTipsVisible(true);
    return made;
}

QAction* WalkthroughTab::choice(QMenu* menu, QActionGroup* group, const QString& text, bool on,
                                std::function<void()> slot) {
    QAction* action = menu->addAction(text);
    action->setCheckable(true);
    action->setChecked(on);
    group->addAction(action);
    connect(action, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return action;
}

QMenu* WalkthroughTab::copyMenu() {
    QMenu* made = menu();
    auto* group = new QActionGroup(made);
    for (std::size_t index = 0; index < COPY_CHOICES.size(); ++index) {
        QAction* action = choice(made, group, qs(COPY_CHOICES[index].text), index == copyIndex_,
                                 [this, index] { onCopyChosen(index); });
        setCopyTip(action, COPY_CHOICES[index].value, true);
    }
    return made;
}

QMenu* WalkthroughTab::agentMenu() {
    QMenu* made = menu();
    auto* group = new QActionGroup(made);
    for (const std::string& agent : writeUpAgents()) {
        const AgentState state = agentState(agent);
        QAction* action = choice(made, group, agentName(agent) + QLatin1Char('\t') + state.status,
                                 agent == agent_, [this, agent] { chooseAgent(agent); });
        if (!state.detail.isEmpty()) {
            action->setToolTip(state.detail);
        }
    }
    return made;
}

QMenu* WalkthroughTab::styleMenu() {
    QMenu* made = menu();
    auto* group = new QActionGroup(made);
    const auto current = currentStyle();
    for (const auto& style : styles_) {
        const QString id = qs(style.id);
        choice(made, group, qs(style.name), current && style.id == current->id,
               [this, id] { useStyle(id); });
    }
    if (!ownPrompt_.isEmpty()) {
        choice(made, group, qs(OWN_PROMPT), !current, [this] { useStyle(QString()); });
    }
    made->addSeparator();
    connect(made->addAction(QStringLiteral("Edit the prompt…")), &QAction::triggered, this,
            [this] { editPrompt(); });
    connect(made->addAction(qs(NEW_STYLE)), &QAction::triggered, this, [this] { newStyle(); });
    return made;
}

// (what the last scan said, found and usable, detail) for `agent`.
WalkthroughTab::AgentState WalkthroughTab::agentState(const std::string& agent) const {
    const auto& seam = agent_picker::scanSeam();
    const std::optional<agents::ScanResult> found =
        seam.cached_agents ? seam.cached_agents() : std::nullopt;
    if (!found) {
        return {QStringLiteral("Checking…"), true, QString()};
    }
    const auto it = found->find(agent);
    const std::optional<agents::InstalledAgent> entry =
        it != found->end() ? it->second : std::nullopt;
    const auto state = agent_picker::describe_agent(agent, entry);
    return {qs(state.pill), state.tone == agent_picker::kFound, qs(state.detail)};
}

// ---- state ----

void WalkthroughTab::loadSettings() {
    if (promptTimer_->isActive()) {
        savePrompt();
    }
    const auto snapshot = settings_.snapshot();
    const Json& settings = *snapshot;
    styles_ = walkthrough::load_styles(settings);
    const std::array<QWidget*, 5> quiet{writeUpSwitch_, notesSwitch_, promptEdit_, minimizeSwitch_,
                                        autoSwitch_};
    for (QWidget* widget : quiet) {
        widget->blockSignals(true);
    }
    writeUpSwitch_->setChecked(keys::walkthrough_write_up.read(settings));
    notesSwitch_->setChecked(keys::walkthrough_write_up_notes.read(settings));
    minimizeSwitch_->setChecked(keys::walkthrough_minimize_to_tray.read(settings));
    autoSwitch_->setChecked(keys::walkthrough_auto_mode.read(settings));
    captureDelay_->setSeconds(static_cast<int>(keys::walkthrough_capture_delay.read(settings)));
    std::string prompt = keys::walkthrough_write_up_instructions.read(settings);
    if (prompt.empty()) {
        prompt = walkthrough::write_up::DEFAULT_INSTRUCTIONS;
    }
    if (stripped(promptEdit_->toPlainText()) != prompt) {
        // Only when it differs: setting it moves the cursor to the start.
        promptEdit_->setPlainText(qs(prompt));
    }
    for (QWidget* widget : quiet) {
        widget->blockSignals(false);
        widget->update();
    }
    showPreset();
    agent_ = keys::walkthrough_write_up_agent.read(settings);
    const std::string copy = keys::walkthrough_copy_format.read(settings);
    for (std::size_t index = 0; index < COPY_CHOICES.size(); ++index) {
        if (COPY_CHOICES[index].value == copy) {
            copyIndex_ = index;
            break;
        }
    }
    syncWriteUp();
    syncCopy();
}

void WalkthroughTab::onWriteUpToggled(bool on) {
    settings_.save_setting(keys::walkthrough_write_up.key(), on);
    syncWriteUp();
}

void WalkthroughTab::chooseAgent(const std::string& agent) {
    agent_ = agent;
    settings_.save_setting(keys::walkthrough_write_up_agent.key(), agent);
    if (!writeUpSwitch_->isChecked()) {
        // Picking an agent is asking for a write-up.
        writeUpSwitch_->setChecked(true);
    }
    syncWriteUp();
}

void WalkthroughTab::onCopyChosen(std::size_t index) {
    copyIndex_ = index;
    settings_.save_setting(keys::walkthrough_copy_format.key(),
                           std::string(COPY_CHOICES[index].value));
    syncCopy();
}

void WalkthroughTab::syncCopy() {
    const auto& [value, text] = COPY_CHOICES[copyIndex_];
    copyField_->setValue(qs(text));
    setCopyTip(copyField_, value, true);
}

void WalkthroughTab::syncWriteUp() {
    const bool on = writeUpSwitch_->isChecked();
    const QString name = agentName(agent_);
    writeUpRowName_->setToolTip(
        on ? name + QStringLiteral(" lays out a clean document from it. The as-spoken PDF is "
                                   "kept too.")
           : QStringLiteral("Off: you get it as spoken."));
    const AgentState state = agentState(agent_);
    agentField_->setValue(
        name,
        name + QStringLiteral(": ") + state.status +
            (state.detail.isEmpty() ? QString() : QStringLiteral(". ") + state.detail),
        state.found ? QString() : QStringLiteral("warning"));
    for (QWidget* each : writeUpRows_) {
        each->setEnabled(on);
    }
}

// ---- the prompt ----

void WalkthroughTab::onPromptEdited() {
    promptTimer_->start();
    showPreset();
}

void WalkthroughTab::savePrompt() {
    promptTimer_->stop();
    const std::string text = stripped(promptEdit_->toPlainText());
    settings_.save_setting(keys::walkthrough_write_up_instructions.key(),
                           text == walkthrough::write_up::DEFAULT_INSTRUCTIONS ? std::string()
                                                                               : text);
}

std::optional<walkthrough::WriteUpStyle> WalkthroughTab::currentStyle() const {
    return walkthrough::style_for(utf8(promptEdit_->toPlainText()), styles_);
}

// Points the style menus at the style the prompt is, or at the user's own words. The combo is
// rebuilt each time: styles are saved and deleted here and from the storyboard's Redo with a new
// prompt.
void WalkthroughTab::showPreset() {
    const std::string text = stripped(promptEdit_->toPlainText());
    const auto style = walkthrough::style_for(text, styles_);
    if (!style) {
        ownPrompt_ = qs(text);
    }
    NoWheelComboBox* combo = presetCombo_;
    combo->clear();
    for (const auto& each : styles_) {
        combo->addItem(qs(each.name), qs(each.id));
    }
    if (!ownPrompt_.isEmpty()) {
        combo->addItem(qs(OWN_PROMPT), QString());
    }
    combo->insertSeparator(combo->count());
    combo->addItem(qs(NEW_STYLE), kNewStyleData);
    combo->setCurrentIndex(combo->findData(style ? qs(style->id) : QString()));
    saveStyleButton_->setVisible(!style && !text.empty());
    deleteStyleButton_->setVisible(style && *style != walkthrough::default_style());
    styleField_->setValue(style ? qs(style->name) : qs(OWN_PROMPT));
}

// Uses a saved style's words, or ("") the user's own.
void WalkthroughTab::useStyle(const QString& styleId) {
    const int index = presetCombo_->findData(styleId);
    if (index >= 0) {
        onPresetChosen(index);
    }
}

void WalkthroughTab::onPresetChosen(int index) {
    // Past the styles: the user's own words, then New style….
    const QString styleId = presetCombo_->itemData(index).toString();
    if (styleId == kNewStyleData) {
        newStyle();
        return;
    }
    QString words = ownPrompt_;
    for (const auto& style : styles_) {
        if (qs(style.id) == styleId) {
            words = qs(style.instructions);
            break;
        }
    }
    if (utf8(words) != stripped(promptEdit_->toPlainText())) {
        promptEdit_->setPlainText(words);
    }
    savePrompt();
}

void WalkthroughTab::reloadStyles() {
    styles_ = walkthrough::load_styles(*settings_.snapshot());
}

// Makes a style from scratch and uses it; cancelled, the menu goes back.
void WalkthroughTab::newStyle() {
    // A dialog over an open popover would close it under the user's hand.
    promptPopover_->hide();
    NewStyleDialog dialog(settings_, this);
    if (dialogRunner_(dialog) != QDialog::Accepted || !dialog.saved()) {
        showPreset();
        return;
    }
    reloadStyles();
    // Its words fill the box, which points the menu at it, as picking a style does.
    promptEdit_->setPlainText(qs(dialog.saved()->instructions));
    savePrompt();
}

void WalkthroughTab::saveAsStyle() {
    const std::string text = stripped(promptEdit_->toPlainText());
    promptPopover_->hide();
    const std::optional<QString> name =
        textPrompt_(this, QStringLiteral("Save as style"),
                    QStringLiteral("Name the style. A name already saved is replaced."));
    if (!name || py::strip(utf8(*name)).empty()) {
        return;
    }
    try {
        settings_.mutate(
            [&](Json& settings) { walkthrough::save_style(settings, utf8(*name), text); });
    } catch (const py::ValueError& failure) {
        warning_(this, QStringLiteral("Save as style"), QString::fromUtf8(failure.what()));
        return;
    }
    reloadStyles();
    showPreset();
}

void WalkthroughTab::deleteStyle() {
    const auto style = currentStyle();
    if (!style || *style == walkthrough::default_style()) {
        return;
    }
    promptPopover_->hide();
    if (question_(this, QStringLiteral("Delete style"),
                  QStringLiteral("Delete the style \"") + qs(style->name) +
                      QStringLiteral("\"? Its words stay."),
                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    settings_.mutate([&](Json& settings) { walkthrough::delete_style(settings, style->id); });
    reloadStyles();
    showPreset();
}

// ---- the window ----

void WalkthroughTab::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        // Settings > Walkthroughs edits the same choices, and the library can delete
        // walkthroughs: show both as they are on return.
        loadSettings();
        if (stageState_ == QStringLiteral("empty")) {
            loadRecent();
        }
    } else if (event->type() == QEvent::StyleChange) {
        // A new type size restyles every widget; the column follows it.
        applySizes();
    }
}

// Labels the agents with the last scan; scans once if none has run.
void WalkthroughTab::showAgents() {
    const auto& seam = agent_picker::scanSeam();
    if (!seam.cached_agents || !seam.cached_agents()) {
        if (!scanStarted_) {
            scanStarted_ = true;
            Task scan = [inbox = inbox_, run = seam.scan_installed_agents] {
                if (run) {
                    try {
                        run(false);
                    } catch (const std::exception& failure) {
                        kLog.warning("Scanning for installed agents failed: {}", failure.what());
                    }
                }
                inbox->post([](WalkthroughTab& tab) { tab.showAgents(); });
            };
            if (io_ != nullptr) {
                io_->post(std::move(scan));
            } else {
                std::thread(std::move(scan)).detach();
            }
        }
        return;
    }
    syncWriteUp();
}

void WalkthroughTab::setHotkeys(const QString& walkthroughKey, const QString& screenshotKey) {
    for (const auto& [chip, key] :
         {std::pair{startKey_, walkthroughKey}, std::pair{snapKey_, screenshotKey}}) {
        chip->setText(key.isEmpty() ? QStringLiteral("Not set") : key);
        ui::setStyleProperty(chip, ui::prop::unset, key.isEmpty());
        restyle(chip);
        chip->setMinimumWidth(0);
    }
    // Both at one width, so the pair reads as a column.
    const int widest = std::max(startKey_->sizeHint().width(), snapKey_->sizeHint().width());
    for (QLabel* chip : {startKey_, snapKey_}) {
        chip->setMinimumWidth(widest);
    }
    walkthroughButton_->setHotkey(walkthroughKey);
    recordingKeys_->setText(QStringLiteral("Finish with <b>") + startKey_->text() +
                            QStringLiteral("</b>  ·  Screenshot with <b>") + snapKey_->text() +
                            QStringLiteral("</b>"));
}

void WalkthroughTab::setRecordingState(bool isRecording, bool walkthroughActive) {
    isRecording_ = isRecording;
    walkthroughActive_ = walkthroughActive;
    if (walkthroughActive && walkthroughBadge_->text() == QStringLiteral("New")) {
        settings_.save_setting(keys::walkthrough_intro_seen.key(), true);
        walkthroughBadge_->setNew(false);
    }
    walkthroughButton_->setText(walkthroughActive ? QStringLiteral("Finish Walkthrough")
                                                  : QStringLiteral("Start Walkthrough"));
    walkthroughButton_->setActive(walkthroughActive || !isRecording);
    cancelButton_->setVisible(walkthroughActive);
    keysBox_->setVisible(!walkthroughActive);
    // Set before a walkthrough starts; it reads them as it starts.
    const bool idle = !isRecording && !walkthroughActive;
    autoSwitch_->setEnabled(idle);
    captureDelay_->setEnabled(idle);
    showStage();
}

// ---- the storyboard ----

bool WalkthroughTab::showWalkthrough(const std::optional<std::filesystem::path>& folder, bool ask) {
    std::optional<std::filesystem::path> shown = folder;
    if (!shown) {
        const auto latest = newest();
        const std::string closed = keys::walkthrough_closed.read(*settings_.snapshot());
        if (latest && path_to_utf8(latest->filename()) != closed) {
            shown = latest;
        }
    }
    return showOnStage(shown, ask);
}

void WalkthroughTab::reloadWalkthrough() {
    if (review_->isBusy() || review_->isDirty()) {
        return;
    }
    const auto folder = review_->folder();
    std::error_code error;
    if (!folder) {
        loadRecent();
    } else if (std::filesystem::is_directory(*folder, error)) {
        showOnStage(folder, false);
    } else {
        showWalkthrough();
    }
}

void WalkthroughTab::closeWalkthrough() {
    const auto latest = newest();
    if (showOnStage(std::nullopt, true) && latest) {
        settings_.save_setting(keys::walkthrough_closed.key(), path_to_utf8(latest->filename()));
    }
}

std::optional<std::filesystem::path> WalkthroughTab::newest() const {
    std::vector<std::filesystem::path> folders;
    try {
        folders = walkthrough::entries(capturesRoot_);
    } catch (const std::filesystem::filesystem_error& failure) {
        kLog.warning("The captures folder can't be read: {}", failure.what());
    }
    if (folders.empty()) {
        return std::nullopt;
    }
    return folders.front();
}

bool WalkthroughTab::showOnStage(const std::optional<std::filesystem::path>& folder, bool ask) {
    if (review_->isBusy() || (review_->isDirty() && !(ask && review_->discardChanges()))) {
        return false;
    }
    review_->showFolder(folder);
    showStage();
    return true;
}

int WalkthroughTab::preferredWidth() const {
    return split_->columnWidth() + studio::kGap + ui::scaled(studio::kStageWidth);
}

void WalkthroughTab::prepareForWidthChange(int grow) {
    // Measure the side-by-side layout a wider window will show, not the stacked one a narrow
    // window still holds, which is far taller.
    if (split_->width() > 0) {
        split_->placeForWidth(split_->width() + grow);
    }
}

void WalkthroughTab::onReviewChanged(const QString& folder) {
    showWalkthrough(folder.isEmpty() ? std::nullopt : std::optional(path_from_utf8(utf8(folder))));
    Q_EMIT walkthroughsChanged();
}

} // namespace fv
