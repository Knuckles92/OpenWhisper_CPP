#pragma once

#include <optional>
#include <string>
#include <string_view>

// The text of sounddevice's PortAudioError, which OpenWhisper logs and shows when a microphone or
// the speakers will not open. Free of PortAudio's header so tests can pin the format.
namespace fv::audio::detail {

// Pa_GetLastHostErrorInfo() for paUnanticipatedHostError.
struct PortAudioHostError {
    // query_hostapis(index)['name'], or "<host API not found>" / "<error getting host API: n>"
    // when PortAudio could not map the host API type to an index.
    std::string host_api;
    long code = 0;
    std::string text;
};

// str(PortAudioError) as sounddevice's _check() raises it: "<message>: <Pa_GetErrorText>
// [PaErrorCode <code>]", then ": '<host text>' [<host API> error <host code>]" for a host error.
// An empty `message` leaves out its prefix.
std::string portaudio_error_text(std::string_view message, std::string_view error_text, int code,
                                 const std::optional<PortAudioHostError>& host = std::nullopt);

} // namespace fv::audio::detail
