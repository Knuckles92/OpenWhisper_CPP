#include "ui_personalize/commands_page.h"

#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <typeinfo>
#include <utility>

#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "settings/keys.h"
#include "text/cleanup_profiles.h"
#include "text/personalize_copy.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/command_shortcut_field.h"
#include "ui_personalize/personalize_host.h"
#include "ui_personalize/transforms_panel.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_commands"};

// SEARCH_FIELDS: (control, title, description).
struct Field {
    const char* target;
    const char* title;
    const char* description;
};

constexpr Field kCommandsField{"commands_tile", "Command Mode",
                               "Select text anywhere, use the shortcut, and say how to change "
                               "it. The rewrite replaces your selection."};
constexpr Field kInsertField{"commands_insert_tile", "Write new text when nothing is selected",
                             "Say what to write, like “a short thank-you note”, and it’s added "
                             "at the cursor."};
constexpr Field kTransformsField{
    "transforms_panel", "Transforms",
    "Saved rewrites for selected text. Give one a shortcut to use it in any app."};
constexpr Field kProfilesField{"commands_profiles_tile", "Profile shortcuts",
                               "Profiles record with their own shortcuts and format what you say."};

QString q(const char* text) {
    return QString::fromUtf8(text);
}

const Json& orEmpty(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

// _link: a flat page link.
QPushButton* pageLink(const QString& text, const QString& tooltip) {
    auto* link = new QPushButton(text);
    link->setObjectName(QStringLiteral("settingsPageLink"));
    link->setFlat(true);
    link->setCursor(Qt::PointingHandCursor);
    link->setToolTip(tooltip);
    return link;
}

// The Home row's own state: the readiness answers it waits for live as long as its field.
class HomeRowState : public QObject {
public:
    explicit HomeRowState(QObject* parent) : QObject(parent) {}
    CommandReadiness::Receiver receiver{this};
    // The last answer, shown until a newer one comes.
    std::optional<bool> ready;
};

QString homeDetail(const Json& settings, std::optional<bool> ready) {
    if (ready.has_value() && !*ready) {
        return QStringLiteral("Needs an AI cleanup model, even while cleanup is off.");
    }
    return QStringLiteral("Select text, then say how to change it. ") +
           qt::toQString(text::commands_page::command_mode_hint(settings));
}

} // namespace

struct CommandReadiness::Receiver::State {
    std::mutex mutex;
    // Null once the owner's destruction has begun.
    QObject* owner = nullptr;
    qt::TokenGate gate;
};

CommandReadiness::Receiver::Receiver(QObject* owner) : state_(std::make_shared<State>()) {
    state_->owner = owner;
}

CommandReadiness::Receiver::~Receiver() {
    const std::lock_guard lock(state_->mutex);
    state_->owner = nullptr;
}

CommandReadiness::CommandReadiness(Check check, Executor* worker) {
    setCheck(std::move(check), worker);
}

void CommandReadiness::setCheck(Check check, Executor* worker) {
    check_ = check ? std::make_shared<const Check>(std::move(check)) : nullptr;
    worker_ = worker;
}

void CommandReadiness::ask(const Json& settings, Receiver& receiver,
                           std::function<void(std::optional<bool>)> done) const {
    std::shared_ptr<Receiver::State> state = receiver.state_;
    const std::uint64_t token = state->gate.next();
    auto deliver = [state, token](std::function<void(std::optional<bool>)> callback,
                                  std::optional<bool> answer) {
        // The owner cannot be destroyed while the lock is held, so post_task's guard is made on
        // a live object; from then on it drops the answer if the owner goes.
        const std::lock_guard lock(state->mutex);
        if (state->owner == nullptr) {
            return;
        }
        state->gate.deliver(state->owner, token,
                            [callback = std::move(callback), answer] { callback(answer); });
    };
    if (check_ == nullptr || worker_ == nullptr) {
        deliver(std::move(done), std::nullopt);
        return;
    }
    worker_->post([check = check_, settings, deliver, done = std::move(done)]() mutable {
        std::optional<bool> answer;
        try {
            answer = (*check)(settings);
        } catch (const std::exception& error) {
            kLog.warning("Couldn't check the AI cleanup model ({})", typeid(error).name());
        }
        deliver(std::move(done), answer);
    });
}

