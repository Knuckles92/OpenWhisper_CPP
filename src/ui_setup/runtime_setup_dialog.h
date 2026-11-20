#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QDialog>
#include <QString>

#include "base/cancel.h"
#include "models/engine_resolution.h"
#include "ui_setup/runtime_setup.h"

#include "models/openwhisper_discovery.h"

class QLabel;
class QVBoxLayout;

namespace fv {

class AnimatedProgressBar;
class Button;
class PrimaryButton;
class WrappedLabel;

// Where the user gets the speech runtime and model from: what an OpenWhisper install already
// downloaded, a NeMo-Speech.cpp folder, or a model file. It looks when it opens (off the GUI
// thread) and closes itself, accepted, as soon as the Parakeet engine would find both; "Later"
// closes it rejected and changes nothing. Once it is closed or destroyed, work still running on
// the io executor is cancelled and its result is dropped.
// Affinity: GUI thread.
class RuntimeSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit RuntimeSetupDialog(std::shared_ptr<const RuntimeSetupPorts> ports,
                                QWidget* parent = nullptr);
    ~RuntimeSetupDialog() override;

    // The last look found a runtime and a model the engine can load.
    bool usable() const { return usable_; }
    // Whether the first look has arrived.
    bool looked() const { return looked_; }
    // An adoption, a folder check or a model import is running.
    bool busy() const { return busy_; }
    // The rows "Use these" will adopt, as shown.
    QStringList offered() const;
    // The runtime and model lines, and the message of the last action (empty before one).
    QString readinessText() const;
    QString resultText() const;

    PrimaryButton* useButton() const { return useButton_; }
    Button* folderButton() const { return folderButton_; }
    Button* modelButton() const { return modelButton_; }
    Button* laterButton() const { return laterButton_; }
    AnimatedProgressBar* progressBar() const { return progress_; }

    void done(int result) override;

private:
    // What one pass on the io executor found.
    struct Snapshot {
        models::Discovery discovery;
        models::ResolvedEngine resolved;
    };
    // The outcome of one user action.
    struct Outcome {
        std::string message;
        bool failed = false;
        bool canceled = false;
    };

    // Runs on the io executor; never throws.
    static Snapshot take(const RuntimeSetupPorts& ports);
    void look();
    void apply(const Snapshot& snapshot);
    void runAction(const QString& busyText, std::function<std::string(const CancelToken&)> work);
    void finish(const Outcome& outcome, const Snapshot& snapshot);
    void setBusy(bool busy);
    void useOffered();
    void chooseFolder();
    void chooseModel();
    void rebuildRows();
    void updateEnabled();

    // Where io work hands its result back: the newest request's token, and the dialog while it
    // lives. A worker must not post to the dialog through a pointer it read unguarded (it races
    // with the destruction, see appkit/qt_bridge.cpp), so the destructor clears the pointer under
    // the mailbox's lock and a worker posts only while holding it.
    struct Mailbox;

    std::shared_ptr<const RuntimeSetupPorts> ports_;
    std::shared_ptr<Mailbox> mailbox_;
    CancelSource cancel_;

    bool usable_ = false;
    bool looked_ = false;
    bool busy_ = false;
    models::Discovery discovery_;

    QLabel* readinessLabel_;
    QLabel* offerInfoLabel_;
    QVBoxLayout* rowsLayout_;
    std::vector<QLabel*> rows_;
    PrimaryButton* useButton_;
    Button* folderButton_;
    Button* modelButton_;
    Button* laterButton_;
    AnimatedProgressBar* progress_;
    WrappedLabel* resultLabel_;
};

} // namespace fv
