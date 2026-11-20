#pragma once

#include <filesystem>
#include <optional>

#include <QDialog>

#include "ui_walkthrough/walkthrough_review.h"

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QToolButton;

namespace fv {

// The persistent Saved Walkthroughs window. Selection, refresh, retention and closing all
// share the storyboard's unsaved-edit and in-progress-operation guards. GUI thread.
class WalkthroughLibraryDialog final : public QDialog {
    Q_OBJECT
public:
    WalkthroughLibraryDialog(SettingsStore& settings, std::filesystem::path root, ReviewHost host,
                             QWidget* parent = nullptr);
    bool openFolder(std::optional<std::filesystem::path> folder = {});
    void reject() override;
    WalkthroughReview* review() const { return review_; }
    // Lists the walkthroughs as they are now, selecting folder (or keeping the selection).
    void refresh(const std::optional<std::filesystem::path>& folder);

Q_SIGNALS:
    void changed();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void selectItem(QListWidgetItem* current, QListWidgetItem* previous);
    void showRetention();
    void changeRetention();
    bool canClose();

    std::filesystem::path root_;
    WalkthroughReview* review_;
    QListWidget* list_;
    QLabel* count_;
    QToolButton* refresh_;
    QComboBox* keep_;
};

} // namespace fv
