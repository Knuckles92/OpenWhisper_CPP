#include "ui_automation/action_fields.h"

#include <algorithm>
#include <array>
#include <string>

#include <QBoxLayout>
#include <QDir>
#include <QEasingCurve>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "agents/installed_agents.h"
#include "appkit/qt_bridge.h"
#include "automation/action_copy.h"
#include "automation/action_model.h"
#include "base/identity.h"
#include "pyport/number.h"
#include "settings/keys.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/agent_mark.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

namespace copy = automation::action_copy;
using automation::AgentMode;
using automation::AgentSurface;
using automation::Output;

const QString& shortcutTip() {
    static const QString tip = QStringLiteral(
        "Click, then press a shortcut. Escape cancels. Press it once to start, again to send.");
    return tip;
}

bool fontChanged(const QEvent* event) {
    return event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange;
}

int atScale(int px) {
    return static_cast<int>(py::round_half_even(px * ui::currentUiFontScale()));
}

QString q(std::string_view text) {
    return qt::toQString(text);
}

// Python's truthiness of a stored parameter.
bool truthy(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<long long>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string() || value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return false;
}

// `value or ""` for a text parameter: a value that is not text reads as none.
QString textOf(const Json& value) {
    return value.is_string() ? qt::toQString(value.get<std::string>()) : QString();
}

// os.path.normpath of the platform: native separators, "." and ".." steps resolved, repeated and
// trailing separators dropped.
QString normPath(const QString& path) {
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

template <std::size_t N>
int indexOf(const std::array<std::string_view, N>& values, std::string_view value) {
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end() ? -1 : static_cast<int>(found - values.begin());
}

QString agentName(const QString& agentId, const QString& fallback) {
    const auto name = automation::agent_display_name(qt::toUtf8(agentId));
    return name ? q(*name) : fallback;
}

// Keeps a tile's body lined up with its title, or gives it the full width when the tile is
// narrow (_BodyIndent). Found again by its object name: it has no meta-object of its own.
class BodyIndent : public QObject {
public:
    BodyIndent(TileBase* tile, int indent) : QObject(tile), tile_(tile), indent_(indent) {
        setObjectName(QStringLiteral("actionBodyIndent"));
        tile->installEventFilter(this);
        apply(tile->width());
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == tile_ && event->type() == QEvent::Resize) {
            apply(static_cast<QResizeEvent*>(event)->size().width());
        }
        return false;
    }

private:
    void apply(int width) {
        const bool narrow = width < atScale(action_fields::kNarrowTileWidth);
        const int margin = narrow || indent_ == 0 ? 0 : indent_;
        QVBoxLayout* layout = tile_->bodyLayout();
        if (layout->contentsMargins().left() != margin) {
            layout->setContentsMargins(margin, 0, 0, 0);
        }
    }

    TileBase* tile_;
    int indent_;
};

const std::array<std::pair<const char*, const char*>, 4> kIconTones{{
    {"-purple", "purple"},
    {"-green", "green"},
    {"-gray", "gray"},
    {"-blue", "blue"},
}};

QWidget* transparentBox() {
    auto* box = new QWidget;
    ui::setStyleProperty(box, ui::prop::tileId, QStringLiteral("actionRow"));
    return box;
}

} // namespace

// ---- helpers ----

namespace action_fields {

bool agentHasNoApp(std::string_view agentId) {
    return agentId == "opencode_cli";
}

void capWidths(const WidthCaps& caps) {
    for (const auto& [widget, width] : caps) {
        widget->setMaximumWidth(atScale(width));
    }
}

QLabel* fieldLabel(const QString& text, QWidget* buddy) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("textModelFieldLabel"));
    if (buddy != nullptr) {
        label->setBuddy(buddy);
    }
    return label;
}

WrappedLabel* hintLabel(const QString& text) {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QStringLiteral("actionHint"));
    label->setTextFormat(Qt::PlainText);
    return label;
}

void tintIcon(TileBase* tile, const QString& icon, int side, bool indentBody) {
    QString stem = icon;
    if (stem.endsWith(QStringLiteral(".svg"))) {
        stem.chop(4);
    }
    QString tone = QStringLiteral("blue");
    for (const auto& [suffix, name] : kIconTones) {
        if (stem.endsWith(QLatin1String(suffix))) {
            tone = QLatin1String(name);
            break;
        }
    }
    QLabel* label = tile->iconLabel();
    label->setFixedSize(side, side);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(ui::designIcon(icon).pixmap(18, 18));
    ui::setStyleProperty(label, ui::prop::iconTone, tone);
    if (tile->findChild<QObject*>(QStringLiteral("actionBodyIndent"), Qt::FindDirectChildrenOnly) ==
        nullptr) {
        new BodyIndent(tile, indentBody ? side + 12 : 0);
    }
}

void markInputField(QWidget* widget) {
    ui::setStyleProperty(widget, ui::prop::tileId, QStringLiteral("actionInput"));
}

QPushButton* pageLink(const QString& text, const QString& tooltip) {
    auto* link = new QPushButton(text);
    link->setObjectName(QStringLiteral("actionLink"));
    link->setFlat(true);
    link->setCursor(Qt::PointingHandCursor);
    if (!tooltip.isEmpty()) {
        link->setToolTip(tooltip);
    }
    return link;
}

QWidget* plainRow(const std::vector<QWidget*>& widgets, int spacing, bool stretchLast) {
    QWidget* row = transparentBox();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    for (QWidget* widget : widgets) {
        if (widget == nullptr) {
            layout->addStretch(1);
        } else {
            layout->addWidget(widget);
        }
    }
    if (stretchLast) {
        layout->addStretch(1);
    }
    return row;
}

