#include "ui_hotkeys/hotkey_bindings.h"

#include <exception>
#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "hotkeys/mouse_shortcuts.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/store.h"
#include "ui_hotkeys/hotkey_row.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.hotkey_bindings"};

QString display(const std::string& hotkey) {
    return qt::toQString(hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary()));
}

} // namespace

namespace hotkey_bindings {

const std::array<Group, 3>& groups() {
    static const std::array<Group, 3> table{{
        {hotkeys::kProfile, "Profiles", destinations::cleanup_profiles, "Open Profiles",
         "Press a profile's shortcut to record and again to stop; what you say comes out in its "
         "format.",
         "No profiles yet. A profile records straight into a format, like an email or a ticket."},
        {hotkeys::kTransform, "Transforms", destinations::commands, "Open Commands",
         "Select text in any app, then press a transform's shortcut to rewrite it.",
         "No transforms yet. A transform rewrites the text you select."},
        {hotkeys::kAction, "Actions", destinations::actions, "Open Actions",
         "Press an action's shortcut, say what you need, and press it again to send it.",
         "No actions yet. An action sends what you say to a chat, your agent or your own "
         "tools."},
    }};
    return table;
}

std::string clip(std::string_view text) {
    std::string joined;
    for (const auto& word : py::split(text)) {
        joined += (joined.empty() ? "" : " ") + word;
    }
    if (py::len(joined) <= kInstructionChars) {
        return joined;
    }
    return py::rstrip(py::slice(joined, 0, static_cast<std::int64_t>(kInstructionChars) - 1)) + "…";
}

} // namespace hotkey_bindings

hotkeys::Bindings conflictBindings(const BindingSources& sources, const Json& settings) {
    hotkeys::Bindings out;
    const auto take = [&](std::string_view kind, std::vector<hotkeys::Binding>& into) {
        const auto found = sources.find(kind);
        if (found == sources.end() || !found->second) {
            return;
        }
        for (const auto& binding : found->second->list(settings)) {
            into.push_back({binding.id, binding.name, binding.hotkey});
        }
    };
    take(hotkeys::kProfile, out.profiles);
    take(hotkeys::kTransform, out.transforms);
    take(hotkeys::kAction, out.actions);
    return out;
}

// _Row: a binding's name and what it does, beside its shortcut and Clear.
class BindingShortcuts::Row : public ReflowCard {
public:
    explicit Row(const hotkey_bindings::Binding& binding) : binding_(binding) {
        setObjectName(QStringLiteral("hotkeyShortcutCard"));
        name_ = new WrappedLabel(qt::toQString(binding.name));
        name_->setObjectName(QStringLiteral("hotkeyShortcutName"));
        copyLayout()->addWidget(name_);
        detail_ = new WrappedLabel(qt::toQString(binding.detail));
        detail_->setObjectName(QStringLiteral("hotkeyShortcutDescription"));
        copyLayout()->addWidget(detail_);
        note_ = new WrappedLabel(QString());
        note_->setObjectName(QStringLiteral("hotkeyShortcutNote"));
        note_->setVisible(false);
        copyLayout()->addWidget(note_);

        field_ = new ProfileHotkeyInput();
        field_->setAccessibleName(qt::toQString(binding.name) + QStringLiteral(" shortcut"));
        field_->setToolTip(QStringLiteral("Click, then press a shortcut. Escape cancels."));
        field_->setMinimumWidth(190);
        field_->setMaximumWidth(220);
        QObject::connect(field_, &HotkeyCaptureInput::notice, field_,
                         [this](const QString& text) { showNote(text, true); });
        addControl(field_);
        clear_ = new Button(QStringLiteral("Clear"));
        clear_->setObjectName(QStringLiteral("hotkeyClearButton"));
        clear_->setAccessibleName(QStringLiteral("Clear ") + qt::toQString(binding.name) +
                                  QStringLiteral(" shortcut"));
        fitCompactButton(clear_, 68);
        addControl(clear_);
    }

    const hotkey_bindings::Binding& binding() const { return binding_; }
    ProfileHotkeyInput* field() const { return field_; }
    Button* clearButton() const { return clear_; }

