#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/backend.h"

// System audio through WASAPI loopback on the render endpoints, the Windows path that replaces
// OpenWhisper's PortAudio "[Loopback]" device and its SoundCard fallback. No Windows types here:
// posix/wasapi_loopback.cpp implements the same functions as "not supported".
namespace fv::audio::detail {

// Active render endpoints with their friendly names, the default flag from
// GetDefaultAudioEndpoint(eRender, eConsole) and the mix format. Throws std::runtime_error when
// the endpoints cannot be read. Initializes COM on a thread of its own, never on the caller's.
std::vector<LoopbackEndpoint> wasapi_loopbacks();

// The id of the default render endpoint (GetDefaultAudioEndpoint(eRender, eConsole)), the one
// wasapi_loopbacks() flags; nullopt when there is none or it cannot be read. Never throws. It
// activates no endpoint, so the meeting watchdog can ask every second. COM on a thread of its own.
std::optional<std::string> wasapi_default_loopback_id();

// Opens and starts loopback capture of `endpoint` on thread fv-loopback (COM MTA). Blocks of at
// most 1024 frames reach `fn` at the endpoint's rate and channels; on_lost runs once on that
// thread when the endpoint goes away or a read fails, never after abort() or destruction.
std::expected<std::unique_ptr<Stream>, std::string>
open_wasapi_loopback(const LoopbackEndpoint& endpoint, CaptureFn fn, void* user,
                     std::function<void(std::string)> on_lost);

} // namespace fv::audio::detail