ButtonRowBox buttonRow(const std::vector<Button*>& buttons, RowAlign align) {
    std::vector<QPushButton*> plain;
    for (Button* button : buttons) {
        fitCompactButton(button);
        plain.push_back(button);
    }
    auto* row = new ActionButtons(std::move(plain));
    QWidget* box = transparentBox();
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    if (align == RowAlign::Right) {
        layout->addStretch(1);
    }
    layout->addWidget(row);
    if (align == RowAlign::Left) {
        layout->addStretch(1);
    }
    return {box, row};
}

QWidget* sectionHeading(const QString& text) {
    QWidget* box = transparentBox();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);
    auto* line = new QFrame;
    line->setObjectName(QStringLiteral("actionDivider"));
    line->setFixedHeight(1);
    layout->addWidget(line);
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("actionSectionLabel"));
    layout->addWidget(label);
    return box;
}

QWidget* stack(const std::vector<QWidget*>& widgets, int spacing) {
    QWidget* box = transparentBox();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    for (QWidget* widget : widgets) {
        layout->addWidget(widget);
    }
    return box;
}

PathChooser& chooseFolder() {
    static PathChooser chooser = [](QWidget* parent, const QString& title, const QString& start) {
        return QFileDialog::getExistingDirectory(parent, title, start);
    };
    return chooser;
}

PathChooser& chooseProgram() {
    static PathChooser chooser = [](QWidget* parent, const QString& title, const QString& start) {
        return QFileDialog::getOpenFileName(parent, title, start);
    };
    return chooser;
}

} // namespace action_fields

using action_fields::capWidths;
using action_fields::fieldLabel;
using action_fields::hintLabel;
using action_fields::pageLink;

// ---- ActionButtons ----

ActionButtons::ActionButtons(std::vector<QPushButton*> buttons, int spacing, QWidget* parent)
    : ButtonRow(std::move(buttons), true, parent, spacing), spacing_(spacing) {
    // Its own width when there's room, so a footer never stretches across a card.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

QSize ActionButtons::sizeHint() const {
    int width = 0;
    int visible = 0;
    for (const QPushButton* button : buttons()) {
        if (button->isHidden()) {
            continue;
        }
        width += std::max(button->minimumWidth(), button->sizeHint().width());
        ++visible;
    }
    width += spacing_ * std::max(0, visible - 1);
    return {width, ButtonRow::sizeHint().height()};
}

// ---- Columns ----

Columns::Columns(QWidget* left, QWidget* right, int minColumn, int spacing, QWidget* parent)
    : QWidget(parent), minColumn_(minColumn) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionRow"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(spacing);
    layout_->addWidget(left, 1, Qt::AlignTop);
    layout_->addWidget(right, 1, Qt::AlignTop);
}

bool Columns::isStacked() const {
    return layout_->direction() == QBoxLayout::TopToBottom;
}

void Columns::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int needed = 2 * atScale(minColumn_) + layout_->spacing();
    const QBoxLayout::Direction direction =
        width() < needed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
    }
}

// ---- StatusLine ----

StatusLine::StatusLine(const QString& linkText, const QString& linkTip, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("actionStatusLine"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(10);
    dot_ = new StatusDot(nullptr, 10);
    // Level with the first line of the message, however many it wraps to.
    auto* dotColumn = new QVBoxLayout;
    dotColumn->setContentsMargins(0, 5, 0, 0);
    dotColumn->addWidget(dot_);
    dotColumn->addStretch(1);
    layout->addLayout(dotColumn);
    message_ = hintLabel();
    ui::setStyleProperty(message_, ui::prop::tileId, QStringLiteral("actionStatusMessage"));
    link_ = pageLink(linkText, linkTip);
    link_->setVisible(!linkText.isEmpty());
    connect(link_, &QPushButton::clicked, this, &StatusLine::linkActivated);
    layout->addWidget(new Reflow(message_, link_, 200, nullptr, Qt::AlignLeft), 1);
}

void StatusLine::showState(const QString& state, const QString& message, bool link) {
    state_ = state;
    message_->setText(message);
    link_->setVisible(link);
    dot_->setStatus(state == QLatin1String(kReady)       ? EngineStatus::Ready
                    : state == QLatin1String(kAttention) ? EngineStatus::Attention
                                                         : EngineStatus::Unknown);
    ui::setStyleProperty(this, ui::prop::state, state);
}

void StatusLine::setState(const QString& state) {
    showState(state, message_->text(), !link_->isHidden());
}

void StatusLine::setMessage(const QString& text) {
    message_->setText(text);
}

void StatusLine::setLink(const QString& text, const QString& tooltip) {
    link_->setText(text);
    link_->setToolTip(tooltip);
    link_->setVisible(!text.isEmpty());
}

void StatusLine::setLinkVisible(bool visible) {
    link_->setVisible(visible);
}

// ---- PathField ----

PathField::PathField(const QString& title, bool folder, const QString& placeholder, QWidget* parent)
    : QWidget(parent), title_(title), folder_(folder) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionPathField"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    line_ = action_fields::inputField(new QLineEdit);
    line_->setReadOnly(true);
    line_->setPlaceholderText(placeholder);
    line_->setAccessibleName(title);
    line_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    choose_ = neutralButton(new Button(QStringLiteral("Choose…")));
    choose_->setAccessibleName(QStringLiteral("Choose ") + title.toLower());
    connect(choose_, &QPushButton::clicked, this, &PathField::choose);
    clear_ = neutralButton(new Button(QStringLiteral("Clear")));
    clear_->setAccessibleName(QStringLiteral("Clear ") + title.toLower());
    connect(clear_, &QPushButton::clicked, this, [this] { set(QString()); });
    // The buttons move under the path when the column is narrow.
    buttons_ = action_fields::buttonRow({choose_, clear_}).box;
    layout->addWidget(new Reflow(line_, buttons_, 160, nullptr, Qt::AlignLeft));
    setPath(QString());
}

