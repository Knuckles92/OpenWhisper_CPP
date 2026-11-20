#include "meeting/model_lease.h"

#include <exception>

#include "base/log.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.refinalize");

} // namespace

SpeechModelLease::SpeechModelLease(speech::SpeechService& service,
                                   std::function<bool()> meeting_model_separate, std::string holder,
                                   std::function<speech::BackendConfig()> configure_meeting)
    : service_(service), meeting_model_separate_(std::move(meeting_model_separate)),
      holder_(std::move(holder)), configure_meeting_(std::move(configure_meeting)) {}

bool SpeechModelLease::acquire() {
    const std::lock_guard lock(mutex_);
    // Overlapping passes share the engine choice held by the first one.
    const bool separate = held_.empty() ? meeting_model_separate_ && meeting_model_separate_()
                                        : dictation_released_ > 0;
    if (separate) {
        if (dictation_released_ == 0) {
            if (configure_meeting_) {
                auto config = configure_meeting_();
                release_dictation();
                try {
                    service_.select(speech::Slot::Meeting, std::move(config));
                } catch (...) {
                    restore_dictation();
                    throw;
                }
            } else {
                release_dictation();
            }
        }
        ++dictation_released_;
        held_.emplace_back(std::nullopt);
        return true;
    }
    held_.emplace_back(service_.lease(holder_));
    return true;
}

void SpeechModelLease::release() {
    const std::lock_guard lock(mutex_);
    if (held_.empty()) {
        return;
    }
    // Passes end in any order; what each took is interchangeable with the others of its kind.
    std::optional<speech::EngineLease> taken = std::move(held_.back());
    held_.pop_back();
    if (taken) {
        taken->release();
        return;
    }
    if (--dictation_released_ == 0) {
        if (configure_meeting_) {
            service_.release_local(speech::Slot::Meeting);
        }
        restore_dictation();
    }
}

void SpeechModelLease::route_dictation(std::function<bool()> release,
                                       std::function<void()> restore) {
    const std::lock_guard lock(mutex_);
    route_release_ = std::move(release);
    route_restore_ = std::move(restore);
}

// Under mutex_.
void SpeechModelLease::release_dictation() {
    if (route_release_) {
        // Whether the controller released anything (nothing for a Remote engine) changes nothing
        // here: the meeting's own model is still selected, and the restore is flag-guarded.
        (void)route_release_();
        return;
    }
    service_.release_local(speech::Slot::Dictation);
}

void SpeechModelLease::restore_dictation() {
    if (route_restore_) {
        route_restore_();
        return;
    }
    service_.restore_local(speech::Slot::Dictation);
}

bool acquire_model_lease(ModelLease* lease) noexcept {
    if (lease == nullptr) {
        return false;
    }
    try {
        return lease->acquire();
    } catch (const std::exception& error) {
        logger.error_with(error, "Model lease acquire failed; continuing without it");
    } catch (...) {
        logger.error("Model lease acquire failed; continuing without it");
    }
    return false;
}

void release_model_lease(ModelLease* lease) noexcept {
    if (lease == nullptr) {
        return;
    }
    try {
        lease->release();
    } catch (const std::exception& error) {
        logger.error_with(error, "Model lease release failed");
    } catch (...) {
        logger.error("Model lease release failed");
    }
}

} // namespace fv::meeting
