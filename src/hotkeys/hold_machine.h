#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace fv::hotkeys {

// config.RECORD_MIN_HOLD_MS: a push-and-hold release this long after its press stops the
// recording; a shorter tap cancels it, or waits for a second tap when the latch is on.
inline constexpr int kRecordMinHoldMs = 250;
// config.RECORD_LATCH_WINDOW_MS: a second press this soon after a tap's release latches.
inline constexpr int kRecordLatchWindowMs = 400;
// How long a release waits for a just-started recording's stream to open.
inline constexpr double kStreamOpenWaitS = 0.5;

// Push-and-hold recording with the hands-free latch (services/runtime/hotkeys.py
// record_key_pressed .. _reset_hold).
//
// Holding: our press started a recording. TapPending: a short tap ended, and a second press
// within the latch window latches the recording; otherwise it is cancelled. Latched: hands-free
// until the next press stops it. A latch belongs to the recording its hold started: when any
// recording ends (stop, cancel, tray, error) the count of ended recordings moves, and a latch
// from before is dropped. Times are seconds on the hook's monotonic clock; a release or press
// carries its own, so a deadline racing an event is settled by the event's time.
// Affinity: owned thread "fv-hotkey-dispatch"; reset() and recording_ended() too (the caller
// posts them there).
class HoldMachine {
public:
    enum class State { Idle, Holding, TapPending, Latched };

    enum class Action {
        Start,          // start a recording; the host answers whether it was accepted
        Stop,           // stop it (it goes on to transcription)
        Cancel,         // cancel it
        Latch,          // hands-free from now ("Hands-free · press <key> to stop")
        Unlatch,        // hands-free ended (hands_free_changed(False))
        ArmDeadline,    // run deadline(generation) at `at` (OrderedDispatcher.call_at)
        CancelDeadline, // drop the armed deadline
    };

    struct Output {
        Action action;
        double at = 0;                // ArmDeadline
        std::uint64_t generation = 0; // ArmDeadline

        friend bool operator==(const Output&, const Output&) = default;
    };

    // What the machine reads from and asks of the recording side.
    class Host {
    public:
        virtual ~Host() = default;
        virtual bool is_recording() = 0;
        // The cancel hotkey already ended this capture (post-roll keeps is_recording true).
        virtual bool capture_canceled() = 0;
        // resolve_recording_hands_free_latch().
        virtual bool hands_free_latch() = 0;
        // Waits up to `timeout_s` for is_recording, since a start can return before the stream
        // opens, and answers is_recording.
        virtual bool wait_until_recording(double timeout_s) = 0;
        // Carries out an output; for Start, whether the recording was accepted.
        virtual bool perform(const Output& output) = 0;
    };

    explicit HoldMachine(Host& host);

    // The record hotkey went down / came up at `at`. Each returns the outputs it performed.
    std::vector<Output> press(double at);
    std::vector<Output> release(double at);
    // The armed deadline came (_on_tap_deadline).
    std::vector<Output> deadline(std::uint64_t generation);
    // A recording ended for any reason. True when a latch may now be stale: the caller then runs
    // drop_stale_latch() on this machine's thread after the events queued before it.
    bool recording_ended();
    std::vector<Output> drop_stale_latch();
    // The trigger mode changed (_reset_hold).
    std::vector<Output> reset();

    State state() const { return state_; }
    std::optional<double> press_time() const { return press_time_; }
    bool start_accepted() const { return start_accepted_; }
    std::uint64_t generation() const { return generation_; }
    double latch_deadline() const { return latch_deadline_; }

private:
    bool perform(std::vector<Output>& done, Output output);
    void await_second_tap(std::vector<Output>& done, double released_at);
    void expire_tap(std::vector<Output>& done, double at);
    void cancel_tap(std::vector<Output>& done);
    void latch(std::vector<Output>& done);
    void drop_stale(std::vector<Output>& done);
    void reset_hold(std::vector<Output>& done);

    Host& host_;
    State state_ = State::Idle;
    std::optional<double> press_time_;
    bool start_accepted_ = false;
    std::uint64_t generation_ = 0;
    double latch_deadline_ = 0;
    std::uint64_t recordings_ended_ = 0;
    std::uint64_t hold_epoch_ = 0;
};

} // namespace fv::hotkeys
