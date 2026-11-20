#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio/backend.h"
#include "base/executor.h"
#include "base/json.h"

namespace fv {
class SettingsStore;
}

// Microphone identity and ranking, shared by dictation and Meeting Mode (services/
// audio_devices.py). PortAudio enumerates devices once, when it initializes, so the device list
// is a snapshot: a microphone plugged in later is invisible and an unplugged one keeps
// enumerating until refresh. Indexes also shift between launches, so a saved choice is a
// {name, hostapi} key matched back to the current list, never an index.
namespace fv::audio {

inline constexpr std::string_view kLoopbackMarker = "[Loopback]";
inline constexpr std::string_view kSoundMapperName = "Microsoft Sound Mapper - Input";
// MME copies device names into a 32-byte buffer, so longer names arrive cut to 31 characters
// ("Microphone (2- USB Audio Device"). Counted in code points, as Python's len().
inline constexpr std::size_t kMmeNameLimit = 31;
inline constexpr std::string_view kRoleDefault = "default";
inline constexpr std::string_view kRoleMapper = "mapper";
// The MME Sound Mapper follows the OS default; only Windows has it (audio_devices.py:350).
#ifdef _WIN32
inline constexpr bool kSoundMapperPlatform = true;
#else
inline constexpr bool kSoundMapperPlatform = false;
#endif

struct InputDevice {
    int index = -1;
    std::string name;
    std::string hostapi;
    int channels = 1;
    int samplerate = 44100;
    // The untruncated name when MME cut it and another host API has it whole.
    std::string label;
    // On the platform's default host API (MME, Core Audio, ALSA).
    bool default_api = false;
    // PortAudio's default input as of its last initialization (DeviceInfo::is_default).
    bool is_default = false;
    // kRoleDefault or kRoleMapper when a candidate stands in for the system default rather than
    // a ranked choice; empty otherwise.
    std::string role;

