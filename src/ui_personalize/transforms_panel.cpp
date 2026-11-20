#include "ui_personalize/transforms_panel.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <set>
#include <typeinfo>
#include <utility>

#include <QBoxLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/ids.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/store.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.command_settings"};

constexpr int kIdRole = Qt::UserRole;
// QWIDGETSIZE_MAX: no maximum.
constexpr int kUnlimited = 16777215;

QString displayHotkey(const std::string& hotkey) {
    return qt::toQString(hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary()));
}

QLabel* fieldLabel(const QString& text, QWidget* buddy) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("textModelFieldLabel"));
    label->setBuddy(buddy);
    return label;
}

text::Transform blankTransform() {
    return {uuid4_hex(), "", "", ""};
}

} // namespace

TransformsPanel::TransformsPanel(SettingsStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    question = [](QWidget* parent, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
        return QMessageBox::question(parent, title, text, buttons, defaultButton);
    };
    setObjectName(QStringLiteral("transformsPanel"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    columns_ = new QHBoxLayout;
    columns_->setSpacing(18);
    root->addLayout(columns_, 1);
    library_ = new QWidget;
    library_->setObjectName(QStringLiteral("transformLibrary"));
    library_->setFixedWidth(kLibraryWidth);
    auto* libraryLayout = new QVBoxLayout(library_);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(8);
    columns_->addWidget(library_);
    editor_ = new QWidget;
    editor_->setObjectName(QStringLiteral("transformEditor"));
    editor_->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(editor_);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);
    columns_->addWidget(editor_, 1);

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("transformsList"));
    list_->setAccessibleName(QStringLiteral("Transforms"));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumHeight(120);
    connect(list_, &QListWidget::currentItemChanged, this, &TransformsPanel::selectionChanged);
    libraryLayout->addWidget(list_, 1);
    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(8);
    newButton_ = compactPrimaryButton(new Button(QStringLiteral("New transform")));
    duplicateButton_ = neutralButton(new Button(QStringLiteral("Duplicate")));
    deleteButton_ = neutralButton(new Button(QStringLiteral("Delete")));
    const struct {
        Button* button;
        void (TransformsPanel::*callback)();
        int row;
        int column;
        int span;
    } placed[] = {
        {newButton_, &TransformsPanel::newTransform, 0, 0, 2},
        {duplicateButton_, &TransformsPanel::duplicateTransform, 1, 0, 1},
        {deleteButton_, &TransformsPanel::deleteTransform, 1, 1, 1},
    };
    for (const auto& entry : placed) {
        entry.button->setBaseMinimumSize(0, 34);
        connect(entry.button, &QPushButton::clicked, this, entry.callback);
        actions->addWidget(entry.button, entry.row, entry.column, 1, entry.span);
    }
    libraryLayout->addLayout(actions);

    nameEdit_ = new QLineEdit;
    nameEdit_->setMaxLength(static_cast<int>(text::NAME_MAX_CHARS));
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. Make it friendlier"));
    layout->addWidget(fieldLabel(QStringLiteral("Name"), nameEdit_));
    layout->addWidget(nameEdit_);
    instructionEdit_ = new QTextEdit;
    instructionEdit_->setAcceptRichText(false);
    instructionEdit_->setMinimumHeight(110);
    instructionEdit_->setPlaceholderText(
        QStringLiteral("Say how to change the selected text, e.g. “Make it warmer and shorter. "
                       "Keep every date.”"));
    layout->addWidget(fieldLabel(QStringLiteral("Instruction"), instructionEdit_));
    layout->addWidget(instructionEdit_, 1);
    auto* startFrom = new QLabel(QStringLiteral("Start from"));
    startFrom->setObjectName(QStringLiteral("textModelFieldLabel"));
    layout->addWidget(startFrom);
    std::vector<QPushButton*> starters;
    for (const text::Transform& starter : text::starter_transforms()) {
        auto* button = neutralButton(new Button(qt::toQString(starter.name)));
        // The page lays out at its minimum height, so the floor is the label's.
        fitCompactButton(button);
        button->setToolTip(qt::toQString(starter.instruction));
        connect(button, &QPushButton::clicked, this, [this, starter] { useStarter(starter); });
        starters.push_back(button);
    }
    starterRow_ = new ButtonRow(starters, false);
    ui::setStyleProperty(starterRow_, ui::prop::tileId, QStringLiteral("transformStarters"));
    layout->addWidget(starterRow_);

    hotkeyInput_ = new ProfileHotkeyInput;
    hotkeyInput_->setAccessibleName(QStringLiteral("Transform shortcut"));
    hotkeyInput_->setToolTip(QStringLiteral("Click, then press a shortcut. Escape cancels."));
    connect(hotkeyInput_, &ProfileHotkeyInput::captureChanged, this,
            &TransformsPanel::onCaptureChanged);
    connect(hotkeyInput_, &ProfileHotkeyInput::captured, this, &TransformsPanel::onHotkeyCaptured);
    layout->addWidget(fieldLabel(QStringLiteral("Shortcut (optional)"), hotkeyInput_));
    auto* shortcut = new QHBoxLayout;
    shortcut->addWidget(hotkeyInput_, 1);
    auto* clear = neutralButton(new Button(QStringLiteral("Clear")));
    clear->setBaseMinimumSize(64, 34);
    connect(clear, &QPushButton::clicked, this, [this] { hotkeyInput_->setHotkey(QString()); });
    shortcut->addWidget(clear);
    layout->addLayout(shortcut);
    auto* hint =
        new WrappedLabel(QStringLiteral("Select text in any app, then press the shortcut to "
                                        "rewrite it."));
    hint->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(hint);

    message_ = new WrappedLabel;
    message_->setTextFormat(Qt::PlainText);
    message_->setObjectName(QStringLiteral("infoLabel"));
    root->addWidget(message_);
    auto* footer = new QHBoxLayout;
    footer->addStretch();
    saveButton_ = compactPrimaryButton(new Button(QStringLiteral("Save transform")));
    fitCompactButton(saveButton_, 132);
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveTransform(); });
    footer->addWidget(saveButton_);
    root->addLayout(footer);
    refresh();
}

