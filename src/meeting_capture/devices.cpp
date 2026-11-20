#include "meeting_capture/devices.h"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <string>

#include "base/log.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.capture.devices");

std::mutex& logged_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::string, std::less<>>& last_logged() {
    static std::map<std::string, std::string, std::less<>> logged;
    return logged;
}

// Logs `message` at `level` when it differs from the last one under `key`; an unchanged outcome
// drops to DEBUG, so an outage is logged when it starts and when it ends, not on every probe.
void log_on_change(std::string_view key, log::Level level, std::string message) {
    bool repeated = false;
    {
        const std::lock_guard lock(logged_mutex());
        auto& logged = last_logged();
        const auto found = logged.find(key);
        repeated = found != logged.end() && found->second == message;
        logged.insert_or_assign(std::string(key), message);
    }
    const log::Level effective = repeated ? log::Level::Debug : level;
    switch (effective) {
    case log::Level::Debug:
        logger.debug("{}", message);
        break;
    case log::Level::Info:
        logger.info("{}", message);
        break;
    case log::Level::Warning:
        logger.warning("{}", message);
        break;
    default:
        logger.error("{}", message);
        break;
    }
}

audio::InputDevice chosen(const audio::InputDevice& device) {
    log_on_change("mic", log::Level::Info,
                  std::format("Microphone device: {} (index {}, {} Hz, {} ch)", device.name,
                              device.index, device.samplerate, device.channels));
    return device;
}

bool is_excluded(const audio::InputDevice& device, std::span<const audio::DeviceKey> exclude) {
    const audio::DeviceKey key = device.key();
    return std::find(exclude.begin(), exclude.end(), key) != exclude.end();
}

} // namespace

std::optional<audio::InputDevice> find_mic_device(audio::Backend& backend,
                                                  std::span<const audio::DeviceKey> priority,
                                                  std::optional<int> legacy_index,
                                                  std::span<const audio::DeviceKey> exclude) {
    try {
        const std::vector<audio::InputDevice> devices = audio::list_input_devices(backend);
        if (priority.empty() && legacy_index) {
            const auto found =
                std::find_if(devices.begin(), devices.end(),
                             [&](const audio::InputDevice& d) { return d.index == *legacy_index; });
            if (found != devices.end() && !is_excluded(*found, exclude)) {
                return chosen(*found);
            }
            log_on_change("mic-preferred", log::Level::Warning,
                          std::format("Preferred mic device index {} is not a valid input "
                                      "device; falling back to default",
                                      *legacy_index));
        }
        audio::RankOptions options;
        options.exclude.assign(exclude.begin(), exclude.end());
        options.check = false;
        const std::vector<audio::InputDevice> ranked =
            audio::ranked_candidates(backend, priority, devices, options);
        if (!ranked.empty()) {
            return chosen(ranked.front());
        }
        for (const audio::InputDevice& device : devices) {
            if (!is_excluded(device, exclude)) {
                return chosen(device);
            }
        }
        log_on_change("mic", log::Level::Warning, "No microphone input device found");
        return std::nullopt;
    } catch (const std::exception& error) {
        logger.error_with(error, "Microphone device discovery failed");
        return std::nullopt;
    }
}

std::optional<audio::LoopbackEndpoint> find_loopback_endpoint(audio::Backend& backend) {
    try {
        const std::vector<audio::LoopbackEndpoint> endpoints = backend.loopbacks();
        if (endpoints.empty()) {
#ifdef _WIN32
            log_on_change("loopback", log::Level::Warning,
                          "No WASAPI render endpoints found; loopback capture unavailable");
#else
            // Expected off Windows; the watchdog polls this every second.
            log_on_change("loopback", log::Level::Debug,
                          "No WASAPI host API found; loopback capture unavailable");
#endif
            return std::nullopt;
        }
        const auto found = std::find_if(
            endpoints.begin(), endpoints.end(),
            [](const audio::LoopbackEndpoint& endpoint) { return endpoint.is_default; });
        if (found != endpoints.end()) {
            log_on_change(
                "loopback", log::Level::Info,
                std::format("Loopback device matches default render device: {}", found->name));
            return *found;
        }
        log_on_change("loopback", log::Level::Info,
                      "No loopback device matches the default render device; using the first "
                      "loopback device");
        return endpoints.front();
    } catch (const std::exception& error) {
        logger.error_with(error, "Loopback device discovery failed");
        return std::nullopt;
    }
}

void forget_logged_probe_outcomes() {
    const std::lock_guard lock(logged_mutex());
    last_logged().clear();
}

} // namespace fv::meeting
