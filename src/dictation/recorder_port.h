#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "audio/pcm_tape.h"
#include "base/signal.h"

namespace fv {
class Recorder;
}

namespace fv::dictation {

// The dictation recorder as the transcription runtime uses it (the parts of AudioRecorder that
// services/runtime/transcription.py calls), so its tests can script a capture as OpenWhisper's
// tests script a fake recorder. RecorderAdapter puts a real Recorder behind it.
// Affinity: thread-safe, as Recorder is; the save and the stop's wait block (never on the UI
// thread).
class RecorderPort {
public:
    virtual ~RecorderPort() = default;

    // From the accepted start until a stop's post-roll ends.
    virtual bool is_recording() const = 0;
    // A previous capture still holds the recorder after is_recording turned false.
    virtual bool recorder_busy() const = 0;
    // A Cancel already threw this capture away; true until the next start.
    virtual bool capture_canceled() const = 0;
    // The input stream is open.
    virtual bool stream_open() const = 0;

    // Opens the microphone; false (and last_start_error) when none opens.
    virtual bool start_recording() = 0;
    virtual std::string last_start_error() const = 0;
    // Requests the stop; false when not recording.
    virtual bool stop_recording() = 0;
    // Waits for the post-roll, the stream's close and storage; false on timeout.
    virtual bool wait_for_stop_completion() = 0;
    virtual bool has_recording_data() const = 0;
    // Saves the capture to output_file(), a capture that lost audio included.
    virtual bool save_recording() = 0;
    // Seconds captured.
    virtual double get_recording_duration() const = 0;
    // The one failure this recording reported; empty when none.
    virtual std::string last_capture_error() const = 0;
    // Ends the capture with `message` as its one failure (AudioRecorder._fail_capture).
    virtual void fail_capture(const std::string& message) = 0;
    virtual void cancel_recording() = 0;
    // Drops the recovery journal once history and audio are saved.
    virtual void acknowledge_recording() = 0;
    // The saved recording (config.RECORDED_AUDIO_FILE in the app).
    virtual const std::filesystem::path& output_file() const = 0;

    // The capture's one visible failure, emitted on the recorder's threads.
    virtual Signal<std::string>& capture_error() = 0;

    // The current recording's samples as they are captured, for the live preview and the early
    // decode of a long dictation; null (the default) when there is no capture to follow.
    virtual std::shared_ptr<const audio::PcmTape> tape() const { return nullptr; }

    // The current capture's crash-safe journal, named by its folder (a walkthrough's draft
    // links to it, so recovery can join the two); "" (the default) without one.
    virtual std::string journal_id() const { return {}; }
    // A walkthrough's Pause: keeps nothing the microphone hears until resume(); false (the
    // default) when the recorder cannot pause now.
    virtual bool pause() { return false; }
    virtual void resume() {}
    virtual bool paused() const { return false; }
};

// RecorderPort over a Recorder, which must outlive it.
// Affinity: thread-safe.
class RecorderAdapter final : public RecorderPort {
public:
    explicit RecorderAdapter(Recorder& recorder) : recorder_(recorder) {}

    bool is_recording() const override;
    // Recorder::is_recording already covers the post-roll and the stream's close, and a start
    // during them is refused, so nothing else holds it.
    bool recorder_busy() const override { return false; }
    bool capture_canceled() const override;
    // Recorder::start_recording opens the stream before the session counts as recording.
    bool stream_open() const override;
    bool start_recording() override;
    std::string last_start_error() const override;
    bool stop_recording() override;
    bool wait_for_stop_completion() override;
    bool has_recording_data() const override;
    bool save_recording() override;
    double get_recording_duration() const override;
    std::string last_capture_error() const override;
    void fail_capture(const std::string& message) override;
    void cancel_recording() override;
    void acknowledge_recording() override;
    const std::filesystem::path& output_file() const override;
    Signal<std::string>& capture_error() override;
    std::shared_ptr<const audio::PcmTape> tape() const override;
    std::string journal_id() const override;
    bool pause() override;
    void resume() override;
    bool paused() const override;

private:
    Recorder& recorder_;
};

} // namespace fv::dictation
