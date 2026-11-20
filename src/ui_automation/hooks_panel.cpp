#include "ui_automation/hooks_panel.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <typeinfo>
#include <utility>

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "automation/catalog.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/store.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/hooks_editors.h"
#include "ui_automation/hooks_format.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/flow_layout.h"
#include "ui_widgets/settings_switch.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.hooks_panel"};

namespace hooks = automation::hooks;

QString q(std::string_view text) {
    return qt::toQString(text);
}

const Clock& clockOf(const HookServices& services) {
    return services.clock != nullptr ? *services.clock : system_clock();
}

// os.path.basename on Windows: the part after the last separator.
std::string baseName(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The last record of an engine's list, which is newest first; none for an engine that is absent
// or that throws (polled by a timer: debug only).
template <class Record, class Read>
std::optional<Record> newest(const Read& read, const char* what) {
    try {
        std::vector<Record> records = read();
        if (!records.empty()) {
            return records.front();
        }
    } catch (const std::exception&) {
        kLog.debug("Could not read the {}", what);
    }
    return std::nullopt;
}

template <class Item> const Item* findById(const std::vector<Item>& items, const std::string& id) {
    for (const Item& item : items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

} // namespace

namespace hooks_panel {

QString errorText(const std::exception& failure) {
    const std::string text = py::strip(failure.what());
    return text.empty() ? q(type_display_name(typeid(failure))) : q(text);
}

void restyle(QWidget* widget, const char* name, const QVariant& value) {
    if (widget->property(name) != value) {
        ui::setStyleProperty(widget, name, value);
    }
}

QLabel* fieldLabel(const QString& text, QWidget* buddy) {
    return action_fields::fieldLabel(text, buddy);
}

WrappedLabel* hint(const QString& text) {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QStringLiteral("hooksHint"));
    label->setTextFormat(Qt::PlainText);
    return label;
}

Button* smallButton(const QString& text, bool primary) {
    auto* button = new Button(text);
    if (primary) {
        compactPrimaryButton(button);
    } else {
        neutralButton(button);
    }
    button->setBaseMinimumSize(0, 34);
    return button;
}

std::vector<QWidget*> tabStops(QWidget* container) {
    std::vector<QWidget*> stops;
    QWidget* widget = container->nextInFocusChain();
    while (widget != container && container->isAncestorOf(widget)) {
        const bool takesTab = (widget->focusPolicy() & Qt::TabFocus) == Qt::TabFocus;
        const bool insideStop = std::any_of(stops.begin(), stops.end(), [widget](QWidget* stop) {
            return stop->isAncestorOf(widget);
        });
        if (takesTab && !insideStop) {
            stops.push_back(widget);
        }
        widget = widget->nextInFocusChain();
    }
    return stops;
}

} // namespace hooks_panel

// ---- small pieces ----

EventChooser::EventChooser(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("hooksEventChooser"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    flow_ = new FlowLayout(this, 6);
    for (const std::string& type : hooks_format::event_types()) {
        const QString label = q(hooks_format::event_label(type));
        auto* chip = new QPushButton(label);
        chip->setObjectName(QStringLiteral("hooksEventChip"));
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        const QString description =
            q(automation::catalog::describe_type(type).value_or(std::string_view()));
        chip->setToolTip(description);
        chip->setAccessibleName(QStringLiteral("Send %1").arg(label));
        chip->setAccessibleDescription(description);
        connect(chip, &QPushButton::toggled, this, [this] { Q_EMIT changed(); });
        flow_->addWidget(chip);
        chips_.emplace_back(type, chip);
    }
}

std::vector<std::string> EventChooser::events() const {
    std::vector<std::string> checked;
    for (const auto& [type, chip] : chips_) {
        if (chip->isChecked()) {
            checked.push_back(type);
        }
    }
    return checked;
}

void EventChooser::setEvents(const std::vector<std::string>& events) {
    for (const auto& [type, chip] : chips_) {
        const QSignalBlocker blocker(chip);
        chip->setChecked(std::find(events.begin(), events.end(), type) != events.end());
    }
}

QPushButton* EventChooser::chip(const std::string& eventType) const {
    for (const auto& [type, chip] : chips_) {
        if (type == eventType) {
            return chip;
        }
    }
    return nullptr;
}

int EventChooser::heightForWidth(int width) const {
    return layout()->heightForWidth(width);
}

QSize EventChooser::sizeHint() const {
    const int w = width() > 0 ? width() : ui::scaled(420);
    return QSize(w, heightForWidth(w));
}

QSize EventChooser::minimumSizeHint() const {
    return QSize(0, heightForWidth(std::max(width(), 1)));
}

void EventChooser::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->oldSize().width() != event->size().width()) {
        updateGeometry();
    }
}

