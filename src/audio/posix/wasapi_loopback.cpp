#include "audio/win/wasapi_loopback.h"

#include <string_view>

namespace fv::audio::detail {

namespace {

// System audio on Linux and macOS (PulseAudio monitors, ScreenCaptureKit) is not ported.
constexpr std::string_view kNotSupported = "Loopback capture needs WASAPI, which is Windows-only.";

} // namespace

std::vector<LoopbackEndpoint> wasapi_loopbacks() {
    return {};
}

std::optional<std::string> wasapi_default_loopback_id() {
    return std::nullopt;
}

std::expected<std::unique_ptr<Stream>, std::string>
open_wasapi_loopback(const LoopbackEndpoint& /*endpoint*/, CaptureFn /*fn*/, void* /*user*/,
                     std::function<void(std::string)> /*on_lost*/) {
    return std::unexpected(std::string(kNotSupported));
}

} // namespace fv::audio::detail
