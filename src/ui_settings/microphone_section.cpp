#include "ui_settings/microphone_section.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <utility>

#include <QApplication>
#include <QBoxLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "audio/devices.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "ui_settings/detail/mailbox.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_microphones"};

namespace mic = appcore::mic;

QString q(std::string_view text) {
    return qt::toQString(text);
}

QVariant tokenData(const std::optional<std::string>& token) {
    return token ? QVariant(q(*token)) : QVariant();
}

} // namespace

// --- PreferredRow ---

PreferredRow::PreferredRow(QWidget* combo, QWidget* button, QWidget* parent)
    : QWidget(parent), button_(button) {
    setObjectName(QStringLiteral("micPreferredRow"));
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
    layout_->addWidget(combo, 1);
    layout_->addWidget(button);
}

void PreferredRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool narrow = width() < ui::scaled(kNarrowBelow);
    const QBoxLayout::Direction direction =
        narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
        layout_->setAlignment(button_, narrow ? Qt::AlignLeft : Qt::AlignVCenter);
    }
}

// --- MicrophoneSection ---

MicrophoneSection::MicrophoneSection(SettingsDialog& dialog, Ports ports, QObject* parent)
    : QObject(parent), dialog_(dialog), ports_(std::move(ports)),
      mailbox_(std::make_shared<detail::Mailbox<MicrophoneSection>>(this)) {}

MicrophoneSection::~MicrophoneSection() {
    mailbox_->close();
}

void MicrophoneSection::build(SettingsPage& page, QVBoxLayout* layout) {
    combo_ = new ElidingComboBox;
    combo_->setObjectName(QStringLiteral("settingsAudioDeviceCombo"));
    combo_->setMinimumHeight(40);
    combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    refreshButton_ = neutralButton(new Button(QStringLiteral("Refresh")));
    refreshButton_->setToolTip(QStringLiteral("Look for microphones plugged in since %1 started")
                                   .arg(q(identity().product)));
    connect(refreshButton_, &QPushButton::clicked, this, [this] { refreshDevices(); });
    auto* row = new PreferredRow(combo_, refreshButton_);
    preferredTile_ = new FieldTile(QStringLiteral("Preferred microphone"),
                                   QStringLiteral("Used for dictation and meetings."), row,
                                   ui::designIcon(QStringLiteral("microphone-blue.svg")));
    status_ = settingsCaption(QString());
    status_->hide();
    preferredTile_->addBody(status_);

    backupTile_ =
        new InfoTile(QStringLiteral("If it's unplugged, use"),
                     QStringLiteral("%1 moves down this list, even in the middle of a dictation.")
                         .arg(q(identity().product)),
                     ui::designIcon(QStringLiteral("stack-slate.svg")));
    rows_ = new QWidget;
    rows_->setObjectName(QStringLiteral("micBackupList"));
    rowsLayout_ = new QVBoxLayout(rows_);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(6);
    backupTile_->addBody(rows_);
    addCombo_ = new ElidingComboBox;
    addCombo_->setMinimumHeight(36);
    addCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addCombo_->setAccessibleName(QStringLiteral("Add a backup microphone"));
    connect(addCombo_, &QComboBox::activated, this, [this] { onAddChosen(); });
    backupTile_->addBody(addCombo_);
    page.addTileGroup(layout, QStringLiteral("Microphone"), {preferredTile_, backupTile_}, 1);
    load(*dialog_.settingsSnapshot());
    enumerate();
    // _on_audio_device_changed: the combo's own fills block its signals, and a load never saves.
    connect(combo_, &QComboBox::currentIndexChanged, this, [this] {
        if (dialog_.isLoading()) {
            return;
        }
        choosePreferred(combo_->currentData().toString());
    });
}

void MicrophoneSection::load(const Json& settings) {
    priority_ = mic::saved_priority(settings);
    render();
}

void MicrophoneSection::enumerate(bool refresh) {
    const std::uint64_t token = generation_.next();
    // Bound here: through the section the worker would hold the window.
    auto list = ports_.listDevices;
    auto reread = ports_.refreshDevices;
    ports_.worker->post([mailbox = mailbox_, gate = generation_, token, refresh, list, reread] {
        std::string outcome;
        std::vector<audio::InputDevice> devices;
        try {
            if (refresh) {
                outcome = std::string(reread() ? mic::kOutcomeRefreshed : mic::kOutcomeBusy);
            }
            devices = list();
        } catch (const std::exception& error) {
            kLog.warning("Couldn't discover audio inputs ({})", error.what());
            devices.clear();
        }
        mailbox->deliver(gate, token,
                         [devices = std::move(devices), outcome](MicrophoneSection& section) {
                             section.applyDevices(devices, q(outcome));
                             section.dialog_.refreshRailValues();
                         });
    });
}

void MicrophoneSection::applyDevices(std::vector<audio::InputDevice> devices,
                                     const QString& outcome) {
    std::vector<mic::MicDevice> listed;
    listed.reserve(devices.size());
    for (const auto& device : devices) {
        listed.push_back(mic::mic_device(device));
    }
    devices_ = std::move(listed);
    if (refreshButton_ != nullptr) {
        refreshButton_->setEnabled(true);
    }
    say(outcome == q(mic::kOutcomeBusy) ? q(mic::kInUse) : QString());
    render();
}

