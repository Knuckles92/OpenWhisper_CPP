#include "ui_settings/api_keys_section.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <tuple>
#include <typeinfo>
#include <utility>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "base/cancel.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "credentials/resolve.h"
#include "credentials/validate.h"
#include "http/client.h"
#include "llm/catalog_client.h"
#include "llm/credentials.h"
#include "settings/store.h"
#include "speech/endpoints.h"
#include "ui_settings/detail/mailbox.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/typesafe_credential.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_api_keys"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// The store error's class, as Python logs type(exc).__name__: the key may be in what().
const char* storeErrorType(const CredentialStoreError& error) {
    return dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr ? "CredentialStoreTimeout"
                                                                          : "CredentialStoreError";
}

// The settings mapping the store holds now.
Json storeSettings(SettingsDialog& dialog) {
    return *dialog.context().settings().snapshot();
}

} // namespace

// --- ApiKeysState ---

ApiKeysState::ApiKeysState(CredentialStore& store, const EnvReader& env, const DotEnv& dotenv,
                           Executor& worker, Executor& ui)
    : store_(store), env_(env), dotenv_(dotenv), worker_(worker), ui_(ui) {}

std::shared_ptr<ApiKeysState> ApiKeysState::create(CredentialStore& store, const EnvReader& env,
                                                   const DotEnv& dotenv, Executor& worker,
                                                   Executor& ui) {
    std::shared_ptr<ApiKeysState> state(new ApiKeysState(store, env, dotenv, worker, ui));
    state->self_ = state;
    return state;
}

ApiKeysState::Snapshot ApiKeysState::readNow(const std::vector<std::string>& names) const {
    Snapshot read;
    read.status = store_.status();
    for (const std::string& name : names) {
        Credential credential;
        try {
            const Resolved resolved = resolve_credential(store_, env_, dotenv_, name);
            credential.source = credential_source(store_, env_, dotenv_, name);
            const bool shadowed = credential.source == CredentialSource::Store &&
                                  environment_shadowed(store_, env_, dotenv_, name);
            credential.status = appcore::status_text(credential.source, read.status.backend_name,
                                                     resolved.value, name, shadowed);
        } catch (const std::exception& error) {
            // The credential layer reports a store failure as an empty lookup; anything that
            // still escapes must not stop the other rows. The type only: what() may name a key.
            kLog.warning("Couldn't read the {} credential ({})", name,
                         type_display_name(typeid(error)));
            credential.status = appcore::status_text(CredentialSource::None,
                                                     read.status.backend_name, std::nullopt, name);
        }
        read.credentials.emplace(name, std::move(credential));
    }
    return read;
}

void ApiKeysState::read(std::vector<std::string> names, std::function<void()> done,
                        std::function<void()> first) {
    const std::uint64_t token = ++requested_;
    for (const std::string& name : names) {
        pending_.insert(name);
    }
    // The worker holds no page and no widget: the state is plain data over services that outlive
    // the pages, and `done` (which may name a page) only runs on the GUI thread, where it checks
    // its page is still there.
    worker_.post([weak = self_, token, names = std::move(names), done = std::move(done),
                  first = std::move(first)]() mutable {
        const std::shared_ptr<ApiKeysState> state = weak.lock();
        if (state == nullptr) {
            return;
        }
        if (first) {
            first();
        }
        auto read = std::make_shared<Snapshot>(state->readNow(names));
        state->ui_.post([weak, token, read, names = std::move(names), done = std::move(done)] {
            const std::shared_ptr<ApiKeysState> alive = weak.lock();
            if (alive == nullptr) {
                return;
            }
            alive->adopt(token, std::move(*read), names);
            if (done) {
                done();
            }
        });
    });
}

void ApiKeysState::adopt(std::uint64_t token, Snapshot read,
                         const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (const auto found = pending_.find(name); found != pending_.end()) {
            pending_.erase(found);
        }
    }
    if (!snapshot_) {
        snapshot_ = std::move(read);
        adopted_ = token;
        return;
    }
    // Reads may finish out of order on a wide worker: the newest one's values win, and an older
    // one only fills in names nothing newer has read.
    const bool newest = token > adopted_;
    if (newest) {
        snapshot_->status = read.status;
        adopted_ = token;
    }
    for (auto& [name, credential] : read.credentials) {
        if (newest || !snapshot_->credentials.contains(name)) {
            snapshot_->credentials[name] = std::move(credential);
        }
    }
}

