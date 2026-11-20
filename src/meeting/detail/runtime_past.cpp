// MeetingRuntime: dashboards in the browser and Past Meetings (services/runtime/meeting.py
// open_dashboard, show_past_meeting, open_report, open_past_meeting,
// _open_past_meeting_worker, copy_past_meeting_transcript, delete_past_meeting,
// clear_past_meetings, meeting_busy, meeting_in_use, retitle_saved_meeting,
// _shutdown_archive_dashboard). Dashboard links carry capability tokens: only their origin is ever
// logged.

#include <algorithm>
#include <exception>
#include <format>
#include <set>
#include <utility>

#include "automation/event_bus.h"
#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/export_txt.h"
#include "meeting_state/stored.h"
#include "meeting_web/server.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

class ServerTitles final : public DashboardTitles {
public:
    ServerTitles(meeting_web::MeetingWebServer& server, meeting_web::DashboardApi& api)
        : server_(server), api_(api) {}

    bool is_running() const override { return server_.is_running(); }
    bool retitle_saved_meeting(const std::string& meeting_id, const std::string& title) override {
        return api_.retitle_saved_meeting(meeting_id, title);
    }
    void refresh_saved_meeting_title(const std::string& meeting_id) override {
        api_.refresh_saved_meeting_title(meeting_id);
    }

private:
    meeting_web::MeetingWebServer& server_;
    meeting_web::DashboardApi& api_;
};

} // namespace

std::shared_ptr<DashboardTitles> dashboard_titles(meeting_web::MeetingWebServer& server,
                                                  meeting_web::DashboardApi& api) {
    return std::make_shared<ServerTitles>(server, api);
}

void MeetingRuntime::report_dashboard_error(const std::string& message) {
    meeting_error.emit(message);
}

bool MeetingRuntime::open_url(const std::string& url) {
    if (!deps_.open_url) {
        logger.warning("No browser is available to open {}", redact_meeting_url(url));
        return false;
    }
    try {
        return deps_.open_url(url);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Could not open {}", redact_meeting_url(url)));
        return false;
    }
}

void MeetingRuntime::open_dashboard() {
    std::optional<std::string> url;
    std::optional<std::string> card_id;
    std::string live_id;
    {
        const std::lock_guard lock(mutex_);
        url = host_url_;
        card_id = card_meeting_id_;
        live_id = engine_ ? engine_->meeting_id() : std::string();
    }
    // A leftover from a different past meeting must not reuse the live URL without a history
    // target: that would open the wrong session.
    if (!is_active() && card_id && !card_id->empty() && *card_id != live_id) {
        open_past_meeting(*card_id);
        return;
    }
    if (!url || url->empty()) {
        // A retained finalization card may outlive its original server.
        if (card_id && !card_id->empty()) {
            open_past_meeting(*card_id);
            return;
        }
        report_dashboard_error(std::string(messages::kNoDashboard));
        return;
    }
    logger.info("Opening meeting dashboard: {}", redact_meeting_url(*url));
    open_url(*url);
}

void MeetingRuntime::show_past_meeting(std::string meeting_id) {
    // Loads one persisted meeting onto the card without starting the archive server or opening
    // a browser; refused while a live meeting runs, so the card cannot read active false.
    const std::string target = py::strip(meeting_id);
    if (target.empty()) {
        return;
    }
    bool live = false;
    {
        const std::lock_guard lock(mutex_);
        live = is_active_locked() || controller_meeting_active_;
    }
    if (live) {
        meeting_status_update.emit(std::string(messages::kFinishCurrentFirst));
        return;
    }
    spawn("meeting-show-past", [this, target] { show_past_meeting_worker(target); });
}

void MeetingRuntime::show_past_meeting_worker(const std::string& meeting_id) {
    try {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            report_dashboard_error(std::string(messages::kMeetingGone));
            return;
        }
        bool background = false;
        {
            const std::lock_guard lock(mutex_);
            background = background_engines_.contains(meeting_id);
        }
        if (background) {
            post_ui([this, meeting_id] { open_past_meeting(meeting_id); });
            return;
        }
        hydrate_finalization_card(*meeting, true);
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to show past meeting");
        report_dashboard_error(messages::could_not_load_past(error.what()));
    }
}