    // set_binding: the binding's saved state, and why its shortcut won't work, if it won't.
    void setBinding(const hotkey_bindings::Binding& binding, const Json& settings,
                    const BindingSources& sources) {
        binding_ = binding;
        name_->setText(qt::toQString(binding.name));
        detail_->setText(qt::toQString(binding.detail));
        field_->setAccessibleName(qt::toQString(binding.name) + QStringLiteral(" shortcut"));
        clear_->setAccessibleName(QStringLiteral("Clear ") + qt::toQString(binding.name) +
                                  QStringLiteral(" shortcut"));
        if (!field_->isCapturing()) {
            field_->setHotkey(qt::toQString(binding.hotkey));
        }
        clear_->setEnabled(!binding.hotkey.empty());
        // The app skips a shortcut that anything else also uses, and an action that is off.
        hotkeys::ConflictOptions options;
        options.exclude = {binding.kind, binding.id};
        options.bindings = conflictBindings(sources, settings);
        const std::string conflict = hotkeys::hotkey_conflict(binding.hotkey, settings, options);
        if (!conflict.empty()) {
            showNote(qt::toQString(conflict) +
                         QStringLiteral(" uses this shortcut too, so it does nothing here."),
                     true);
        } else if (!binding.hotkey.empty() && !binding.enabled) {
            showNote(QStringLiteral("This action is turned off, so its shortcut does nothing."));
        } else {
            showNote(qt::toQString(hotkeys::mouse_shortcut_note(binding.hotkey)));
        }
    }

    void showNote(const QString& text, bool warning = false) {
        note_->setText(text);
        ui::setStyleProperty(note_, ui::prop::tone,
                             warning ? QStringLiteral("warning") : QStringLiteral("info"));
        note_->setVisible(!text.isEmpty());
    }

private:
    hotkey_bindings::Binding binding_;
    WrappedLabel* name_;
    WrappedLabel* detail_;
    WrappedLabel* note_;
    ProfileHotkeyInput* field_;
    Button* clear_;
};

BindingShortcuts::BindingShortcuts(SettingsStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);
    for (const auto& group : hotkey_bindings::groups()) {
        auto* header = new QHBoxLayout();
        header->setContentsMargins(0, 0, 0, 0);
        auto* title = new QLabel(qt::toQString(group.title));
        title->setObjectName(QStringLiteral("hotkeyGroupTitle"));
        header->addWidget(title);
        header->addStretch();
        auto* link = new QPushButton(qt::toQString(group.link));
        link->setObjectName(QStringLiteral("settingsPageLink"));
        link->setFlat(true);
        link->setCursor(Qt::PointingHandCursor);
        link->setToolTip(QStringLiteral("Create and edit ") + qt::toQString(group.title).toLower() +
                         QStringLiteral(" on their own page"));
        const QString destination = qt::toQString(group.destination);
        connect(link, &QPushButton::clicked, this,
                [this, destination] { Q_EMIT openRequested(destination); });
        header->addWidget(link, 0, Qt::AlignBottom);
        root->addLayout(header);
        auto* caption = new WrappedLabel(qt::toQString(group.caption));
        caption->setObjectName(QStringLiteral("infoLabel"));
        root->addWidget(caption);
        auto* rows = new QVBoxLayout();
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(10);
        root->addLayout(rows);
        auto* empty = new WrappedLabel(qt::toQString(group.empty));
        empty->setObjectName(QStringLiteral("hotkeyShortcutDescription"));
        rows->addWidget(empty);
        groups_[std::string(group.kind)] = GroupWidgets{rows, caption, empty, {}};
    }
}

BindingShortcuts::~BindingShortcuts() = default;

void BindingShortcuts::registerSource(std::string kind, std::shared_ptr<BindingSource> source) {
    sources_[std::move(kind)] = std::move(source);
}

bool BindingShortcuts::capturing() const {
    for (const auto& [kind, group] : groups_) {
        for (const Row* row : group.items) {
            if (row->field()->isCapturing()) {
                return true;
            }
        }
    }
    return false;
}

