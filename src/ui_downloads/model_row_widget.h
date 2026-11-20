#pragma once

#include <cstdint>
#include <string>

#include <QString>

#include "models/downloads_model.h"
#include "ui_downloads/download_row.h"

class QCheckBox;
class QLabel;
class QProgressBar;

namespace fv {

class Button;
class ElidingLabel;

// One catalog model in the Downloads list: its name, languages, size (on disk when downloaded,
// the bundled estimate otherwise), a status chip and the actions that apply (Download, Set
// Active, Delete). The row is "dumb": it renders the state handed to updateState and re-emits
// button clicks with its model name; scanning the cache and downloading belong to the page.
// Affinity: GUI thread.
class ModelRowWidget : public DownloadRow {
    Q_OBJECT
public:
    explicit ModelRowWidget(const QString& modelName, QWidget* parent = nullptr);

    // Renders a state from models::model_row_state. A downloaded model (no Download button, no
    // checkbox) is_cached; a state carries only active_style (is_active && cached), so isActive
    // reads that here.
    void updateState(const models::ModelRowState& state);
    // model_row_state(modelName, input), rendered; isActive is input.is_active, as in Python,
    // whether or not the model is downloaded.
    void updateState(const models::ModelRowInput& input);

    // The in-row download bar over the row's bottom edge: indeterminate when the total is not
    // known, else a percentage and "<done> of <total>" in the size label.
    void setProgress(std::int64_t done, std::int64_t total);

    // The usage chip ("Dictation", "Dictation · Meetings"); empty hides it.
    void setUsage(const QString& text);

    // ModelRowWidget.matches_filter: fuzzy match against the model name, then the repository id.
    bool matchesFilter(const QString& text) const;
    // What sorting and filtering read of the row.
    models::ModelRowFacts facts() const;

    const QString& modelName() const { return modelName_; }
    const QString& repoId() const { return repoId_; }
    bool isCached() const { return isCached_; }
    bool isActive() const { return isActive_; }
    double sortSizeBytes() const { return sortSizeBytes_; }

    QCheckBox* selectCheckbox() const { return selectCheckbox_; }
    QLabel* sizeLabel() const { return sizeLabel_; }
    QLabel* usageLabel() const { return usageLabel_; }
    Button* downloadButton() const { return downloadButton_; }
    Button* setActiveButton() const { return setActiveButton_; }
    Button* deleteButton() const { return deleteButton_; }
    QProgressBar* progressBar() const { return progress_; }

Q_SIGNALS:
    void downloadClicked(const QString& modelName);
    void deleteClicked(const QString& modelName);
    void setActiveClicked(const QString& modelName);
    void selectionToggled(const QString& modelName, bool checked);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void showProgress();
    void hideProgress();
    // Lays the bar over the row's bottom edge, inside the margins: a layout child would raise
    // the list column's minimum width even when hidden.
    void placeProgress();

    QString modelName_;
    QString repoId_;
    std::string backend_;
    bool isCached_ = false;
    bool isActive_ = false;
    double sortSizeBytes_ = 0;

    QCheckBox* selectCheckbox_ = nullptr;
    ElidingLabel* nameLabel_ = nullptr;
    ElidingLabel* repoLabel_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    QLabel* usageLabel_ = nullptr;
    Button* downloadButton_ = nullptr;
    Button* setActiveButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
};

} // namespace fv