StatusChip::StatusChip(QWidget* parent) : QLabel(parent) {
    setObjectName(QStringLiteral("hooksChip"));
    setTextFormat(Qt::PlainText);
    // A chip keeps its own width, even when its buttons stack.
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    hide();
    errorLine_ = hooks_panel::hint();
    ui::setStyleProperty(errorLine_, ui::prop::tone, QStringLiteral("error"));
    errorLine_->hide();
}

StatusChip::~StatusChip() {
    // A line the editor never placed has no parent to delete it.
    if (errorLine_ != nullptr && errorLine_->parent() == nullptr) {
        delete errorLine_;
    }
}

void StatusChip::showResult(const QString& text, const QString& tone) {
    errorLine_->setText(QString());
    errorLine_->hide();
    setText(text);
    setToolTip(text);
    ui::setStyleProperty(this, ui::prop::tone, tone);
    setVisible(!text.isEmpty());
}

void StatusChip::showError(const QString& text) {
    setText(QString());
    hide();
    errorLine_->setText(text);
    errorLine_->setVisible(!text.isEmpty());
}

RecentList::RecentList(const QString& emptyText, QWidget* parent)
    : QWidget(parent), emptyText_(emptyText) {
    setObjectName(QStringLiteral("hooksRecent"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(2);
    setRows({});
}

void RecentList::setRows(const std::vector<RecentRow>& all) {
    std::vector<RecentRow> rows(
        all.begin(),
        all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size(), hooks_format::RECENT_ROWS)));
    if (shown_ && *shown_ == rows) {
        return;
    }
    shown_ = rows;
    while (layout_->count() > 0) {
        QLayoutItem* item = layout_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    if (rows.empty()) {
        auto* empty = new QLabel(emptyText_);
        empty->setObjectName(QStringLiteral("hooksRecentEmpty"));
        layout_->addWidget(empty);
        return;
    }
    for (const RecentRow& entry : rows) {
        auto* line = new QWidget;
        line->setObjectName(QStringLiteral("hooksRecentRow"));
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);
        auto* stamp = new QLabel(entry.at);
        stamp->setObjectName(QStringLiteral("hooksRecentTime"));
        row->addWidget(stamp);
        auto* what = new ElidingLabel(entry.what);
        what->setObjectName(QStringLiteral("hooksRecentWhat"));
        row->addWidget(what, 1);
        auto* outcome = new ElidingLabel(entry.result);
        outcome->setObjectName(QStringLiteral("hooksRecentResult"));
        ui::setStyleProperty(outcome, ui::prop::tone,
                             entry.ok ? QStringLiteral("ok") : QStringLiteral("error"));
        row->addWidget(outcome, 0, Qt::AlignRight);
        layout_->addWidget(line);
    }
}

AdaptiveButtons::AdaptiveButtons(const std::vector<QWidget*>& items, int spacing, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("hooksButtonRow"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    for (QWidget* item : items) {
        if (item != nullptr) {
            widgets_.push_back(item);
        }
    }
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(spacing);
    for (QWidget* item : items) {
        if (item == nullptr) {
            layout_->addStretch(1);
            gaps_.push_back(layout_->count() - 1);
        } else {
            layout_->addWidget(item);
        }
    }
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { reflow(); });
}

void AdaptiveButtons::reflow() {
    int needed = 0;
    int shownCount = 0;
    for (QWidget* widget : widgets_) {
        if (!widget->isHidden()) {
            needed += std::max(widget->sizeHint().width(), widget->minimumWidth());
            ++shownCount;
        }
    }
    needed += layout_->spacing() * std::max(0, shownCount - 1);
    const bool narrow = width() < needed;
    const auto direction = narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
    }
    for (const int index : gaps_) {
        layout_->setStretch(index, narrow ? 0 : 1);
    }
}

void AdaptiveButtons::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

bool AdaptiveButtons::event(QEvent* event) {
    if (timer_ != nullptr &&
        (event->type() == QEvent::LayoutRequest || event->type() == QEvent::FontChange ||
         event->type() == QEvent::StyleChange)) {
        timer_->start(0);
    }
    return QWidget::event(event);
}

// ---- rows ----