QString PathField::path() const {
    return line_->text();
}

void PathField::setPath(const QString& path) {
    line_->setText(path);
    line_->setToolTip(path);
    line_->setCursorPosition(0);
    clear_->setVisible(!path.isEmpty());
}

void PathField::set(const QString& path) {
    if (path != this->path()) {
        setPath(path);
        Q_EMIT changed(path);
    }
}

void PathField::choose() {
    const action_fields::PathChooser& pick =
        folder_ ? action_fields::chooseFolder() : action_fields::chooseProgram();
    const QString chosen = pick ? pick(window(), title_, path()) : QString();
    if (!chosen.isEmpty()) {
        set(normPath(chosen));
    }
}

// ---- ShortcutField ----

ShortcutField::ShortcutField(const QString& name, QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionShortcutField"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    input_ = new ProfileHotkeyInput;
    input_->setAccessibleName(name);
    input_->setToolTip(shortcutTip());
    input_->setPlaceholderText(QStringLiteral("Click to set a shortcut"));
    input_->setMinimumWidth(0);
    connect(input_, &ProfileHotkeyInput::captureChanged, this, [this](bool capturing) {
        if (capturing) {
            before_ = input_->hotkey();
        }
        Q_EMIT captureChanged(capturing);
    });
    connect(input_, &ProfileHotkeyInput::captured, this, &ShortcutField::captured);
    clear_ = neutralButton(new Button(QStringLiteral("Clear")));
    fitCompactButton(clear_, 64);
    clear_->setAccessibleName(QStringLiteral("Clear ") + name.toLower());
    connect(clear_, &QPushButton::clicked, this, [this] {
        Q_EMIT captured(QString());
        Q_EMIT cleared();
    });
    // Clear moves under the box when the column is narrow.
    row_ = new Reflow(input_, clear_, 170, nullptr, Qt::AlignLeft);
    layout->addWidget(row_);
    caps_ = {{row_, action_fields::kShortcutMaxWidth}};
    capWidths(caps_);
}

void ShortcutField::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (fontChanged(event) && !caps_.empty()) {
        capWidths(caps_);
    }
}

QString ShortcutField::hotkey() const {
    return input_->hotkey();
}

void ShortcutField::setHotkey(const QString& hotkey) {
    input_->setHotkey(hotkey);
    clear_->setEnabled(!hotkey.isEmpty());
}

void ShortcutField::restore() {
    setHotkey(before_);
}

void ShortcutField::beginCapture() {
    input_->setFocus();
    input_->beginCapture();
}

void ShortcutField::cancelCapture() {
    input_->cancelCapture();
}

// ---- RouteShortcutField ----

namespace {

// A label-fitted Button holds a floor above its size hint; Qt draws the box over a button
// squeezed below it.
int buttonWidth(const QPushButton* button) {
    return std::max(button->sizeHint().width(), button->minimumWidth());
}

QString routeHotkey(const Json& hotkeys) {
    if (!hotkeys.is_object()) {
        return {};
    }
    const auto found = hotkeys.find(RouteShortcutField::kAction);
    return found != hotkeys.end() && found->is_string() ? qt::toQString(found->get<std::string>())
                                                        : QString();
}

} // namespace

RouteShortcutField::RouteShortcutField(StandardShortcutHost& host, QWidget* parent)
    : QWidget(parent), host_(host) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    // Settings pages ignore minimum widths, so a narrow tile would draw the buttons over the box;
    // resizeEvent stacks them instead.
    row_ = new QBoxLayout(QBoxLayout::LeftToRight);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(8);
    input_ = new ProfileHotkeyInput;
    input_->setAccessibleName(QStringLiteral("Route by voice shortcut"));
    input_->setToolTip(QStringLiteral("Click, then press a shortcut. Escape cancels."));
    input_->setMinimumWidth(kInputMinWidth);
    input_->setMaximumWidth(kInputMaxWidth);
    connect(input_, &ProfileHotkeyInput::captureChanged, this, [this](bool capturing) {
        host_.setHotkeyCaptureSuspended(capturing);
        Q_EMIT captureChanged(capturing);
    });
    connect(input_, &ProfileHotkeyInput::captured, this, [this](const QString& hotkey) {
        Q_EMIT captured(hotkey);
        save(hotkey);
    });
    row_->addWidget(input_, 1);
    clear_ = neutralButton(new Button(QStringLiteral("Clear")));
    clear_->setBaseMinimumSize(64, 34);
    clear_->setToolTip(QStringLiteral("Remove the Route by voice shortcut"));
    connect(clear_, &QPushButton::clicked, this, [this] {
        Q_EMIT cleared();
        save(QString());
    });
    row_->addWidget(clear_);
    row_->addStretch();
    column->addLayout(row_);
    message_ = new WrappedLabel;
    message_->setObjectName(QStringLiteral("infoLabel"));
    ui::setStyleProperty(message_, ui::prop::tileId, QStringLiteral("commandShortcutError"));
    message_->setTextFormat(Qt::PlainText);
    message_->hide();
    column->addWidget(message_);
    sync();
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionRouteShortcut"));
}

QString RouteShortcutField::hotkey() const {
    Json hotkeys = host_.currentHotkeys();
    if (!hotkeys.is_object() || hotkeys.empty()) {
        const Json settings = host_.settings();
        hotkeys = settings.is_object() ? keys::hotkeys.read(settings) : Json();
    }
    return routeHotkey(hotkeys);
}

void RouteShortcutField::sync() {
    if (!input_->isCapturing()) {
        input_->setHotkey(hotkey());
    }
    clear_->setEnabled(!hotkey().isEmpty());
}

