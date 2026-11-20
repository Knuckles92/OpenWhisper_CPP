#include "ui_remote/history_publication.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

#include <QDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "agentapi/history_store.h"
#include "base/executor.h"
#include "remote/connection.h"
#include "remote/errors.h"
#include "remote/wire.h"

namespace fv::remote_history {
namespace {
using namespace std::chrono_literals;

constexpr auto kSession = 15min;
constexpr std::size_t kMaxRequest = 8192;

std::string field(const Json& object, const char* name) {
    const auto value = object.find(name);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}

std::string record_id(const Json& params, const char* name) {
    const auto id = field(params, name);
    if (params.size() != 1 || id.empty() || id.size() > 200 ||
        id.find_first_of("/\\?#") != std::string::npos ||
        std::ranges::any_of(id, [](unsigned char ch) { return ch < 32; })) {
        throw agentapi::InvalidQuery("Invalid history record id.");
    }
    return id;
}

Json projected(const Json& record) {
    Json result = Json::object();
    for (const auto* key :
         {"id", "title", "text", "preview", "started_at", "ended_at", "timestamp"}) {
        if (const auto value = record.find(key); value != record.end() && value->is_string()) {
            result[key] = *value;
        }
    }
    return result;
}

Json list_history(agentapi::HistoryStore& store, bool meetings, const Json& params) {
    agentapi::ListQuery query;
    if (!params.empty()) {
        if (params.size() != 1 || !params.contains("limit") ||
            !params["limit"].is_number_integer()) {
            throw agentapi::InvalidQuery("Invalid history limit.");
        }
        query.limit = params["limit"].get<std::int64_t>();
        if (query.limit < 1 || query.limit > 20) {
            throw agentapi::InvalidQuery("Invalid history limit.");
        }
    }
    const auto page = meetings ? store.meetings(query) : store.transcriptions(query);
    Json rows = Json::array();
    for (const auto& row : page.at("items")) {
        rows.push_back(projected(row));
    }
    return {{"items", std::move(rows)}};
}
} // namespace

Json publication_reply(agentapi::HistoryStore& store, const Json& request) {
    const auto id = field(request, "id");
    if (id.empty() || id.size() > 128 || field(request, "type") != "history_query") {
        return nullptr;
    }
    auto reply = Json{{"type", "history_result"}, {"id", id}};
    const auto operation = field(request, "operation");
    const auto params = request.find("params");
    try {
        if (params == request.end() || !params->is_object()) {
            throw agentapi::InvalidQuery("Invalid history request.");
        }
        if (operation == "transcriptions" || operation == "meetings") {
            reply["result"] = list_history(store, operation == "meetings", *params);
        } else if (operation == "transcription") {
            reply["result"] = projected(store.transcription(record_id(*params, "record_id")));
        } else if (operation == "meeting") {
            reply["result"] = projected(store.meeting(record_id(*params, "meeting_id")));
        } else if (operation == "check" && params->empty()) {
            store.check();
            reply["result"] = Json::object();
        } else {
            throw agentapi::InvalidQuery("Invalid history request.");
        }
    } catch (const agentapi::NotFound&) {
        reply["error"] = "not_found";
    } catch (const agentapi::InvalidQuery&) {
        reply["error"] = "invalid_query";
    } catch (const std::exception&) {
        reply["error"] = "unavailable";
    }
    return reply;
}

Publication::Publication(Executor& worker, std::filesystem::path database, CurrentPeer current,
                         Connect connect)
    : worker_(worker), database_(std::move(database)), current_(std::move(current)),
      connect_(std::move(connect)) {}

std::optional<std::uint64_t> Publication::start(std::string fingerprint, Status status) {
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (active_ || fingerprint.empty()) {
            return std::nullopt;
        }
        generation = ++generation_;
        active_ = true;
        cancel_ = CancelSource{};
    }
    worker_.post([self = shared_from_this(), fingerprint = std::move(fingerprint),
                  status = std::move(status)] { self->run(fingerprint, status); });
    return generation;
}

void Publication::stop(std::optional<std::uint64_t> generation) {
    std::lock_guard lock(mutex_);
    if (!active_ || (generation && generation_ != *generation)) {
        return;
    }
    cancel_.cancel();
    if (connection_) {
        connection_->close();
    }
}