CommandsPage::CommandsPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store,
                           const CommandReadiness& readiness)
    : PersonalizePage(host, container), readiness_(readiness) {
    QVBoxLayout* layout = pageLayout();

    gateTile_ = new InfoTile(QStringLiteral("Set up AI cleanup first"),
                             QStringLiteral("Command Mode and transforms use your AI cleanup "
                                            "model, even while cleanup is turned off."),
                             ui::designIcon(QStringLiteral("info-warning.svg")));
    ui::setStyleProperty(gateTile_, ui::prop::kind, QStringLiteral("notice"));
    auto* openCleanup = compactPrimaryButton(new Button(QStringLiteral("Open AI cleanup")));
    openCleanup->setToolTip(QStringLiteral("Choose a provider and model for AI cleanup"));
    connect(openCleanup, &QPushButton::clicked, this, [this] { focusCleanupModel(this->host()); });
    gateTile_->addActions({openCleanup});
    gateTile_->hide();
    layout->addWidget(gateTile_);

    commandsTile_ = new InfoTile(q(kCommandsField.title), q(kCommandsField.description),
                                 ui::designIcon(QStringLiteral("wand-purple.svg")));
    shortcut_ = new CommandShortcutField(host);
    commandsTile_->addBody(shortcut_);
    modeHint_ = new WrappedLabel;
    modeHint_->setObjectName(QStringLiteral("infoLabel"));
    commandsTile_->addBody(modeHint_);

    insertTile_ = new SettingTile(q(kInsertField.title), q(kInsertField.description),
                                  ui::designIcon(QStringLiteral("text-plus-green.svg")));
    connect(insertTile_->checkbox(), &QAbstractButton::toggled, this, &CommandsPage::insertToggled);
    addTileGroup(QString(), {commandsTile_, insertTile_});
    layout->addWidget(settingsCaption(QStringLiteral(
        "Text you rewrite goes to your AI cleanup model and is saved in History, with the "
        "original kept alongside it.")));
    layout->addSpacing(8);

    groupTitle(layout, q(kTransformsField.title));
    layout->addWidget(settingsCaption(q(kTransformsField.description)));
    panel_ = new TransformsPanel(store);
    connect(panel_, &TransformsPanel::captureChanged, this,
            [this](bool capturing) { this->host().setHotkeyCaptureSuspended(capturing); });
    connect(panel_, &TransformsPanel::transformsChanged, this,
            [this] { this->host().notifyChanged(QStringLiteral("transforms")); });
    layout->addWidget(panel_);
    layout->addSpacing(8);

    profilesTile_ = new InfoTile(q(kProfilesField.title), q(kProfilesField.description),
                                 ui::designIcon(QStringLiteral("keyboard-green.svg")));
    profilesList_ = new WrappedLabel;
    profilesList_->setObjectName(QStringLiteral("infoLabel"));
    profilesTile_->addBody(profilesList_);
    profilesLink_ = pageLink(QStringLiteral("Open Profiles"),
                             QStringLiteral("Change profile recording shortcuts"));
    connect(profilesLink_, &QPushButton::clicked, this, [this] {
        this->host().selectDestination(qt::toQString(destinations::cleanup_profiles));
    });
    profilesTile_->addTrailing(profilesLink_);
    addTileGroup(QString(), {profilesTile_}, 1);
    // OpenWhisper's page column ends in a stretch (ensure_page).
    layout->addStretch();

    exposeControl(kCommandsField.target, commandsTile_);
    exposeControl(kInsertField.target, insertTile_);
    exposeControl("commands_gate_tile", gateTile_);
    exposeControl(kTransformsField.target, panel_);
    exposeControl(kProfilesField.target, profilesTile_);
    exposeControl("commands_shortcut", shortcut_);
}

CommandsPage::~CommandsPage() = default;

QString CommandsPage::profileShortcuts(const Json& settings) {
    QStringList lines;
    for (const text::CleanupProfile& profile : text::load_cleanup_profiles(orEmpty(settings))) {
        if (!profile.hotkey.empty()) {
            lines << QStringLiteral("%1 · %2").arg(
                qt::toQString(profile.name), qt::toQString(hotkeys::format_hotkey_display(
                                                 profile.hotkey, hotkeys::platform_vocabulary())));
        }
    }
    return lines.isEmpty() ? QStringLiteral("No profile has a shortcut yet.")
                           : lines.join(QStringLiteral("\n"));
}