HookRow::HookRow(const QString& itemId, bool showsResults, QWidget* parent)
    : QFrame(parent), itemId_(itemId) {
    setObjectName(QStringLiteral("hooksRow"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(12, 9, 12, 9);
    grid_->setHorizontalSpacing(10);
    grid_->setVerticalSpacing(8);
    // The last delivery or run, where there is one to report.
    dot_ = new StatusDot(nullptr, 9);
    dot_->setVisible(showsResults);
    text_ = new QWidget;
    text_->setObjectName(QStringLiteral("hooksRowText"));
    auto* column = new QVBoxLayout(text_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    name_ = new ElidingLabel(QString());
    name_->setObjectName(QStringLiteral("hooksRowName"));
    column->addWidget(name_);
    detail_ = new ElidingLabel(QString());
    detail_->setObjectName(QStringLiteral("hooksRowDetail"));
    column->addWidget(detail_);
    editButton_ = hooks_panel::smallButton(QStringLiteral("Edit"));
    connect(editButton_, &QPushButton::clicked, this, [this] { Q_EMIT editRequested(itemId_); });
    switch_ = new SettingsSwitch;
    connect(switch_, &QAbstractButton::toggled, this,
            [this](bool on) { Q_EMIT enabledChanged(itemId_, on); });
    controls_ = new QWidget;
    controls_->setObjectName(QStringLiteral("hooksRowText"));
    auto* controls = new QHBoxLayout(controls_);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(10);
    controls->addWidget(editButton_, 0, Qt::AlignVCenter);
    controls->addWidget(switch_, 0, Qt::AlignVCenter);
    arrange(false);
}

void HookRow::arrange(bool narrow) {
    if (narrow_ && *narrow_ == narrow) {
        return;
    }
    narrow_ = narrow;
    for (QWidget* widget : {static_cast<QWidget*>(dot_), text_, controls_}) {
        grid_->removeWidget(widget);
    }
    grid_->addWidget(dot_, 0, 0, Qt::AlignVCenter);
    grid_->addWidget(text_, 0, 1);
    if (narrow) {
        grid_->addWidget(controls_, 1, 1, Qt::AlignRight);
    } else {
        grid_->addWidget(controls_, 0, 2, Qt::AlignVCenter);
    }
    grid_->setColumnStretch(1, 1);
}

void HookRow::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    const QMargins margins = grid_->contentsMargins();
    const int room = width() - margins.left() - margins.right();
    const int needed = controls_->sizeHint().width() + grid_->horizontalSpacing() * 2 +
                       dot_->width() + ui::scaled(140);
    arrange(room < needed);
}

void HookRow::showItem(const QString& name, const QString& detail, bool enabled,
                       EngineStatus status) {
    name_->setText(name);
    detail_->setText(enabled ? detail : QStringLiteral("Off · %1").arg(detail));
    editButton_->setAccessibleName(QStringLiteral("Edit %1").arg(name));
    switch_->setAccessibleName(QStringLiteral("%1 on").arg(name));
    {
        const QSignalBlocker blocker(switch_);
        switch_->setChecked(enabled);
    }
    if (!enabled) {
        status = EngineStatus::Unknown;
    }
    if (dot_->status() != status) {
        dot_->setStatus(status);
    }
    hooks_panel::restyle(this, ui::prop::off, !enabled);
}

void HookRow::setSelected(bool selected) {
    hooks_panel::restyle(this, ui::prop::selected, selected);
}

void HookRow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT editRequested(itemId_);
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

// ---- sections ----

HookSection::HookSection(const HookServices& services, HookKind kind, HookEditor* editor,
                         QWidget* parent)
    : QWidget(parent), services_(services), kind_(std::move(kind)), editor_(editor),
      requests_(this) {
    setObjectName(QStringLiteral("hooksSection"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    countLabel_ = new QLabel;
    countLabel_->setObjectName(QStringLiteral("hooksCount"));
    countLabel_->setAlignment(Qt::AlignCenter);

    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(8);
    listArea_ = new QWidget;
    listArea_->setObjectName(QStringLiteral("hooksList"));
    list_ = new QVBoxLayout(listArea_);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(6);
    root_->addWidget(listArea_);

    emptyState_ = new EmptyState(kind_.emptyTitle, kind_.emptyHint);
    root_->addWidget(emptyState_);

    editor_->hide();
    connect(editor_, &HookEditor::saveRequested, this, [this] { save(); });
    connect(editor_, &HookEditor::cancelRequested, this, [this] { cancel(); });
    connect(editor_, &HookEditor::deleteRequested, this, [this] { deleteItem(); });
    editor_->saveFirst = [this] {
        return save();
    };
    list_->addWidget(editor_);

    addButton_ = hooks_panel::smallButton(QStringLiteral("Add %1").arg(kind_.noun));
    addButton_->setAccessibleName(QStringLiteral("Add a %1").arg(kind_.noun));
    connect(addButton_, &QPushButton::clicked, this, [this] { openEditor(); });
    message_ = new WrappedLabel(QString());
    message_->setObjectName(QStringLiteral("hooksMessage"));
    message_->setTextFormat(Qt::PlainText);
    message_->hide();
    footer_ = new AdaptiveButtons({addButton_, nullptr});
    root_->addWidget(footer_);
    root_->addWidget(message_);

    live_ = new QTimer(this);
    live_->setInterval(hooks_format::LIVE_INTERVAL_MS);
    connect(live_, &QTimer::timeout, this, [this] { refreshLive(); });
}

HookSection::~HookSection() {
    // The page puts the count in its tile; one that was never placed has no parent to delete it.
    if (countLabel_ != nullptr && countLabel_->parent() == nullptr) {
        delete countLabel_;
    }
}

HookRow* HookSection::row(const std::string& itemId) const {
    const auto found = rows_.find(itemId);
    return found == rows_.end() ? nullptr : found->second;
}

std::string HookSection::editingId() const {
    return editor_->isHidden() ? std::string() : qt::toUtf8(editor_->itemId());
}

bool HookSection::hasUnsavedChanges() const {
    return !editor_->isHidden() && editor_->hasUnsavedChanges();
}

Json HookSection::settingsNow() const {
    return *services_.store->snapshot();
}

void HookSection::refresh(const std::optional<std::string>& selectId) {
    entries_ = loadItems(settingsNow());
    countLabel_->setText(QStringLiteral("%1 / %2").arg(entries_.size()).arg(kind_.maximum));
    countLabel_->setVisible(!entries_.empty());
    std::set<std::string> keep;
    for (const HookEntry& entry : entries_) {
        keep.insert(entry.id);
    }
    for (auto it = rows_.begin(); it != rows_.end();) {
        list_->removeWidget(it->second);
        if (!keep.contains(it->first)) {
            it->second->hide();
            it->second->deleteLater();
            it = rows_.erase(it);
        } else {
            ++it;
        }
    }
    list_->removeWidget(editor_);
    for (const HookEntry& entry : entries_) {
        HookRow* rowWidget = row(entry.id);
        if (rowWidget == nullptr) {
            rowWidget = new HookRow(q(entry.id), kind_.showsResults);
            connect(rowWidget, &HookRow::editRequested, this,
                    [this](const QString& id) { openEditor(qt::toUtf8(id)); });
            connect(rowWidget, &HookRow::enabledChanged, this,
                    [this](const QString& id, bool on) { setItemEnabled(qt::toUtf8(id), on); });
            rows_[entry.id] = rowWidget;
        }
        list_->addWidget(rowWidget);
        rowWidget->show();
    }
    emptyState_->setVisible(entries_.empty() && editor_->isHidden());
    const bool room = entries_.size() < kind_.maximum;
    addButton_->setEnabled(room);
    addButton_->setToolTip(room ? QString()
                                : QStringLiteral("You can keep up to %1. Delete one to add "
                                                 "another.")
                                      .arg(kind_.maximum));
    const auto known = [this](const std::string& id) {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&id](const HookEntry& entry) { return entry.id == id; });
    };
    if (selectId) {
        if (known(*selectId)) {
            showEditor(*selectId, true);
        } else {
            closeEditor();
        }
    } else if (!editor_->isHidden() && !editor_->hasUnsavedChanges()) {
        const std::string open = qt::toUtf8(editor_->itemId());
        if (known(open) && changedElsewhere(open)) {
            // Changed elsewhere while open and untouched here.
            showEditor(open, true);
        } else if (!known(open) && editor_->isSaved()) {
            // Deleted elsewhere while open and untouched.
            closeEditor();
        }
    }
    placeEditor();
    refreshLive();
}

void HookSection::refreshLive() {
    for (const HookEntry& entry : entries_) {
        if (HookRow* rowWidget = row(entry.id)) {
            rowWidget->showItem(q(entry.name), detail(entry.id), entry.enabled, status(entry.id));
        }
    }
    if (!editor_->isHidden()) {
        editor_->refreshLive();
    }
}

void HookSection::placeEditor() {
    // The editor goes under the row it edits, or after the list for a new item.
    const std::string editing = editingId();
    for (const auto& [id, rowWidget] : rows_) {
        rowWidget->setSelected(id == editing);
    }
    list_->removeWidget(editor_);
    HookRow* rowWidget = row(editing);
    const int index = rowWidget != nullptr ? list_->indexOf(rowWidget) + 1 : list_->count();
    list_->insertWidget(index, editor_);
    listArea_->setVisible(!rows_.empty() || !editor_->isHidden());
    chainTabOrder();
}

void HookSection::chainTabOrder() {
    // Tab goes down the list as it reads, then to Add. Rows made after the section joined the end
    // of the window's Tab order, and the editor moves under the row it edits, so every stop is
    // put back between whatever comes before the section and Add.
    std::vector<QWidget*> stops;
    for (int index = 0; index < list_->count(); ++index) {
        QWidget* widget = list_->itemAt(index)->widget();
        if (widget == editor_) {
            if (!editor_->isHidden()) {
                stops.insert(stops.end(), editor_->tabStops().begin(), editor_->tabStops().end());
            }
        } else if (auto* rowWidget = qobject_cast<HookRow*>(widget)) {
            stops.push_back(rowWidget->editButton());
            stops.push_back(rowWidget->switchButton());
        }
    }
    stops.push_back(addButton_);
    QWidget* before = addButton_->previousInFocusChain();
    while (before != addButton_ && (before->focusPolicy() == Qt::NoFocus || isAncestorOf(before))) {
        before = before->previousInFocusChain();
    }
    for (QWidget* widget : stops) {
        QWidget::setTabOrder(before, widget);
        before = widget;
    }
}

void HookSection::showEditor(const std::string& itemId, bool saved) {
    showInEditor(itemId, saved);
    editor_->show();
    emptyState_->hide();
    placeEditor();
}

void HookSection::closeEditor() {
    editor_->hide();
    editor_->clear();
    emptyState_->setVisible(entries_.empty());
    placeEditor();
}

bool HookSection::saveBeforeSwitch() {
    if (!hasUnsavedChanges()) {
        return true;
    }
    const auto answer = action_prompts::ask(
        this, QStringLiteral("Unsaved %1").arg(kind_.noun),
        QStringLiteral("Save your changes to this %1?").arg(kind_.noun),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return save();
    }
    return answer == QMessageBox::Discard;
}

bool HookSection::openEditor(const std::string& itemId) {
    if (!itemId.empty() && itemId == editingId()) {
        return true;
    }
    if (!saveBeforeSwitch()) {
        return false;
    }
    setMessage(QString());
    const bool saved = std::any_of(entries_.begin(), entries_.end(),
                                   [&](const HookEntry& entry) { return entry.id == itemId; });
    showEditor(saved ? itemId : std::string(), saved);
    editor_->nameEdit()->setFocus();
    return true;
}

void HookSection::cancel() {
    closeEditor();
}

bool HookSection::save() {
    Draft draft;
    try {
        draft = prepareDraft();
        services_.store->mutate(draft.apply);
    } catch (const py::ValueError& refused) {
        editor_->setMessage(q(refused.what()), true);
        return false;
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the {} ({})", kind_.noun.toStdString(),
                     type_display_name(typeid(failure)));
        editor_->setMessage(QStringLiteral("Couldn't save the %1: %2")
                                .arg(kind_.noun, hooks_panel::errorText(failure)),
                            true);
        return false;
    }
    refresh(draft.id);
    editor_->setMessage(QStringLiteral("Saved."));
    Q_EMIT changed();
    return true;
}

bool HookSection::saveDraft(bool ask) {
    if (!hasUnsavedChanges() || save() || !ask) {
        return true;
    }
    const auto answer =
        action_prompts::ask(this, QStringLiteral("Unsaved %1").arg(kind_.noun),
                            QStringLiteral("%1 Discard your changes to this %2?")
                                .arg(editor_->message()->text(), kind_.noun),
                            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return false;
    }
    closeEditor();
    return true;
}

void HookSection::deleteItem() {
    const std::string id = qt::toUtf8(editor_->itemId());
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&id](const HookEntry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        closeEditor();
        return;
    }
    const HookEntry item = *found;
    const auto answer = action_prompts::ask(
        this, QStringLiteral("Delete %1?").arg(kind_.noun), deleteQuestion(item),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    try {
        services_.store->mutate([&](Json& settings) { removeFromSettings(settings, item.id); });
    } catch (const std::exception& failure) {
        kLog.warning("Could not delete the {} ({})", kind_.noun.toStdString(),
                     type_display_name(typeid(failure)));
        editor_->setMessage(QStringLiteral("Couldn't delete the %1: %2")
                                .arg(kind_.noun, hooks_panel::errorText(failure)),
                            true);
        return;
    }
    closeEditor();
    refresh();
    afterDelete(item);
    Q_EMIT changed();
}

void HookSection::setItemEnabled(const std::string& itemId, bool enabled) {
    const std::function<void(Json&)> apply = enabledApplier(itemId, enabled);
    if (!apply) {
        return;
    }
    try {
        services_.store->mutate(apply);
    } catch (const std::exception& failure) {
        kLog.warning("Could not turn the {} on or off ({})", kind_.noun.toStdString(),
                     type_display_name(typeid(failure)));
        setMessage(hooks_panel::errorText(failure), true);
        refreshLive();
        return;
    }
    setMessage(QString());
    entries_ = loadItems(settingsNow());
    if (qt::toUtf8(editor_->itemId()) == itemId) {
        editor_->setSourceEnabled(enabled);
    }
    refreshLive();
    Q_EMIT changed();
}

bool HookSection::isPolling() const {
    return live_->isActive();
}

void HookSection::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshLive();
    live_->start();
}

