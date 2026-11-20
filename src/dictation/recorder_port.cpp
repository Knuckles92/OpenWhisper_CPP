#include "dictation/recorder_port.h"

#include "base/utf8.h"
#include "recorder/recorder.h"

namespace fv::dictation {

bool RecorderAdapter::is_recording() const {
    return recorder_.is_recording();
}

bool RecorderAdapter::capture_canceled() const {
    return recorder_.capture_canceled();
}

bool RecorderAdapter::stream_open() const {
    return recorder_.is_recording();
}

bool RecorderAdapter::start_recording() {
    return recorder_.start_recording();
}

std::string RecorderAdapter::last_start_error() const {
    return recorder_.last_start_error();
}

bool RecorderAdapter::stop_recording() {
    return recorder_.stop_recording();
}

bool RecorderAdapter::wait_for_stop_completion() {
    return recorder_.wait_for_stop_completion();
}

bool RecorderAdapter::has_recording_data() const {
    return recorder_.has_recording_data();
}

bool RecorderAdapter::save_recording() {
    return recorder_.save_recording(std::nullopt, true);
}

double RecorderAdapter::get_recording_duration() const {
    return recorder_.get_recording_duration();
}

std::string RecorderAdapter::last_capture_error() const {
    return recorder_.last_capture_error();
}

void RecorderAdapter::fail_capture(const std::string& message) {
    recorder_.fail_capture(message);
}

void RecorderAdapter::cancel_recording() {
    recorder_.cancel_recording();
}

void RecorderAdapter::acknowledge_recording() {
    recorder_.acknowledge_recording();
}

const std::filesystem::path& RecorderAdapter::output_file() const {
    return recorder_.output_file();
}

Signal<std::string>& RecorderAdapter::capture_error() {
    return recorder_.capture_error;
}

std::shared_ptr<const audio::PcmTape> RecorderAdapter::tape() const {
    return recorder_.tape();
}

std::string RecorderAdapter::journal_id() const {
    const auto journal = recorder_.journal();
    return journal ? base_utf8::path_to_utf8(journal->directory().filename()) : std::string();
}

bool RecorderAdapter::pause() {
    return recorder_.pause();
}

void RecorderAdapter::resume() {
    recorder_.resume();
}

bool RecorderAdapter::paused() const {
    return recorder_.paused();
}

} // namespace fv::dictation