void BindingShortcuts::cancelCapture() {
    for (auto& [kind, group] : groups_) {
        for (Row* row : group.items) {
            row->field()->cancelCapture();
        }
    }
}

std::vector<std::pair<std::string, std::vector<hotkey_bindings::Binding>>>
BindingShortcuts::bindings(const Json& settings) const {
    std::vector<std::pair<std::string, std::vector<hotkey_bindings::Binding>>> out;
    for (const auto& group : hotkey_bindings::groups()) {
        std::vector<hotkey_bindings::Binding> items;
        const auto found = sources_.find(group.kind);
        if (found != sources_.end() && found->second) {
            items = found->second->list(settings);
        }
        out.emplace_back(std::string(group.kind), std::move(items));
    }
    return out;
}

void BindingShortcuts::load(const Json& settings) {
    if (capturing()) {
        return;
    }
    for (auto& [kind, items] : bindings(settings)) {
        GroupWidgets& group = groups_.at(kind);
        std::vector<std::string> shown;
        for (const Row* row : group.items) {
            shown.push_back(row->binding().id);
        }
        std::vector<std::string> wanted;
        for (const auto& binding : items) {
            wanted.push_back(binding.id);
        }
        if (shown != wanted) {
            rebuild(kind, items);
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            group.items[i]->setBinding(items[i], settings, sources_);
        }
        // An empty group says what it's for instead.
        group.caption->setVisible(!items.empty());
        group.empty->setVisible(items.empty());
    }
}

void BindingShortcuts::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Another page may have changed a shortcut, or added or removed a binding.
    load(*store_.snapshot());
}

void BindingShortcuts::rebuild(const std::string& kind,
                               const std::vector<hotkey_bindings::Binding>& items) {
    GroupWidgets& group = groups_.at(kind);
    for (Row* row : group.items) {
        group.rows->removeWidget(row);
        // Off the page now, not when deleteLater gets to it.
        row->setParent(nullptr);
        row->deleteLater();
    }
    group.items.clear();
    for (const auto& binding : items) {
        auto* row = new Row(binding);
        connect(row->field(), &ProfileHotkeyInput::captureChanged, this,
                &BindingShortcuts::captureChanged);
        connect(row->field(), &ProfileHotkeyInput::captured, this,
                [this, row](const QString& hotkey) { save(row, hotkey); });
        connect(row->clearButton(), &QPushButton::clicked, this,
                [this, row] { save(row, QString()); });
        group.rows->addWidget(row);
        group.items.push_back(row);
    }
}

void BindingShortcuts::save(Row* row, const QString& hotkey) {
    const hotkey_bindings::Binding binding = row->binding();
    const auto found = sources_.find(binding.kind);
    const std::string wanted = qt::toUtf8(hotkey);
    try {
        if (found == sources_.end() || !found->second) {
            throw py::ValueError(binding.name + " no longer exists.");
        }
        BindingSource& source = *found->second;
        store_.mutate([&](Json& settings) {
            if (!source.save(settings, binding.id, wanted)) {
                throw py::ValueError(binding.name + " no longer exists.");
            }
        });
    } catch (const py::ValueError& error) {
        // The same reason its own editor would give.
        row->field()->setHotkey(qt::toQString(binding.hotkey));
        row->showNote(qt::toQString(error.what()), true);
        return;
    } catch (const std::exception& error) {
        // The settings file couldn't be written.
        kLog.warning("Could not save the shortcut: {}", error.what());
        row->field()->setHotkey(qt::toQString(binding.hotkey));
        row->showNote(QStringLiteral("Couldn't save the shortcut: ") + qt::toQString(error.what()),
                      true);
        return;
    }
    load(*store_.snapshot());
    const QString shortcut = display(wanted);
    const QString name = qt::toQString(binding.name);
    const QString message = shortcut.isEmpty() ? name + QStringLiteral(" shortcut cleared.")
                                               : name + QStringLiteral(" shortcut set to ") +
                                                     shortcut + QStringLiteral(".");
    Q_EMIT saved(qt::toQString(binding.kind), message);
}

} // namespace fv