bool ApiKeysState::knows(const std::vector<std::string>& names) const {
    if (!snapshot_) {
        return false;
    }
    return std::ranges::all_of(
        names, [this](const std::string& name) { return snapshot_->credentials.contains(name); });
}

bool ApiKeysState::reading(const std::vector<std::string>& names) const {
    return std::ranges::any_of(names,
                               [this](const std::string& name) { return pending_.contains(name); });
}

std::string ApiKeysState::railValue(const std::vector<appcore::CredentialEntry>& entries) const {
    return appcore::api_keys_rail_value(entries, [this](std::string_view name) {
        if (!snapshot_) {
            return CredentialSource::None;
        }
        const auto found = snapshot_->credentials.find(name);
        return found == snapshot_->credentials.end() ? CredentialSource::None
                                                     : found->second.source;
    });
}

// --- the credential rows ---

std::vector<appcore::CredentialOwner> credentialOwners(const Json& settings) {
    std::vector<appcore::CredentialOwner> owners;
    for (llm::TextLLMProfile& profile : llm::list_profiles(settings)) {
        if (!profile.api_key_env.empty()) {
            owners.push_back({std::move(profile.api_key_env), std::move(profile.name),
                              std::string(appcore::kUseText)});
        }
    }
    for (auto& [env_name, owner] : speech::credential_owners(settings)) {
        owners.push_back({std::move(env_name), std::move(owner), std::string(appcore::kUseSpeech)});
    }
    // TypeSafe is a decision service rather than a text-model profile, so it is not in the lists
    // above but still needs a home for its key.
    owners.push_back(typesafe_credential::owner());
    return owners;
}

std::vector<appcore::CredentialEntry> credentialEntries(const Json& settings) {
    const std::vector<appcore::CredentialOwner> owners = credentialOwners(settings);
    return appcore::key_entries(owners);
}

std::optional<llm::TextLLMProfile> textProfileFor(const Json& settings, std::string_view envName) {
    for (llm::TextLLMProfile& profile : llm::list_profiles(settings)) {
        if (profile.api_key_env == envName) {
            return std::move(profile);
        }
    }
    return std::nullopt;
}

std::optional<speech::SpeechEndpoint> endpointFor(const Json& settings, std::string_view envName) {
    for (const std::string& id : speech::endpoint_ids()) {
        speech::SpeechEndpoint endpoint = speech::configured_endpoint(id, settings);
        if (endpoint.api_key_env == envName) {
            return endpoint;
        }
    }
    return std::nullopt;
}

QString credentialLabel(const Json& settings, std::string_view envName) {
    if (typesafe_credential::is_typesafe(envName)) {
        return q(typesafe_credential::kLabel);
    }
    if (const std::optional<llm::TextLLMProfile> profile = textProfileFor(settings, envName)) {
        return q(llm::credential_label(*profile));
    }
    const std::optional<speech::SpeechEndpoint> endpoint = endpointFor(settings, envName);
    if (!endpoint) {
        return q(envName);
    }
    // text_llm.credential_label of the endpoint's connection profile.
    if (endpoint->id == speech::kOpenAiEndpointId) {
        return QStringLiteral("OpenAI API key");
    }
    if (endpoint->builtin) {
        return endpoint->api_key_env.empty() ? QString()
                                             : q(endpoint->name) + QStringLiteral(" API key");
    }
    return q(endpoint->api_key_env);
}

// --- ApiKeysSection ---

