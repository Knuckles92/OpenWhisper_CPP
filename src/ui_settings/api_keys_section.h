#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "appcore/api_keys_model.h"
#include "appkit/qt_bridge.h"
#include "appkit/settings_page.h"
#include "base/json.h"
#include "credentials/store.h"
#include "llm/profiles.h"
#include "ui_settings/settings_page.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace fv {

class Button;
class DangerButton;
class DotEnv;
class ElidingComboBox;
class EnvReader;
class Executor;
class FieldTile;
class InfoTile;
class SettingsDialog;
class WrappedLabel;

namespace http {
class Client;
}
namespace speech {
struct SpeechEndpoint;
}
namespace detail {
template <class Widget> class Mailbox;
}

// What the store and the environment say about the API keys page's credentials, read off the GUI
// thread (CredentialStore blocks) and kept for the rail, which the window redraws on every save:
// the store's status, and per variable its source and its status line (which shows the key only
// masked, and says whether a saved key hides the environment's). A read is asked for whenever
// the page loads or a key changes; the rail counts a variable not read yet as not set.
// Affinity: GUI thread; reads run on the worker.
class ApiKeysState {
public:
    struct Credential {
        CredentialSource source = CredentialSource::None;
        appcore::KeyStatus status;
    };
    struct Snapshot {
        CredentialStatus status{false, "", ""};
        std::map<std::string, Credential, std::less<>> credentials;
    };

    // Everything given must outlive the state; `ui` runs the results on the GUI thread.
    static std::shared_ptr<ApiKeysState> create(CredentialStore& store, const EnvReader& env,
                                                const DotEnv& dotenv, Executor& worker,
                                                Executor& ui);

    // Runs `first` (a save or a removal), then reads the store's status and `names`, on the
    // worker; on the GUI thread the read is kept and `done` runs. Reads may finish out of order:
    // the newest read's values win.
    void read(std::vector<std::string> names, std::function<void()> done = {},
              std::function<void()> first = {});
    // The last reads, or nullopt before the first.
    const std::optional<Snapshot>& snapshot() const { return snapshot_; }
    // Whether every name has been read, and whether a read of one is on its way.
    bool knows(const std::vector<std::string>& names) const;
    bool reading(const std::vector<std::string>& names) const;
    // "None needed" or "N of M set", counting the variables read as set.
    std::string railValue(const std::vector<appcore::CredentialEntry>& entries) const;

    CredentialStore& store() const { return store_; }
    const EnvReader& env() const { return env_; }
    const DotEnv& dotenv() const { return dotenv_; }
    Executor& worker() const { return worker_; }

private:
    ApiKeysState(CredentialStore& store, const EnvReader& env, const DotEnv& dotenv,
                 Executor& worker, Executor& ui);

    Snapshot readNow(const std::vector<std::string>& names) const;
    void adopt(std::uint64_t token, Snapshot read, const std::vector<std::string>& names);

    CredentialStore& store_;
    const EnvReader& env_;
    const DotEnv& dotenv_;
    Executor& worker_;
    Executor& ui_;
    std::weak_ptr<ApiKeysState> self_;
    std::uint64_t requested_ = 0;
    std::uint64_t adopted_ = 0;
    std::multiset<std::string, std::less<>> pending_;
    std::optional<Snapshot> snapshot_;
};

// The credential owners of a settings state, in Python's order: the text-model endpoints that
// send a key (llm::list_profiles: the built-ins, then the custom ones), then the speech endpoints
// that send one, then TypeSafe's fixed row.
std::vector<appcore::CredentialOwner> credentialOwners(const Json& settings);
// The page's rows (key_entries).
std::vector<appcore::CredentialEntry> credentialEntries(const Json& settings);
// profile_for, text half: the text-model endpoint that sends `envName`; nullopt when none does.
std::optional<llm::TextLLMProfile> textProfileFor(const Json& settings, std::string_view envName);
// profile_for, speech half: the speech server that sends `envName`; nullopt when none does.
std::optional<speech::SpeechEndpoint> endpointFor(const Json& settings, std::string_view envName);
// _label: text_llm.credential_label of the text endpoint that sends `envName`, else of the speech
// server ("OpenAI API key", "Groq API key", the variable for a custom endpoint), else the
// variable itself.
QString credentialLabel(const Json& settings, std::string_view envName);

