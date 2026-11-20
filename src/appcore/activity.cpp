#include "appcore/activity.h"

#include <format>

#include "base/identity.h"
#include "base/log.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.application_controller"};

} // namespace

ActivityState::ActivityState(const ActivitySnapshot& initial) : state_(initial) {}

void ActivityState::set(bool ActivitySnapshot::*flag, bool value) {
    std::unique_lock lock(mutex_);
    if (state_.*flag == value) {
        return;
    }
    state_.*flag = value;
    if (emitting_) {
        // Another thread (or a slot of this one) is emitting; it sends this state next.
        dirty_ = true;
        return;
    }
    // One emitter at a time, never under the lock (section 4 rule 6): two setters emitting
    // their own snapshots outside the lock could deliver them in the wrong order and leave
    // listeners on a stale state.
    emitting_ = true;
    do {
        dirty_ = false;
        const ActivitySnapshot after = state_;
        lock.unlock();
        try {
            changed.emit(after);
        } catch (...) {
            // A slot run inline threw: the next set must still be able to emit.
            lock.lock();
            emitting_ = false;
            throw;
        }
        lock.lock();
    } while (dirty_);
    emitting_ = false;
}

void ActivityState::set_startup_ready(bool value) {
    set(&ActivitySnapshot::startup_ready, value);
}
void ActivityState::set_shutting_down(bool value) {
    set(&ActivitySnapshot::shutting_down, value);
}
void ActivityState::set_recovery_running(bool value) {
    set(&ActivitySnapshot::recovery_running, value);
}
void ActivityState::set_backup_in_progress(bool value) {
    set(&ActivitySnapshot::backup_in_progress, value);
}
void ActivityState::set_meeting_setup_open(bool value) {
    set(&ActivitySnapshot::meeting_setup_open, value);
}
void ActivityState::set_meeting_active(bool value) {
    set(&ActivitySnapshot::meeting_active, value);
}
void ActivityState::set_meeting_finishing(bool value) {
    set(&ActivitySnapshot::meeting_finishing, value);
}
void ActivityState::set_recording(bool value) {
    set(&ActivitySnapshot::recording, value);
}
void ActivityState::set_transcribing(bool value) {
    set(&ActivitySnapshot::transcribing, value);
}
void ActivityState::set_walkthrough_job(bool value) {
    set(&ActivitySnapshot::walkthrough_job, value);
}
void ActivityState::set_write_up_running(bool value) {
    set(&ActivitySnapshot::write_up_running, value);
}
void ActivityState::set_library_change(bool value) {
    set(&ActivitySnapshot::library_change, value);
}
void ActivityState::set_rule_dictation_busy(bool value) {
    set(&ActivitySnapshot::rule_dictation_busy, value);
}
void ActivityState::set_host_update_active(bool value) {
    set(&ActivitySnapshot::host_update_active, value);
}
void ActivityState::set_component_installing(bool value) {
    set(&ActivitySnapshot::component_installing, value);
}

ActivitySnapshot ActivityState::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::optional<std::string> refuse_dictation(const ActivitySnapshot& activity) {
    if (activity.backup_in_progress) {
        return "Wait for backup or restore preparation to finish";
    }
    if (activity.meeting_setup_open) {
        return "Close Meeting Mode setup before using Quick Record.";
    }
    if (!activity.meeting_active) {
        return std::nullopt;
    }
    kLog.info("Dictation start refused: Meeting Mode is active");
    return "Meeting Mode is active — end the meeting to use dictation";
}

std::optional<std::string> refuse_engine_load(const ActivitySnapshot& activity) {
    if (!activity.meeting_active) {
        return std::nullopt;
    }
    kLog.info("Engine load refused: a meeting owns the Whisper engine");
    return "End the meeting before changing the engine";
}

std::optional<std::string> backup_refusal(const ActivitySnapshot& activity) {
    if (activity.shutting_down || !activity.startup_ready) {
        return std::format("Wait for {} to finish starting before backing up or restoring.",
                           identity().product);
    }
    if (activity.recovery_running) {
        return "Wait for recording recovery to finish before backing up or restoring.";
    }
    if (activity.library_change || activity.write_up_running) {
        return "Wait for the walkthrough to finish saving before backing up or restoring.";
    }
    if (activity.backup_in_progress || activity.recording || activity.transcribing) {
        return "Finish recording or transcription before backing up or restoring.";
    }
    if (activity.meeting_active || activity.meeting_finishing) {
        return "Finish the meeting and its processing before backing up or restoring.";
    }
    if (activity.rule_dictation_busy) {
        return "Finish dictating the cleanup rule before backing up or restoring.";
    }
    return std::nullopt;
}

std::optional<std::string> walkthrough_library_refusal(const ActivitySnapshot& activity) {
    if (activity.shutting_down || !activity.startup_ready) {
        return std::format("Wait for {} to finish starting.", identity().product);
    }
    if (activity.recovery_running) {
        // Startup recovery attaches audio to drafts and prunes old entries.
        return "Wait for recording recovery to finish.";
    }
    if (activity.backup_in_progress) {
        return "Wait for the backup or restore to finish.";
    }
    if (activity.library_change) {
        return "Wait for the other walkthrough change to finish.";
    }
    if (activity.write_up_running) {
        return "Wait for the AI write-up to finish.";
    }
    if (activity.walkthrough_job) {
        return "Wait for the walkthrough being transcribed to finish.";
    }
    return std::nullopt;
}

std::vector<std::string_view> busy_reasons(const ActivitySnapshot& activity) {
    std::vector<std::string_view> reasons;
    if (activity.recording) {
        reasons.emplace_back("a recording");
    }
    if (activity.meeting_active) {
        reasons.emplace_back("a meeting");
    }
    if (activity.transcribing) {
        reasons.emplace_back("transcription");
    }
    if (activity.component_installing) {
        reasons.emplace_back("a component install");
    }
    return reasons;
}

} // namespace fv