void HookSection::hideEvent(QHideEvent* event) {
    live_->stop();
    QWidget::hideEvent(event);
}

void HookSection::setMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
    ui::setStyleProperty(message_, ui::prop::tone,
                         error ? QStringLiteral("error") : QStringLiteral("ok"));
}

// ---- webhooks ----

WebhookSection::WebhookSection(const HookServices& services, QWidget* parent)
    : HookSection(services,
                  {QStringLiteral("webhook"), hooks::MAX_WEBHOOKS,
                   QStringLiteral("No webhooks yet"),
                   QStringLiteral("Add one to send finished dictations to a web address."), true},
                  new WebhookEditor(services), parent) {
    refresh();
}

WebhookEditor* WebhookSection::webhookEditor() const {
    return static_cast<WebhookEditor*>(editor());
}

const hooks::Webhook* WebhookSection::find(const std::string& itemId) const {
    return findById(items_, itemId);
}

std::vector<HookEntry> WebhookSection::loadItems(const Json& settings) {
    items_ = hooks::load_webhooks(settings);
    std::vector<HookEntry> entries;
    for (const hooks::Webhook& item : items_) {
        entries.push_back({item.id, item.name, item.enabled});
    }
    return entries;
}

void WebhookSection::showInEditor(const std::string& itemId, bool saved) {
    if (const hooks::Webhook* item = saved ? find(itemId) : nullptr) {
        webhookEditor()->showItem(*item, true);
    } else {
        webhookEditor()->showItem({hooks::new_id(), "", "", hooks_format::default_events()}, false);
    }
}

