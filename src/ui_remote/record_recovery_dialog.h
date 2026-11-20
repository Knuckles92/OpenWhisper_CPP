#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QDialog>

#include "remote/control.h"

class QPushButton;

namespace fv {

class ElidingComboBox;

// "Recover stored records" (settings_remote.py _recover_records; object name
// "remoteRecordRecoveryDialog"): which old pairing's records go to which paired computer. Nothing
// is preselected for the person: Recover records is not the default button.
// Affinity: GUI thread.
class RecordRecoveryDialog : public QDialog {
    Q_OBJECT
public:
    RecordRecoveryDialog(const std::vector<remote::RecordOwnerGroup>& owners,
                         const std::vector<remote::PairedDevice>& devices,
                         QWidget* parent = nullptr);

    ElidingComboBox* ownerCombo() const { return source_; }
    ElidingComboBox* deviceCombo() const { return target_; }
    QPushButton* recoverButton() const { return recover_; }
    // (owner id, device id) as now chosen.
    std::pair<std::string, std::string> choice() const;

    // Runs the dialog: the choice, or nullopt when canceled.
    static std::optional<std::pair<std::string, std::string>>
    choose(QWidget* parent, const std::vector<remote::RecordOwnerGroup>& owners,
           const std::vector<remote::PairedDevice>& devices);

private:
    ElidingComboBox* source_;
    ElidingComboBox* target_;
    QPushButton* recover_;
};

} // namespace fv
