#include "ui_remote/client_history_dialog.h"

#include <exception>
#include <memory>
#include <utility>

#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "base/executor.h"
#include "remote/client_history_source.h"
#include "remote_records/sync.h"

namespace fv {
namespace {

QString label(const Json& item, const char* key) {
    const auto found = item.find(key);
    return found != item.end() && found->is_string()
               ? QString::fromStdString(found->get_ref<const std::string&>()).left(128 * 1024)
               : QString();
}

std::string string(const QString& value) {
    const auto encoded = value.toUtf8();
    return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
}

class HeldMeetingSource final : public remote::ClientHistorySource {
public:
    HeldMeetingSource(remote_records::RecordSync& sync, std::string host, std::string fingerprint)
        : sync_(sync), host_(std::move(host)), fingerprint_(std::move(fingerprint)) {}

    std::vector<remote::ClientHistoryStatus> clients() override {
        return {{fingerprint_, host_, "online"}};
    }

    Json query(std::string_view device, std::string_view operation, const Json& params) override {
        if (device != fingerprint_) {
            throw remote::ClientUnavailable("unavailable");
        }
        if (operation == "meetings") {
            return {{"items", sync_.list_held_meetings(fingerprint_, 20)}};
        }
        if (operation == "meeting") {
            return sync_.read_held_meeting(fingerprint_,
                                           params.at("meeting_id").get<std::string>());
        }
        throw remote::ClientUnavailable("invalid_query");
    }

private:
    remote_records::RecordSync& sync_;
    std::string host_;
    std::string fingerprint_;
};

class ClientHistoryDialog final : public QDialog {
public:
    ClientHistoryDialog(QWidget& parent, std::shared_ptr<remote::ClientHistorySource> source,
                        Executor& io, Executor& ui, bool meetingsOnly = false)
        : QDialog(&parent), source_(std::move(source)), io_(io), ui_(ui) {
        setObjectName(QStringLiteral("pairedHistoryDialog"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(meetingsOnly ? tr("Host-kept meetings") : tr("Paired computers' history"));
        resize(640, 510);
        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(
            meetingsOnly ? tr("Read a preview of a meeting kept on your paired host.")
                         : tr("Browse history that paired computers allow this host to read."));
        description->setWordWrap(true);
        layout->addWidget(description);
        auto* filters = new QHBoxLayout;
        devices_ = new QComboBox(this);
        devices_->setObjectName(QStringLiteral("pairedHistoryComputer"));
        devices_->setAccessibleName(tr("Computer"));
        kind_ = new QComboBox(this);
        kind_->setAccessibleName(tr("Record type"));
        kind_->addItem(tr("Dictations"));
        kind_->addItem(tr("Meetings"));
        if (meetingsOnly) {
            kind_->setCurrentIndex(1);
            kind_->hide();
        }
        list_ = new QPushButton(tr("List recent entries"), this);
        list_->setObjectName(QStringLiteral("pairedHistoryList"));
        refresh_ = new QPushButton(tr("Refresh computers"), this);
        filters->addWidget(devices_, 1);
        filters->addWidget(kind_);
        filters->addWidget(list_);
        filters->addWidget(refresh_);
        layout->addLayout(filters);
        notice_ = new QLabel(this);
        notice_->setTextFormat(Qt::PlainText);
        notice_->setWordWrap(true);
        layout->addWidget(notice_);
        rows_ = new QListWidget(this);
        rows_->setObjectName(QStringLiteral("pairedHistoryRows"));
        layout->addWidget(rows_, 1);
        open_ = new QPushButton(tr("Open selected entry"), this);
        open_->setObjectName(QStringLiteral("pairedHistoryOpen"));
        layout->addWidget(open_, 0, Qt::AlignLeft);
        detail_ = new QPlainTextEdit(this);
        detail_->setObjectName(QStringLiteral("pairedHistoryDetail"));
        detail_->setReadOnly(true);
        layout->addWidget(detail_, 1);

        connect(refresh_, &QPushButton::clicked, this, &ClientHistoryDialog::readClients);
        connect(list_, &QPushButton::clicked, this, &ClientHistoryDialog::readRows);
        connect(open_, &QPushButton::clicked, this, &ClientHistoryDialog::readEntry);
        connect(rows_, &QListWidget::currentItemChanged, this, [this] { updateActions(); });
        connect(devices_, &QComboBox::currentIndexChanged, this, [this] { resetView(); });
        connect(kind_, &QComboBox::currentIndexChanged, this, [this] { resetView(); });
        readClients();
    }

private:
    void updateActions() {
        const bool readable = !busy_ && !devices_->currentData().toString().isEmpty();
        devices_->setEnabled(!busy_);
        kind_->setEnabled(!busy_);
        list_->setEnabled(readable);
        refresh_->setEnabled(!busy_);
        open_->setEnabled(readable && rows_->currentItem() != nullptr);
    }

    void resetView() {
        rows_->clear();
        detail_->clear();
        updateActions();
    }

    void readClients() {
        busy_ = true;
        resetView();
        notice_->setText(tr("Checking paired computers…"));
        QPointer<ClientHistoryDialog> self(this);
        io_.post([self, source = source_, ui = &ui_] {
            std::vector<remote::ClientHistoryStatus> clients;
            QString error;
            try {
                clients = source->clients();
            } catch (const std::exception& problem) {
                error = QString::fromUtf8(problem.what());
            }
            ui->post([self, clients = std::move(clients), error] {
                if (!self) {
                    return;
                }
                self->devices_->clear();
                for (const auto& peer : clients) {
                    auto caption = QString::fromStdString(peer.name);
                    if (peer.status == "online") {
                        self->devices_->addItem(caption, QString::fromStdString(peer.device_id));
                    } else {
                        caption += peer.status == "sharing_disabled" ? tr(" · Sharing off")
                                                                     : tr(" · Unavailable");
                        self->devices_->addItem(caption);
                    }
                }
                self->busy_ = false;
                self->notice_->setText(!error.isEmpty()  ? error
                                       : clients.empty() ? tr("No paired computers.")
                                                         : QString());
                self->updateActions();
            });
        });
    }

    void readRows() {
        resetView();
        query(kind_->currentIndex() == 0 ? "transcriptions" : "meetings", {{"limit", 20}}, true);
    }

    void readEntry() {
        if (auto* selected = rows_->currentItem()) {
            const bool dictation = kind_->currentIndex() == 0;
            const auto id = string(selected->data(Qt::UserRole).toString());
            query(dictation ? "transcription" : "meeting",
                  {{dictation ? "record_id" : "meeting_id", id}}, false);
        }
    }

    void query(std::string operation, Json params, bool listing) {
        const auto device = string(devices_->currentData().toString());
        if (device.empty() || busy_) {
            return;
        }
        busy_ = true;
        updateActions();
        detail_->clear();
        notice_->setText(tr("Reading from %1…").arg(devices_->currentText()));
        QPointer<ClientHistoryDialog> self(this);
        io_.post([self, source = source_, ui = &ui_, device, operation = std::move(operation),
                  params = std::move(params), listing] {
            Json result;
            QString error;
            try {
                result = source->query(device, operation, params);
            } catch (const std::exception& problem) {
                error = QString::fromUtf8(problem.what());
            }
            ui->post([self, result = std::move(result), error, listing] {
                if (self) {
                    self->finishedQuery(result, error, listing);
                }
            });
        });
    }

    void finishedQuery(const Json& result, const QString& error, bool listing) {
        busy_ = false;
        notice_->setText(error);
        if (error.isEmpty() && listing) {
            if (const auto items = result.find("items");
                items != result.end() && items->is_array()) {
                for (const auto& row : *items) {
                    auto id = label(row, "id");
                    if (id.isEmpty() || id.size() > 256 || rows_->count() == 20) {
                        continue;
                    }
                    auto caption = label(row, "title");
                    if (caption.isEmpty()) {
                        caption = label(row, kind_->currentIndex() == 0 ? "text" : "preview");
                    }
                    if (caption.isEmpty()) {
                        caption = tr("Untitled entry");
                    }
                    auto* item = new QListWidgetItem(caption.simplified().left(180), rows_);
                    item->setData(Qt::UserRole, std::move(id));
                }
            }
            if (rows_->count() == 0) {
                notice_->setText(tr("No history was shared."));
            }
        } else if (error.isEmpty()) {
            auto content = label(result, "text");
            if (content.isEmpty()) {
                content = label(result, "transcript");
            }
            if (content.isEmpty()) {
                content = label(result, "preview");
            }
            if (content.isEmpty()) {
                content = label(result, "title");
            }
            detail_->setPlainText(content.isEmpty() ? tr("No text was shared for this entry.")
                                                    : content);
        }
        updateActions();
    }

    std::shared_ptr<remote::ClientHistorySource> source_;
    Executor& io_;
    Executor& ui_;
    QComboBox* devices_;
    QComboBox* kind_;
    QPushButton* list_;
    QPushButton* refresh_;
    QLabel* notice_;
    QListWidget* rows_;
    QPushButton* open_;
    QPlainTextEdit* detail_;
    bool busy_ = false;
};

} // namespace

void openClientHistory(QWidget& parent, remote::ClientHistorySource& source, Executor& io,
                       Executor& ui) {
    auto borrowed = std::shared_ptr<remote::ClientHistorySource>(&source, [](auto*) {});
    auto* dialog = new ClientHistoryDialog(parent, std::move(borrowed), io, ui);
    dialog->open();
}

void openHeldMeetingHistory(QWidget& parent, remote_records::RecordSync& records, std::string host,
                            std::string fingerprint, Executor& io, Executor& ui) {
    auto source =
        std::make_shared<HeldMeetingSource>(records, std::move(host), std::move(fingerprint));
    auto* dialog = new ClientHistoryDialog(parent, std::move(source), io, ui, true);
    dialog->open();
}

} // namespace fv
