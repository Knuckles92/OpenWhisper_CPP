#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "audio/backend.h"
#include "audio/devices.h"

// meeting/capture/devices.py: the devices of Meeting Mode's two channels. The microphone ("Me")
// comes from the ranked microphone list; system audio ("Others") is WASAPI loopback of the
// default render endpoint. OpenWhisper found loopback as a PortAudio "[Loopback]" input device
// with a SoundCard fallback; PortAudio as built here has no such devices, so the render endpoints
// of audio::Backend::loopbacks() are the one path. Every probe degrades to nullopt, logging the
// outcome only when it changed (the capture watchdog probes every second).
namespace fv::meeting {

// The microphone for the mic channel: the first ranked device (`priority`, the saved
// {name, hostapi} order), else the legacy device index when no order is saved, else the system
// default, else any other input; nullopt when none is left or the devices cannot be listed.
// `exclude` names devices that already failed this meeting: PortAudio's list is a snapshot, so a
// dead microphone keeps showing up. Devices open at their native rate, so formats are not checked.
std::optional<audio::InputDevice> find_mic_device(audio::Backend& backend,
                                                  std::span<const audio::DeviceKey> priority,
                                                  std::optional<int> legacy_index = std::nullopt,
                                                  std::span<const audio::DeviceKey> exclude = {});

// The render endpoint whose output the loopback channel records: the default one, else the
// first; nullopt when there is none or the endpoints cannot be read.
std::optional<audio::LoopbackEndpoint> find_loopback_endpoint(audio::Backend& backend);

// Forgets the probe outcomes logged so far, so the next of each kind logs at its own level
// (tests; OpenWhisper's fixture clears _last_logged).
void forget_logged_probe_outcomes();

} // namespace fv::meeting