void CommandsPage::load(const Json& settings) {
    showState(settings);
}

void CommandsPage::sync(const Json& settings) {
    showState(settings);
}

void CommandsPage::refreshPage() {
    {
        const LoadingScope scope(*this);
        showState(host().settings());
    }
    panel_->refresh();
}

bool CommandsPage::saveDrafts(bool ask) {
    return panel_->saveDraft(ask);
}

void CommandsPage::cancelCapture() {
    shortcut_->cancelCapture();
    panel_->cancelCapture();
}

void CommandsPage::showState(const Json& settings) {
    const Json& values = orEmpty(settings);
    const QPointer<InfoTile> gate = gateTile_;
    readiness_.ask(values, readinessReceiver_, [gate](std::optional<bool> ready) {
        if (gate != nullptr) {
            gate->setVisible(ready.has_value() && !*ready);
        }
    });
    shortcut_->sync();
    modeHint_->setText(qt::toQString(text::commands_page::command_mode_hint(values)));
    const bool insert = keys::command_mode_insert_without_selection.read(values);
    {
        const QSignalBlocker blocker(insertTile_->checkbox());
        insertTile_->checkbox()->setChecked(insert);
    }
    ui::setStyleProperty(insertTile_, ui::prop::checked, insert);
    profilesList_->setText(profileShortcuts(values));
}

void CommandsPage::insertToggled(bool checked) {
    if (loading()) {
        return;
    }
    if (!host().persist(keys::command_mode_insert_without_selection.key(), checked)) {
        const QSignalBlocker blocker(insertTile_->checkbox());
        insertTile_->checkbox()->setChecked(!checked);
    }
}

void focusCleanupModel(PersonalizeHost& host) {
    host.selectDestination(qt::toQString(destinations::cleanup));
    if (QWidget* tile = host.control("cleanup_model_tile")) {
        host.reveal(tile);
    }
}

void registerCommandsPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::commands;
    descriptor.title = QStringLiteral("Commands");
    descriptor.subtitle =
        QStringLiteral("Rewrite selected text by voice, and keep the rewrites you use often.");
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new CommandsPage(host, container, context.settings(),
                                context.service<CommandReadiness>());
    };
    descriptor.railValue = [](const Json& settings) {
        return qt::toQString(text::commands_page::rail_value(settings));
    };
    for (const Field& field : {kCommandsField, kInsertField, kTransformsField, kProfilesField}) {
        descriptor.searchFields.push_back({field.target, q(field.title), q(field.description)});
    }
    descriptor.controls = {"commands_tile", "commands_insert_tile", "commands_gate_tile",
                           "transforms_panel", "commands_profiles_tile"};
    registerPersonalizePage(context, std::move(descriptor));
}

void addCommandsHomeRows(HomePage& home) {
    SettingsDialog& dialog = home.dialog();
    // Owned by the window, which deletes its pages (Home and this field with it) first.
    auto* host = new DialogPersonalizeHost(dialog, &dialog);
    auto* field = new CommandShortcutField(*host, true, false, false);
    field->input()->setMaximumWidth(210);
    const QPointer<WrappedLabel> detail = home.addRow(
        HomeCardId::Personalize, QStringLiteral("Command Mode shortcut"), QString(), field);
    const CommandReadiness* readiness = dialog.context().find<CommandReadiness>();
    auto* state = new HomeRowState(field);
    const QPointer<CommandShortcutField> shown = field;
    home.addRefreshHook([shown, detail, readiness, state](const Json& settings) {
        if (shown == nullptr) {
            return;
        }
        shown->sync();
        if (detail != nullptr) {
            detail->setText(homeDetail(settings, state->ready));
        }
        if (readiness == nullptr) {
            return;
        }
        readiness->ask(settings, state->receiver,
                       [detail, state, settings](std::optional<bool> answer) {
                           state->ready = answer;
                           if (detail != nullptr) {
                               detail->setText(homeDetail(settings, answer));
                           }
                       });
    });
}

} // namespace fv