void RouteShortcutField::beginCapture() {
    input_->setFocus();
    input_->beginCapture();
}

void RouteShortcutField::cancelCapture() {
    input_->cancelCapture();
}

void RouteShortcutField::showError(const QString& text) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
}

void RouteShortcutField::save(const QString& hotkey) {
    const QString error = host_.setStandardHotkey(QString::fromLatin1(kAction), hotkey);
    showError(error);
    sync();
    if (error.isEmpty()) {
        Q_EMIT changed();
    }
}

void RouteShortcutField::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    sync();
}

int RouteShortcutField::sideBySideWidth() const {
    return atScale(kInputMinWidth) + row_->spacing() + buttonWidth(clear_);
}

// Both hints are the same whichever way resizeEvent last arranged the field, so the width a
// layout gives it never depends on that choice: when they followed the arrangement, a row could
// give the field less room side by side than stacked, and it flipped between the two forever.
QSize RouteShortcutField::sizeHint() const {
    return {sideBySideWidth(), QWidget::sizeHint().height()};
}

QSize RouteShortcutField::minimumSizeHint() const {
    return {buttonWidth(clear_), QWidget::minimumSizeHint().height()};
}

void RouteShortcutField::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int box = atScale(kInputMinWidth);
    const bool stacked = width() < sideBySideWidth();
    input_->setMinimumWidth(stacked ? std::min(box, width()) : box);
    const QBoxLayout::Direction direction =
        stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (row_->direction() != direction) {
        row_->setDirection(direction);
        row_->setAlignment(clear_, stacked ? Qt::AlignLeft : Qt::Alignment());
    }
}

// ---- SwitchRow ----

SwitchRow::SwitchRow(const QString& title, const QString& detail, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("actionSwitchRow"));
    setCursor(Qt::PointingHandCursor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(12);
    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    title_ = new WrappedLabel(title);
    title_->setObjectName(QStringLiteral("settingsTileTitle"));
    text->addWidget(title_);
    detail_ = new WrappedLabel(detail);
    detail_->setObjectName(QStringLiteral("settingsTileDescription"));
    text->addWidget(detail_);
    layout->addLayout(text, 1);
    switch_ = new SettingsSwitch;
    switch_->setAccessibleName(title);
    switch_->setAccessibleDescription(detail);
    layout->addWidget(switch_, 0, Qt::AlignTop);
}

void SwitchRow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && switch_->isEnabled()) {
        switch_->toggle();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

// ---- OutputCombo ----

OutputCombo::OutputCombo(const QString& name, bool allowNone, QWidget* parent)
    : ElidingComboBox(parent) {
    setObjectName(QStringLiteral("actionOutputCombo"));
    setAccessibleName(name);
    setMinimumHeight(36);
    for (const std::string_view value : Output::ALL) {
        if (value == Output::NONE && !allowNone) {
            continue;
        }
        addItem(q(copy::label_for(copy::OUTPUT_LABELS(), value)), q(value));
    }
    capWidths({{this, action_fields::kComboMaxWidth}});
}

QString OutputCombo::output() const {
    const QString value = currentData().toString();
    return value.isEmpty() ? q(Output::NONE) : value;
}

void OutputCombo::setOutput(const QString& value) {
    setCurrentIndex(std::max(0, findData(value)));
}

QSize OutputCombo::minimumSizeHint() const {
    const QSize hint = QComboBox::minimumSizeHint();
    return {std::min(hint.width(), kMinimumWidth), hint.height()};
}

void OutputCombo::changeEvent(QEvent* event) {
    ElidingComboBox::changeEvent(event);
    if (fontChanged(event)) {
        capWidths({{this, action_fields::kComboMaxWidth}});
    }
}

// ---- AgentChoice ----

AgentChoice::AgentChoice(const QString& agentId, QWidget* parent)
    : QPushButton(parent), agentId_(agentId) {
    setObjectName(QStringLiteral("actionAgentChoice"));
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    const QString name = agentName(agentId, agentId);
    setAccessibleName(name);
    const agents::AgentSpec* spec = agents::find_agent_spec(qt::toUtf8(agentId));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 7, 14, 7);
    layout->setSpacing(10);
    mark_ = new AgentMark(spec != nullptr ? q(spec->monogram) : name.left(2),
                          QColor(spec != nullptr ? q(spec->accent) : QStringLiteral("#0a84ff")));
    layout->addWidget(mark_, 0, Qt::AlignVCenter);
    auto* text = new QVBoxLayout;
    text->setSpacing(0);
    name_ = new ElidingLabel(name);
    name_->setObjectName(QStringLiteral("actionAgentName"));
    detail_ = new ElidingLabel(QStringLiteral("Checking…"));
    detail_->setObjectName(QStringLiteral("actionAgentDetail"));
    for (QLabel* label : {static_cast<QLabel*>(name_), static_cast<QLabel*>(detail_)}) {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->addWidget(label);
    }
    layout->addLayout(text, 1);
    arrival_ = new QVariantAnimation(this);
    arrival_->setDuration(action_fields::kArrivalMs);
    arrival_->setEasingCurve(QEasingCurve::OutCubic);
    connect(arrival_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { mark_->setWarmth(value.toDouble()); });
    connect(this, &QPushButton::toggled, this,
            [this](bool checked) { ui::setStyleProperty(name_, ui::prop::selected, checked); });
}

void AgentChoice::setFound(std::optional<bool> found, bool animate) {
    found_ = found;
    detail_->setText(!found   ? QStringLiteral("Checking…")
                     : *found ? QStringLiteral("Found")
                              : QStringLiteral("Not found"));
    ui::setStyleProperty(detail_, ui::prop::tone,
                         found == true    ? QStringLiteral("found")
                         : found == false ? QStringLiteral("missing")
                                          : QString());
    setAccessibleDescription(detail_->text());
    arrival_->stop();
    mark_->setMissing(found == false ? 1.0 : 0.0);
    if (found == true && animate) {
        arrival_->setStartValue(mark_->warmth());
        arrival_->setEndValue(1.0);
        arrival_->start();
    } else {
        mark_->setWarmth(found == true ? 1.0 : 0.0);
    }
}