void MicrophoneSection::refreshDevices() {
    bool busy = audio::open_stream_count() > 0 || (ports_.meetingActive && ports_.meetingActive());
    if (!busy && ports_.playbackPlaying) {
        busy = ports_.playbackPlaying();
    }
    if (busy) {
        say(q(mic::kInUse));
        return;
    }
    say(QString());
    refreshButton_->setEnabled(false);
    enumerate(true);
}

void MicrophoneSection::choosePreferred(const QString& token) {
    std::optional<mic::MicSave> saved =
        mic::choose_preferred(qt::toUtf8(token), priority_, devices_);
    if (!saved) {
        return;
    }
    save(std::move(saved->priority), std::move(saved->updates), std::move(saved->drops));
}

void MicrophoneSection::move(std::size_t index, int offset) {
    if (auto moved = mic::move(priority_, index, offset)) {
        save(std::move(*moved));
    }
}

void MicrophoneSection::remove(std::size_t index) {
    if (auto left = mic::remove(priority_, index)) {
        save(std::move(*left));
    }
}

void MicrophoneSection::onAddChosen() {
    const QString token = addCombo_->currentData().toString();
    addCombo_->setCurrentIndex(0);
    if (auto added = mic::add(priority_, qt::toUtf8(token))) {
        save(std::move(*added));
    }
}

void MicrophoneSection::save(std::vector<audio::DeviceKey> priority, std::optional<Json> updates,
                             std::vector<std::string> drops) {
    const Json patch = updates ? *updates : mic::priority_save(priority).updates;
    if (!dialog_.persistMany(patch, std::move(drops))) {
        render();
        return;
    }
    priority_ = std::move(priority);
    render();
    Q_EMIT audioDeviceChanged(audio::priority_json(priority_));
}

void MicrophoneSection::say(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

void MicrophoneSection::render() {
    const std::optional<Held> held = heldFocus();
    preferredTile_->setDescription(q(mic::preferred_description(!priority_.empty())));
    fillPreferred();
    const std::vector<QWidget*> oldRows = fillRows();
    fillAddCombo();
    // Focus moves to the new rows before the old ones hide: hiding the focused row would hand
    // focus down the chain to the window header.
    QWidget* target = held ? focusTarget(*held) : nullptr;
    if (target != nullptr) {
        target->setFocus(Qt::OtherFocusReason);
    }
    for (QWidget* old : oldRows) {
        old->hide();
        old->deleteLater();
    }
}

QString MicrophoneSection::tokenOf(const QWidget* widget) const {
    for (const auto& [row, token] : rowTokens_) {
        if (row != nullptr && (row == widget || row->isAncestorOf(widget))) {
            return token;
        }
    }
    return {};
}

std::optional<MicrophoneSection::Held> MicrophoneSection::heldFocus() const {
    QWidget* focused = QApplication::focusWidget();
    if (focused == addCombo_) {
        return Held{QStringLiteral("add"), QString(), -1};
    }
    if (focused == nullptr || !rows_->isAncestorOf(focused) ||
        focused->property(ui::prop::kind).toString().isEmpty()) {
        return std::nullopt;
    }
    return Held{focused->property(ui::prop::kind).toString(), tokenOf(focused),
                rowsLayout_->indexOf(focused->parentWidget())};
}

QPushButton* MicrophoneSection::rowButton(QWidget* row, const QString& kind) {
    for (QPushButton* button : row->findChildren<QPushButton*>()) {
        if (button->property(ui::prop::kind).toString() == kind) {
            return button;
        }
    }
    return nullptr;
}

QWidget* MicrophoneSection::focusTarget(const Held& held) const {
    std::vector<QWidget*> rows;
    for (int index = 0; index + 1 < rowsLayout_->count(); ++index) {
        rows.push_back(rowsLayout_->itemAt(index)->widget());
    }
    if (held.kind == QStringLiteral("add")) {
        // Adding the last free microphone disables the picker.
        return addCombo_->isEnabled() || rows.empty()
                   ? nullptr
                   : rowButton(rows.back(), QStringLiteral("remove"));
    }
    QWidget* same = nullptr;
    for (QWidget* row : rows) {
        if (tokenOf(row) == held.token) {
            same = row;
            break;
        }
    }
    if (same != nullptr) {
        // Up at the top and Down at the bottom are gone; the other arrow is next to it.
        const QString other =
            held.kind == QStringLiteral("up") ? QStringLiteral("down") : QStringLiteral("up");
        for (const QString& choice : {held.kind, other, QStringLiteral("remove")}) {
            QPushButton* button = rowButton(same, choice);
            if (button != nullptr && button->isEnabled()) {
                return button;
            }
        }
    }
    if (!rows.empty()) {
        const auto index = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, held.index)),
                                                 rows.size() - 1);
        return rowButton(rows[index], QStringLiteral("remove"));
    }
    for (QWidget* widget : {static_cast<QWidget*>(addCombo_), static_cast<QWidget*>(refreshButton_),
                            static_cast<QWidget*>(combo_)}) {
        if (widget->isEnabled()) {
            return widget;
        }
    }
    return nullptr;
}

