#include "meeting_web/archive.h"

#include <utility>

#include "meeting_state/stored.h"
#include "meeting_web/auth.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/server.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting_web {

namespace {

// engine.py MAX_GUEST_NAME_LEN, in characters.
constexpr std::int64_t kMaxGuestNameLen = 80;

std::string required_id(const Json& meeting) {
    if (!meeting.is_object() || !meeting.contains("id")) {
        throw py::PyError("KeyError", "'id'");
    }
    return detail::py_str(meeting.at("id"));
}

} // namespace

ArchivedMeetingDashboard::ArchivedMeetingDashboard(
    std::shared_ptr<meeting_state::MeetingRepository> repository, const Json& meeting,
    std::filesystem::path spool_root, const Json& settings, const Clock& clock)
    : repository_(std::move(repository)), meeting_id_(required_id(meeting)),
      spool_root_(std::move(spool_root)),
      // Historical dashboards must never claim work is still in flight.
      store_(meeting_state::open_store(repository_, meeting_id_, meeting, true, settings, clock)) {}

void ArchivedMeetingDashboard::attach_server(MeetingWebServer* server) {
    std::lock_guard lock(mutex_);
    server_ = server;
}

std::string ArchivedMeetingDashboard::host_url() const {
    std::lock_guard lock(mutex_);
    return server_ != nullptr ? server_->host_url() : std::string();
}

bool ArchivedMeetingDashboard::is_running() const {
    std::lock_guard lock(mutex_);
    return server_ != nullptr && server_->is_running();
}

void ArchivedMeetingDashboard::shutdown() {
    MeetingWebServer* server = nullptr;
    {
        std::lock_guard lock(mutex_);
        server = std::exchange(server_, nullptr);
    }
    if (server != nullptr) {
        server->stop();
    }
}

std::vector<Json> ArchivedMeetingDashboard::get_transcript(double after_start_s,
                                                           std::optional<std::int64_t> limit) {
    return repository_->get_segments(meeting_id_, after_start_s, limit);
}

std::string ArchivedMeetingDashboard::meeting_id() const {
    return meeting_id_;
}

std::shared_ptr<meeting_state::MeetingStateStore> ArchivedMeetingDashboard::store() const {
    return store_;
}

bool ArchivedMeetingDashboard::is_active() const {
    return false;
}

std::vector<meeting_state::OpResult> ArchivedMeetingDashboard::apply_client_action(
    std::string_view actor, const std::optional<std::string>& participant_id, const Json& op) {
    return store_->apply(actor, participant_id, Json::array({op}));
}

std::vector<meeting_state::OpResult>
ArchivedMeetingDashboard::undo(std::int64_t seq, const std::optional<std::string>& participant_id) {
    return store_->undo(seq, participant_id);
}

Json ArchivedMeetingDashboard::add_guest(std::string_view name) {
    std::string display = py::slice(py::strip(name), 0, kMaxGuestNameLen);
    if (display.empty()) {
        display = "Guest";
    }
    const auto results = store_->apply("system", std::nullopt,
                                       Json::array({Json{{"op", "upsert_participant"},
                                                         {"display_name", display},
                                                         {"kind", "guest"},
                                                         {"is_provisional", false}}}));
    const auto& result = results.at(0);
    if (!result.ok || !detail::truthy(result.effect)) {
        throw py::ValueError(result.reason.value_or("guest_rejected"));
    }
    return result.effect.at("participant");
}

void ArchivedMeetingDashboard::end() {}

void ArchivedMeetingDashboard::pause() {}

void ArchivedMeetingDashboard::resume() {}

Json ArchivedMeetingDashboard::regenerate_tokens() {
    const auto [host_token, guest_token] = generate_token_pair();
    repository_->replace_tokens(meeting_id_, host_token, guest_token);
    MeetingWebServer* server = nullptr;
    {
        std::lock_guard lock(mutex_);
        server = server_;
    }
    if (server != nullptr) {
        server->invalidate_connections();
    }
    return Json{{"host_url", server != nullptr ? server->host_url() : std::string()},
                {"guest_url", server != nullptr ? server->guest_url() : std::string()}};
}

std::filesystem::path ArchivedMeetingDashboard::spool_root() const {
    return spool_root_;
}

void ArchivedMeetingDashboard::set_cloud_enabled(bool enabled) {
    (void)enabled;
    throw EngineRefusal(std::string(kArchiveCloudRefusal));
}

} // namespace fv::meeting_web