QSize AgentChoice::sizeHint() const {
    return layout()->sizeHint().expandedTo(QSize(0, 44));
}

QSize AgentChoice::minimumSizeHint() const {
    return {layout()->minimumSize().width(), sizeHint().height()};
}

void AgentChoice::paintEvent(QPaintEvent*) {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text = QString(); // The child labels carry the text.
    QStylePainter painter(this);
    painter.drawControl(QStyle::CE_PushButton, option);
}

// ---- AgentChooser ----

AgentChooser::AgentChooser(QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionAgentChooser"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    for (const std::string_view agentId : automation::AGENTS) {
        auto* choice = new AgentChoice(q(agentId));
        connect(choice, &QPushButton::clicked, this,
                [this, choice] { onClicked(choice->agentId()); });
        choices_.push_back(choice);
    }
    // Side by side while their names fit, else one above the other.
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
    for (AgentChoice* choice : choices_) {
        layout_->addWidget(choice, 1);
    }
    current_ = q(automation::AGENTS[0]);
    sync();
}

AgentChoice* AgentChooser::choice(const QString& agentId) const {
    for (AgentChoice* choice : choices_) {
        if (choice->agentId() == agentId) {
            return choice;
        }
    }
    return nullptr;
}

bool AgentChooser::isStacked() const {
    return layout_->direction() == QBoxLayout::TopToBottom;
}

void AgentChooser::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    int needed = 0;
    for (const AgentChoice* choice : choices_) {
        needed += choice->sizeHint().width();
    }
    needed += layout_->spacing() * (static_cast<int>(choices_.size()) - 1);
    const QBoxLayout::Direction direction =
        width() < needed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
    }
}

void AgentChooser::setCurrent(const QString& agentId) {
    current_ = choice(agentId) != nullptr ? agentId : q(automation::AGENTS[0]);
    sync();
}

void AgentChooser::setAvailable(const std::map<std::string, bool>& available, bool scanned,
                                bool animate) {
    if (scanned) {
        std::map<std::string, bool> known;
        for (const std::string_view agentId : automation::AGENTS) {
            const auto found = available.find(std::string(agentId));
            known[std::string(agentId)] = found != available.end() && found->second;
        }
        available_ = std::move(known);
    } else {
        available_.reset();
    }
    for (AgentChoice* choice : choices_) {
        std::optional<bool> found;
        if (available_) {
            found = available_->at(qt::toUtf8(choice->agentId()));
        }
        choice->setFound(found, animate);
    }
    sync();
}

std::vector<QString> AgentChooser::found() const {
    std::vector<QString> agents;
    if (!available_) {
        return agents;
    }
    for (const std::string_view agentId : automation::AGENTS) {
        const auto it = available_->find(std::string(agentId));
        if (it != available_->end() && it->second) {
            agents.push_back(q(agentId));
        }
    }
    return agents;
}

void AgentChooser::sync() {
    for (AgentChoice* choice : choices_) {
        choice->setChecked(choice->agentId() == current_);
        // The chosen agent stays shown even when it's missing, so the card never claims a
        // different agent than the one saved.
        bool found = false;
        if (available_) {
            const auto it = available_->find(qt::toUtf8(choice->agentId()));
            found = it != available_->end() && it->second;
        }
        choice->setEnabled(found);
    }
}

void AgentChooser::onClicked(const QString& agentId) {
    if (agentId != current_) {
        current_ = agentId;
        sync();
        Q_EMIT chosen(agentId);
    } else {
        sync();
    }
}

// ---- AgentFields ----

namespace {

QString newFolderHint() {
    return QStringLiteral("A fresh folder per request in ~/%1 Agent Tasks.")
        .arg(q(identity().product));
}

template <std::size_t N>
std::vector<std::pair<QString, QString>> segments(const std::array<std::string_view, N>& values,
                                                  const copy::Labels& labels) {
    std::vector<std::pair<QString, QString>> items;
    for (const std::string_view value : values) {
        items.emplace_back(q(copy::label_for(labels, value)), QString());
    }
    return items;
}

} // namespace

