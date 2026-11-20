#include "audio/detail/portaudio_error.h"

#include <format>

namespace fv::audio::detail {

std::string portaudio_error_text(std::string_view message, std::string_view error_text, int code,
                                 const std::optional<PortAudioHostError>& host) {
    std::string text(error_text);
    if (!message.empty()) {
        text = std::format("{}: {}", message, text);
    }
    text += std::format(" [PaErrorCode {}]", code);
    if (host) {
        text += std::format(": '{}' [{} error {}]", host->text, host->host_api, host->code);
    }
    return text;
}

} // namespace fv::audio::detail
