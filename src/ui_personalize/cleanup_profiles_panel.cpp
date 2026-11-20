#include "ui_personalize/cleanup_profiles_panel.h"

#include <algorithm>
#include <exception>
#include <set>
#include <typeinfo>
#include <utility>

#include <QBoxLayout>
#include <QCheckBox>
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

constexpr log::Logger kLog{"ui_qt.widgets.cleanup_profiles_panel"};

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

text::CleanupProfile blankProfile() {
    return {uuid4_hex(), "", "", "", true};
}

} // namespace

CleanupProfilesPanel::CleanupProfilesPanel(SettingsStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    question = [](QWidget* parent, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
        return QMessageBox::question(parent, title, text, buttons, defaultButton);
    };
    setObjectName(QStringLiteral("cleanupProfilesPanel"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);
    auto* intro = new WrappedLabel(QStringLiteral(
        "Choose a profile on Quick Record, or give it a dedicated recording shortcut. Profiles "
        "always run AI cleanup using your configured text model. Standard dictation and uploads "
        "keep their usual settings."));
    intro->setObjectName(QStringLiteral("infoLabel"));
    root->addWidget(intro);

    columns_ = new QHBoxLayout;
    columns_->setSpacing(18);
    root->addLayout(columns_, 1);
    library_ = new QWidget;
    library_->setObjectName(QStringLiteral("cleanupProfileLibrary"));
    library_->setFixedWidth(kLibraryWidth);
    auto* libraryLayout = new QVBoxLayout(library_);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(8);
    columns_->addWidget(library_);
    editor_ = new QWidget;
    editor_->setObjectName(QStringLiteral("cleanupProfileEditor"));
    editor_->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(editor_);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);
    columns_->addWidget(editor_, 1);

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("cleanupProfilesList"));
    list_->setAccessibleName(QStringLiteral("Cleanup profiles"));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumHeight(95);
    connect(list_, &QListWidget::currentItemChanged, this, &CleanupProfilesPanel::selectionChanged);
    libraryLayout->addWidget(list_, 1);
    // "New profile" is the one action most visits want; the other two sit side by side beneath
    // it instead of three stacked full-width blocks.
    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(8);
    newButton_ = compactPrimaryButton(new Button(QStringLiteral("New profile")));
    duplicateButton_ = neutralButton(new Button(QStringLiteral("Duplicate")));
    deleteButton_ = neutralButton(new Button(QStringLiteral("Delete")));
    const struct {
        Button* button;
        void (CleanupProfilesPanel::*callback)();
        int row;
        int column;
        int span;
    } placed[] = {
        {newButton_, &CleanupProfilesPanel::newProfile, 0, 0, 2},
        {duplicateButton_, &CleanupProfilesPanel::duplicateProfile, 1, 0, 1},
        {deleteButton_, &CleanupProfilesPanel::deleteProfile, 1, 1, 1},
    };
    for (const auto& entry : placed) {
        entry.button->setBaseMinimumSize(0, 34);
        connect(entry.button, &QPushButton::clicked, this, entry.callback);
        actions->addWidget(entry.button, entry.row, entry.column, 1, entry.span);
    }
    libraryLayout->addLayout(actions);

    nameEdit_ = new QLineEdit;
    nameEdit_->setMaxLength(kNameMaxChars);
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. Support ticket, Email, Release notes"));
    layout->addWidget(fieldLabel(QStringLiteral("Name"), nameEdit_));
    layout->addWidget(nameEdit_);
    instructionsEdit_ = new QTextEdit;
    instructionsEdit_->setAcceptRichText(false);
    instructionsEdit_->setMinimumHeight(135);
    instructionsEdit_->setPlaceholderText(QStringLiteral(
        "Describe the structure, tone, headings, and details you want in the finished text…"));
    layout->addWidget(fieldLabel(QStringLiteral("Output instructions"), instructionsEdit_));
    layout->addWidget(instructionsEdit_, 1);
    auto* startFrom = new QLabel(QStringLiteral("Start from"));
    startFrom->setObjectName(QStringLiteral("textModelFieldLabel"));
    layout->addWidget(startFrom);
    std::vector<QPushButton*> templates;
    for (const text::CleanupProfile& starter : text::starter_profiles()) {
        auto* button = neutralButton(new Button(qt::toQString(starter.name)));
        fitCompactButton(button);
        connect(button, &QPushButton::clicked, this, [this, starter] { useTemplate(starter); });
        templates.push_back(button);
    }
    // The same chip row as the transform editor's starters.
    templateRow_ = new ButtonRow(templates, false);
    ui::setStyleProperty(templateRow_, ui::prop::tileId, QStringLiteral("transformStarters"));
    layout->addWidget(templateRow_);
    rulesCheck_ = new QCheckBox(QStringLiteral("Also apply my learned rules"));
    layout->addWidget(rulesCheck_);

    hotkeyInput_ = new ProfileHotkeyInput;
    connect(hotkeyInput_, &ProfileHotkeyInput::captureChanged, this,
            &CleanupProfilesPanel::captureChanged);
    layout->addWidget(fieldLabel(QStringLiteral("Recording shortcut (optional)"), hotkeyInput_));
    auto* shortcut = new QHBoxLayout;
    shortcut->addWidget(hotkeyInput_, 1);
    auto* clear = neutralButton(new Button(QStringLiteral("Clear")));
    clear->setBaseMinimumSize(60, 34);
    connect(clear, &QPushButton::clicked, this, [this] { hotkeyInput_->setHotkey(QString()); });
    shortcut->addWidget(clear);
    layout->addLayout(shortcut);
    auto* hint = new WrappedLabel(QStringLiteral(
        "Press once to start, again to stop. Profile shortcuts use toggle mode; the standard "
        "recording shortcut keeps its own mode."));
    hint->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(hint);

    message_ = new WrappedLabel;
    message_->setTextFormat(Qt::PlainText);
    message_->setObjectName(QStringLiteral("infoLabel"));
    root->addWidget(message_);
    auto* footer = new QHBoxLayout;
    modelButton_ = neutralButton(new Button(QStringLiteral("Choose cleanup model…")));
    connect(modelButton_, &QPushButton::clicked, this, &CleanupProfilesPanel::modelRequested);
    footer->addWidget(modelButton_);
    footer->addStretch();
    saveButton_ = compactPrimaryButton(new Button(QStringLiteral("Save profile")));
    fitCompactButton(saveButton_, 132);
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveProfile(); });
    footer->addWidget(saveButton_);
    root->addLayout(footer);
    refresh();
}

