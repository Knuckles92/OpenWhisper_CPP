#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QWidget>

#include "base/cancel.h"
#include "speech/endpoints.h"

class QComboBox;

namespace fv {

class Clock;
class Executor;
class SettingsStore;
class SpeechServerDialog;

namespace http {
class Client;
}

// The API backend's Server and Model fields.
//
// Server lists OpenAI, the hosted and self-hosted presets and the Custom slot, then an
// "Edit server…" action for the selected self-hosted one. Picking the Custom slot before it has
// an address, or the action, opens the editor and restores the current selection, so backing out
// changes nothing. Model lists OpenAI's curated models, or what the selected server's catalog
// reports beside the model already chosen. A catalog is fetched on the io executor when the
// server is picked and kept for the session, shared by every instance (both tabs and the Voice
// model page show the same servers). A worker never holds the widget: its result is delivered
// through a token gate that drops a result for an older request or a gone widget.
//
// Choices are saved as they are made; changed() follows each one. Affinity: GUI thread.
class SpeechServerControls : public QWidget {
    Q_OBJECT
public:
    // The key to send to endpoint's catalog route, or nullopt for none: speech::api_key over the
    // credential store. Runs on the io executor, because the store is blocking. Unset, catalog
    // requests carry no key.
    using KeyProvider = std::function<std::optional<std::string>(const speech::SpeechEndpoint&)>;
    // Runs the editor and says whether it was accepted. Unset, the dialog runs modally; a test
    // sets one to answer it without a nested event loop.
    using EditorRunner = std::function<bool(SpeechServerDialog&)>;

    // client and io outlive the widget and every request it started (the widget cancels them
    // when it goes, but a request still running holds both).
    SpeechServerControls(SettingsStore& settings, http::Client& client, Executor& io,
                         QWidget* parent = nullptr);
    ~SpeechServerControls() override;

    // The endpoint the saved settings select.
    const speech::SpeechEndpoint& endpoint() const { return endpoint_; }

    // Shows the saved server and model, without saving anything.
    void refresh();
    // Disables both fields while a reload is in flight or a recording runs.
    void setBusy(bool busy);

    // Asks the shown server for its catalog again, with the key, unless the session already holds
    // it: the constructor's request went out before a provider could be set.
    void setKeyProvider(KeyProvider provider);
    void setEditorRunner(EditorRunner runner) { editorRunner_ = std::move(runner); }
    // The clock OpenAI's retirement dates are judged by; the system clock until set. It applies
    // from the next refresh().
    void setClock(const Clock& clock) { clock_ = &clock; }

    QComboBox* serverCombo() const { return serverCombo_; }
    QComboBox* modelCombo() const { return modelCombo_; }
    QWidget* serverField() const { return serverField_; }
    QWidget* modelField() const { return modelField_; }

    // Combo data of the trailing "Edit server…" action, outside the endpoint-id space.
    static constexpr const char* kEditOption = "__edit__";
    // How many catalogs the session holds, and forgetting them all (tests start from none).
    static std::size_t catalogCount();
    static void resetCatalogs();

Q_SIGNALS:
    // The server or model changed.
    void changed();
    // A help link was followed: the Settings destination to open.
    void helpRequested(const QString& destination);

private:
    struct Mailbox;

    void showModels(const QString& current);
    void fetchModels();
    void onModelsDone(const speech::SpeechEndpoint& endpoint,
                      const std::optional<std::vector<std::string>>& models);
    void restoreServer();
    void onServerChanged();
    void edit(const speech::SpeechEndpoint& endpoint, bool select);
    void onModelChanged();
    std::chrono::year_month_day today() const;

    SettingsStore& settings_;
    http::Client& client_;
    Executor& io_;
    const Clock* clock_;
    speech::SpeechEndpoint endpoint_;
    KeyProvider keyProvider_;
    EditorRunner editorRunner_;
    CancelSource cancel_;
    std::shared_ptr<Mailbox> mailbox_;
    QComboBox* serverCombo_;
    QComboBox* modelCombo_;
    QWidget* serverField_;
    QWidget* modelField_;
};

} // namespace fv
