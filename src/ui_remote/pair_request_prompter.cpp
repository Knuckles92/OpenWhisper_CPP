#include "ui_remote/pair_request_prompter.h"

#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QPointer>

#include "base/executor.h"
#include "base/log.h"
#include "base/subscription.h"
#include "remote/control.h"
#include "ui_remote/pair_request_dialog.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.pair_request_dialog"};

// PairRequestPrompter: shows a dialog for each pairing request the host gets, and passes the
// answer on. It is a child of the window and hears the service through a Subscription, so a
// server thread never holds or reaches it once the window is gone.
class PairRequestPrompter final : public QObject {
public:
    PairRequestPrompter(remote::RemoteEngineControl& service, Executor& io, Executor& ui,
                        QWidget& window)
        : QObject(&window), service_(service), io_(io), ui_(ui), window_(window) {
        events_ = service_.changed.connect(ui_, [this](const std::string& event) {
            if (event == "pair_request" || event == "state") {
                sync();
            }
        });
    }

private:
    // Reads the pending request off the GUI thread, then shows, keeps or dismisses the dialog.
    void sync() {
        const auto generation = ++generation_;
        const QPointer<PairRequestPrompter> self(this);
        io_.post([self, service = &service_, ui = &ui_, generation] {
            std::optional<remote::PairRequestInfo> request;
            std::vector<std::string> grants;
            try {
                request = service->pending_pair_request();
            } catch (const std::exception& error) {
                kLog.warning("Could not read the pending pairing request: {}", error.what());
            }
            if (request) {
                try {
                    grants = service->pairing_grants();
                } catch (const std::exception& error) {
                    kLog.warning("Could not read what paired computers may do: {}", error.what());
                }
            }
            ui->post([self, generation, request = std::move(request), grants = std::move(grants)] {
                if (self && self->generation_ == generation) {
                    self->show(request, grants);
                }
            });
        });
    }

    void show(const std::optional<remote::PairRequestInfo>& request,
              const std::vector<std::string>& grants) {
        if (dialog_ && (!request || request->id != dialog_->requestId())) {
            // Withdrawn or replaced: nobody is waiting for this answer any more.
            QPointer<PairRequestDialog> gone = dialog_;
            dialog_.clear();
            gone->dismiss();
            gone->deleteLater();
        }
        if (!request || dialog_) {
            return;
        }
        auto* dialog = new PairRequestDialog(*request, &window_, grants);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &PairRequestDialog::answered, this, [this, id = request->id](bool allow) {
            io_.post([service = &service_, id, allow] {
                try {
                    (void)service->answer_pair_request(id, allow);
                } catch (const std::exception& error) {
                    kLog.warning("Could not answer the pairing request: {}", error.what());
                }
            });
        });
        dialog_ = dialog;
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
        QApplication::alert(dialog);
    }

    remote::RemoteEngineControl& service_;
    Executor& io_;
    Executor& ui_;
    QWidget& window_;
    Subscription events_;
    std::size_t generation_ = 0;
    QPointer<PairRequestDialog> dialog_;
};

} // namespace

void installPairRequestPrompter(remote::RemoteEngineControl& service, Executor& io, Executor& ui,
                                QWidget& window) {
    new PairRequestPrompter(service, io, ui, window);
}

} // namespace fv
