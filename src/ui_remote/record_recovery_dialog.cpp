#include "ui_remote/record_recovery_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/json.h"
#include "pyport/text.h"
#include "ui_remote/remote_text.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

// id[:8], in code points as Python slices.
QString shortId(const std::string& id) {
    return qt::toQString(py::slice(id, std::nullopt, 8));
}

} // namespace

RecordRecoveryDialog::RecordRecoveryDialog(const std::vector<remote::RecordOwnerGroup>& owners,
                                           const std::vector<remote::PairedDevice>& devices,
                                           QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("remoteRecordRecoveryDialog"));
    setWindowTitle(QStringLiteral("Recover stored records"));
    setMinimumWidth(300);
    resize(460, 280);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    layout->addWidget(new WrappedLabel(QStringLiteral(
        "Choose the old computer's records and its new pairing. The selected paired computer will "
        "be able to read, edit, and delete these records.")));
    source_ = new ElidingComboBox;
    source_->setObjectName(QStringLiteral("remoteRecoveryOwnerCombo"));
    source_->setAccessibleName(QStringLiteral("Stored records from old computer"));
    for (const remote::RecordOwnerGroup& owner : owners) {
        // Counts only: the size of an old pairing's records is not known here.
        Json counts = Json::object();
        for (const auto& [kind, count] : owner.counts) {
            counts[kind] = Json{{"count", count}};
        }
        source_->addItem(QStringLiteral("%1 · %2 · %3")
                             .arg(qt::toQString(owner.name), shortId(owner.id),
                                  qt::toQString(remote_text::stored_phrase(counts))),
                         qt::toQString(owner.id));
    }
    layout->addWidget(new QLabel(QStringLiteral("Stored records from")));
    layout->addWidget(source_);
    target_ = new ElidingComboBox;
    target_->setObjectName(QStringLiteral("remoteRecoveryDeviceCombo"));
    target_->setAccessibleName(QStringLiteral("Paired computer to receive access"));
    for (const remote::PairedDevice& device : devices) {
        target_->addItem(
            QStringLiteral("%1 · %2").arg(qt::toQString(device.name), shortId(device.id)),
            qt::toQString(device.id));
    }
    layout->addWidget(new QLabel(QStringLiteral("Assign to paired computer")));
    layout->addWidget(target_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    recover_ = buttons->addButton(QStringLiteral("Recover records"), QDialogButtonBox::AcceptRole);
    recover_->setObjectName(QStringLiteral("primaryButton"));
    recover_->setDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

std::pair<std::string, std::string> RecordRecoveryDialog::choice() const {
    return {qt::toUtf8(source_->currentData().toString()),
            qt::toUtf8(target_->currentData().toString())};
}

std::optional<std::pair<std::string, std::string>>
RecordRecoveryDialog::choose(QWidget* parent, const std::vector<remote::RecordOwnerGroup>& owners,
                             const std::vector<remote::PairedDevice>& devices) {
    RecordRecoveryDialog dialog(owners, devices, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.choice();
}

} // namespace fv