void MeetingRuntime::open_report() {
    std::optional<std::string> meeting_id;
    {
        const std::lock_guard lock(mutex_);
        meeting_id = card_meeting_id_;
    }
    if (!meeting_id || meeting_id->empty()) {
        report_dashboard_error(std::string(messages::kNoReport));
        return;
    }
    open_past_meeting(*meeting_id, true);
}

void MeetingRuntime::open_past_meeting(std::string meeting_id, bool report) {
    // Reuses the live or most recent meeting server; after a restart a light archive server
    // starts without capture, ASR, diarization or meeting intelligence.
    const std::string target = py::strip(meeting_id);
    if (target.empty()) {
        return;
    }
    bool busy = false;
    {
        const std::lock_guard lock(mutex_);
        busy = archive_starting_;
        archive_starting_ = true;
    }
    if (busy) {
        meeting_status_update.emit(std::string(messages::kOpeningAPastMeeting));
        return;
    }
    sync_activity();
    spawn("meeting-history-dashboard",
          [this, target, report] { open_past_meeting_worker(target, report ? "report" : ""); });
}

void MeetingRuntime::open_past_meeting_worker(const std::string& meeting_id,
                                              const std::string& view) {
    struct Release {
        MeetingRuntime& runtime;
        ~Release() {
            {
                const std::lock_guard lock(runtime.mutex_);
                runtime.archive_starting_ = false;
            }
            runtime.sync_activity();
        }
    } release{*this};
    try {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            report_dashboard_error(std::string(messages::kMeetingGone));
            return;
        }
        std::optional<std::string> background_url;
        bool in_background = false;
        {
            const std::lock_guard lock(mutex_);
            if (const auto found = background_host_urls_.find(meeting_id);
                found != background_host_urls_.end()) {
                background_url = found->second;
            }
            in_background = background_engines_.contains(meeting_id);
        }
        if (in_background) {
            if (background_url) {
                open_url(*background_url);
            } else {
                meeting_status_update.emit(std::string(messages::kStillInBackground));
            }
            return;
        }
        hydrate_finalization_card(*meeting, true);

        std::string url;
        std::shared_ptr<RuntimeArchive> archive;
        {
            const std::lock_guard lock(mutex_);
            url = host_url_.value_or("");
            archive = archive_;
        }
        if (url.empty() && archive && archive->is_running()) {
            url = archive->host_url();
        }
        if (url.empty()) {
            if (is_active()) {
                report_dashboard_error(std::string(messages::kLiveDashboardUnavailable));
                return;
            }
            shutdown_engine(std::nullopt);
            shutdown_archive_dashboard();
            const Json settings = settings_snapshot();
            bool background = false;
            {
                const std::lock_guard lock(mutex_);
                background = !background_engines_.empty();
            }
            const auto port = static_cast<std::uint16_t>(
                background
                    ? 0
                    : std::clamp<std::int64_t>(keys::meeting_server_port.read(settings), 0, 65535));
            archive =
                deps_.archive_factory(*meeting, keys::meeting_server_bind.read(settings), port);
            {
                const std::lock_guard lock(mutex_);
                archive_ = archive;
            }
            url = archive ? archive->host_url() : std::string();
        }
        if (url.empty()) {
            report_dashboard_error(std::string(messages::kNoDashboardLink));
            return;
        }
        const std::string history_url = with_history_target(url, meeting_id, view);
        logger.info("Opening past meeting dashboard: {}", redact_meeting_url(history_url));
        meeting_status_update.emit(std::string(view == "report" ? messages::kOpeningReport
                                                                : messages::kOpeningPastMeeting));
        open_url(history_url);
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to open past meeting");
        report_dashboard_error(messages::could_not_open_past(error.what()));
    }
}

void MeetingRuntime::shutdown_archive_dashboard() {
    std::shared_ptr<RuntimeArchive> archive;
    {
        const std::lock_guard lock(mutex_);
        archive = std::move(archive_);
        archive_.reset();
    }
    if (!archive) {
        return;
    }
    try {
        archive->shutdown();
    } catch (const std::exception& error) {
        logger.debug("Error shutting down archive dashboard: {}", error.what());
    }
}

