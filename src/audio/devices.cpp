#include "audio/devices.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <utility>

#include "base/log.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::audio {

namespace {

const log::Logger kLog("services.audio_devices");

// Windows' own aliases for "whatever the default input is"; Settings shows them as the fixed
// System default row instead of as microphones. Lower case.
constexpr std::string_view kSystemAliases[] = {"microsoft sound mapper - input",
                                               "primary sound capture driver"};
constexpr std::string_view kGenericOuter[] = {
    "microphone",          "microphone array",    "mic",  "headset", "headset microphone",
    "external microphone", "internal microphone", "line", "line in",
};

// Windows numbers same-named endpoints "2- USB Audio Device", and the number can change on
// replug or another USB port.
const py::Regex& duplicate_prefix() {
    static const py::Regex pattern(R"((^|\()\d+- )");
    return pattern;
}

const py::Regex& alsa_hw_suffix() {
    static const py::Regex pattern(R"(\s*\(hw:\d+,\d+\)\s*$)");
    return pattern;
}

std::string strip_duplicate_prefix(std::string_view name) {
    return py::strip(duplicate_prefix().sub(R"(\1)", name));
}

std::string strip_hw_suffix(std::string_view name) {
    return py::strip(alsa_hw_suffix().sub("", name));
}

bool same_exact(const std::string& a, const std::string& b) {
    return a == b;
}

bool same_without_duplicate_prefix(const std::string& a, const std::string& b) {
    return strip_duplicate_prefix(a) == strip_duplicate_prefix(b);
}

// One name is MME's 31-character cut of the other (numbering aside).
bool same_but_truncated(const std::string& a, const std::string& b) {
    const std::pair<const std::string*, const std::string*> orders[] = {{&a, &b}, {&b, &a}};
    for (const auto& [cut, whole] : orders) {
        if (py::len(*cut) != kMmeNameLimit || *cut == *whole) {
            continue;
        }
        const std::string shorter = strip_duplicate_prefix(*cut);
        if (!shorter.empty() && strip_duplicate_prefix(*whole).starts_with(shorter)) {
            return true;
        }
    }
    return false;
}

bool same_without_hw_suffix(const std::string& a, const std::string& b) {
    return strip_duplicate_prefix(strip_hw_suffix(a)) == strip_duplicate_prefix(strip_hw_suffix(b));
}

using Rung = bool (*)(const std::string&, const std::string&);
constexpr Rung kLadder[] = {same_exact, same_without_duplicate_prefix, same_but_truncated,
                            same_without_hw_suffix};

// Python's truthiness of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

// str(value or "") for the JSON values a settings file can hold.
std::string str_or_empty(const Json* value) {
    if (value == nullptr || !truthy(*value)) {
        return "";
    }
    if (value->is_string()) {
        return value->get<std::string>();
    }
    if (value->is_boolean()) {
        return "True";
    }
    if (value->is_number_integer()) {
        return std::to_string(value->get<std::int64_t>());
    }
    if (value->is_number_unsigned()) {
        return std::to_string(value->get<std::uint64_t>());
    }
    if (value->is_number_float()) {
        return py::float_repr(value->get<double>());
    }
    return value->dump();
}

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

InputDevice with_role(InputDevice device, std::string_view role) {
    if (!role.empty()) {
        device.role = role;
    }
    return device;
}

class StreamRegistry {
public:
    std::set<const void*> streams;
};

StreamRegistry& registry() {
    static StreamRegistry instance;
    return instance;
}

} // namespace

DeviceKey InputDevice::key() const {
    return DeviceKey{name, hostapi};
}

std::string InputDevice::display() const {
    return display_name(label.empty() ? name : label);
}

DeviceKey device_key(const InputDevice& device) {
    return device.key();
}

DeviceKey device_key(const Json& mapping) {
    return DeviceKey{str_or_empty(find(mapping, "name")), str_or_empty(find(mapping, "hostapi"))};
}

Json device_key_json(const DeviceKey& key) {
    Json entry = Json::object();
    entry["name"] = key.name;
    entry["hostapi"] = key.hostapi;
    return entry;
}