    DeviceKey key() const;
    std::string display() const; // display_name(label or name)
    bool operator==(const InputDevice&) const = default;
};

// The saved identity of a device: its name and host API name. A mapping's values pass through
// Python's str(value or "").
DeviceKey device_key(const InputDevice& device);
DeviceKey device_key(const Json& mapping);
Json device_key_json(const DeviceKey& key); // {"name": ..., "hostapi": ...}

// A clean audio_input_priority list: {name, hostapi} entries whose name is a non-blank string
// and hostapi a string (absent: ""), no repeats, order kept. Anything but a list is [].
std::vector<DeviceKey> normalize_priority(const Json& value);
std::vector<DeviceKey> normalize_priority(std::span<const DeviceKey> value);
Json priority_json(std::span<const DeviceKey> priority);

// True when `device` can back a microphone: it has input channels and is not a WASAPI loopback.
bool is_usable_input(const DeviceInfo& device);
// The index a backend marks is_default: the first of PortAudio's default input and the default
// host API's default input that is a usable input, else -1. `inputs` is what inputs() lists.
int default_input_index(std::span<const DeviceInfo> inputs, int portaudio_default,
                        int hostapi_default);

// Every input of `devices` except WASAPI loopback devices; MME names cut at exactly 31
// characters get `label` from a longer name another host API has whole.
std::vector<InputDevice> list_input_devices(std::span<const DeviceInfo> devices);
// The same over backend.inputs(), under the PortAudio lock. Throws what inputs() throws.
std::vector<InputDevice> list_input_devices(Backend& backend);

bool is_system_alias(const InputDevice& device);

// The current device a saved entry names, or nullopt. The entry's own host API goes through
// every rung of the ladder first (exact, without "N- ", MME's 31-character cut, without ALSA's
// "(hw:X,Y)"), then the platform's default host API, then the rest; within a rung the lowest
// index wins. Conservative: "Microphone (USB" does not match.
std::optional<InputDevice> match_entry(const DeviceKey& entry,
                                       std::span<const InputDevice> devices);

// WASAPI refuses a rate other than its mix format unless asked to convert (extra_settings).
bool needs_wasapi_conversion(const InputDevice* device);
// The stream settings PortAudio checks or opens for `device` (null: the default input).
StreamConfig input_config(const InputDevice* device, int samplerate = 44100, int channels = 1);
// Whether PortAudio accepts this format on `device`, without opening it.
bool can_open(Backend& backend, const InputDevice& device, int samplerate = 44100,
              int channels = 1);

// PortAudio's default input, as of its last initialization.
std::optional<InputDevice> default_input(std::span<const InputDevice> devices);
std::optional<InputDevice> sound_mapper(std::span<const InputDevice> devices);

struct RankOptions {
    std::vector<DeviceKey> exclude;
    // Each candidate must accept the format (dictation's 44.1 kHz mono int16); Meeting Mode
    // opens native rates and passes false.
    bool check = true;
    int samplerate = 44100;
    int channels = 1;
    // Append the MME Sound Mapper last (sys.platform == "win32" in Python).
    bool sound_mapper = kSoundMapperPlatform;
};

// Devices to try, best first: the ranked entries that match a current device, in order, then
// PortAudio's default input (role "default"), then on Windows the MME Sound Mapper (role
// "mapper"), which follows the default the OS has now. Excluded keys are skipped and a device
// is considered once, by index. Lists devices through the backend (throws what it throws).
std::vector<InputDevice> ranked_candidates(Backend& backend, std::span<const DeviceKey> priority,
                                           const RankOptions& options = {});
// The same over an already listed `devices`; the backend only checks formats.
std::vector<InputDevice> ranked_candidates(Backend& backend, std::span<const DeviceKey> priority,
                                           std::span<const InputDevice> devices,
                                           const RankOptions& options);

// The legacy audio_input_device index: an integer that is not a bool, else nullopt.
std::optional<std::int64_t> legacy_index(const Json& settings);
// Whether load_priority would list devices to migrate the legacy index.
bool legacy_migration_pending(const Json& settings);
// The saved microphone order, migrating the legacy index once: the first load that finds an
// input at that index saves it as the first entry and keeps the old key for a downgrade. Until
// then the order is empty (the system default) and nothing is saved. Lists devices while the
// migration is pending, so never call it on the GUI thread then.
std::vector<DeviceKey> load_priority(SettingsStore& settings, Backend& backend);
// For callers on the GUI thread (recorder.from_settings): returns the order at once without
// listing devices; while a migration is pending it returns [] (the system default) and runs the
// migration on `migration`, which then calls `on_migrated` with the result there. `settings`
// and `backend` must outlive that task.
std::vector<DeviceKey> load_priority(SettingsStore& settings, Backend& backend, Executor& migration,
                                     std::function<void(std::vector<DeviceKey>)> on_migrated);

// Serializes device queries, stream opens and the PortAudio re-initialization: re-initializing
// invalidates every device index and every open stream in the process.
std::recursive_mutex& portaudio_mutex();
// The process's open PortAudio streams, by an identity token; refresh waits for zero.
void register_stream(const void* stream);
void unregister_stream(const void* stream);
std::size_t open_stream_count();

// Terminate and initialize of the audio system, for refresh_portaudio. initialize throws on
// failure.
class PortAudioLifecycle {
public:
    virtual ~PortAudioLifecycle() = default;
    virtual void terminate() = 0;
    virtual void initialize() = 0;
};
// Re-reads the device list so newly plugged microphones appear, only while no stream is open;
// false when refused. A failed initialize is logged and tried once more (a second failure
// throws). Never on the way to opening a stream: it costs about 25 ms, more with Bluetooth.
bool refresh_portaudio(PortAudioLifecycle& lifecycle);

// A device name as people should read it: drops Windows' "N- " numbering and ALSA's
// "(hw:X,Y)", and closes a parenthesis MME cut off.
std::string display_name(std::string_view name);
// The distinctive part: "Microphone (Blue Snowball)" becomes "Blue Snowball".
std::string short_name(std::string_view name);
// "Windows DirectSound" reads as "DirectSound".
std::string hostapi_label(std::string_view hostapi);

} // namespace fv::audio