std::string MeetingRuntime::copy_past_meeting_transcript(std::string meeting_id) {
    const std::string target = py::strip(meeting_id);
    if (target.empty()) {
        return {};
    }
    try {
        const auto meeting = deps_.repository->get_meeting(target);
        if (!meeting) {
            meeting_status_update.emit(std::string(messages::kMeetingGone));
            return {};
        }
        const Json state = meeting_state::stored_state_dict(*meeting);
        const std::vector<Json> segments =
            deps_.repository->get_segments(target, -1.0, std::nullopt);
        const bool any_text = std::ranges::any_of(segments, [](const Json& segment) {
            return !py::strip(detail::text_of(segment, "text")).empty();
        });
        if (!any_text) {
            meeting_status_update.emit(std::string(messages::kNoTranscript));
            return {};
        }
        return meeting_state::export_transcript_txt(*meeting, state, segments, clock_);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Could not copy transcript for meeting {}", target));
        meeting_error.emit(messages::could_not_copy(error.what()));
        return {};
    }
}

void MeetingRuntime::forget_deleted(const std::string& /*meeting_id*/, bool /*has_origin*/) {
    // Meeting segments carry what was said: a reconnecting event stream must not get them back.
    try {
        if (deps_.forget_replayable_events) {
            deps_.forget_replayable_events();
        } else {
            automation::clear_bus();
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not forget replayable meeting events");
    }
}

void MeetingRuntime::delete_past_meeting(std::string meeting_id, bool delete_recordings) {
    const std::string target = py::strip(meeting_id);
    if (target.empty()) {
        return;
    }
    std::string live_id;
    bool background = false;
    bool live = false;
    {
        const std::lock_guard lock(mutex_);
        live_id = engine_ ? engine_->meeting_id() : std::string();
        background = background_engines_.contains(target);
        live = is_active_locked() || controller_meeting_active_;
    }
    if (background) {
        meeting_status_update.emit(std::string(messages::kStillInBackground));
        return;
    }
    if (live && live_id == target) {
        meeting_status_update.emit(std::string(messages::kFinishCurrentFirst));
        return;
    }
    if (meeting_busy(target)) {
        // Finishing in the foreground, or its post-meeting steps re-running.
        meeting_status_update.emit(std::string(messages::kWaitPostMeeting));
        return;
    }
    spawn("meeting-delete", [this, target, delete_recordings] {
        delete_past_meeting_worker(target, delete_recordings);
    });
}

void MeetingRuntime::delete_past_meeting_worker(const std::string& meeting_id,
                                                bool delete_recordings) {
    try {
        {
            const std::lock_guard lock(mutex_);
            if (background_engines_.contains(meeting_id)) {
                return;
            }
        }
        meeting_state::delete_meeting_data(
            *deps_.repository, meeting_id, deps_.meetings_root, delete_recordings,
            [this](std::string id, bool has_origin) { forget_deleted(id, has_origin); });
        bool shown = false;
        {
            const std::lock_guard lock(mutex_);
            shown = card_meeting_id_ == meeting_id;
        }
        if (shown) {
            hide_finalization_card();
        } else {
            refresh_past_meetings();
        }
        meeting_status_update.emit(std::string(messages::kDeleted));
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to delete meeting '{}'", meeting_id));
        meeting_error.emit(messages::could_not_delete(error.what()));
    }
}

void MeetingRuntime::clear_past_meetings(bool delete_recordings) {
    // The in-progress meeting is never removed; one worker walks the repository.
    spawn("meeting-clear",
          [this, delete_recordings] { clear_past_meetings_worker(delete_recordings); });
}

void MeetingRuntime::clear_past_meetings_worker(bool delete_recordings) {
    try {
        std::set<std::string, std::less<>> skip_ids;
        std::string live_id;
        bool live = false;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& entry : background_engines_) {
                skip_ids.insert(entry.first);
            }
            live_id = engine_ ? engine_->meeting_id() : std::string();
            live = is_active_locked() || controller_meeting_active_;
        }
        if (live && !live_id.empty()) {
            skip_ids.insert(live_id);
        } else if (!live_id.empty() && meeting_busy(live_id)) {
            // Capture ended but the end passes are still writing.
            skip_ids.insert(live_id);
        }
        {
            const std::lock_guard lock(mutex_);
            if (retry_meeting_id_) {
                skip_ids.insert(*retry_meeting_id_);
            }
        }
        meeting_state::clear_meetings(
            *deps_.repository, deps_.meetings_root, delete_recordings, skip_ids,
            [this](std::string id, bool has_origin) { forget_deleted(id, has_origin); });
        std::optional<std::string> card_id;
        {
            const std::lock_guard lock(mutex_);
            card_id = card_meeting_id_;
        }
        if (card_id && !card_id->empty() && !skip_ids.contains(*card_id)) {
            hide_finalization_card();
        } else {
            refresh_past_meetings();
        }
        meeting_status_update.emit(
            std::string(delete_recordings ? messages::kClearedWithRecordings : messages::kCleared));
    } catch (const std::exception& error) {
        logger.error("Failed to clear past meetings: {}", error.what());
        meeting_error.emit(messages::could_not_clear(error.what()));
    }
}

