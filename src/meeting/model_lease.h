#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "speech/service.h"

// The model lease a post-meeting pass takes around the speech model it loads (refinalize.py
// ModelLease, acquire_model_lease, release_model_lease): only one copy of the weights should be
// resident beside the dictation engine. The meeting code takes the lease as an interface, so the
// policy of who owns the engine stays with the caller.
namespace fv::meeting {

// Affinity: acquire and release are called on the thread running the pass, release only after an
// acquire that returned true.
class ModelLease {
public:
    virtual ~ModelLease() = default;
    // Frees, or holds, whatever must not stay as it is while the pass's model is loaded. True when
    // it took something, which release() then gives back. May throw: callers go through
    // acquire_model_lease.
    virtual bool acquire() = 0;
    virtual void release() = 0;
};

// The lease of a pass that needs none: acquire takes nothing.
class NullModelLease final : public ModelLease {
public:
    bool acquire() override { return false; }
    void release() override {}
};

// Python's (acquire, release) pair.
class FunctionModelLease final : public ModelLease {
public:
    FunctionModelLease(std::function<bool()> acquire, std::function<void()> release)
        : acquire_(std::move(acquire)), release_(std::move(release)) {}

    bool acquire() override { return acquire_ ? acquire_() : false; }
    void release() override {
        if (release_) {
            release_();
        }
    }

private:
    std::function<bool()> acquire_;
    std::function<void()> release_;
};

// The application's lease over the speech service (architecture 5.6). While the meeting model is
// the dictation model, the pass holds an EngineLease so no reload starts under it and nothing is
// unloaded. When the meeting has a model of its own (`meeting_model_separate`, asked at the first
// acquire), the dictation slot is released (release_local) and loaded again on release
// (restore_local), which keeps OpenWhisper's release_local_engine and restore_local_engine.
// The optional override selects the meeting model after releasing dictation and unloads it
// before restoring dictation.
// Passes of different meetings may share one lease and overlap (a recovery finalize beside a
// retry): each acquire is matched by one release, the dictation slot comes back only when the
// last pass that released it ends, and no pass's EngineLease is dropped under another pass.
// Affinity: thread-safe; the service must outlive the lease.
class SpeechModelLease final : public ModelLease {
public:
    SpeechModelLease(speech::SpeechService& service, std::function<bool()> meeting_model_separate,
                     std::string holder = "meeting-finalization",
                     std::function<speech::BackendConfig()> configure_meeting = {});

    bool acquire() override;
    void release() override;

    // Releases and restores the dictation slot through the app's controller instead of the
    // speech service (MeetingRuntime._model_lease: controller.release_local_engine and
    // restore_local_engine), so the engine reload machine knows of the lease: the card reads
    // "Released while Meeting Mode runs", the restore shows "Reloading speech engine..." and the
    // busy state, a lease ending during a reload restores after it, and a Remote dictation engine
    // (no weights here) is left alone. `release` answers whether it released anything; either
    // may block until the UI thread has run it. Unset: the speech service directly.
    void route_dictation(std::function<bool()> release, std::function<void()> restore);

private:
    void release_dictation();
    void restore_dictation();

    speech::SpeechService& service_;
    std::function<bool()> route_release_;
    std::function<void()> route_restore_;
    std::function<bool()> meeting_model_separate_;
    std::string holder_;
    std::function<speech::BackendConfig()> configure_meeting_;
    std::mutex mutex_;
    // What each outstanding acquire took, oldest first: an EngineLease, or nullopt for a hold on
    // the released dictation slot.
    std::vector<std::optional<speech::EngineLease>> held_;
    // Outstanding acquires holding the dictation slot released.
    int dictation_released_ = 0;
};

// Runs the lease's acquire; never throws. True when it took (a failure is logged "Model lease
// acquire failed; continuing without it" and reads false).
bool acquire_model_lease(ModelLease* lease) noexcept;
// Runs the lease's release; never throws (a failure is logged "Model lease release failed").
void release_model_lease(ModelLease* lease) noexcept;

} // namespace fv::meeting