std::vector<DeviceKey> normalize_priority(const Json& value) {
    std::vector<DeviceKey> result;
    if (!value.is_array()) {
        return result;
    }
    for (const Json& entry : value) {
        if (!entry.is_object()) {
            continue;
        }
        const Json* name = find(entry, "name");
        const Json* hostapi = find(entry, "hostapi");
        if (name == nullptr || !name->is_string() ||
            py::strip(name->get_ref<const std::string&>()).empty()) {
            continue;
        }
        if (hostapi != nullptr && !hostapi->is_string()) {
            continue;
        }
        DeviceKey key{name->get<std::string>(),
                      hostapi == nullptr ? "" : hostapi->get<std::string>()};
        if (std::find(result.begin(), result.end(), key) != result.end()) {
            continue;
        }
        result.push_back(std::move(key));
    }
    return result;
}

std::vector<DeviceKey> normalize_priority(std::span<const DeviceKey> value) {
    std::vector<DeviceKey> result;
    for (const DeviceKey& key : value) {
        if (py::strip(key.name).empty() ||
            std::find(result.begin(), result.end(), key) != result.end()) {
            continue;
        }
        result.push_back(key);
    }
    return result;
}

Json priority_json(std::span<const DeviceKey> priority) {
    Json list = Json::array();
    for (const DeviceKey& key : priority) {
        list.push_back(device_key_json(key));
    }
    return list;
}

bool is_usable_input(const DeviceInfo& device) {
    return device.max_input_channels > 0 && device.name.find(kLoopbackMarker) == std::string::npos;
}

int default_input_index(std::span<const DeviceInfo> inputs, int portaudio_default,
                        int hostapi_default) {
    for (const int index : {portaudio_default, hostapi_default}) {
        if (index < 0) {
            continue;
        }
        const auto it = std::find_if(inputs.begin(), inputs.end(),
                                     [&](const DeviceInfo& d) { return d.index == index; });
        if (it != inputs.end() && is_usable_input(*it)) {
            return index;
        }
    }
    return -1;
}

std::vector<InputDevice> list_input_devices(std::span<const DeviceInfo> devices) {
    std::vector<InputDevice> listed;
    for (const DeviceInfo& info : devices) {
        if (!is_usable_input(info)) {
            continue;
        }
        InputDevice device;
        device.index = info.index;
        device.name = info.name;
        device.hostapi = info.host_api;
        device.channels = info.max_input_channels > 0 ? info.max_input_channels : 1;
        // int(default_samplerate or 44100): truncated toward zero.
        device.samplerate = info.default_rate != 0.0 ? static_cast<int>(info.default_rate) : 44100;
        device.default_api = info.default_api;
        device.is_default = info.is_default;
        listed.push_back(std::move(device));
    }
    for (InputDevice& device : listed) {
        if (py::len(device.name) != kMmeNameLimit) {
            continue;
        }
        for (const InputDevice& other : listed) {
            if (py::len(other.name) > py::len(device.name) && other.name.starts_with(device.name)) {
                device.label = other.name;
                break;
            }
        }
    }
    return listed;
}

std::vector<InputDevice> list_input_devices(Backend& backend) {
    std::vector<DeviceInfo> raw;
    {
        const std::lock_guard lock(portaudio_mutex());
        raw = backend.inputs();
    }
    return list_input_devices(raw);
}

bool is_system_alias(const InputDevice& device) {
    const std::string lowered = py::lower(device.name);
    return std::ranges::find(kSystemAliases, lowered) != std::end(kSystemAliases);
}

std::optional<InputDevice> match_entry(const DeviceKey& entry,
                                       std::span<const InputDevice> devices) {
    if (entry.name.empty()) {
        return std::nullopt;
    }
    std::vector<const InputDevice*> groups[3];
    for (const InputDevice& device : devices) {
        if (device.hostapi == entry.hostapi) {
            groups[0].push_back(&device);
        } else if (device.default_api) {
            groups[1].push_back(&device);
        } else {
            groups[2].push_back(&device);
        }
    }
    for (const auto& group : groups) {
        for (const Rung rung : kLadder) {
            for (const InputDevice* device : group) {
                if (rung(entry.name, device->name)) {
                    return *device;
                }
            }
        }
    }
    return std::nullopt;
}

bool needs_wasapi_conversion(const InputDevice* device) {
    return device != nullptr && py::lower(device->hostapi).find("wasapi") != std::string::npos;
}