TransformsPanel::~TransformsPanel() = default;

void TransformsPanel::setActionBindings(std::function<std::vector<hotkeys::Binding>()> bindings) {
    actions_ = std::move(bindings);
}

int TransformsPanel::libraryWidth() const {
    // Wider fonts (a large scale) need more than the default for Duplicate and Delete to keep
    // their gap.
    int buttons = 0;
    for (const Button* button : {duplicateButton_, deleteButton_}) {
        buttons += std::max(button->minimumWidth(), button->sizeHint().width());
    }
    return std::max(kLibraryWidth, buttons + 8);
}

void TransformsPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int library = libraryWidth();
    const bool narrow =
        width() < library + columns_->spacing() + editor_->minimumSizeHint().width();
    columns_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    library_->setMinimumWidth(narrow ? 0 : library);
    library_->setMaximumWidth(narrow ? kUnlimited : library);
    list_->setMaximumHeight(narrow ? 170 : kUnlimited);
}

Json TransformsPanel::settings() const {
    try {
        return *store_.snapshot();
    } catch (const std::exception& error) {
        kLog.warning("Could not load settings for the transforms ({})", typeid(error).name());
        return Json::object();
    }
}

text::ShortcutContext TransformsPanel::shortcutContext() const {
    text::ShortcutContext context;
    if (actions_) {
        context.actions = actions_();
    }
    return context;
}

text::Transform TransformsPanel::draft() const {
    return {transformId_, py::strip(qt::toUtf8(nameEdit_->text())),
            py::strip(qt::toUtf8(instructionEdit_->toPlainText())),
            qt::toUtf8(hotkeyInput_->hotkey())};
}

