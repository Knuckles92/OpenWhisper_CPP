#pragma once

#include <optional>
#include <string>

#include <QDialog>

#include "speech/endpoints.h"

class QLabel;
class QLineEdit;

namespace fv {

class Button;
class SettingsStore;

// Editor for a configurable speech server's address, model and key variable (object name
// speechServerDialog, modal, at least 460 px wide). Save server writes api_transcription_servers
// through settings and accepts; a value the endpoint functions refuse stays in the dialog as the
// text of the error label. Cancel changes nothing. Affinity: GUI thread.
class SpeechServerDialog : public QDialog {
    Q_OBJECT
public:
    // What Save server wrote, as the endpoint functions now read it back.
    struct Saved {
        std::string baseUrl;
        std::string apiKeyEnv;
        std::string model;
    };

    // endpoint is the server to edit (its address and key variable prefill the fields); the model
    // comes from settings, which the dialog also writes.
    SpeechServerDialog(SettingsStore& settings, const speech::SpeechEndpoint& endpoint,
                       QWidget* parent = nullptr);

    const speech::SpeechEndpoint& endpoint() const { return endpoint_; }
    // Nullopt until Save server has succeeded.
    const std::optional<Saved>& saved() const { return saved_; }

    QLineEdit* urlEdit() const { return urlEdit_; }
    QLineEdit* modelEdit() const { return modelEdit_; }
    QLineEdit* envEdit() const { return envEdit_; }
    QLabel* errorLabel() const { return errorLabel_; }
    Button* saveButton() const { return saveButton_; }

public Q_SLOTS:
    // Save server: writes the fields and accepts, or shows why it cannot.
    void save();

private:
    SettingsStore& settings_;
    speech::SpeechEndpoint endpoint_;
    std::optional<Saved> saved_;
    QLineEdit* urlEdit_;
    QLineEdit* modelEdit_;
    QLineEdit* envEdit_;
    QLabel* errorLabel_;
    Button* saveButton_;
};

} // namespace fv
