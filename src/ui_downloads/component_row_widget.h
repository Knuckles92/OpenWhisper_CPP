#pragma once

#include <cstdint>
#include <string_view>

#include <QString>

#include "models/downloads_model.h"
#include "ui_downloads/download_row.h"

class QLabel;
class QProgressBar;
class QTimer;

namespace fv {

class Button;
class ElidingLabel;

// One downloadable component (a speech runtime) in the Downloads list: its name, summary, size,
// install state and the actions that apply (Install, Update, Reinstall or Repair; Cancel while
// installing; Remove), with a progress bar while an install runs. The row is "dumb": it renders
// what updateState and setProgress hand it and re-emits clicks with its component id.
// Affinity: GUI thread.
class ComponentRowWidget : public DownloadRow {
    Q_OBJECT
public:
    // Once an install ends, Remove takes the place of Install and Cancel, under the pointer. It
    // stays disabled this long (milliseconds) so the second click of a double-click, or an
    // impatient one, cannot land on it.
    static constexpr int kRemoveGuardMs = models::kRemoveGuardMs;

    explicit ComponentRowWidget(const QString& componentId, QWidget* parent = nullptr);

    // component_row_state(info, installing, <was installing>), rendered.
    void updateState(const models::ComponentInfo& info, bool installing);
    // A state from models::component_row_state; `installing` is whether an install is in
    // flight, which setProgress and the primary button depend on.
    void updateState(const models::ComponentRowState& state, bool installing);

    // The badge by phase and the bar; the size label reads "<done> of <total>" while
    // downloading. Ignored unless an install is in flight.
    void setProgress(std::string_view phase, std::int64_t done, std::int64_t total);

    const QString& componentId() const { return componentId_; }
    bool installing() const { return installing_; }
    // The Remove button is held disabled after an install ends.
    bool removeGuardActive() const;

    ElidingLabel* nameLabel() const { return nameLabel_; }
    ElidingLabel* summaryLabel() const { return summaryLabel_; }
    ElidingLabel* sizeLabel() const { return sizeLabel_; }
    Button* installButton() const { return installButton_; }
    Button* removeButton() const { return removeButton_; }
    QProgressBar* progressBar() const { return progress_; }

Q_SIGNALS:
    void installClicked(const QString& componentId);
    void cancelClicked(const QString& componentId);
    void removeClicked(const QString& componentId);

private:
    // Install, or Cancel while installing.
    void onPrimaryClicked();
    void releaseRemove();

    QString componentId_;
    bool installing_ = false;

    ElidingLabel* nameLabel_ = nullptr;
    ElidingLabel* summaryLabel_ = nullptr;
    ElidingLabel* sizeLabel_ = nullptr;
    Button* installButton_ = nullptr;
    Button* removeButton_ = nullptr;
    QTimer* removeGuard_ = nullptr;
    QProgressBar* progress_ = nullptr;
};

} // namespace fv