ApiKeysSection::ApiKeysSection(SettingsPage& page, SettingsDialog& dialog,
                               std::shared_ptr<ApiKeysState> state, Verify verify)
    : QWidget(&page), dialog_(dialog), state_(std::move(state)), verify_(std::move(verify)),
      mailbox_(std::make_shared<detail::Mailbox<ApiKeysSection>>(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    build(page);
}

ApiKeysSection::~ApiKeysSection() {
    mailbox_->close();
}

void ApiKeysSection::build(SettingsPage& page) {
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    combo_ = new ElidingComboBox;
    combo_->setObjectName(QStringLiteral("apiKeyCredentialCombo"));
    combo_->setMinimumHeight(40);
    connect(combo_, &QComboBox::currentIndexChanged, this, [this] { onCredentialChanged(); });
    credentialTile_ =
        new FieldTile(QStringLiteral("Credential"),
                      QStringLiteral("Choose the provider whose key you want to save or test."),
                      combo_, ui::designIcon(QStringLiteral("key-blue.svg")));

    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    statusIcon_ = new QLabel;
    statusIcon_->setObjectName(QStringLiteral("apiKeyStatusIcon"));
    statusIcon_->setFixedSize(16, 16);
    status_ = new WrappedLabel(QString());
    status_->setObjectName(QStringLiteral("apiKeyStatus"));
    statusRow->addWidget(statusIcon_, 0, Qt::AlignTop);
    statusRow->addWidget(status_, 1);
    credentialTile_->addBodyLayout(statusRow);
    usesCaption_ = settingsCaption(QString());
    credentialTile_->addBody(usesCaption_);
    page.addTileGroup(layout, QString(), {credentialTile_}, 1);

    // The store's own words arrive with the first credential read (the store blocks).
    QString caption;
    QString detail;
    if (state_->snapshot()) {
        caption = q(appcore::store_caption(state_->snapshot()->status));
        detail = q(appcore::store_detail(state_->snapshot()->status));
    }
    entryTile_ = new InfoTile(QStringLiteral("Add or replace a key"), caption,
                              ui::designIcon(QStringLiteral("plus-blue.svg")));
    entryTile_->descriptionLabel()->setToolTip(detail);

    // Password echo also makes Qt refuse copy/cut from the field, and the input-method hints
    // keep IMEs and predictive text from retaining it.
    edit_ = new QLineEdit;
    edit_->setObjectName(QStringLiteral("apiKeyInput"));
    edit_->setEchoMode(QLineEdit::Password);
    edit_->setPlaceholderText(QStringLiteral("Paste a key to save or test"));
    edit_->setMaxLength(static_cast<int>(kMaxApiKeyLength));
    edit_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoAutoUppercase |
                               Qt::ImhNoPredictiveText);
    edit_->setMinimumHeight(40);
    // The key itself is set in a spaced monospace; the placeholder keeps the normal field font
    // (QSS can't style a placeholder on its own).
    connect(edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        ui::setStyleProperty(edit_, ui::prop::hasText, !text.isEmpty());
    });
    connect(edit_, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(edit_, &QLineEdit::returnPressed, this, [this] { saveKey(); });
    showButton_ = neutralButton(new Button(QStringLiteral("Show")));
    showButton_->setObjectName(QStringLiteral("apiKeyShowButton"));
    showButton_->setCheckable(true);
    showButton_->setBaseMinimumSize(80, 40);
    showButton_->setMinimumHeight(40);
    showButton_->setMaximumHeight(40);
    connect(showButton_, &QPushButton::toggled, this,
            [this](bool checked) { onShowToggled(checked); });
    auto* inputRow = new QWidget;
    inputRow->setObjectName(QStringLiteral("modelManagerFieldGroup"));
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(8);
    inputLayout->addWidget(edit_, 1);
    inputLayout->addWidget(showButton_);
    entryTile_->addBody(inputRow);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    // A plain Button: PrimaryButton's own padding would make it taller than Test.
    saveButton_ = new Button(QStringLiteral("Save key"));
    saveButton_->setObjectName(QStringLiteral("apiKeySaveButton"));
    compactPrimaryButton(saveButton_);
    fitCompactButton(saveButton_, 120);
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveKey(); });
    testButton_ = neutralButton(new Button(QStringLiteral("Test")));
    testButton_->setObjectName(QStringLiteral("apiKeyTestButton"));
    fitCompactButton(testButton_, 90);
    connect(testButton_, &QPushButton::clicked, this, [this] { testKey(); });
    removeButton_ = new DangerButton(QStringLiteral("Remove saved key"));
    ui::setStyleProperty(removeButton_, ui::prop::buttonId, QStringLiteral("apiKeyRemoveButton"));
    fitCompactButton(removeButton_, 160);
    connect(removeButton_, &QPushButton::clicked, this, [this] { removeKey(); });
    buttons->addWidget(saveButton_);
    buttons->addWidget(testButton_);
    buttons->addStretch();
    buttons->addWidget(removeButton_);
    entryTile_->addBodyLayout(buttons);
    page.addTileGroup(layout, QString(), {entryTile_}, 1);
}

WrappedLabel* ApiKeysSection::storeCaption() const {
    return entryTile_->descriptionLabel();
}

Json ApiKeysSection::settings() const {
    return storeSettings(dialog_);
}

QString ApiKeysSection::label(const QString& envName) const {
    return credentialLabel(settings(), qt::toUtf8(envName));
}

