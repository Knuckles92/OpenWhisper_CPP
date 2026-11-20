#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QDialog>

#include "llm/profiles.h"

class QComboBox;
class QLabel;
class QLineEdit;

namespace fv {

class Button;

// Editor for an OpenAI-compatible text-model endpoint (object name textEndpointDialog, modal, at
// least 460 px wide). It adds a custom endpoint, edits one, and edits a built-in local server
// preset, whose name and API are fixed so only its address and key variable change.
//
// The dialog validates and holds the fields; it writes nothing. Save runs the llm validators
// (the preset's address through normalize_local_server_url, any other through
// normalize_base_url), shows the first refusal as the error label's text, and on success keeps
// the cleaned fields for resultPayload() and accepts. A preset's name field is not shown but is
// still validated. Affinity: GUI thread.
class TextEndpointDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode {
        Add,         // a new custom endpoint
        EditCustom,  // an existing custom endpoint
        LocalPreset, // a built-in local server: address and key variable only
    };

    // The validated fields after a successful save. `id` is set when a profile was edited.
    struct Payload {
        std::string name;
        std::string baseUrl;
        std::string apiKeyEnv;
        std::string protocol; // "chat" or "responses"
        std::optional<std::string> id;
    };

    // profile is the custom endpoint or local server preset to edit; nullopt adds a custom one.
    explicit TextEndpointDialog(std::optional<llm::TextLLMProfile> profile = std::nullopt,
                                QWidget* parent = nullptr);
    ~TextEndpointDialog() override;

    Mode mode() const { return mode_; }
    // The fields of the last successful save; nullopt until then.
    const std::optional<Payload>& resultPayload() const { return payload_; }

    QLineEdit* nameEdit() const { return nameEdit_; }
    QLineEdit* urlEdit() const { return urlEdit_; }
    QLineEdit* envEdit() const { return envEdit_; }
    QComboBox* protocolCombo() const { return protocolCombo_; }
    QLabel* errorLabel() const { return errorLabel_; }
    Button* cancelButton() const { return cancelButton_; }
    Button* saveButton() const { return saveButton_; }

public Q_SLOTS:
    // Save endpoint / Save server: validates the fields and accepts, or shows why it cannot.
    void save();

private:
    std::optional<llm::TextLLMProfile> profile_;
    Mode mode_;
    std::optional<Payload> payload_;
    QLineEdit* nameEdit_;
    QLineEdit* urlEdit_;
    QLineEdit* envEdit_;
    QComboBox* protocolCombo_;
    QLabel* errorLabel_;
    Button* cancelButton_;
    Button* saveButton_;
    // The fields a preset keeps but never shows: they have no parent and are not in the layout,
    // so this dialog owns them.
    std::vector<std::unique_ptr<QWidget>> unplaced_;
};

} // namespace fv
