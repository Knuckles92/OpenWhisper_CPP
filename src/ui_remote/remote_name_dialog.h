#pragma once

#include <optional>

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace fv {

// The name this computer shows for its paired host, or for a computer paired with it
// (settings_remote.py _ask_name; object name "remoteRenameDialog"): a line, a name field that
// starts from the current name with the original as its placeholder, Cancel and Rename.
// Affinity: GUI thread.
class RemoteNameDialog : public QDialog {
    Q_OBJECT
public:
    RemoteNameDialog(const QString& title, const QString& text, const QString& current,
                     const QString& own, QWidget* parent = nullptr);

    QLineEdit* edit() const { return edit_; }
    QPushButton* renameButton() const { return rename_; }

    // Runs the dialog: the name typed in ("" goes back to `own`), or nullopt when canceled.
    static std::optional<QString> ask(QWidget* parent, const QString& title, const QString& text,
                                      const QString& current, const QString& own);

private:
    QLineEdit* edit_;
    QPushButton* rename_;
};

} // namespace fv