AgentFields::AgentFields(QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionAgentFields"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);

    chooser_ = new AgentChooser;
    connect(chooser_, &AgentChooser::chosen, this, &AgentFields::onAgentChosen);
    outer->addWidget(fieldLabel(QStringLiteral("Agent")));
    outer->addWidget(chooser_);
    agentNote_ = hintLabel();
    ui::setStyleProperty(agentNote_, ui::prop::tone, QStringLiteral("warning"));
    agentNote_->hide();
    outer->addWidget(agentNote_);

    // Everything after the agent, hidden while the card only invites setup.
    details_ = transparentBox();
    auto* layout = new QVBoxLayout(details_);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);
    outer->addWidget(details_);
    layout->addWidget(fieldLabel(QStringLiteral("When you finish speaking")));
    modeBar_ = new SegmentedBar(segments(AgentMode::ALL, copy::MODE_LABELS()), true);
    modeBar_->setAccessibleName(QStringLiteral("Agent mode"));
    connect(modeBar_, &SegmentedBar::currentIndexChanged, this, [this] { emitChanged(); });
    layout->addWidget(modeBar_);
    modeHint_ = hintLabel();
    layout->addWidget(modeHint_);

    sessionBox_ = transparentBox();
    auto* session = new QVBoxLayout(sessionBox_);
    session->setContentsMargins(0, 4, 0, 0);
    session->setSpacing(6);
    session->addWidget(fieldLabel(QStringLiteral("Opens in")));
    surfaceBar_ = new SegmentedBar(segments(AgentSurface::ALL, copy::SURFACE_LABELS()), true);
    surfaceBar_->setAccessibleName(QStringLiteral("Where the session opens"));
    connect(surfaceBar_, &SegmentedBar::currentIndexChanged, this, [this] { emitChanged(); });
    session->addWidget(surfaceBar_);
    surfaceHint_ = hintLabel();
    session->addWidget(surfaceHint_);
    // Only a session starts somewhere: a quick answer runs with no files.
    session->addSpacing(4);
    newFolderRow_ =
        new SwitchRow(QStringLiteral("Create a new folder automatically"), newFolderHint());
    connect(newFolderRow_->switchButton(), &SettingsSwitch::toggled, this,
            [this] { emitChanged(); });
    session->addWidget(newFolderRow_);
    folderBox_ = transparentBox();
    auto* folderLayout = new QVBoxLayout(folderBox_);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(6);
    folder_ = new PathField(QStringLiteral("Working folder"), true,
                            QStringLiteral("Optional · where the agent starts"));
    connect(folder_, &PathField::changed, this, [this] { emitChanged(); });
    folderLayout->addWidget(fieldLabel(QStringLiteral("Working folder"), folder_->line()));
    folderLayout->addWidget(folder_);
    session->addWidget(folderBox_);
    layout->addWidget(sessionBox_);

    answerBox_ = transparentBox();
    auto* answer = new QHBoxLayout(answerBox_);
    answer->setContentsMargins(0, 4, 0, 0);
    answer->setSpacing(12);
    auto* outputColumn = new QVBoxLayout;
    outputColumn->setSpacing(6);
    outputCombo_ = new OutputCombo(QStringLiteral("Where the answer goes"), false);
    connect(outputCombo_, &QComboBox::activated, this, [this] { emitChanged(); });
    outputColumn->addWidget(fieldLabel(QStringLiteral("Answer goes to"), outputCombo_));
    outputColumn->addWidget(outputCombo_);
    answer->addLayout(outputColumn, 1);
    auto* modelColumn = new QVBoxLayout;
    modelColumn->setSpacing(6);
    modelEdit_ = action_fields::inputField(new QLineEdit);
    modelEdit_->setPlaceholderText(QStringLiteral("The agent's default"));
    modelEdit_->setAccessibleName(QStringLiteral("Model for quick answers"));
    modelEdit_->setMaxLength(120);
    connect(modelEdit_, &QLineEdit::editingFinished, this, &AgentFields::onModelEdited);
    modelColumn->addWidget(fieldLabel(QStringLiteral("Model (optional)"), modelEdit_));
    modelColumn->addWidget(modelEdit_);
    answer->addLayout(modelColumn, 1);
    layout->addWidget(answerBox_);

    layout->addSpacing(4);
    selectionRow_ =
        new SwitchRow(QStringLiteral("Include what I've selected"),
                      QStringLiteral("Sends the selected text and the app's name along with your "
                                     "words."));
    connect(selectionRow_->switchButton(), &SettingsSwitch::toggled, this,
            [this] { emitChanged(); });
    layout->addWidget(selectionRow_);
    selectionNote_ = transparentBox();
    auto* note = new QHBoxLayout(selectionNote_);
    note->setContentsMargins(0, 0, 0, 0);
    note->setSpacing(8);
    selectionHint_ =
        hintLabel(QStringLiteral(u"Needs “Know which app I'm dictating into” on Styles."));
    ui::setStyleProperty(selectionHint_, ui::prop::tone, QStringLiteral("warning"));
    note->addWidget(selectionHint_, 1);
    stylesLink_ = pageLink(QStringLiteral("Open Styles"), QStringLiteral("Turn on app awareness"));
    connect(stylesLink_, &QPushButton::clicked, this, &AgentFields::stylesRequested);
    note->addWidget(stylesLink_, 0, Qt::AlignTop);
    selectionNote_->hide();
    layout->addWidget(selectionNote_);
    caps_ = {{chooser_, action_fields::kChooserMaxWidth},
             {modeBar_, action_fields::kBarMaxWidth},
             {surfaceBar_, action_fields::kBarMaxWidth},
             {answerBox_, action_fields::kChooserMaxWidth},
             {folderBox_, action_fields::kChooserMaxWidth},
             {newFolderRow_, action_fields::kChooserMaxWidth},
             {selectionRow_, action_fields::kChooserMaxWidth},
             {selectionNote_, action_fields::kChooserMaxWidth}};
    capWidths(caps_);
    setParams(Json::object());
}

void AgentFields::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (fontChanged(event) && !caps_.empty()) {
        capWidths(caps_);
    }
}

Json AgentFields::params() const {
    const int mode = std::max(0, modeBar_->currentIndex());
    const int surface = std::max(0, surfaceBar_->currentIndex());
    Json params = extra_;
    params["agent"] = qt::toUtf8(chooser_->current());
    params["mode"] = std::string(AgentMode::ALL[static_cast<std::size_t>(mode)]);
    params["surface"] = std::string(AgentSurface::ALL[static_cast<std::size_t>(surface)]);
    params["folder"] = qt::toUtf8(folder_->path());
    params["new_folder"] = newFolderRow_->switchButton()->isChecked();
    params["include_selection"] = selectionRow_->switchButton()->isChecked();
    params["model"] = qt::toUtf8(modelEdit_->text().trimmed());
    params["output"] = qt::toUtf8(outputCombo_->output());
    return params;
}

