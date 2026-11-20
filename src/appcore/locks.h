#pragma once

namespace fv::appcore {

// The order in which code that holds several of the app's locks takes them (architecture section
// 4 rule 7): a lower rank is taken first, and a thread holding a lock never takes one of a lower
// rank. Acquisitions of several at once use std::scoped_lock.
//
// OpenWhisper nests the capture lock outside the meeting lock and both outside the job lock
// (transcription.py 288, 311, 498-501; _acquire_backup, application_controller.py 764: "with
// runtime._capture_lock, meeting._lock, runtime._job_lock"), so capture comes before the job
// slot here. The meeting store and the database writer are taken inside all of them.
enum class LockRank : int {
    Capture = 1,        // the transcription runtime's capture lock (recording start and stop)
    MeetingRuntime = 2, // the meeting runtime's state lock
    JobSlot = 3,        // the transcription runtime's job lock (the single active job)
    MeetingStore = 4,   // the meeting store's lock
    DatabaseWriter = 5, // the database's single writer
};

constexpr bool may_take_after(LockRank held, LockRank next) {
    return static_cast<int>(next) > static_cast<int>(held);
}

} // namespace fv::appcore