bool TransformsPanel::hasUnsavedChanges() const {
    return saved_.has_value() && draft() != *saved_;
}

bool TransformsPanel::saveBeforeSwitch() {
    if (!hasUnsavedChanges()) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved transform"),
                 QStringLiteral("Save your changes to this transform?"),
                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return saveTransform();
    }
    return answer == QMessageBox::Discard;
}

bool TransformsPanel::saveDraft(bool ask) {
    if (!hasUnsavedChanges() || saveTransform() || !ask) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved transform"),
                 QStringLiteral("%1 Discard your changes to this transform?").arg(message_->text()),
                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return false;
    }
    refresh(transformId_);
    return true;
}

void TransformsPanel::refresh(std::optional<std::string> selectedId) {
    // Coming back to Settings must not throw away a draft in progress.
    if (!selectedId.has_value() && hasUnsavedChanges()) {
        followSavedHotkey();
        return;
    }
    const std::string wanted = selectedId.value_or(transformId_);
    loading_ = true;
    struct Done {
        bool& loading;
        ~Done() { loading = false; }
    } done{loading_};
    list_->clear();
    const std::vector<text::Transform> transforms = text::load_transforms(settings());
    QListWidgetItem* selected = nullptr;
    const text::Transform* shown = nullptr;
    for (const text::Transform& transform : transforms) {
        QString label = qt::toQString(transform.name);
        if (!transform.hotkey.empty()) {
            label += QStringLiteral("\n") + displayHotkey(transform.hotkey);
        }
        auto* item = new QListWidgetItem(label);
        item->setToolTip(qt::toQString(transform.instruction));
        item->setData(kIdRole, qt::toQString(transform.id));
        list_->addItem(item);
        if (transform.id == wanted) {
            selected = item;
            shown = &transform;
        }
    }
    if (!transforms.empty()) {
        if (selected == nullptr) {
            selected = list_->item(0);
            shown = &transforms.front();
        }
        list_->setCurrentItem(selected);
        showTransform(*shown);
    } else {
        showTransform(blankTransform());
    }
}

void TransformsPanel::followSavedHotkey() {
    const auto found = text::find_transform(settings(), transformId_);
    if (!found.has_value() || found->hotkey == saved_->hotkey) {
        return;
    }
    if (qt::toUtf8(hotkeyInput_->hotkey()) == saved_->hotkey) {
        hotkeyInput_->setHotkey(qt::toQString(found->hotkey));
    }
    saved_->hotkey = found->hotkey;
}

void TransformsPanel::showTransform(const text::Transform& transform) {
    hotkeyInput_->cancelCapture();
    transformId_ = transform.id;
    nameEdit_->setText(qt::toQString(transform.name));
    instructionEdit_->setPlainText(qt::toQString(transform.instruction));
    hotkeyInput_->setHotkey(qt::toQString(transform.hotkey));
    saved_ = transform;
    const std::vector<text::Transform> transforms = text::load_transforms(settings());
    const bool exists = std::ranges::any_of(transforms, [&transform](const text::Transform& known) {
        return known.id == transform.id;
    });
    deleteButton_->setEnabled(exists);
    duplicateButton_->setEnabled(!transform.name.empty());
    message_->setText(QString());
}

void TransformsPanel::selectionChanged(QListWidgetItem* current, QListWidgetItem* previous) {
    if (loading_ || current == nullptr) {
        return;
    }
    // Saving first rebuilds the list, which deletes `current`.
    const std::string transformId = qt::toUtf8(current->data(kIdRole).toString());
    if (!saveBeforeSwitch()) {
        const QSignalBlocker blocker(list_);
        list_->setCurrentItem(previous);
        return;
    }
    refresh(transformId);
}

void TransformsPanel::startDraft(const text::Transform& transform) {
    {
        const QSignalBlocker blocker(list_);
        list_->setCurrentRow(-1);
    }
    showTransform(transform);
}

