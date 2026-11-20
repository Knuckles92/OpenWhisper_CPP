#include "hotkeys/hold_machine.h"

namespace fv::hotkeys {

HoldMachine::HoldMachine(Host& host) : host_(host) {}

bool HoldMachine::perform(std::vector<Output>& done, Output output) {
    done.push_back(output);
    return host_.perform(output);
}

std::vector<HoldMachine::Output> HoldMachine::press(double at) {
    std::vector<Output> done;
    expire_tap(done, at);
    drop_stale(done);
    if (state_ == State::TapPending) {
        latch(done);
        return done;
    }
    if (state_ == State::Latched) {
        reset_hold(done);
        perform(done, {Action::Stop});
        return done;
    }
    if (state_ == State::Holding && start_accepted_ && recordings_ended_ == hold_epoch_ &&
        host_.is_recording()) {
        // This hold's release was lost (a hook refresh forgot the held key), so the press
        // continues the hold and its release stops it.
        return done;
    }
    reset_hold(done);
    if (host_.is_recording()) {
        // A previous stop's post-roll still owns the recorder; ignore the press rather than
        // surfacing a failed-start status.
        return done;
    }
    press_time_ = at;
    hold_epoch_ = recordings_ended_;
    start_accepted_ = perform(done, {Action::Start});
    if (start_accepted_) {
        state_ = State::Holding;
    }
    return done;
}

std::vector<HoldMachine::Output> HoldMachine::release(double at) {
    std::vector<Output> done;
    expire_tap(done, at);
    if (state_ == State::TapPending || state_ == State::Latched) {
        return done; // the latching press's own release
    }
    const std::optional<double> pressed = press_time_;
    press_time_.reset();
    state_ = State::Idle;
    if (!start_accepted_) {
        return done; // the start was refused; its status already surfaced
    }
    start_accepted_ = false;
    if (!host_.wait_until_recording(kStreamOpenWaitS)) {
        return done; // the start failed inside; its failure path updated the status
    }
    if (host_.capture_canceled()) {
        // The cancel hotkey already claimed this hold. No audio yet is not a cancel: the first
        // block can arrive after a quick tap.
        return done;
    }
    // Python's `if press_time` is false for a press at 0.0 as well.
    const double held_ms = (pressed && *pressed != 0) ? (at - *pressed) * 1000 : 0;
    if (held_ms >= kRecordMinHoldMs) {
        perform(done, {Action::Stop});
    } else if (!host_.hands_free_latch()) {
        perform(done, {Action::Cancel});
    } else if (recordings_ended_ == hold_epoch_) {
        await_second_tap(done, at);
    }
    return done;
}

std::vector<HoldMachine::Output> HoldMachine::deadline(std::uint64_t generation) {
    std::vector<Output> done;
    if (generation == generation_ && state_ == State::TapPending) {
        cancel_tap(done);
    }
    return done;
}

bool HoldMachine::recording_ended() {
    ++recordings_ended_;
    return state_ == State::TapPending || state_ == State::Latched;
}

std::vector<HoldMachine::Output> HoldMachine::drop_stale_latch() {
    std::vector<Output> done;
    drop_stale(done);
    return done;
}

std::vector<HoldMachine::Output> HoldMachine::reset() {
    std::vector<Output> done;
    reset_hold(done);
    return done;
}

void HoldMachine::await_second_tap(std::vector<Output>& done, double released_at) {
    state_ = State::TapPending;
    ++generation_;
    latch_deadline_ = released_at + kRecordLatchWindowMs / 1000.0;
    perform(done, {Action::ArmDeadline, latch_deadline_, generation_});
}

void HoldMachine::expire_tap(std::vector<Output>& done, double at) {
    // A pending tap whose window closed before an event at `at`.
    if (state_ == State::TapPending && at > latch_deadline_) {
        cancel_tap(done);
    }
}

void HoldMachine::cancel_tap(std::vector<Output>& done) {
    reset_hold(done);
    if (recordings_ended_ != hold_epoch_) {
        return; // it already ended another way, maybe into transcription
    }
    if (host_.is_recording() && !host_.capture_canceled()) {
        perform(done, {Action::Cancel});
    }
}

void HoldMachine::latch(std::vector<Output>& done) {
    state_ = State::Latched;
    ++generation_;
    perform(done, {Action::CancelDeadline});
    perform(done, {Action::Latch});
}

void HoldMachine::drop_stale(std::vector<Output>& done) {
    if ((state_ == State::TapPending || state_ == State::Latched) &&
        recordings_ended_ != hold_epoch_) {
        reset_hold(done);
    }
}

void HoldMachine::reset_hold(std::vector<Output>& done) {
    const bool was_latched = state_ == State::Latched;
    state_ = State::Idle;
    ++generation_;
    press_time_.reset();
    start_accepted_ = false;
    if (was_latched) {
        perform(done, {Action::Unlatch});
    }
}

} // namespace fv::hotkeys