bool WebhookSection::changedElsewhere(const std::string& itemId) const {
    const hooks::Webhook* item = find(itemId);
    const auto& source = webhookEditor()->source();
    return item != nullptr && source && !(*item == *source);
}

HookSection::Draft WebhookSection::prepareDraft() {
    hooks::Webhook draft = webhookEditor()->draft();
    // The switch on the row decides on/off, even while the editor is open.
    if (const hooks::Webhook* saved = find(draft.id)) {
        draft.enabled = saved->enabled;
    }
    return {draft.id, [draft](Json& settings) {
                hooks::save_webhook(settings, draft);
            }};
}

std::function<void(Json&)> WebhookSection::enabledApplier(const std::string& itemId, bool enabled) {
    const hooks::Webhook* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    hooks::Webhook changed = *item;
    changed.enabled = enabled;
    return [changed](Json& settings) {
        hooks::save_webhook(settings, changed);
    };
}

void WebhookSection::removeFromSettings(Json& settings, const std::string& itemId) {
    hooks::delete_webhook(settings, itemId);
}

std::optional<automation::DeliveryRecord> WebhookSection::last(const std::string& itemId) const {
    if (services().webhooks == nullptr) {
        return std::nullopt;
    }
    return newest<automation::DeliveryRecord>(
        [&] { return services().webhooks->deliveries(itemId); }, "last webhook delivery");
}