void AgentFields::setParams(const Json& params) {
    const Json& defaults = automation::PARAMS(automation::ActionKind::AGENT);
    Json values = defaults;
    extra_ = Json::object();
    if (params.is_object()) {
        for (const auto& [key, value] : params.items()) {
            values[key] = value;
            if (!defaults.contains(key)) {
                extra_[key] = value;
            }
        }
    }
    loading_ = true;
    try {
        chooser_->setCurrent(textOf(values["agent"]));
        const std::string mode =
            values["mode"].is_string() ? values["mode"].get<std::string>() : std::string();
        const int modeIndex = indexOf(AgentMode::ALL, mode);
        modeBar_->setCurrentIndex(modeIndex >= 0 ? modeIndex
                                                 : indexOf(AgentMode::ALL, AgentMode::HANDOFF));
        const std::string surface =
            values["surface"].is_string() ? values["surface"].get<std::string>() : std::string();
        const int surfaceIndex = indexOf(AgentSurface::ALL, surface);
        surfaceBar_->setCurrentIndex(
            surfaceIndex >= 0 ? surfaceIndex : indexOf(AgentSurface::ALL, AgentSurface::AUTO));
        const Json& output = values["output"];
        outputCombo_->setOutput(output.is_string() && output.get<std::string>() == Output::NONE
                                    ? q(Output::SCRATCHPAD)
                                    : textOf(output));
        modelEdit_->setText(textOf(values["model"]));
        folder_->setPath(textOf(values["folder"]));
        newFolderRow_->switchButton()->setChecked(truthy(values["new_folder"]));
        selectionRow_->switchButton()->setChecked(truthy(values["include_selection"]));
    } catch (...) {
        loading_ = false;
        throw;
    }
    loading_ = false;
    sync();
}

void AgentFields::setAvailable(const std::map<std::string, bool>& available, bool scanned,
                               bool animate) {
    chooser_->setAvailable(available, scanned, animate);
    sync();
}

void AgentFields::setAppAwareness(bool on) {
    appAwareness_ = on;
    sync();
}

void AgentFields::setCompact(bool compact) {
    compact_ = compact;
    details_->setVisible(!compact);
    sync();
}

void AgentFields::sync() {
    const bool handoff = modeBar_->currentIndex() != indexOf(AgentMode::ALL, AgentMode::ASK);
    const QString current = chooser_->current();
    const QString name = agentName(current, QStringLiteral("the agent"));
    // Whether the agent waits for you depends on where it opens; Opens in says.
    modeHint_->setText(handoff
                           ? QStringLiteral("%1 opens in a new session with your words.").arg(name)
                           : QStringLiteral("%1 answers with no tools, so nothing on this "
                                            "computer changes.")
                                 .arg(name));
    sessionBox_->setVisible(handoff);
    answerBox_->setVisible(!handoff);
    const std::string_view surface =
        AgentSurface::ALL[static_cast<std::size_t>(std::max(0, surfaceBar_->currentIndex()))];
    folderBox_->setVisible(!newFolderRow_->switchButton()->isChecked());
    QString folderHint = newFolderHint();
    if (current == QLatin1String("codex") && surface != AgentSurface::TERMINAL) {
        folderHint += QStringLiteral(" The Codex app adds it as a local project.");
    }
    newFolderRow_->detailLabel()->setText(folderHint);
    newFolderRow_->switchButton()->setAccessibleDescription(folderHint);
    SegmentButton* appButton = surfaceBar_->buttons().at(
        static_cast<std::size_t>(indexOf(AgentSurface::ALL, AgentSurface::APP)));
    const bool hasApp = !action_fields::agentHasNoApp(qt::toUtf8(current));
    appButton->setEnabled(hasApp);
    appButton->setToolTip(
        hasApp ? QString() : QStringLiteral("%1 has no app; it opens in a terminal.").arg(name));
    surfaceHint_->setText(hasApp ? q(copy::label_for(copy::SURFACE_HINTS(), surface))
                                 : q(copy::no_app_hint(qt::toUtf8(name))));
    selectionNote_->setVisible(selectionRow_->switchButton()->isChecked() && !appAwareness_);
    const std::optional<std::map<std::string, bool>> available = chooser_->available();
    // A saved choice that went missing; a card inviting setup says so itself.
    bool missing = false;
    if (!compact_ && available) {
        const auto it = available->find(qt::toUtf8(current));
        missing = it == available->end() || !it->second;
    }
    if (missing && !chooser_->found().empty()) {
        agentNote_->setText(
            QStringLiteral("%1 wasn't found on this computer. Choose another agent.").arg(name));
    } else if (missing) {
        agentNote_->setText(QStringLiteral("%1 wasn't found on this computer.").arg(name));
    }
    agentNote_->setVisible(missing);
}

void AgentFields::emitChanged() {
    sync();
    if (!loading_) {
        Q_EMIT changed();
    }
}

void AgentFields::onAgentChosen(const QString& agentId) {
    const int app = indexOf(AgentSurface::ALL, AgentSurface::APP);
    if (action_fields::agentHasNoApp(qt::toUtf8(agentId)) && surfaceBar_->currentIndex() == app) {
        // Nothing to open as an app: the same choice as Auto makes for it.
        const bool wasLoading = loading_;
        loading_ = true;
        surfaceBar_->setCurrentIndex(indexOf(AgentSurface::ALL, AgentSurface::AUTO));
        loading_ = wasLoading;
    }
    emitChanged();
}

void AgentFields::onModelEdited() {
    if (modelEdit_->isModified()) {
        modelEdit_->setModified(false);
        emitChanged();
    }
}

// ---- ShapingField ----