void TransformsPanel::newTransform() {
    if (saveBeforeSwitch()) {
        startDraft(blankTransform());
        nameEdit_->setFocus();
    }
}

void TransformsPanel::duplicateTransform() {
    const text::Transform source = draft();
    if (!saveBeforeSwitch()) {
        return;
    }
    std::set<std::string> names;
    for (const text::Transform& transform : text::load_transforms(settings())) {
        names.insert(py::casefold(transform.name));
    }
    const std::string stem =
        py::slice(source.name, 0, static_cast<std::int64_t>(text::NAME_MAX_CHARS) - 8);
    std::string name = stem + " copy";
    for (int suffix = 2; names.contains(py::casefold(name)); ++suffix) {
        name = stem + " copy " + std::to_string(suffix);
    }
    startDraft({uuid4_hex(), name, source.instruction, ""});
    // A duplicate is a draft until it is saved.
    saved_ = text::Transform{transformId_, "", "", ""};
}

void TransformsPanel::useStarter(const text::Transform& starter) {
    if (!py::strip(qt::toUtf8(instructionEdit_->toPlainText())).empty()) {
        const QMessageBox::StandardButton answer =
            question(this, QStringLiteral("Use this starter?"),
                     QStringLiteral("Replace the current instruction?"),
                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    if (py::strip(qt::toUtf8(nameEdit_->text())).empty()) {
        nameEdit_->setText(qt::toQString(starter.name));
    }
    instructionEdit_->setPlainText(qt::toQString(starter.instruction));
}

void TransformsPanel::cancelCapture() {
    hotkeyInput_->cancelCapture();
}

void TransformsPanel::onCaptureChanged(bool capturing) {
    if (capturing) {
        hotkeyBeforeCapture_ = hotkeyInput_->hotkey();
    }
    Q_EMIT captureChanged(capturing);
}

void TransformsPanel::onHotkeyCaptured(const QString& hotkey) {
    const std::string conflict = text::transform_hotkey_conflict(qt::toUtf8(hotkey), settings(),
                                                                 transformId_, shortcutContext());
    if (!conflict.empty()) {
        hotkeyInput_->setHotkey(hotkeyBeforeCapture_);
        message_->setText(QStringLiteral("That shortcut is already used by %1. Choose another.")
                              .arg(qt::toQString(conflict)));
    } else {
        message_->setText(QString());
    }
}

bool TransformsPanel::saveTransform() {
    hotkeyInput_->cancelCapture();
    const text::Transform transform = draft();
    const text::ShortcutContext context = shortcutContext();
    try {
        store_.mutate([&transform, &context](Json& saved) {
            text::save_transform(saved, transform, context);
        });
    } catch (const py::ValueError& refused) {
        // A validation message.
        message_->setText(qt::toQString(refused.what()));
        return false;
    } catch (const std::exception& error) {
        kLog.warning("Could not save the transform ({})", typeid(error).name());
        message_->setText(qt::toQString(error.what()));
        return false;
    }
    saved_ = transform;
    refresh(transform.id);
    const QString shortcut = displayHotkey(transform.hotkey);
    message_->setText(shortcut.isEmpty()
                          ? QStringLiteral("Saved %1.").arg(qt::toQString(transform.name))
                          : QStringLiteral("Saved %1. Select text anywhere and press %2.")
                                .arg(qt::toQString(transform.name), shortcut));
    Q_EMIT transformsChanged();
    return true;
}

void TransformsPanel::deleteTransform() {
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Delete transform?"),
                 QStringLiteral("Delete this transform and its shortcut?"),
                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const std::string transformId = transformId_;
    try {
        store_.mutate([&transformId](Json& saved) { text::delete_transform(saved, transformId); });
    } catch (const std::exception& error) {
        kLog.warning("Could not delete the transform ({})", typeid(error).name());
        message_->setText(qt::toQString(error.what()));
        return;
    }
    saved_.reset();
    refresh(std::string());
    Q_EMIT transformsChanged();
}

} // namespace fv
