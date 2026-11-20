#pragma once

#include <optional>
#include <vector>

#include <QDialog>
#include <QString>

#include "backup/import.h"

class QCheckBox;
class QLabel;
class QVBoxLayout;

namespace fv {

class AnimatedProgressBar;
class Button;
class Card;
class PrimaryButton;
class WrappedLabel;

// What an import of OpenWhisper's data would bring, as OpenWhisperImport found it.
struct ImportFindings {
    // The data folder; nullopt when none was found.
    std::optional<backup::ImportSource> source;
    // The display names of the speech runtimes and the labels of the speech models adopted with
    // it (models::OpenWhisperAdopter::discover).
    std::vector<QString> runtimes;
    std::vector<QString> models;
    // OpenWhisper holds its data lease: it must be closed first.
    bool running = false;
};

// The window that brings an OpenWhisper install's data over (FioriVoce only, architecture 7.5):
// what was found (the folder, its settings, history, recordings, and the speech runtime and
// model that come with it), the safety rules it is imported under, the "Copy API keys" choice
// (off until ticked), and "Import and restart". It starts no work itself: importRequested and
// chooseFolderRequested ask its owner (OpenWhisperImport), which feeds back setFindings,
// setBusy and setError, and accepts the window once the import is staged.
// Affinity: GUI thread.
class OpenWhisperImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit OpenWhisperImportDialog(QWidget* parent = nullptr);

    // Before the first findings: "Looking for <legacy>'s data…", Import off.
    void setLooking();
    void setFindings(const ImportFindings& findings);
    // While the import runs: the bar shows, every button and the box are off, and the window
    // cannot be closed.
    void setBusy(bool busy);
    void setError(const QString& message);
    bool busy() const { return busy_; }
    bool copyKeys() const;

    // --- Parts, for tests ---
    QLabel* headerLabel() const { return header_; }
    QVBoxLayout* summaryLayout() const { return summary_; }
    // The summary's lines, in order.
    std::vector<QString> summaryLines() const;
    WrappedLabel* runningLabel() const { return running_; }
    WrappedLabel* statusLabel() const { return status_; }
    QCheckBox* copyKeysBox() const { return copyKeys_; }
    AnimatedProgressBar* progressBar() const { return progress_; }
    Button* folderButton() const { return folderButton_; }
    Button* laterButton() const { return laterButton_; }
    PrimaryButton* importButton() const { return importButton_; }

public Q_SLOTS:
    // Not while an import runs: its stage is the restart contract.
    void reject() override;

Q_SIGNALS:
    void importRequested(bool copyKeys);
    void chooseFolderRequested();

private:
    void clearSummary();
    void addLine(const QString& text);
    void updateEnabled();

    bool busy_ = false;
    bool hasSource_ = false;

    QLabel* header_ = nullptr;
    Card* card_ = nullptr;
    QVBoxLayout* summary_ = nullptr;
    WrappedLabel* running_ = nullptr;
    QCheckBox* copyKeys_ = nullptr;
    AnimatedProgressBar* progress_ = nullptr;
    WrappedLabel* status_ = nullptr;
    Button* folderButton_ = nullptr;
    Button* laterButton_ = nullptr;
    PrimaryButton* importButton_ = nullptr;
};

} // namespace fv