void Publication::run(std::string fingerprint, Status status) {
    const auto deadline = std::chrono::steady_clock::now() + kSession;
    const auto token = cancel_.token();
    std::string outcome = "History sharing ended.";
    try {
        if (!token.cancelled()) {
            const auto selected = current_();
            if (!selected || selected->fingerprint != fingerprint) {
                throw remote::RemoteEngineError("The paired host changed. Start a new session.");
            }
            agentapi::HistoryStore store(database_);
            auto connection = connect_(*selected);
            if (!connection) {
                throw remote::RemoteEngineError("Unable to connect to the paired host.");
            }
            {
                std::lock_guard lock(mutex_);
                if (!token.cancelled()) {
                    connection_ = connection;
                }
            }
            const auto ready = connection->connect(token);
            const auto capabilities = ready.value("capabilities", Json::object());
            if (capabilities.value("client_history", false) != true) {
                throw remote::RemoteEngineError(
                    "This host does not support session history sharing.");
            }
            status("Sharing history. Close this window or choose Stop sharing to end the session.",
                   false);
            std::set<std::string> seenDictation;
            std::set<std::string> seenMeetings;
            while (!token.cancelled() && std::chrono::steady_clock::now() < deadline) {
                const auto current = current_();
                if (!current || current->fingerprint != fingerprint ||
                    current->device_id != selected->device_id) {
                    break;
                }
                const auto message = connection->receive_history(250ms);
                if (!message || message->size() > kMaxRequest) {
                    continue;
                }
                const auto request = Json::parse(*message, nullptr, false);
                if (!request.is_object()) {
                    continue;
                }
                const auto operation = field(request, "operation");
                const auto params = request.value("params", Json::object());
                const auto id = field(request, "id");
                auto reply = Json();
                if ((operation == "transcription" &&
                     !seenDictation.contains(field(params, "record_id"))) ||
                    (operation == "meeting" &&
                     !seenMeetings.contains(field(params, "meeting_id")))) {
                    if (!id.empty() && id.size() <= 128 &&
                        field(request, "type") == "history_query") {
                        reply = {{"type", "history_result"}, {"id", id}, {"error", "not_found"}};
                    }
                } else {
                    reply = publication_reply(store, request);
                }
                if (const auto result = reply.find("result");
                    result != reply.end() &&
                    (operation == "transcriptions" || operation == "meetings")) {
                    auto& seen = operation == "transcriptions" ? seenDictation : seenMeetings;
                    seen.clear();
                    for (const auto& item : result->at("items")) {
                        seen.insert(field(item, "id"));
                    }
                }
                if (reply.is_null()) {
                    continue;
                }
                auto payload = remote::dumps_default(reply);
                if (payload.size() > remote::MAX_REPLY_BYTES) {
                    reply.erase("result");
                    reply["error"] = "unavailable";
                    payload = remote::dumps_default(reply);
                }
                std::lock_guard lock(mutex_);
                const auto latest = current_();
                if (!token.cancelled() && std::chrono::steady_clock::now() < deadline && latest &&
                    latest->fingerprint == fingerprint &&
                    latest->device_id == selected->device_id) {
                    connection->send_history(std::move(payload));
                }
            }
        }
    } catch (const std::exception& error) {
        if (!token.cancelled()) {
            outcome = error.what();
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (connection_) {
            connection_->close();
            connection_.reset();
        }
        active_ = false;
    }
    status(std::move(outcome), true);
}

void openSession(QWidget& parent, const QString& host, std::string fingerprint,
                 std::shared_ptr<Publication> publication, Executor& ui) {
    const auto title = QObject::tr("Share history with %1").arg(host);
    QMessageBox consent(
        QMessageBox::Question, title,
        QObject::tr("Let %1 read this computer’s latest saved dictation text and meeting titles "
                    "and dates? This computer will answer those requests automatically while "
                    "the sharing window is open, for up to 15 minutes. Recordings and copies "
                    "kept for other computers are excluded.")
            .arg(host),
        QMessageBox::Yes | QMessageBox::Cancel, &parent);
    consent.setTextFormat(Qt::PlainText);
    consent.setDefaultButton(QMessageBox::Cancel);
    if (consent.exec() != QMessageBox::Yes) {
        return;
    }
    auto* dialog = new QDialog(&parent);
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(QObject::tr("Connecting to %1…").arg(host), dialog);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* stop = new QPushButton(QObject::tr("Stop sharing"), dialog);
    layout->addWidget(stop);
    QObject::connect(stop, &QPushButton::clicked, dialog, &QDialog::reject);
    const QPointer dialogGuard(dialog);
    const QPointer labelGuard(label);
    const QPointer stopGuard(stop);
    const auto started =
        publication->start(std::move(fingerprint), [&ui, dialogGuard, labelGuard,
                                                    stopGuard](std::string status, bool finished) {
            ui.post([dialogGuard, labelGuard, stopGuard, status = std::move(status), finished] {
                if (dialogGuard) {
                    labelGuard->setText(QString::fromStdString(status));
                    if (finished) {
                        stopGuard->setText(QObject::tr("Close"));
                    }
                }
            });
        });
    if (started) {
        QObject::connect(dialog, &QDialog::finished, dialog,
                         [publication, started] { publication->stop(started); });
        QObject::connect(dialog, &QObject::destroyed,
                         [publication, started] { publication->stop(started); });
    } else {
        label->setText(QObject::tr("A history sharing session is already running."));
        stop->setText(QObject::tr("Close"));
    }
    dialog->open();
}

} // namespace fv::remote_history
