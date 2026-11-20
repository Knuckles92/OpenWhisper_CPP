#include "dictation/job_slot.h"

#include <utility>

namespace fv::dictation {

bool JobSlot::claim(std::shared_ptr<const DictationJob> job, const ClaimGates& gates) {
    std::lock_guard lock(mutex_);
    if (active_ || gates.shutting_down || gates.backup_in_progress || gates.host_update_active ||
        gates.recorder_busy) {
        return false;
    }
    active_ = true;
    active_job_ = std::move(job);
    // A new source rather than a cleared one: a program filter the last job abandoned on its
    // cancel looks again only every 50 ms, and must still see that cancel to be stopped.
    cancel_ = CancelSource();
    return true;
}

bool JobSlot::settle() {
    std::lock_guard lock(mutex_);
    saving_ = true;
    return cancel_.cancelled();
}

void JobSlot::finish() {
    std::lock_guard lock(mutex_);
    recording_job_.reset();
    active_job_.reset();
    saving_ = false;
    active_ = false;
}

bool JobSlot::has_active_job() const {
    std::lock_guard lock(mutex_);
    return active_;
}

bool JobSlot::working() const {
    std::lock_guard lock(mutex_);
    return active_ && !saving_ && !cancel_.cancelled();
}

bool JobSlot::saving() const {
    std::lock_guard lock(mutex_);
    return saving_;
}

std::shared_ptr<const DictationJob> JobSlot::current_job() const {
    std::lock_guard lock(mutex_);
    return active_ ? active_job_ : recording_job_;
}

std::shared_ptr<const DictationJob> JobSlot::active_job() const {
    std::lock_guard lock(mutex_);
    return active_job_;
}

bool JobSlot::replace_active_job(const DictationJob* expected,
                                 std::shared_ptr<const DictationJob> job) {
    std::lock_guard lock(mutex_);
    if (!active_ || expected == nullptr || active_job_.get() != expected) {
        return false;
    }
    active_job_ = std::move(job);
    return true;
}

void JobSlot::set_recording_job(std::shared_ptr<const DictationJob> job) {
    std::lock_guard lock(mutex_);
    recording_job_ = std::move(job);
}

std::shared_ptr<const DictationJob> JobSlot::recording_job() const {
    std::lock_guard lock(mutex_);
    return recording_job_;
}

void JobSlot::forget_recording() {
    std::lock_guard lock(mutex_);
    recording_job_.reset();
    previewing_.reset();
}

void JobSlot::start_preview(std::shared_ptr<const DictationJob> job) {
    std::lock_guard lock(mutex_);
    previewing_ = std::move(job);
}

void JobSlot::end_preview() {
    std::lock_guard lock(mutex_);
    previewing_.reset();
}

std::shared_ptr<const DictationJob> JobSlot::previewing() const {
    std::lock_guard lock(mutex_);
    return previewing_;
}

CancelRequest JobSlot::request_cancel() {
    std::lock_guard lock(mutex_);
    CancelRequest request;
    request.working = active_ && !saving_;
    request.repeat = request.working && cancel_.cancelled();
    if (request.working) {
        cancel_.cancel();
    }
    request.job = active_ ? active_job_ : recording_job_;
    return request;
}

void JobSlot::cancel() {
    std::lock_guard lock(mutex_);
    cancel_.cancel();
}

bool JobSlot::cancel_requested() const {
    std::lock_guard lock(mutex_);
    return cancel_.cancelled();
}

CancelToken JobSlot::cancel_token() const {
    std::lock_guard lock(mutex_);
    return cancel_.token();
}

} // namespace fv::dictation