QString ApiKeysSection::selectedName() const {
    return combo_->currentData().toString();
}

bool ApiKeysSection::stateCurrent() const {
    const std::string name = qt::toUtf8(selectedName());
    if (name.empty()) {
        return state_->snapshot().has_value() && !state_->reading(names_);
    }
    return state_->knows({name}) && !state_->reading({name});
}

void ApiKeysSection::say(const QString& text) {
    dialog_.showMessage(text);
}

// ---- loading and focus ----

void ApiKeysSection::loadSettings(const Json& settings) {
    const QString current = selectedName();
    const std::vector<appcore::CredentialEntry> entries = credentialEntries(settings);
    {
        const QSignalBlocker blocker(combo_);
        combo_->clear();
        for (const auto& entry : entries) {
            combo_->addItem(q(entry.label), q(entry.env_name));
        }
        const int index = current.isEmpty() ? -1 : combo_->findData(current);
        combo_->setCurrentIndex(std::max(0, index));
    }
    names_.clear();
    for (const auto& entry : entries) {
        names_.push_back(entry.env_name);
    }
    clearInput();
    renderStatus();
    readState();
}

void ApiKeysSection::readState() {
    QPointer<ApiKeysSection> self = this;
    QPointer<SettingsDialog> dialog = &dialog_;
    state_->read(names_, [self, dialog] {
        if (self != nullptr) {
            self->renderStatus();
        }
        if (dialog != nullptr) {
            dialog->refreshRailValues();
        }
    });
}

void ApiKeysSection::focusCredential(const QString& env) {
    if (!env.isEmpty()) {
        const int index = combo_->findData(env);
        if (index >= 0) {
            combo_->setCurrentIndex(index);
        }
    }
    edit_->setFocus();
}

QWidget* ApiKeysSection::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"api_key_combo", combo_},
        {"api_key_credential_tile", credentialTile_},
        {"api_key_status_icon", statusIcon_},
        {"api_key_status", status_},
        {"api_key_uses_caption", usesCaption_},
        {"api_key_entry_tile", entryTile_},
        {"api_key_store_caption", entryTile_->descriptionLabel()},
        {"api_key_edit", edit_},
        {"api_key_show_button", showButton_},
        {"api_key_save_button", saveButton_},
        {"api_key_test_button", testButton_},
        {"api_key_remove_button", removeButton_},
    };
    for (const auto& [control, widget] : controls) {
        if (control == name) {
            return widget;
        }
    }
    return nullptr;
}

void ApiKeysSection::clearInput() {
    edit_->clear();
    showButton_->setChecked(false);
}

// ---- rendering ----

void ApiKeysSection::renderStatus() {
    const std::string name = qt::toUtf8(selectedName());
    const auto& snapshot = state_->snapshot();
    if (snapshot) {
        storeCaption()->setText(q(appcore::store_caption(snapshot->status)));
        storeCaption()->setToolTip(q(appcore::store_detail(snapshot->status)));
    }
    std::optional<appcore::KeyStatus> status;
    source_ = CredentialSource::None;
    if (name.empty()) {
        status = appcore::status_text(CredentialSource::None, "", std::nullopt, "");
    } else if (snapshot) {
        if (const auto found = snapshot->credentials.find(name);
            found != snapshot->credentials.end()) {
            source_ = found->second.source;
            status = found->second.status;
        }
    }
    if (status) {
        status_->setText(q(status->text));
        statusIcon_->setPixmap(ui::designIcon(q(status->icon)).pixmap(16, 16));
        ui::setStyleProperty(status_, ui::prop::tone, q(status->tone));
    } else {
        // Not read yet: nothing is said about a key until the store has answered.
        status_->setText(QString());
        statusIcon_->clear();
    }
    renderUses(selectedName());
    updateControls();
}

void ApiKeysSection::renderUses(const QString& envName) {
    const QString text =
        q(appcore::uses_caption(qt::toUtf8(envName), credentialOwners(settings())));
    usesCaption_->setText(text);
    usesCaption_->setVisible(!text.isEmpty());
}

void ApiKeysSection::updateControls() {
    const bool named = !selectedName().isEmpty();
    const auto& snapshot = state_->snapshot();
    const bool canStore = named && snapshot.has_value() && snapshot->status.available;
    const bool typed = !edit_->text().trimmed().isEmpty();
    const bool busy = testing_ || storing_;
    edit_->setEnabled(named && !busy);
    showButton_->setEnabled(named && !busy);
    saveButton_->setEnabled(canStore && typed && !busy);
    testButton_->setEnabled(named && !busy && (typed || source_ != CredentialSource::None));
    removeButton_->setEnabled(canStore && !busy && source_ == CredentialSource::Store);
}