// Settings → App → API keys (ui_qt/dialogs/settings_api_keys.py ApiKeysSection): the credential
// combo with its status line and "uses" caption, then the entry tile with the masked field, Show,
// Save key, Test and Remove saved key. Keys go to the credential store, never to settings or
// logs. Every store call and every key test runs on the worker; their results come back through
// a TokenGate on this page, so a closed page is not held by the worker.
// Affinity: GUI thread.
class ApiKeysSection : public QWidget, public SettingsPageModule, public SettingsDeepLinks {
    Q_OBJECT
public:
    // `verify` checks a key against a speech server (speech::verify_endpoint_key); blocking, on
    // the worker. A key of a text-model endpoint goes to the text check (setTextVerify).
    // `confirmRemove` asks "Remove saved key" (Yes/No, default No); null asks with a QMessageBox.
    using Verify = std::function<std::pair<bool, std::string>(const speech::SpeechEndpoint&,
                                                              const std::string& key)>;
    // The same for a key that belongs to a text-model endpoint (llm::verify_api_key); blocking,
    // on the worker.
    using VerifyText = std::function<std::pair<bool, std::string>(const llm::TextLLMProfile&,
                                                                  const std::string& key)>;
    // The same for the TypeSafe key (a one-judgment probe, llm::verify_key on TypeSafe's route);
    // blocking, on the worker.
    using VerifyTypeSafe = std::function<std::pair<bool, std::string>(const std::string& key)>;
    using ConfirmRemove =
        std::function<bool(QWidget* parent, const QString& title, const QString& text)>;

    ApiKeysSection(SettingsPage& page, SettingsDialog& dialog, std::shared_ptr<ApiKeysState> state,
                   Verify verify);
    ~ApiKeysSection() override;

    void setConfirmRemove(ConfirmRemove confirm) { confirmRemove_ = std::move(confirm); }
    // Replaces the text-model key check, which otherwise is llm::verify_api_key over the
    // context's HTTP client.
    void setTextVerify(VerifyText verify) { verifyText_ = std::move(verify); }
    // Replaces the TypeSafe key check, which otherwise is the context's JudgeKeys.
    void setTypeSafeVerify(VerifyTypeSafe verify) { verifyTypeSafe_ = std::move(verify); }

    // The section's controls (CONTROLS).
    ElidingComboBox* combo() const { return combo_; }
    FieldTile* credentialTile() const { return credentialTile_; }
    QLabel* statusIcon() const { return statusIcon_; }
    WrappedLabel* status() const { return status_; }
    WrappedLabel* usesCaption() const { return usesCaption_; }
    InfoTile* entryTile() const { return entryTile_; }
    WrappedLabel* storeCaption() const;
    QLineEdit* edit() const { return edit_; }
    Button* showButton() const { return showButton_; }
    Button* saveButton() const { return saveButton_; }
    Button* testButton() const { return testButton_; }
    DangerButton* removeButton() const { return removeButton_; }

    // A key test is running.
    bool testing() const { return testing_; }
    // A save or removal is running.
    bool storing() const { return storing_; }
    // Whether the credential state of the selected row has been read since the last change.
    bool stateCurrent() const;
    QString selectedName() const;

    void saveKey();
    void removeKey();
    void testKey();
    void updateControls();

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;
    // SettingsDeepLinks: select one credential, if given, and put the cursor in the key field.
    void focusCredential(const QString& env) override;

Q_SIGNALS:
    // keys_changed: a key was saved or removed; the rail and the app should re-read keys.
    void keysChanged();

private:
    void build(SettingsPage& page);
    void readState();
    void renderStatus();
    void renderUses(const QString& envName);
    void clearInput();
    void onCredentialChanged();
    void onShowToggled(bool checked);
    void onVerified(const QString& envName, bool ok, const QString& detail);
    void onStored(const QString& message, bool changed);
    Json settings() const;
    QString label(const QString& envName) const;
    void say(const QString& text);

    SettingsDialog& dialog_;
    std::shared_ptr<ApiKeysState> state_;
    Verify verify_;
    VerifyText verifyText_;
    VerifyTypeSafe verifyTypeSafe_;
    ConfirmRemove confirmRemove_;
    qt::TokenGate testGate_;
    std::shared_ptr<detail::Mailbox<ApiKeysSection>> mailbox_;

    ElidingComboBox* combo_ = nullptr;
    FieldTile* credentialTile_ = nullptr;
    QLabel* statusIcon_ = nullptr;
    WrappedLabel* status_ = nullptr;
    WrappedLabel* usesCaption_ = nullptr;
    InfoTile* entryTile_ = nullptr;
    QLineEdit* edit_ = nullptr;
    Button* showButton_ = nullptr;
    Button* saveButton_ = nullptr;
    Button* testButton_ = nullptr;
    DangerButton* removeButton_ = nullptr;

    bool testing_ = false;
    bool storing_ = false;
    bool loading_ = false;
    CredentialSource source_ = CredentialSource::None;
    std::vector<std::string> names_;
};

} // namespace fv