bool MeetingRuntime::meeting_busy(const std::string& meeting_id) const {
    std::shared_ptr<RuntimeEngine> engine;
    bool owned = false;
    {
        const std::lock_guard lock(mutex_);
        if (background_engines_.contains(meeting_id) || retry_meeting_id_ == meeting_id) {
            return true;
        }
        engine = engine_;
        owned = is_active_locked() || finalizing_ || controller_meeting_active_;
    }
    if (engine && owned && engine->meeting_id() == meeting_id) {
        return true;
    }
    return is_running(meeting_id);
}

bool MeetingRuntime::meeting_in_use(const std::string& meeting_id) const {
    // The ended meeting's dashboard stays open until the next meeting.
    std::shared_ptr<RuntimeEngine> engine;
    std::shared_ptr<RuntimeArchive> archive;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
        archive = archive_;
    }
    if (engine && engine->meeting_id() == meeting_id) {
        return true;
    }
    if (archive && archive->is_running()) {
        return true;
    }
    return meeting_busy(meeting_id);
}

void MeetingRuntime::retitle_saved_meeting(const std::string& meeting_id,
                                           const std::string& title) {
    // Keeps MCP title edits consistent with live and historical dashboards.
    struct Owner {
        std::string meeting_id;
        std::shared_ptr<DashboardTitles> titles;
    };
    std::vector<Owner> owners;
    bool starting = false;
    {
        const std::lock_guard lock(mutex_);
        starting = archive_starting_;
        if (engine_) {
            owners.push_back({engine_->meeting_id(), engine_->dashboard()});
        }
        if (archive_) {
            owners.push_back({archive_->meeting_id(), archive_->dashboard()});
        }
        for (const auto& entry : background_engines_) {
            owners.push_back({entry.second->meeting_id(), entry.second->dashboard()});
        }
    }
    if (starting || meeting_busy(meeting_id)) {
        throw MeetingControlError(std::string(messages::kRecordBusy));
    }
    // An owner of the meeting holds the authoritative in-memory state; the other dashboards may
    // hold a historical review store of it.
    std::ranges::stable_sort(owners, [&meeting_id](const Owner& a, const Owner& b) {
        return (a.meeting_id == meeting_id) && (b.meeting_id != meeting_id);
    });
    std::vector<std::shared_ptr<DashboardTitles>> servers;
    for (const Owner& owner : owners) {
        if (!owner.titles || !owner.titles->is_running()) {
            continue;
        }
        if (std::ranges::find(servers, owner.titles) == servers.end()) {
            servers.push_back(owner.titles);
        }
    }
    if (!servers.empty()) {
        if (!servers.front()->retitle_saved_meeting(meeting_id, title)) {
            throw MeetingControlError(std::string(messages::kUpdateRejected));
        }
    } else {
        deps_.repository->rename_meeting(meeting_id, title);
    }
    for (std::size_t i = 1; i < servers.size(); ++i) {
        try {
            servers[i]->refresh_saved_meeting_title(meeting_id);
        } catch (const std::exception& error) {
            // The committed rename stands even if a dashboard closed; repository writes keep the
            // canonical title as well.
            logger.error_with(error, "Could not refresh a dashboard's meeting title");
        }
    }
}

} // namespace fv::meeting