CleanupProfilesPanel::~CleanupProfilesPanel() = default;

void CleanupProfilesPanel::setActionBindings(
    std::function<std::vector<hotkeys::Binding>()> bindings) {
    actions_ = std::move(bindings);
}

void CleanupProfilesPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool narrow =
        width() < kLibraryWidth + columns_->spacing() + editor_->minimumSizeHint().width();
    columns_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    library_->setMinimumWidth(narrow ? 0 : kLibraryWidth);
    library_->setMaximumWidth(narrow ? kUnlimited : kLibraryWidth);
    list_->setMaximumHeight(narrow ? 160 : kUnlimited);
}

Json CleanupProfilesPanel::settings() const {
    try {
        return *store_.snapshot();
    } catch (const std::exception& error) {
        kLog.warning("Could not load settings for the cleanup profiles ({})", typeid(error).name());
        return Json::object();
    }
}

text::CleanupProfile CleanupProfilesPanel::draft() const {
    return {profileId_, py::strip(qt::toUtf8(nameEdit_->text())),
            py::strip(qt::toUtf8(instructionsEdit_->toPlainText())),
            qt::toUtf8(hotkeyInput_->hotkey()), rulesCheck_->isChecked()};
}

bool CleanupProfilesPanel::hasUnsavedChanges() const {
    return saved_.has_value() && draft() != *saved_;
}

bool CleanupProfilesPanel::saveDraft(bool ask) {
    if (!hasUnsavedChanges() || saveProfile() || !ask) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved profile"),
                 QStringLiteral("%1 Discard your changes to this profile?").arg(message_->text()),
                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return false;
    }
    refresh(profileId_);
    return true;
}

bool CleanupProfilesPanel::saveBeforeSwitch() {
    if (!hasUnsavedChanges()) {
        return true;
    }
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Unsaved profile"),
                 QStringLiteral("Save your changes to this profile?"),
                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return saveProfile();
    }
    return answer == QMessageBox::Discard;
}

void CleanupProfilesPanel::refresh(std::optional<std::string> selectedId) {
    // Raising Settings must not discard an in-progress draft.
    if (!selectedId.has_value() && hasUnsavedChanges()) {
        followSavedHotkey();
        return;
    }
    const std::string wanted = selectedId.value_or(profileId_);
    loading_ = true;
    struct Done {
        bool& loading;
        ~Done() { loading = false; }
    } done{loading_};
    list_->clear();
    const std::vector<text::CleanupProfile> profiles = text::load_cleanup_profiles(settings());
    QListWidgetItem* selected = nullptr;
    const text::CleanupProfile* shown = nullptr;
    for (const text::CleanupProfile& profile : profiles) {
        QString label = qt::toQString(profile.name);
        if (!profile.hotkey.empty()) {
            label += QStringLiteral("\n") + displayHotkey(profile.hotkey);
        }
        auto* item = new QListWidgetItem(label);
        item->setToolTip(label);
        item->setData(kIdRole, qt::toQString(profile.id));
        list_->addItem(item);
        if (profile.id == wanted) {
            selected = item;
            shown = &profile;
        }
    }
    if (!profiles.empty()) {
        if (selected == nullptr) {
            selected = list_->item(0);
            shown = &profiles.front();
        }
        list_->setCurrentItem(selected);
        showProfile(*shown);
    } else {
        showProfile(blankProfile());
    }
}

