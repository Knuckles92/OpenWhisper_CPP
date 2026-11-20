#include "meeting_capture/testing/fake_capture_source.h"

#include <stdexcept>
#include <utility>

namespace fv::meeting::testing {

FakeCaptureSource::FakeCaptureSource(std::string channel, DeviceId device_id,
                                     bool requires_audio_blocks)
    : channel_(std::move(channel)), device_id_(std::move(device_id)),
      requires_audio_blocks_(requires_audio_blocks) {}

void FakeCaptureSource::fail_start(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    start_failure_ = std::move(message);
}

void FakeCaptureSource::start_inactive(bool inactive) {
    const std::lock_guard lock(mutex_);
    start_inactive_ = inactive;
}

void FakeCaptureSource::throw_from_is_active_after(int calls, std::string message) {
    const std::lock_guard lock(mutex_);
    is_active_budget_ = calls;
    is_active_failure_ = std::move(message);
}

void FakeCaptureSource::kill() {
    const std::lock_guard lock(mutex_);
    killed_ = true;
}

void FakeCaptureSource::set_default_device_current(std::optional<bool> current) {
    const std::lock_guard lock(mutex_);
    default_current_ = current;
}

bool FakeCaptureSource::deliver(std::span<const std::int16_t> samples, int rate, double t_mono) {
    // Held through the sink, so stop() returns only after a block in flight is done, as a real
    // stream's stop does (a channel's spool queue takes one producer at a time), and start()
    // cannot replace the sink under it. The sink is called in place: copying the std::function
    // would allocate on the "device thread".
    const std::lock_guard delivering(deliver_mutex_);
    {
        const std::lock_guard lock(mutex_);
        if (!running_ || killed_ || !sink_) {
            return false;
        }
    }
    sink_(CaptureBlock{.channel = channel_,
                       .frames = samples.data(),
                       .count = samples.size(),
                       .rate = rate,
                       .t_mono = t_mono});
    return true;
}

int FakeCaptureSource::start_count() const {
    const std::lock_guard lock(mutex_);
    return starts_;
}

int FakeCaptureSource::stop_count() const {
    const std::lock_guard lock(mutex_);
    return stops_;
}

int FakeCaptureSource::is_active_calls() const {
    const std::lock_guard lock(mutex_);
    return is_active_calls_;
}

void FakeCaptureSource::start(BlockSink on_block) {
    // deliver_mutex_ before mutex_, as deliver() takes them.
    const std::lock_guard delivering(deliver_mutex_);
    const std::lock_guard lock(mutex_);
    ++starts_;
    if (start_failure_) {
        throw std::runtime_error(*start_failure_);
    }
    sink_ = std::move(on_block);
    running_ = !start_inactive_;
}

void FakeCaptureSource::stop() {
    {
        const std::lock_guard lock(mutex_);
        ++stops_;
        running_ = false;
    }
    // Waits out a deliver() in progress; none starts after this.
    const std::lock_guard delivering(deliver_mutex_);
}

bool FakeCaptureSource::is_active() const {
    const std::lock_guard lock(mutex_);
    ++is_active_calls_;
    if (is_active_budget_) {
        if (*is_active_budget_ <= 0) {
            throw std::runtime_error(is_active_failure_);
        }
        --*is_active_budget_;
    }
    return running_ && !killed_;
}

std::optional<bool> FakeCaptureSource::is_default_device_current() {
    const std::lock_guard lock(mutex_);
    return default_current_;
}

} // namespace fv::meeting::testing