void MicrophoneSection::fillPreferred() {
    const mic::PreferredCombo filled = mic::preferred_combo(priority_, devices_);
    const QSignalBlocker blocker(combo_);
    combo_->clear();
    for (const auto& item : filled.items) {
        combo_->addItem(q(item.text), tokenData(item.token));
    }
    combo_->setCurrentIndex(static_cast<int>(filled.current));
}

std::vector<QWidget*> MicrophoneSection::fillRows() {
    std::vector<QWidget*> oldRows;
    while (rowsLayout_->count() > 0) {
        QLayoutItem* item = rowsLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            oldRows.push_back(item->widget());
        }
        delete item;
    }
    rowTokens_.clear();
    backupTile_->setVisible(!priority_.empty());
    std::vector<QWidget*> buttons;
    for (const mic::BackupRow& backup : mic::backup_rows(priority_, devices_)) {
        const std::size_t index = backup.index;
        std::function<void()> up;
        std::function<void()> down;
        if (backup.up) {
            up = [this, index] {
                move(index, -1);
            };
        }
        if (backup.down) {
            down = [this, index] {
                move(index, 1);
            };
        }
        QFrame* added = row(q(backup.label), q(backup.note), q(backup.state), q(backup.token), up,
                            down, [this, index] { remove(index); });
        rowsLayout_->addWidget(added);
        // Shown now rather than when the layout gets to it: focus given to a hidden button waits
        // for it and is lost to the next focus change.
        added->show();
        for (QPushButton* button : added->findChildren<QPushButton*>()) {
            if (button->isEnabled()) {
                buttons.push_back(button);
            }
        }
    }
    rowsLayout_->addWidget(
        row(q(mic::kFixedRowLabel), q(mic::kFixedRowNote), QStringLiteral("fixed")));
    // New rows join the end of the window's Tab order, after the sections below.
    QWidget* previous = refreshButton_;
    buttons.push_back(addCombo_);
    for (QWidget* widget : buttons) {
        QWidget::setTabOrder(previous, widget);
        previous = widget;
    }
    return oldRows;
}

QFrame* MicrophoneSection::row(const QString& label, const QString& note, const QString& state,
                               const QString& entry, std::function<void()> up,
                               std::function<void()> down, std::function<void()> remove) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("micBackupRow"));
    ui::setStyleProperty(frame, ui::prop::state, state);
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(12, 6, 6, 6);
    layout->setSpacing(4);
    auto* text = new QWidget;
    text->setObjectName(QStringLiteral("micBackupText"));
    auto* column = new QVBoxLayout(text);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    // Wrapped, not elided: a narrow column would cut every name to "Microphone (…" and hide the
    // part that tells them apart.
    auto* name = new WrappedLabel(label);
    name->setObjectName(QStringLiteral("micBackupName"));
    column->addWidget(name);
    if (!note.isEmpty()) {
        auto* hint = new ElidingLabel(note);
        hint->setObjectName(QStringLiteral("micBackupNote"));
        column->addWidget(hint);
    }
    layout->addWidget(text, 1);
    if (state != QStringLiteral("fixed")) {
        const std::pair<QString, std::function<void()>> actions[] = {
            {QStringLiteral("up"), std::move(up)},
            {QStringLiteral("down"), std::move(down)},
            {QStringLiteral("remove"), std::move(remove)},
        };
        const QString tips[] = {QStringLiteral("Move up"), QStringLiteral("Move down"),
                                QStringLiteral("Remove")};
        for (std::size_t i = 0; i < std::size(actions); ++i) {
            const auto& [kind, action] = actions[i];
            auto* button = new QPushButton;
            button->setObjectName(QStringLiteral("micBackupButton"));
            ui::setStyleProperty(button, ui::prop::kind, kind);
            button->setFixedSize(28, 28);
            button->setIconSize(QSize(14, 14));
            button->setFlat(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(tips[i]);
            button->setAccessibleName(QStringLiteral("%1: %2").arg(tips[i], label));
            if (action) {
                connect(button, &QPushButton::clicked, this, [action] { action(); });
            } else {
                // The top row has no Up and the bottom no Down; keeping the space lines every
                // row's buttons up.
                QSizePolicy policy = button->sizePolicy();
                policy.setRetainSizeWhenHidden(true);
                button->setSizePolicy(policy);
                button->setEnabled(false);
                button->hide();
            }
            layout->addWidget(button);
        }
        rowTokens_.emplace_back(frame, entry);
    }
    return frame;
}

void MicrophoneSection::fillAddCombo() {
    const mic::AddCombo filled = mic::add_combo(priority_, devices_);
    const QSignalBlocker blocker(addCombo_);
    addCombo_->clear();
    for (const auto& item : filled.items) {
        addCombo_->addItem(q(item.text), tokenData(item.token));
    }
    addCombo_->setEnabled(filled.enabled);
    addCombo_->setCurrentIndex(0);
}

} // namespace fv
