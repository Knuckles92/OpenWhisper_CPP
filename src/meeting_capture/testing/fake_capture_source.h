#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "meeting_capture/sources.h"

// A CaptureSource the test plays: it delivers blocks the test hands it, fails to start, starts
// inactive, dies, throws from is_active(), and says whether its device is still the default.
// Hand it to MeetingCapture through SourceFactory.
namespace fv::meeting::testing {

// Affinity: thread-safe; deliver() calls the sink on the calling thread.
class FakeCaptureSource final : public CaptureSource {
public:
    explicit FakeCaptureSource(std::string channel, DeviceId device_id = {},
                               bool requires_audio_blocks = false);

    // start() throws std::runtime_error(message) while set.
    void fail_start(std::optional<std::string> message);
    // start() succeeds but the source reports inactive (it opened no device).
    void start_inactive(bool inactive);
    // is_active() answers `calls` more times, then throws std::runtime_error(message).
    void throw_from_is_active_after(int calls, std::string message);
    // The device went away: inactive from now on.
    void kill();
    // What is_default_device_current() answers; nullopt (the default) means it cannot tell.
    void set_default_device_current(std::optional<bool> current);

    // One block to the sink, as the device thread would; false when not started or stopped.
    // stop() waits for a block in the sink, so the sink must not stop this source.
    bool deliver(std::span<const std::int16_t> samples, int rate, double t_mono);

    int start_count() const;
    int stop_count() const;
    int is_active_calls() const;

    std::string_view channel() const override { return channel_; }
    void start(BlockSink on_block) override;
    void stop() override;
    bool is_active() const override;
    DeviceId device_id() const override { return device_id_; }
    bool requires_audio_blocks() const override { return requires_audio_blocks_; }
    std::optional<bool> is_default_device_current() override;

private:
    const std::string channel_;
    const DeviceId device_id_;
    const bool requires_audio_blocks_;
    mutable std::mutex mutex_;
    std::mutex deliver_mutex_; // held while a block is in the sink; taken before mutex_
    BlockSink sink_;           // written under both mutexes
    bool running_ = false;
    bool killed_ = false;
    bool start_inactive_ = false;
    std::optional<std::string> start_failure_;
    mutable std::optional<int> is_active_budget_;
    std::string is_active_failure_;
    std::optional<bool> default_current_;
    int starts_ = 0;
    int stops_ = 0;
    mutable int is_active_calls_ = 0;
};

} // namespace fv::meeting::testing
