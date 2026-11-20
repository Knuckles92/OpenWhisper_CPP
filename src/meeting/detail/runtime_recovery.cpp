// MeetingRuntime: crash recovery at startup and the recovery dialog's actions
// (services/runtime/meeting.py _recovery_scan_worker, retry_recovery_scan,
// _restore_last_finalization_worker, finalize_recovered, _finalize_recovered_worker,
// discard_recovered, _discard_recovered_worker).

#include <exception>
#include <format>

#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "meeting_state/data_lifecycle.h"
#include "pyport/errors.h"
#include "settings/keys.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

} // namespace

void MeetingRuntime::recovery_scan_worker() {
    std::vector<Json> meetings;
    try {
        meetings = deps_.find_recoverable(*deps_.repository);
    } catch (const std::exception& error) {
        // A failed scan never reads as "nothing to recover": the files stay and a retry is
        // offered.
        logger.error_with(error, "Meeting recovery scan failed");
        meeting_recovery_scan_failed.emit(messages::recovery_scan_failed(error.what()));
        return;
    }
    if (!meetings.empty()) {
        logger.info("Found {} interrupted meeting(s)", meetings.size());
        meeting_recovery_found.emit(meetings);
    }
}

void MeetingRuntime::restore_last_finalization_worker() {
    {
        const std::lock_guard lock(mutex_);
        if (is_active_locked() || controller_meeting_active_) {
            return;
        }
    }
    std::vector<Json> meetings;
    try {
        meetings = deps_.repository->list_meetings();
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not restore the last meeting checklist");
        return;
    }
    if (meetings.empty()) {
        return;
    }
    const Json& latest = meetings.front();
    const std::string status = detail::text_of(latest, "status");
    if (status == "active" || status == "paused" || status == "ending") {
        return;
    }
    hydrate_finalization_card(latest, false);
}

void MeetingRuntime::finalize_recovered(std::string meeting_id) {
    spawn("meeting-recover",
          [this, meeting_id = std::move(meeting_id)] { finalize_recovered_worker(meeting_id); });
}

void MeetingRuntime::finalize_recovered_worker(const std::string& meeting_id) {
    std::optional<std::string> follow_up;
    try {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            throw py::ValueError(messages::unknown_meeting(meeting_id));
        }
        const Json settings = settings_snapshot();
        // Recovery loads a speech model of its own, so it takes the lease that frees the
        // dictation engine first.
        if (!deps_.finalize_recovered(*deps_.repository, *meeting,
                                      keys::meeting_language.read(settings), *model_lease_)) {
            meeting_error.emit(std::string(messages::kFinalizeFailed));
            return;
        }
        const Json recovered = deps_.repository->get_meeting(meeting_id).value_or(*meeting);
        if (detail::text_of(recovered, "status") == "failed") {
            meeting_status_update.emit(std::string(messages::kRecoveredIncomplete));
        } else {
            meeting_status_update.emit(std::string(messages::kRecoveredFinalized));
            follow_up = recovered_follow_up_step(*meeting, settings);
        }
    } catch (const std::exception& error) {
        logger.error("Failed to finalize meeting '{}': {}", meeting_id, error.what());
        meeting_error.emit(messages::could_not_finalize(error.what()));
        return;
    }
    if (follow_up) {
        // Recovery only transcribes the leftover audio; the retry the Past Meetings card uses
        // runs the steps a live End would have.
        post_ui([this, step = *follow_up, meeting_id] { retry_step(step, meeting_id); });
    }
}

void MeetingRuntime::discard_recovered(std::string meeting_id) {
    spawn("meeting-discard",
          [this, meeting_id = std::move(meeting_id)] { discard_recovered_worker(meeting_id); });
}

void MeetingRuntime::discard_recovered_worker(const std::string& meeting_id) {
    try {
        meeting_state::delete_meeting_data(
            *deps_.repository, meeting_id, deps_.meetings_root, true,
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
        meeting_status_update.emit(std::string(messages::kDiscarded));
    } catch (const std::exception& error) {
        logger.error("Failed to discard meeting '{}': {}", meeting_id, error.what());
        meeting_error.emit(messages::could_not_discard(error.what()));
    }
}

} // namespace fv::meeting