// ---- actions ----

void ApiKeysSection::onCredentialChanged() {
    if (dialog_.isLoading()) {
        return;
    }
    clearInput();
    say(QString());
    renderStatus();
}

void ApiKeysSection::onShowToggled(bool checked) {
    edit_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    showButton_->setText(checked ? QStringLiteral("Hide") : QStringLiteral("Show"));
}

void ApiKeysSection::saveKey() {
    const QString name = selectedName();
    if (name.isEmpty() || !saveButton_->isEnabled()) {
        return;
    }
    std::string key;
    try {
        key = validate_api_key(qt::toUtf8(edit_->text()));
    } catch (const std::invalid_argument& error) {
        // The typed key failing validation, shown to the user.
        say(q(error.what()));
        return;
    }
    const QString saved = label(name) + QStringLiteral(" saved.");
    storing_ = true;
    updateControls();
    auto outcome = std::make_shared<std::optional<QString>>();
    CredentialStore& store = state_->store();
    const std::string variable = qt::toUtf8(name);
    QPointer<ApiKeysSection> self = this;
    QPointer<SettingsDialog> dialog = &dialog_;
    state_->read(
        names_,
        [self, dialog, outcome, saved] {
            if (dialog != nullptr) {
                dialog->refreshRailValues();
            }
            if (self == nullptr) {
                return;
            }
            if (outcome->has_value()) {
                self->storing_ = false;
                self->updateControls();
                self->say(**outcome);
                return;
            }
            self->onStored(saved, true);
        },
        [&store, variable, key, outcome] {
            try {
                store.set(variable, key);
            } catch (const CredentialStoreError& error) {
                // A store failure also goes to the log, type only.
                kLog.warning("Couldn't save {} to the credential store ({})", variable,
                             storeErrorType(error));
                *outcome = q(error.what());
            } catch (const std::invalid_argument& error) {
                *outcome = q(error.what());
            }
        });
}

void ApiKeysSection::removeKey() {
    const QString name = selectedName();
    if (name.isEmpty()) {
        return;
    }
    const QString keyLabel = label(name);
    const auto& snapshot = state_->snapshot();
    const QString backend = snapshot ? q(snapshot->status.backend_name) : QString();
    const QString title = QStringLiteral("Remove saved key");
    const QString text =
        QStringLiteral("Remove the saved %1 from %2?\n\n%3 will fall back to a %4 environment "
                       "variable or .env entry if one exists; otherwise features that need this "
                       "key become unavailable.")
            .arg(keyLabel, backend, q(identity().product), name);
    bool yes = false;
    if (confirmRemove_) {
        yes = confirmRemove_(this, title, text);
    } else {
        yes = QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No) == QMessageBox::Yes;
    }
    if (!yes) {
        return;
    }
    storing_ = true;
    updateControls();
    struct Outcome {
        std::optional<QString> error;
        bool removed = false;
    };
    auto outcome = std::make_shared<Outcome>();
    CredentialStore& store = state_->store();
    const std::string variable = qt::toUtf8(name);
    QPointer<ApiKeysSection> self = this;
    QPointer<SettingsDialog> dialog = &dialog_;
    state_->read(
        names_,
        [self, dialog, outcome, keyLabel] {
            if (dialog != nullptr) {
                dialog->refreshRailValues();
            }
            if (self == nullptr) {
                return;
            }
            if (outcome->error) {
                self->storing_ = false;
                self->updateControls();
                self->say(*outcome->error);
                return;
            }
            self->onStored(outcome->removed
                               ? keyLabel + QStringLiteral(" removed.")
                               : QStringLiteral("No saved %1 to remove.").arg(keyLabel),
                           true);
        },
        [&store, variable, outcome] {
            try {
                outcome->removed = store.remove(variable);
            } catch (const CredentialStoreError& error) {
                kLog.warning("Couldn't remove {} from the credential store ({})", variable,
                             storeErrorType(error));
                outcome->error = q(error.what());
            }
        });
}

void ApiKeysSection::onStored(const QString& message, bool changed) {
    storing_ = false;
    clearInput();
    renderStatus();
    say(message);
    if (changed) {
        Q_EMIT keysChanged();
    }
}