QString WebhookSection::detail(const std::string& itemId) {
    const hooks::Webhook* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    std::string text =
        hooks_format::host_of(item->url) + " · " + hooks_format::events_summary(item->events);
    if (const auto record = last(itemId)) {
        text += " · Last delivery " + hooks_format::delivery_result(*record).text;
    }
    return q(text);
}

EngineStatus WebhookSection::status(const std::string& itemId) {
    const auto record = last(itemId);
    if (!record) {
        return EngineStatus::Unknown;
    }
    return record->ok ? EngineStatus::Ready : EngineStatus::Attention;
}

QString WebhookSection::deleteQuestion(const HookEntry& item) {
    return QStringLiteral("Delete “%1”? %2 stops sending to it and forgets its signing secret.")
        .arg(q(item.name), q(identity().product));
}

void WebhookSection::afterDelete(const HookEntry& item) {
    automation::WebhookControl* engine = services().webhooks;
    if (engine == nullptr || services().io == nullptr) {
        return;
    }
    requests().inBackground<bool>(
        *services().io,
        [engine, id = item.id] {
            engine->delete_secret(id);
            return true;
        },
        [this](std::optional<bool>, QString error) {
            if (!error.isEmpty()) {
                setMessage(QStringLiteral("Deleted, but its signing secret couldn't be removed: %1")
                               .arg(error),
                           true);
            }
        });
}

// ---- programs ----

ProgramSection::ProgramSection(const HookServices& services, QWidget* parent)
    : HookSection(services,
                  {QStringLiteral("program"), hooks::MAX_PROGRAMS,
                   QStringLiteral("No programs yet"),
                   QStringLiteral("Add one to run your own script when you dictate."), true},
                  new ProgramEditor(services), parent) {
    refresh();
}

ProgramEditor* ProgramSection::programEditor() const {
    return static_cast<ProgramEditor*>(editor());
}

const hooks::Program* ProgramSection::find(const std::string& itemId) const {
    return findById(items_, itemId);
}

std::vector<HookEntry> ProgramSection::loadItems(const Json& settings) {
    items_ = hooks::load_programs(settings);
    std::vector<HookEntry> entries;
    for (const hooks::Program& item : items_) {
        entries.push_back({item.id, item.name, item.enabled});
    }
    return entries;
}

