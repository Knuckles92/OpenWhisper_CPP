#pragma once

#include <vector>

#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace fv {

// The compact assignment of a local speech model for Settings: a combo of the models that can be
// chosen, a "Manage downloads" link to the Downloads page, and a caption line. Object name
// localModelPicker. Downloads owns download and delete; this only chooses which model to load.
//
// OpenWhisper's picker also offered "auto", Whisper sizes and custom models; FioriVoce has none
// of them, so the choices are the Parakeet models of the speech catalog. Affinity: GUI thread.
class LocalModelPicker : public QWidget {
    Q_OBJECT
public:
    struct Model {
        QString key;
        QString label;
    };

    explicit LocalModelPicker(QWidget* parent = nullptr);

    // The models of the speech catalog in its order. With meetingOnly, only those a meeting can
    // use (the Meeting page's picker: SpeechModel::meeting), sorted by label without regard to
    // case.
    static std::vector<Model> catalogModels(bool meetingOnly = false);

    // The staged model id: the selected item's key, else the combo's text, else the last
    // selected one.
    QString currentModel() const;
    // Rebuilds the combo from models without emitting modelChanged and shows selected, or the
    // first model when selected is not among them. Clears the caption.
    void setModels(const std::vector<Model>& models, const QString& selected);
    // Shows key (when listed) without emitting, so a later change to the same model is not a
    // change.
    void setCurrent(const QString& key);
    // The line under the combo; empty hides it.
    void setCaption(const QString& text);

    QComboBox* modelCombo() const { return modelCombo_; }
    QPushButton* manageButton() const { return manageButton_; }
    QLabel* captionLabel() const { return captionLabel_; }

Q_SIGNALS:
    // A person chose another model.
    void modelChanged(const QString& key);
    // The "Manage downloads" link was clicked.
    void manageRequested();

private:
    void onComboChanged();

    QString selected_;
    QComboBox* modelCombo_;
    QPushButton* manageButton_;
    QLabel* captionLabel_;
};

} // namespace fv
