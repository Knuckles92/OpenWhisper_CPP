#include "ui_remote/share_meeting_dialog.h"

#include <exception>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "remote/protocol.h"
#include "remote_records/sync.h"

namespace fv {
namespace {

QString text(const Json& row, const char* name) {
    const auto value = row.find(name);
    return value != row.end() && value->is_string()
               ? qt::toQString(value->get_ref<const std::string&>())
               : QString();
}

class ShareMeetingDialog final : public QDialog {
public:
    ShareMeetingDialog(QWidget& parent, remote_records::RecordSync& records, QString name,
                       std::string fingerprint, Executor& io, Executor& ui)
        : QDialog(&parent), records_(records), name_(std::move(name)),
          fingerprint_(std::move(fingerprint)), io_(io), ui_(ui) {
        setWindowTitle(tr("Share a finished meeting"));
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName(QStringLiteral("shareFinishedMeeting"));
        resize(570, 235);
        auto* layout = new QVBoxLayout(this);
        auto* destination =
            new QLabel(tr("Destination: %1 (%2)")
                           .arg(name_, qt::toQString(remote::short_fingerprint(fingerprint_))),
                       this);
        destination->setTextFormat(Qt::PlainText);
        destination->setWordWrap(true);
        layout->addWidget(destination);
        meeting_ = new QComboBox(this);
        meeting_->setObjectName(QStringLiteral("shareMeetingSelection"));
        meeting_->setAccessibleName(tr("Finished meeting"));
        layout->addWidget(meeting_);
        audio_ = new QCheckBox(tr("Include captured microphone and system audio"), this);
        audio_->setObjectName(QStringLiteral("shareMeetingAudio"));
        layout->addWidget(audio_);
        status_ = new QLabel(tr("Loading finished meetings from this computer…"), this);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        share_ = buttons->addButton(tr("Share this meeting"), QDialogButtonBox::ActionRole);
        share_->setObjectName(QStringLiteral("shareSelectedMeeting"));
        share_->setAutoDefault(false);
        share_->setEnabled(false);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(share_, &QPushButton::clicked, this, &ShareMeetingDialog::confirm);
        connect(this, &QDialog::finished, this, [this] { operation_.cancel(); });
        layout->addWidget(buttons);
        readLocal();
    }

    ~ShareMeetingDialog() override { operation_.cancel(); }

private:
    void readLocal() {
        QPointer<ShareMeetingDialog> self(this);
        io_.post([self, records = &records_, ui = &ui_] {
            std::vector<Json> meetings;
            QString error;
            try {
                meetings = records->shareable_meetings(50);
            } catch (const std::exception& problem) {
                error = QString::fromUtf8(problem.what());
            }
            ui->post([self, meetings = std::move(meetings), error] {
                if (!self) {
                    return;
                }
                for (const auto& item : meetings) {
                    const auto title = text(item, "title");
                    const auto date = text(item, "started_at").left(10);
                    self->meeting_->addItem(
                        tr("%1 (%2)").arg(title.isEmpty() ? tr("Untitled meeting") : title, date),
                        text(item, "id"));
                }
                self->share_->setEnabled(self->meeting_->count() > 0);
                self->status_->setText(!error.isEmpty() ? error
                                       : meetings.empty()
                                           ? tr("No finished meetings are available.")
                                           : tr("This shares a copy. The original stays here."));
            });
        });
    }

    void confirm() {
        const auto id = qt::toUtf8(meeting_->currentData().toString());
        if (id.empty()) {
            return;
        }
        const bool includeAudio = audio_->isChecked();
        const auto contents = includeAudio ? tr("meeting transcript, notes, and captured audio")
                                           : tr("meeting transcript and notes");
        QMessageBox question(
            QMessageBox::Question, tr("Confirm meeting sharing"),
            tr("Send the %1 for %2 to %3?").arg(contents, meeting_->currentText(), name_),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        question.setTextFormat(Qt::PlainText);
        question.setDefaultButton(QMessageBox::Cancel);
        if (question.exec() != QMessageBox::Yes) {
            return;
        }

        operation_ = CancelSource();
        share_->setEnabled(false);
        meeting_->setEnabled(false);
        audio_->setEnabled(false);
        status_->setText(tr("Sharing the selected meeting with %1…").arg(name_));
        QPointer<ShareMeetingDialog> self(this);
        io_.post([self, records = &records_, ui = &ui_, host = fingerprint_, id, includeAudio,
                  cancel = operation_.token()] {
            QString error;
            try {
                records->share_meeting(host, id, includeAudio, cancel);
            } catch (const std::exception& problem) {
                error = QString::fromUtf8(problem.what());
            }
            ui->post([self, error] {
                if (!self || !self->isVisible()) {
                    return;
                }
                self->meeting_->setEnabled(true);
                self->audio_->setEnabled(true);
                self->share_->setEnabled(true);
                self->status_->setText(error.isEmpty()
                                           ? tr("The meeting was copied to %1.").arg(self->name_)
                                           : tr("Couldn't share the meeting: %1").arg(error));
            });
        });
    }

    remote_records::RecordSync& records_;
    QString name_;
    std::string fingerprint_;
    Executor& io_;
    Executor& ui_;
    CancelSource operation_;
    QComboBox* meeting_;
    QCheckBox* audio_;
    QPushButton* share_;
    QLabel* status_;
};

} // namespace

void openShareMeetingDialog(QWidget& parent, remote_records::RecordSync& records,
                            const QString& name, std::string fingerprint, Executor& io,
                            Executor& ui) {
    auto* dialog = new ShareMeetingDialog(parent, records, name, std::move(fingerprint), io, ui);
    dialog->open();
}

} // namespace fv
