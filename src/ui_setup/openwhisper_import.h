#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "backup/import.h"
#include "base/subscription.h"
#include "models/openwhisper_discovery.h"
#include "ui_setup/openwhisper_import_dialog.h"

class QWidget;

namespace fv {

class AppContext;
class Executor;
class SettingsStore;

namespace backup {
class BackupCoordinator;
}

// Bringing an OpenWhisper install over (architecture 7.5 and 7.6), provided by
// SetupFeature::provide (ctx.service<OpenWhisperImport>()); the guided setup's Welcome page and
// the Backup page open it. It finds OpenWhisper's data folder and the speech runtime and model
// that can be adopted, off the GUI thread, and shows them in OpenWhisperImportDialog. "Import and
// restart" adopts the runtime and model (hard links into this app's own folders), then asks the
// backup coordinator to stage the data folder (BackupCoordinator::prepare_import); the window
// closes once the coordinator reports the stage through restore_staged, and the app's handler
// of that signal restarts it as after a restore.
// Affinity: GUI thread (UI actor); its blocking work runs on the worker executor.
class OpenWhisperImport : public QObject {
    Q_OBJECT
public:
    // Every blocking step and every service it reaches; a test replaces them.
    struct Ports {
        // backup::find_openwhisper_data: OpenWhisper's data folders, first match first.
        std::function<std::vector<backup::ImportSource>()> findSources;
        // backup::inspect_import_source of a folder the user picked.
        std::function<std::optional<backup::ImportSource>(const std::filesystem::path&)>
            inspectSource;
        // backup::openwhisper_running.
        std::function<bool(const std::filesystem::path&)> running;
        // models::discover_openwhisper: the runtimes and models to adopt.
        std::function<models::Discovery()> discoverModels;
        // models::adopt_component and adopt_model for each discovered one.
        std::function<void(const models::Discovery&)> adopt;
        // The app's backup coordinator; null while there is none.
        std::function<backup::BackupCoordinator*()> coordinator;
        // The folder the user picks ("" when cancelled), on the GUI thread.
        std::function<QString(QWidget* parent)> pickFolder;
    };

    // `worker` runs the blocking steps; `ui` is the GUI thread's executor. Both, and `store`,
    // must outlive this object.
    OpenWhisperImport(SettingsStore& store, Ports ports, Executor& worker, Executor& ui,
                      QObject* parent = nullptr);
    // The real steps over the context's paths, jobs and UI executors, and the coordinator the
    // context provides (backup::BackupCoordinator).
    explicit OpenWhisperImport(AppContext& context);
    ~OpenWhisperImport() override;

    // The last discovery (refresh(), open()) found an OpenWhisper data folder. Nothing looks
    // before a window asks: false until then.
    bool available() const;
    // fv_import records an import (it has an "at").
    bool imported() const;
    // What the guided setup's Welcome page offers the import by: available() and not
    // imported().
    bool offered() const;
    const ImportFindings& findings() const { return findings_; }

    // Discovery again, off the GUI thread; availabilityChanged follows when available() changes.
    // A folder the user picked is looked at again instead, and kept, until it no longer holds
    // anything to import (then the automatic discovery's answer replaces it).
    void refresh();
    // Shows the window (created once, on `parent`), with what is known, and looks again.
    void open(QWidget* parent);
    OpenWhisperImportDialog* dialog() const { return dialog_; }

    // The real ports over `context`.
    static Ports portsFor(AppContext& context);

Q_SIGNALS:
    void availabilityChanged(bool available);

private:
    struct Shared;

    void applyFindings(const ImportFindings& findings);
    void chooseFolder();
    void startImport(bool copyKeys);
    void finishImport(const std::string& stage, const std::string& error);

    SettingsStore& store_;
    Ports ports_;
    Executor& worker_;
    Executor& ui_;
    std::shared_ptr<Shared> shared_;
    ImportFindings findings_;
    models::Discovery discovery_;
    bool discovered_ = false;
    bool importing_ = false;
    // The coordinator's running job is this import's: another caller's refusal, reported through
    // the same restore_staged while it runs, is not this import's result.
    bool ownsJob_ = false;
    // The folder the user chose with "Choose folder...", kept across refresh() and open().
    std::optional<std::filesystem::path> pickedRoot_;
    QPointer<OpenWhisperImportDialog> dialog_;
    Subscription staged_;
};

} // namespace fv
