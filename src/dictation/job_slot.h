#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include "base/cancel.h"
#include "dictation/job.h"

namespace fv::dictation {

// What may refuse a claim besides a job already holding the slot (transcription.py _claim_job),
// read by the caller under its capture lock just before claiming.
struct ClaimGates {
    bool shutting_down = false;
    // A backup or restore is being prepared.
    bool backup_in_progress = false;
    // A paired PC is updating this host.
    bool host_update_active = false;
    // The claim needs an idle recorder (require_idle_recorder) and the recorder is recording.
    bool recorder_busy = false;
};

// What request_cancel found, under the slot's lock (the slot half of transcription.py _cancel).
struct CancelRequest {
    // A job held the slot and was still working on its result (not only being saved); its
    // cancel is now set.
    bool working = false;
    // It was working and an earlier Cancel had already reached it.
    bool repeat = false;
    // current_job() at the time.
    std::shared_ptr<const DictationJob> job;
};

// The single transcription workflow slot (transcription.py 151-303, 386-416): one job at a time
// (a recording's result, an upload, a retranscription, a rewrite, a walkthrough). A claim gives
// the job a fresh CancelSource, so the next job never un-cancels one whose filters or routing
// are still being stopped; settle() marks the outcome decided, after which a Cancel stops
// nothing; finish() frees the slot. It also holds the recording's job from its start until its
// stop claims the slot, and the job whose live preview is published. Python's _job_lock is the
// slot's own mutex (appcore::LockRank::JobSlot): the runtime takes its capture lock first.
// Affinity: thread-safe.
class JobSlot {
public:
    JobSlot() = default;
    JobSlot(const JobSlot&) = delete;
    JobSlot& operator=(const JobSlot&) = delete;

    // Reserves the slot for `job` (null for an upload) unless a job holds it or a gate refuses.
    bool claim(std::shared_ptr<const DictationJob> job, const ClaimGates& gates = {});
    // _settle: from here a Cancel stops nothing. Returns whether a Cancel got there first, read
    // under the lock request_cancel() sets the cancel with, so a Cancel lands either before (and
    // is honored) or after (and reaches running actions instead).
    bool settle();
    // _finish_job's slot fields: the recording's job, the active job, the preview, saving and
    // the slot itself.
    void finish();

    // has_active_job: a result is queued, transcribing, cleaning or being saved.
    bool has_active_job() const;
    // The first half of `cancelable`: a job still working on its result (not only being saved)
    // that no Cancel has reached yet.
    bool working() const;
    bool saving() const;
    // current_job: the slot's job once a stop, rewrite or run claimed it, else the recording's.
    std::shared_ptr<const DictationJob> current_job() const;
    // The job the claim was made for (null for an upload, or when the slot is free).
    std::shared_ptr<const DictationJob> active_job() const;
    // Swaps in `job` (the same job updated, as _with_final_recognition's dataclasses.replace)
    // while the slot still works on `expected`; false, changing nothing, once it does not (the
    // slot was released meanwhile and must never be given its job back).
    bool replace_active_job(const DictationJob* expected, std::shared_ptr<const DictationJob> job);

    // The recording's job (_job), from its start until a stop claims the slot.
    void set_recording_job(std::shared_ptr<const DictationJob> job);
    std::shared_ptr<const DictationJob> recording_job() const;
    // _forget_recording's slot fields: the recording's job and its preview.
    void forget_recording();

    // The job whose live preview is published: from just after recording.started until just
    // before recording.stopped or recording.canceled.
    void start_preview(std::shared_ptr<const DictationJob> job);
    // _end_preview: publish no more of it. Under the lock with_preview runs under, so a preview
    // being published finishes first and none comes after.
    void end_preview();
    std::shared_ptr<const DictationJob> previewing() const;
    // Runs publish() under the slot's lock when `job` is the one whose preview is published;
    // publish must not block or take another lock. Returns whether it ran.
    template <class Publish> bool with_preview(const DictationJob* job, Publish&& publish) const {
        std::lock_guard lock(mutex_);
        if (job == nullptr || !previewing_ || previewing_->id != job->id) {
            return false;
        }
        std::forward<Publish>(publish)();
        return true;
    }

    // The slot half of _cancel: when a job is working, sets its cancel (a stop's post-roll, the
    // worker, filters, routing and cleanup each look at it).
    CancelRequest request_cancel();
    // begin_shutdown: cancels the current job's work whatever its state.
    void cancel();
    // _cancel_requested.is_set().
    bool cancel_requested() const;
    // The current job's cancel token (the last claim's). The cancel is set under the slot's lock
    // (so settle() sees it or not, never half), which is where its on_cancel tasks run: they
    // must not call back into the slot.
    CancelToken cancel_token() const;

private:
    mutable std::mutex mutex_;
    bool active_ = false;
    bool saving_ = false;
    std::shared_ptr<const DictationJob> active_job_;
    std::shared_ptr<const DictationJob> recording_job_;
    std::shared_ptr<const DictationJob> previewing_;
    CancelSource cancel_;
};

} // namespace fv::dictation