StreamConfig input_config(const InputDevice* device, int samplerate, int channels) {
    StreamConfig config{.device_index = device == nullptr ? -1 : device->index};
    config.rate = samplerate;
    config.channels = channels;
    config.wasapi_auto_convert = needs_wasapi_conversion(device);
    return config;
}

bool can_open(Backend& backend, const InputDevice& device, int samplerate, int channels) {
    try {
        const std::lock_guard lock(portaudio_mutex());
        return backend.check_input(input_config(&device, samplerate, channels)).has_value();
    } catch (const std::exception&) {
        return false; // PortAudio rejects unsupported formats with assorted errors.
    }
}

std::optional<InputDevice> default_input(std::span<const InputDevice> devices) {
    for (const InputDevice& device : devices) {
        if (device.is_default) {
            return device;
        }
    }
    return std::nullopt;
}

std::optional<InputDevice> sound_mapper(std::span<const InputDevice> devices) {
    for (const InputDevice& device : devices) {
        if (device.hostapi == "MME" && device.name == kSoundMapperName) {
            return device;
        }
    }
    return std::nullopt;
}

std::vector<InputDevice> ranked_candidates(Backend& backend, std::span<const DeviceKey> priority,
                                           const RankOptions& options) {
    const std::vector<InputDevice> devices = list_input_devices(backend);
    return ranked_candidates(backend, priority, devices, options);
}

std::vector<InputDevice> ranked_candidates(Backend& backend, std::span<const DeviceKey> priority,
                                           std::span<const InputDevice> devices,
                                           const RankOptions& options) {
    const auto excluded = [&](const DeviceKey& key) {
        return std::find(options.exclude.begin(), options.exclude.end(), key) !=
               options.exclude.end();
    };
    std::vector<InputDevice> ranked;
    std::set<int> seen;
    const auto consider = [&](const std::optional<InputDevice>& device, std::string_view role) {
        if (!device || seen.contains(device->index) || excluded(device->key())) {
            return;
        }
        seen.insert(device->index);
        if (options.check && !can_open(backend, *device, options.samplerate, options.channels)) {
            return;
        }
        ranked.push_back(with_role(*device, role));
    };
    for (const DeviceKey& entry : normalize_priority(priority)) {
        if (!excluded(entry)) {
            consider(match_entry(entry, devices), "");
        }
    }
    consider(default_input(devices), kRoleDefault);
    if (options.sound_mapper) {
        consider(sound_mapper(devices), kRoleMapper);
    }
    return ranked;
}

std::optional<std::string> Backend::default_loopback_id() {
    try {
        for (const LoopbackEndpoint& endpoint : loopbacks()) {
            if (endpoint.is_default) {
                return endpoint.id;
            }
        }
    } catch (const std::exception&) {
        // The answer is only "none": the caller lists the endpoints when it needs the reason.
    }
    return std::nullopt;
}

std::vector<DeviceInfo> ranked_candidates(std::span<const DeviceKey> priority,
                                          std::span<const DeviceInfo> devices) {
    const std::vector<InputDevice> listed = list_input_devices(devices);
    std::vector<DeviceInfo> ranked;
    std::set<int> seen;
    const auto consider = [&](const std::optional<InputDevice>& device) {
        if (!device || !seen.insert(device->index).second) {
            return;
        }
        const auto it = std::find_if(devices.begin(), devices.end(),
                                     [&](const DeviceInfo& d) { return d.index == device->index; });
        ranked.push_back(*it);
    };
    for (const DeviceKey& entry : normalize_priority(priority)) {
        consider(match_entry(entry, listed));
    }
    consider(default_input(listed));
    if (kSoundMapperPlatform) {
        consider(sound_mapper(listed));
    }
    return ranked;
}

std::optional<std::int64_t> legacy_index(const Json& settings) {
    const Json* legacy = find(settings, keys::audio_input_device.key());
    if (legacy == nullptr || !legacy->is_number_integer()) {
        return std::nullopt;
    }
    if (legacy->is_number_unsigned()) {
        const auto value = legacy->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(INT64_MAX)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    }
    return legacy->get<std::int64_t>();
}

bool legacy_migration_pending(const Json& settings) {
    return find(settings, keys::audio_input_priority.key()) == nullptr &&
           legacy_index(settings).has_value();
}