void ProgramSection::showInEditor(const std::string& itemId, bool saved) {
    if (const hooks::Program* item = saved ? find(itemId) : nullptr) {
        programEditor()->showItem(*item, true);
    } else {
        hooks::Program blank;
        blank.id = hooks::new_id();
        blank.events = hooks_format::default_events();
        programEditor()->showItem(blank, false);
    }
}

bool ProgramSection::changedElsewhere(const std::string& itemId) const {
    const hooks::Program* item = find(itemId);
    const auto& source = programEditor()->source();
    return item != nullptr && source && !(*item == *source);
}

HookSection::Draft ProgramSection::prepareDraft() {
    hooks::Program draft = programEditor()->draft();
    if (const hooks::Program* saved = find(draft.id)) {
        draft.enabled = saved->enabled;
    }
    return {draft.id, [draft](Json& settings) {
                hooks::save_program(settings, draft);
            }};
}

std::function<void(Json&)> ProgramSection::enabledApplier(const std::string& itemId, bool enabled) {
    const hooks::Program* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    hooks::Program changed = *item;
    changed.enabled = enabled;
    return [changed](Json& settings) {
        hooks::save_program(settings, changed);
    };
}

void ProgramSection::removeFromSettings(Json& settings, const std::string& itemId) {
    hooks::delete_program(settings, itemId);
}

std::optional<automation::RunRecord> ProgramSection::last(const std::string& itemId) const {
    if (services().programs == nullptr) {
        return std::nullopt;
    }
    return newest<automation::RunRecord>([&] { return services().programs->runs(itemId); },
                                         "last program run");
}

QString ProgramSection::detail(const std::string& itemId) {
    const hooks::Program* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    const std::string name = baseName(item->program);
    std::string text =
        (name.empty() ? item->program : name) + " · " + hooks_format::events_summary(item->events);
    if (const auto record = last(itemId)) {
        text += " · Last run " + hooks_format::run_result(*record).text;
    }
    return q(text);
}

EngineStatus ProgramSection::status(const std::string& itemId) {
    const auto record = last(itemId);
    if (!record) {
        return EngineStatus::Unknown;
    }
    return record->ok ? EngineStatus::Ready : EngineStatus::Attention;
}

QString ProgramSection::deleteQuestion(const HookEntry& item) {
    return QStringLiteral("Delete “%1”? %2 stops running it.")
        .arg(q(item.name), q(identity().product));
}

// ---- leases ----

