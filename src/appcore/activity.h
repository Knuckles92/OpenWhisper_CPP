#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/signal.h"

namespace fv {

// What the app is doing right now, as the refusal gates, the busy confirmations and the engine
// reload machine read it. Each flag is the OpenWhisper state named beside it.
struct ActivitySnapshot {
    bool startup_ready = false;       // _startup_database_ready: the startup database is open
    bool shutting_down = false;       // _shutting_down: cleanup has started
    bool recovery_running = false;    // _startup_recovery_future not done
    bool backup_in_progress = false;  // _backup_in_progress (backup or restore preparation)
    bool meeting_setup_open = false;  // ui_controller._meeting_setup_open
    bool meeting_active = false;      // meeting_runtime.is_claimed (is_meeting_active)
    bool meeting_finishing = false;   // finalizing, background engines, archive start or retry
    bool recording = false;           // recorder.is_recording
    bool transcribing = false;        // a job is active or the engine is decoding
    bool walkthrough_job = false;     // the active job is a walkthrough (JobMode.WALKTHROUGH)
    bool write_up_running = false;    // transcription_runtime.writing_up (AI write-up)
    bool library_change = false;      // _walkthrough_library_busy (a library edit)
    bool rule_dictation_busy = false; // Settings' cleanup-rule dictation is not idle
    bool host_update_active = false;  // remote_engine.update_active(): a paired update runs
    bool component_installing = false;

    bool operator==(const ActivitySnapshot&) const = default;
};

// The activity flags, set by the runtimes that own each activity. `changed` carries the new
// snapshot after every set that changes a flag; a set to the current value emits nothing.
// Snapshots are emitted one at a time, in the order the state changed, and the last one emitted
// is always the current state. A set made while another thread is emitting is sent by that
// thread, coalesced with any other set made meanwhile.
// Affinity: thread-safe; `changed` is emitted on a setting thread, outside the lock.
class ActivityState {
public:
    ActivityState() = default;
    explicit ActivityState(const ActivitySnapshot& initial);
    ActivityState(const ActivityState&) = delete;
    ActivityState& operator=(const ActivityState&) = delete;

    void set_startup_ready(bool value);
    void set_shutting_down(bool value);
    void set_recovery_running(bool value);
    void set_backup_in_progress(bool value);
    void set_meeting_setup_open(bool value);
    void set_meeting_active(bool value);
    void set_meeting_finishing(bool value);
    void set_recording(bool value);
    void set_transcribing(bool value);
    void set_walkthrough_job(bool value);
    void set_write_up_running(bool value);
    void set_library_change(bool value);
    void set_rule_dictation_busy(bool value);
    void set_host_update_active(bool value);
    void set_component_installing(bool value);

    ActivitySnapshot snapshot() const;

    Signal<ActivitySnapshot> changed;

private:
    void set(bool ActivitySnapshot::*flag, bool value);

    mutable std::mutex mutex_;
    ActivitySnapshot state_;
    bool emitting_ = false; // a setter is emitting `changed`
    bool dirty_ = false;    // the state changed while it was emitting
};

// Why dictation must not start now (Quick Record, shortcuts, uploads, retranscribe and a model
// change all ask): backup or restore preparation, then the meeting setup dialog, then a running
// meeting. nullopt when it may start.
std::optional<std::string> refuse_dictation(const ActivitySnapshot& activity);

// Why nothing may load into the dictation engine now: a meeting owns it. nullopt otherwise.
std::optional<std::string> refuse_engine_load(const ActivitySnapshot& activity);

// Why a backup or restore cannot start now, or nullopt (the gate of _acquire_backup; taking the
// reservation is the caller's).
std::optional<std::string> backup_refusal(const ActivitySnapshot& activity);

// Why the walkthrough library cannot be edited now, or nullopt (_acquire_walkthrough_library's
// gate). Dictation never blocks it.
std::optional<std::string> walkthrough_library_refusal(const ActivitySnapshot& activity);

// The work that quitting or installing an update would stop, most important first: "a recording",
// "a meeting", "transcription", "a component install". The confirmations name the first one.
std::vector<std::string_view> busy_reasons(const ActivitySnapshot& activity);

} // namespace fv