std::vector<DeviceKey> load_priority(SettingsStore& store, Backend& backend) {
    const std::shared_ptr<const Json> settings = store.snapshot();
    const std::string_view priority_key = keys::audio_input_priority.key();
    if (const Json* saved = find(*settings, priority_key)) {
        return normalize_priority(*saved);
    }
    const std::optional<std::int64_t> legacy = legacy_index(*settings);
    if (!legacy) {
        return {};
    }
    // Saving "no microphone" while the microphone is merely unplugged (or PortAudio is
    // unavailable) would drop the user's choice for good.
    std::optional<InputDevice> device;
    try {
        for (InputDevice& listed : list_input_devices(backend)) {
            if (listed.index == *legacy) {
                device = std::move(listed);
                break;
            }
        }
    } catch (const std::exception&) {
        kLog.warning("Couldn't read audio inputs to migrate the saved microphone");
        return {};
    }
    if (!device) {
        kLog.info("The saved microphone index matches no input now; trying again on a later load");
        return {};
    }
    const std::vector<DeviceKey> migrated{device->key()};
    std::vector<DeviceKey> result;
    try {
        store.mutate([&](Json& current) {
            if (const Json* saved = find(current, priority_key)) {
                result = normalize_priority(*saved);
                return;
            }
            current[std::string(priority_key)] = priority_json(migrated);
            result = migrated;
        });
    } catch (const std::exception&) {
        kLog.warning("Couldn't save the migrated microphone choice");
        return migrated;
    }
    kLog.info("Migrated the saved microphone index to {} ranked entr{}", result.size(),
              result.size() == 1 ? "y" : "ies");
    return result;
}

std::vector<DeviceKey> load_priority(SettingsStore& store, Backend& backend, Executor& migration,
                                     std::function<void(std::vector<DeviceKey>)> on_migrated) {
    const std::shared_ptr<const Json> settings = store.snapshot();
    if (!legacy_migration_pending(*settings)) {
        return load_priority(store, backend);
    }
    migration.post([&store, &backend, on_migrated = std::move(on_migrated)] {
        std::vector<DeviceKey> priority = load_priority(store, backend);
        if (on_migrated) {
            on_migrated(std::move(priority));
        }
    });
    return {};
}

std::recursive_mutex& portaudio_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void register_stream(const void* stream) {
    const std::lock_guard lock(portaudio_mutex());
    registry().streams.insert(stream);
}

void unregister_stream(const void* stream) {
    const std::lock_guard lock(portaudio_mutex());
    registry().streams.erase(stream);
}

std::size_t open_stream_count() {
    const std::lock_guard lock(portaudio_mutex());
    return registry().streams.size();
}

bool refresh_portaudio(PortAudioLifecycle& lifecycle) {
    {
        const std::lock_guard lock(portaudio_mutex());
        if (!registry().streams.empty()) {
            return false;
        }
        lifecycle.terminate();
        try {
            lifecycle.initialize();
        } catch (const std::exception& error) {
            // Leaving PortAudio terminated would fail every later recording.
            kLog.error_with(error, "PortAudio didn't restart after a device refresh; retrying");
            lifecycle.initialize();
        }
    }
    kLog.info("Re-read the audio device list");
    return true;
}

std::string display_name(std::string_view name) {
    std::string shown = strip_hw_suffix(strip_duplicate_prefix(name));
    if (std::ranges::count(shown, '(') > std::ranges::count(shown, ')')) {
        shown = py::rstrip(shown) + ")";
    }
    static const py::Regex space_before_close(R"(\s+\))");
    return space_before_close.sub(")", shown);
}

std::string short_name(std::string_view name) {
    const std::string shown = display_name(name);
    static const py::Regex outer(R"((.*?)\s*\((.+)\))");
    if (const auto match = outer.fullmatch(shown)) {
        const std::string generic = py::lower(py::strip(match->group(1)));
        if (std::ranges::find(kGenericOuter, generic) != std::end(kGenericOuter)) {
            return py::strip(match->group(2));
        }
    }
    return shown;
}

std::string hostapi_label(std::string_view hostapi) {
    constexpr std::string_view prefix = "Windows ";
    return std::string(hostapi.starts_with(prefix) ? hostapi.substr(prefix.size()) : hostapi);
}

} // namespace fv::audio