namespace {

const std::array<const char*, 4> kShapingKinds{"cleanup", "raw", "profile", "transform"};

QString shapingHint(const QString& kind) {
    if (kind == QLatin1String("raw")) {
        return QStringLiteral("Exactly what you said, after your dictionary and snippets.");
    }
    if (kind == QLatin1String("profile")) {
        return QStringLiteral("Formatted by one of your cleanup profiles.");
    }
    if (kind == QLatin1String("transform")) {
        return QStringLiteral("Cleaned up, then rewritten by one of your transforms.");
    }
    return QStringLiteral("Your usual AI cleanup, whenever it's on.");
}

int shapingIndex(const QString& kind) {
    for (std::size_t i = 0; i < kShapingKinds.size(); ++i) {
        if (kind == QLatin1String(kShapingKinds[i])) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace

ShapingField::ShapingField(QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionShapingField"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    bar_ = new SegmentedBar({{QStringLiteral("Cleaned up"), QString()},
                             {QStringLiteral("Raw"), QString()},
                             {QStringLiteral("Profile"), QString()},
                             {QStringLiteral("Transform"), QString()}},
                            true);
    bar_->setAccessibleName(QStringLiteral("How the words are prepared"));
    connect(bar_, &SegmentedBar::currentIndexChanged, this, &ShapingField::onKindChanged);
    layout->addWidget(bar_);
    combo_ = new ElidingComboBox;
    combo_->setObjectName(QStringLiteral("actionShapingCombo"));
    combo_->setMinimumHeight(36);
    connect(combo_, &QComboBox::activated, this, [this] { Q_EMIT changed(); });
    layout->addWidget(combo_);
    hint_ = hintLabel();
    layout->addWidget(hint_);
    caps_ = {{bar_, action_fields::kBarMaxWidth + 80}, {combo_, action_fields::kComboMaxWidth}};
    capWidths(caps_);
    sync();
}

void ShapingField::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (fontChanged(event) && !caps_.empty()) {
        capWidths(caps_);
    }
}

void ShapingField::setChoices(const Json& settings) {
    Items profiles;
    for (const text::CleanupProfile& profile : text::load_cleanup_profiles(settings)) {
        profiles.emplace_back(qt::toQString(profile.id), qt::toQString(profile.name));
    }
    Items transforms;
    for (const text::Transform& transform : text::load_transforms(settings)) {
        transforms.emplace_back(qt::toQString(transform.id), qt::toQString(transform.name));
    }
    setChoices(std::move(profiles), std::move(transforms));
}

void ShapingField::setChoices(Items profiles, Items transforms) {
    const QString current = value();
    profiles_ = std::move(profiles);
    transforms_ = std::move(transforms);
    bar_->buttons().at(2)->setEnabled(!profiles_.empty());
    bar_->buttons().at(3)->setEnabled(!transforms_.empty());
    bar_->buttons().at(2)->setToolTip(
        profiles_.empty() ? QStringLiteral("You have no cleanup profiles yet.") : QString());
    bar_->buttons().at(3)->setToolTip(
        transforms_.empty() ? QStringLiteral("You have no transforms yet.") : QString());
    setValue(current);
}

QString ShapingField::kind() const {
    return QLatin1String(
        kShapingKinds[static_cast<std::size_t>(std::max(0, bar_->currentIndex()))]);
}

QString ShapingField::value() const {
    const QString current = kind();
    if (current == QLatin1String("raw")) {
        return q(automation::SHAPING_RAW);
    }
    if (current == QLatin1String("profile") || current == QLatin1String("transform")) {
        const QString item = combo_->currentData().toString();
        return item.isEmpty() ? q(automation::SHAPING_CLEANUP) : current + u':' + item;
    }
    return q(automation::SHAPING_CLEANUP);
}

void ShapingField::setValue(const QString& shaping) {
    const QString value = shaping.isEmpty() ? q(automation::SHAPING_CLEANUP) : shaping;
    const qsizetype colon = value.indexOf(u':');
    QString kind = colon < 0 ? value : value.left(colon);
    const QString item = colon < 0 ? QString() : value.mid(colon + 1);
    if (value == q(automation::SHAPING_RAW)) {
        kind = QStringLiteral("raw");
    } else if (kind != QLatin1String("profile") && kind != QLatin1String("transform")) {
        kind = QStringLiteral("cleanup");
    }
    loading_ = true;
    bar_->setCurrentIndex(shapingIndex(kind));
    fillCombo(kind, item);
    loading_ = false;
    sync();
}

void ShapingField::fillCombo(const QString& kind, const QString& selected) {
    combo_->clear();
    const Items* items = kind == QLatin1String("profile")     ? &profiles_
                         : kind == QLatin1String("transform") ? &transforms_
                                                              : nullptr;
    if (items != nullptr) {
        for (const auto& [id, name] : *items) {
            combo_->addItem(name, id);
        }
    }
    if (!selected.isEmpty() && combo_->findData(selected) < 0) {
        // Deleted since it was chosen: say so instead of quietly picking another; saving asks
        // for a new choice.
        const QString noun = kind == QLatin1String("profile") ? QStringLiteral("Profile")
                                                              : QStringLiteral("Transform");
        combo_->insertItem(0, noun + QStringLiteral(" no longer exists"), selected);
        combo_->setCurrentIndex(0);
    } else if (!selected.isEmpty()) {
        combo_->setCurrentIndex(combo_->findData(selected));
    }
    combo_->setAccessibleName(kind == QLatin1String("profile") ? QStringLiteral("Cleanup profile")
                                                               : QStringLiteral("Transform"));
}

void ShapingField::onKindChanged() {
    if (loading_) {
        return;
    }
    fillCombo(kind());
    sync();
    Q_EMIT changed();
}

void ShapingField::sync() {
    const QString current = kind();
    combo_->setVisible(current == QLatin1String("profile") ||
                       current == QLatin1String("transform"));
    hint_->setText(shapingHint(current));
}

} // namespace fv