LeaseList::LeaseList(const HookServices& services, QWidget* parent)
    : QWidget(parent), services_(services) {
    setObjectName(QStringLiteral("hooksLeases"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("Added by your programs"));
    title->setObjectName(QStringLiteral("hooksSectionLabel"));
    layout->addWidget(title);
    rows_ = new QWidget;
    rows_->setObjectName(QStringLiteral("hooksLeaseRows"));
    rowsLayout_ = new QVBoxLayout(rows_);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(4);
    layout->addWidget(rows_);
    note_ = hooks_panel::hint(
        QStringLiteral("A program on this computer can add a filter for a minute at a time "
                       "through the live API, once you allow Change dictated text in API & MCP."));
    layout->addWidget(note_);
    openApi_ = new QPushButton(QStringLiteral("Open API && MCP"));
    // Its own style: settingsPageLink's min-height: 0 lets a page laid out at its minimum height
    // squeeze the link to a sliver.
    openApi_->setObjectName(QStringLiteral("hooksLink"));
    openApi_->setFlat(true);
    openApi_->setCursor(Qt::PointingHandCursor);
    connect(openApi_, &QPushButton::clicked, this, [this] { Q_EMIT openApiRequested(); });
    layout->addWidget(openApi_, 0, Qt::AlignLeft);
}

QLabel* LeaseList::lineDetail(QFrame* line) const {
    const auto found = details_.find(line);
    return found == details_.end() ? nullptr : found->second;
}

void LeaseList::refresh() {
    std::vector<automation::Lease> leases;
    if (services_.leases != nullptr) {
        try {
            leases = services_.leases->leases();
        } catch (const std::exception&) {
            kLog.debug("Could not read text filter leases");
        }
    }
    const double now =
        std::chrono::duration<double>(clockOf(services_).utc().time_since_epoch()).count();
    std::vector<Shown> shown;
    for (const automation::Lease& lease : leases) {
        if (lease.expires_at > now) {
            shown.emplace_back(lease.id, lease.url, lease.stage,
                               std::max<long long>(0, static_cast<long long>(
                                                          std::nearbyint(lease.expires_at - now))));
        }
    }
    if (shown_ && *shown_ == shown) {
        return;
    }
    shown_ = shown;
    // Rows are rebuilt only when the leases change; the countdown updates in place.
    std::vector<std::tuple<std::string, std::string, std::string>> now3;
    for (const Shown& entry : shown) {
        now3.emplace_back(std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
    }
    if (now3 != lineLeases_) {
        while (rowsLayout_->count() > 0) {
            QLayoutItem* item = rowsLayout_->takeAt(0);
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
        lines_.clear();
        details_.clear();
        lineLeases_ = now3;
        for (const Shown& entry : shown) {
            auto* line = new QFrame;
            line->setObjectName(QStringLiteral("hooksLeaseRow"));
            auto* row = new QHBoxLayout(line);
            row->setContentsMargins(10, 6, 10, 6);
            row->setSpacing(10);
            auto* address = new ElidingLabel(q(std::get<1>(entry)));
            address->setObjectName(QStringLiteral("hooksRowName"));
            row->addWidget(address, 1);
            auto* detail = new QLabel;
            detail->setObjectName(QStringLiteral("hooksRowDetail"));
            row->addWidget(detail);
            rowsLayout_->addWidget(line);
            lines_.push_back(line);
            details_[line] = detail;
        }
    }
    for (std::size_t i = 0; i < lines_.size() && i < shown.size(); ++i) {
        details_[lines_[i]]->setText(QStringLiteral("%1 · ends in %2 s")
                                         .arg(q(hooks_format::stage_label(std::get<2>(shown[i]))))
                                         .arg(std::get<3>(shown[i])));
    }
    rows_->setVisible(!shown.empty());
    note_->setVisible(shown.empty());
    openApi_->setVisible(shown.empty());
}

// ---- text filters ----

FilterSection::FilterSection(const HookServices& services, QWidget* parent)
    : HookSection(
          services,
          {QStringLiteral("filter"), hooks::MAX_FILTERS, QStringLiteral("No text filters yet"),
           QStringLiteral("Add one to let your own tool fix words before they're pasted."), false},
          new FilterEditor(services), parent) {
    leases_ = new LeaseList(services);
    rootLayout()->addWidget(leases_);
    refresh();
}

FilterEditor* FilterSection::filterEditor() const {
    return static_cast<FilterEditor*>(editor());
}

const hooks::TextFilter* FilterSection::find(const std::string& itemId) const {
    return findById(items_, itemId);
}

std::vector<HookEntry> FilterSection::loadItems(const Json& settings) {
    items_ = hooks::load_filters(settings);
    std::vector<HookEntry> entries;
    for (const hooks::TextFilter& item : items_) {
        entries.push_back({item.id, item.name, item.enabled});
    }
    return entries;
}

void FilterSection::showInEditor(const std::string& itemId, bool saved) {
    if (const hooks::TextFilter* item = saved ? find(itemId) : nullptr) {
        filterEditor()->showItem(*item, true);
    } else {
        hooks::TextFilter blank;
        blank.id = hooks::new_id();
        blank.kind = std::string(hooks::FILTER_HTTP);
        blank.stage = std::string(hooks::AFTER_CLEANUP);
        filterEditor()->showItem(blank, false);
    }
}

bool FilterSection::changedElsewhere(const std::string& itemId) const {
    const hooks::TextFilter* item = find(itemId);
    const auto& source = filterEditor()->source();
    return item != nullptr && source && !(*item == *source);
}

HookSection::Draft FilterSection::prepareDraft() {
    hooks::TextFilter draft = filterEditor()->draft();
    if (const hooks::TextFilter* saved = find(draft.id)) {
        draft.enabled = saved->enabled;
    }
    return {draft.id, [draft](Json& settings) {
                hooks::save_filter(settings, draft);
            }};
}

std::function<void(Json&)> FilterSection::enabledApplier(const std::string& itemId, bool enabled) {
    const hooks::TextFilter* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    hooks::TextFilter changed = *item;
    changed.enabled = enabled;
    return [changed](Json& settings) {
        hooks::save_filter(settings, changed);
    };
}

void FilterSection::removeFromSettings(Json& settings, const std::string& itemId) {
    hooks::delete_filter(settings, itemId);
}

QString FilterSection::detail(const std::string& itemId) {
    const hooks::TextFilter* item = find(itemId);
    if (item == nullptr) {
        return {};
    }
    std::string target;
    if (item->kind == hooks::FILTER_HTTP) {
        target = hooks_format::host_of(item->url);
    } else {
        target = baseName(item->program);
        if (target.empty()) {
            target = item->program;
        }
    }
    return q(target + " · " + hooks_format::stage_label(item->stage) + " · " +
             std::to_string(item->timeout_ms) + " ms");
}

QString FilterSection::deleteQuestion(const HookEntry& item) {
    return QStringLiteral("Delete “%1”? Your words will no longer pass through it.")
        .arg(q(item.name));
}

void FilterSection::refreshLive() {
    HookSection::refreshLive();
    leases_->refresh();
}

} // namespace fv