void CleanupProfilesPanel::followSavedHotkey() {
    const auto found = text::find_cleanup_profile(settings(), profileId_);
    if (!found.has_value() || found->hotkey == saved_->hotkey) {
        return;
    }
    if (qt::toUtf8(hotkeyInput_->hotkey()) == saved_->hotkey) {
        hotkeyInput_->setHotkey(qt::toQString(found->hotkey));
    }
    saved_->hotkey = found->hotkey;
}

void CleanupProfilesPanel::showProfile(const text::CleanupProfile& profile) {
    hotkeyInput_->cancelCapture();
    profileId_ = profile.id;
    nameEdit_->setText(qt::toQString(profile.name));
    instructionsEdit_->setPlainText(qt::toQString(profile.instructions));
    hotkeyInput_->setHotkey(qt::toQString(profile.hotkey));
    rulesCheck_->setChecked(profile.use_learned_rules);
    saved_ = profile;
    const std::vector<text::CleanupProfile> profiles = text::load_cleanup_profiles(settings());
    const bool exists = std::ranges::any_of(
        profiles, [&profile](const text::CleanupProfile& known) { return known.id == profile.id; });
    deleteButton_->setEnabled(exists);
    duplicateButton_->setEnabled(!profile.name.empty());
    message_->setText(QString());
}

void CleanupProfilesPanel::selectionChanged(QListWidgetItem* current, QListWidgetItem* previous) {
    if (loading_ || current == nullptr) {
        return;
    }
    // Saving first rebuilds the list, which deletes `current`.
    const std::string profileId = qt::toUtf8(current->data(kIdRole).toString());
    if (!saveBeforeSwitch()) {
        const QSignalBlocker blocker(list_);
        list_->setCurrentItem(previous);
        return;
    }
    refresh(profileId);
}

void CleanupProfilesPanel::startDraft(const text::CleanupProfile& profile) {
    {
        const QSignalBlocker blocker(list_);
        list_->setCurrentRow(-1);
    }
    showProfile(profile);
}

void CleanupProfilesPanel::newProfile() {
    if (saveBeforeSwitch()) {
        startDraft(blankProfile());
        nameEdit_->setFocus();
    }
}

void CleanupProfilesPanel::duplicateProfile() {
    const text::CleanupProfile source = draft();
    if (!saveBeforeSwitch()) {
        return;
    }
    std::set<std::string> names;
    for (const text::CleanupProfile& profile : text::load_cleanup_profiles(settings())) {
        names.insert(py::casefold(profile.name));
    }
    const std::string stem = py::slice(source.name, 0, 65);
    std::string name = stem + " copy";
    for (int suffix = 2; names.contains(py::casefold(name)); ++suffix) {
        name = stem + " copy " + std::to_string(suffix);
    }
    startDraft({uuid4_hex(), name, source.instructions, "", source.use_learned_rules});
    // A duplicate is a draft until saved.
    saved_ = text::CleanupProfile{profileId_, "", "", "", true};
}

void CleanupProfilesPanel::useTemplate(const text::CleanupProfile& profile) {
    if (!py::strip(qt::toUtf8(instructionsEdit_->toPlainText())).empty()) {
        const QMessageBox::StandardButton answer =
            question(this, QStringLiteral("Use starter instructions?"),
                     QStringLiteral("Replace the current output instructions?"),
                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    if (py::strip(qt::toUtf8(nameEdit_->text())).empty()) {
        nameEdit_->setText(qt::toQString(profile.name));
    }
    instructionsEdit_->setPlainText(qt::toQString(profile.instructions));
}

void CleanupProfilesPanel::cancelCapture() {
    hotkeyInput_->cancelCapture();
}

bool CleanupProfilesPanel::saveProfile() {
    hotkeyInput_->cancelCapture();
    const text::CleanupProfile profile = draft();
    text::ShortcutContext context;
    if (actions_) {
        context.actions = actions_();
    }
    try {
        store_.mutate([&profile, &context](Json& saved) {
            text::save_cleanup_profile(saved, profile, context);
        });
    } catch (const py::ValueError& refused) {
        // A validation message.
        message_->setText(qt::toQString(refused.what()));
        return false;
    } catch (const std::exception& error) {
        kLog.warning("Could not save the cleanup profile ({})", typeid(error).name());
        message_->setText(qt::toQString(error.what()));
        return false;
    }
    saved_ = profile;
    refresh(profile.id);
    message_->setText(
        QStringLiteral("Saved %1. Available on Quick Record.").arg(qt::toQString(profile.name)));
    Q_EMIT profilesChanged();
    return true;
}

void CleanupProfilesPanel::deleteProfile() {
    const QMessageBox::StandardButton answer =
        question(this, QStringLiteral("Delete profile?"),
                 QStringLiteral("Delete this profile and its recording shortcut?"),
                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const std::string profileId = profileId_;
    try {
        store_.mutate(
            [&profileId](Json& saved) { text::delete_cleanup_profile(saved, profileId); });
    } catch (const std::exception& error) {
        kLog.warning("Could not delete the cleanup profile ({})", typeid(error).name());
        message_->setText(qt::toQString(error.what()));
        return;
    }
    refresh(std::string());
    Q_EMIT profilesChanged();
}

} // namespace fv
