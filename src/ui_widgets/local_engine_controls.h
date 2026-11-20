#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

#include "base/json.h"

class QComboBox;

namespace fv {

class SettingsStore;

// What the local engine fields need to know of the speech catalog: the models of a backend, what
// settings select among them, and the languages the backend takes. The widgets sit below the
// catalog (fv_models, fv_speech), so the feature that builds them hands it over. Every member
// must be set. Affinity: called on the GUI thread.
struct LocalEngineCatalog {
    struct Model {
        QString key;
        QString label;
    };
    // BACKENDS: whether backend has local fields at all.
    std::function<bool(const QString& backend)> knows;
    // The backend's models, in catalog order.
    std::function<std::vector<Model>(const QString& backend)> models;
    // selected_model / selected_device: what settings select for backend, never outside it.
    std::function<QString(const Json& settings, const QString& backend)> selectedModel;
    std::function<QString(const Json& settings, const QString& backend)> selectedDevice;
    // language_choices as (code, label), in order; selected_language for a stored code.
    std::function<std::vector<std::pair<QString, QString>>(const QString& backend)> languages;
    std::function<QString(const QString& backend, const QString& stored)> selectedLanguage;
    // Whether "Languages I dictate in" decides the dictation language for backend, as the saved
    // settings stand with backend selected (dictation_language.job_language is set and the engine
    // takes more than one language). Optional: unset, the engine's Language field always shows.
    std::function<bool(const Json& settings, const QString& backend)> dictationDecidesLanguage;
};

// Gives each model of combo its download size as a tooltip ("About 714 MB download"), read from
// the bundled model profiles by the model key kept as item data. Only the tooltip: the item text
// stays what the catalog labels it. A model without a known size has no tooltip. Affinity: GUI
// thread.
void showDownloadSizes(QComboBox* combo);

// The Model / Device / Language columns of the engine card's field row, for the Parakeet
// engine: persists changes to settings and emits engineSettingsChanged so the controller can
// reload the backend. One per tab; every view of the saved engine reloads through setBackend,
// which blocks signals during the update.
//
// With dictation (Quick Record, the Voice model page), the Language field steps aside while
// "Languages I dictate in" sets the dictation language, so the page shows one language control.
// Upload File keeps it: files always use the engine's language. Affinity: GUI thread.
class LocalEngineControls : public QWidget {
    Q_OBJECT
public:
    // backend is the engine whose fields show first (a selected_model value).
    LocalEngineControls(SettingsStore& settings, LocalEngineCatalog catalog, const QString& backend,
                        bool dictation = false, QWidget* parent = nullptr);

    // Shows the fields of backend as saved, without emitting.
    void setBackend(const QString& backend);
    // Shows the Language field unless the dictation languages decide instead.
    void syncLanguageField();
    bool dictation() const { return dictation_; }
    // Switches between the dictation surfaces (Quick Record, Voice model) and the file one
    // (Upload File), and shows or hides the Language field to match.
    void setDictation(bool dictation);
    const QString& backend() const { return backend_; }
    // Populates the fields from persisted settings (no signal emitted).
    void loadFromSettings();
    // Disables the fields while a reload is in flight or during recording.
    void setBusy(bool busy);

    QComboBox* modelCombo() const { return modelCombo_; }
    QComboBox* deviceCombo() const { return deviceCombo_; }
    QComboBox* languageCombo() const { return languageCombo_; }
    QWidget* languageField() const { return languageField_; }

Q_SIGNALS:
    // After a person's change has been persisted to settings.
    void engineSettingsChanged();
    // A help link was followed: the Settings destination to open.
    void helpRequested(const QString& destination);

private:
    void onChanged();

    SettingsStore& settings_;
    LocalEngineCatalog catalog_;
    QString backend_;
    bool dictation_;
    QComboBox* modelCombo_;
    QComboBox* deviceCombo_;
    QComboBox* languageCombo_;
    QWidget* languageField_;
};

} // namespace fv