void ApiKeysSection::testKey() {
    const QString name = selectedName();
    if (testing_ || name.isEmpty()) {
        return;
    }
    const Json current = settings();
    // profile_for: the text-model endpoint that sends the key, else the speech server.
    std::optional<llm::TextLLMProfile> textProfile = textProfileFor(current, qt::toUtf8(name));
    std::optional<speech::SpeechEndpoint> endpoint;
    if (!textProfile) {
        endpoint = endpointFor(current, qt::toUtf8(name));
        // TypeSafe is not an OpenAI-compatible endpoint and so has no profile; it verifies
        // through its own one-judgment probe.
        if (!endpoint && !typesafe_credential::is_typesafe(qt::toUtf8(name))) {
            return;
        }
    }
    const QString keyLabel = credentialLabel(current, qt::toUtf8(name));
    const QString typed = edit_->text();
    std::optional<std::string> key;
    if (!typed.trimmed().isEmpty()) {
        try {
            key = validate_api_key(qt::toUtf8(typed));
        } catch (const std::invalid_argument& error) {
            say(q(error.what()));
            return;
        }
    } else if (source_ == CredentialSource::None) {
        say(QStringLiteral("Paste a key to test."));
        return;
    }
    testing_ = true;
    updateControls();
    say(QStringLiteral("Testing %1…").arg(keyLabel));
    const std::uint64_t token = testGate_.next();
    // What the worker runs, bound here: it must not reach back into the page.
    using Check = std::function<std::pair<bool, std::string>(const std::string&)>;
    Check check;
    if (textProfile) {
        VerifyText verify = verifyText_;
        if (!verify) {
            http::Client* client = nullptr;
            try {
                client = &dialog_.context().http();
            } catch (const std::exception&) {
                // No HTTP client in this context: the check reports that it could not run.
            }
            verify = [client](const llm::TextLLMProfile& profile, const std::string& value) {
                if (client == nullptr) {
                    throw std::runtime_error("No HTTP client is available.");
                }
                const llm::KeyCheck result =
                    llm::verify_api_key(*client, profile, value, CancelToken{});
                return std::pair<bool, std::string>(result.ok, result.detail);
            };
        }
        check = [verify, profile = std::move(*textProfile)](const std::string& value) {
            return verify(profile, value);
        };
    } else if (endpoint) {
        check = [verify = verify_, endpoint = std::move(*endpoint)](const std::string& value) {
            return verify(endpoint, value);
        };
    } else {
        VerifyTypeSafe verify = verifyTypeSafe_;
        if (!verify) {
            JudgeKeys* keys = dialog_.context().find<JudgeKeys>();
            verify = [keys](const std::string& value) {
                if (keys == nullptr) {
                    throw std::runtime_error("No judge is available.");
                }
                const llm::VerifyResult result = keys->verify(llm::typesafe_route(), value);
                return std::pair<bool, std::string>(result.ok, result.message);
            };
        }
        check = std::move(verify);
    }
    qt::TokenGate gate = testGate_;
    CredentialStore& store = state_->store();
    const EnvReader& env = state_->env();
    const DotEnv& dotenv = state_->dotenv();
    const std::string variable = qt::toUtf8(name);
    state_->worker().post([mailbox = mailbox_, gate, token, check = std::move(check), key, &store,
                           &env, &dotenv, variable]() mutable {
        bool ok = false;
        std::string detail;
        bool missing = false;
        try {
            if (!key) {
                key = resolve_credential(store, env, dotenv, variable).value;
            }
            if (!key || key->empty()) {
                missing = true;
            } else {
                std::tie(ok, detail) = check(*key);
            }
        } catch (const std::exception&) {
            // Never the message: a provider's error can echo the key back.
            ok = false;
            detail = "Verification failed.";
        }
        mailbox->deliver(gate, token, [variable, ok, detail, missing](ApiKeysSection& section) {
            if (missing) {
                section.testing_ = false;
                section.updateControls();
                section.say(QStringLiteral("Paste a key to test."));
                return;
            }
            section.onVerified(q(variable), ok, q(detail));
        });
    });
}

void ApiKeysSection::onVerified(const QString& envName, bool ok, const QString& detail) {
    testing_ = false;
    updateControls();
    const QString keyLabel = label(envName);
    say(ok ? QStringLiteral("%1 works: %2").arg(keyLabel, detail)
           : QStringLiteral("%1 failed: %2").arg(keyLabel, detail));
}

} // namespace fv
