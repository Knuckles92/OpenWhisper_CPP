#include "appcore/mic_priority.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "audio/devices.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "settings/keys.h"

namespace fv::appcore::mic {

namespace {

audio::InputDevice input_device(const MicDevice& device) {
    audio::InputDevice input;
    input.index = device.index;
    input.name = device.name;
    input.hostapi = device.hostapi;
    input.label = device.label;
    input.default_api = device.default_api;
    return input;
}

bool ranked(std::span<const DeviceKey> priority, const DeviceKey& entry) {
    return std::ranges::find(priority, entry) != priority.end();
}

} // namespace

std::string MicDevice::display() const {
    return audio::display_name(label.empty() ? name : label);
}

MicDevice mic_device(const audio::InputDevice& device) {
    return MicDevice{device.index,
                     device.name,
                     device.hostapi,
                     device.default_api,
                     audio::is_system_alias(device),
                     device.label};
}

std::string entry_token(const DeviceKey& entry) {
    return py::dumps(Json::array({entry.hostapi, entry.name}));
}

std::optional<DeviceKey> token_entry(std::string_view token) {
    Json value;
    try {
        value = py::loads(token);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    // `hostapi, name = value` unpacks any two-item iterable: a list, a string of two code
    // points, or an object's two keys.
    Json first;
    Json second;
    if (value.is_array() && value.size() == 2) {
        first = value[0];
        second = value[1];
    } else if (value.is_object() && value.size() == 2) {
        auto it = value.begin();
        first = it.key();
        second = std::next(it).key();
    } else if (value.is_string()) {
        const std::u32string chars = py::to_code_points(value.get<std::string>());
        if (chars.size() != 2) {
            return std::nullopt;
        }
        first = py::to_utf8(chars.substr(0, 1));
        second = py::to_utf8(chars.substr(1, 1));
    } else {
        return std::nullopt;
    }
    if (!first.is_string() || !second.is_string() || second.get<std::string>().empty()) {
        return std::nullopt;
    }
    return DeviceKey{second.get<std::string>(), first.get<std::string>()};
}

std::vector<DeviceKey> saved_priority(const Json& settings) {
    return audio::normalize_priority(keys::audio_input_priority.read(settings));
}

std::string preferred_token(const Json& settings) {
    const auto priority = saved_priority(settings);
    return priority.empty() ? std::string(kSystemDefault) : entry_token(priority.front());
}

std::string rail_value(const Json& settings) {
    const auto priority = saved_priority(settings);
    if (priority.empty()) {
        return "System default";
    }
    std::string value = audio::short_name(priority[0].name);
    if (priority.size() > 1) {
        value += " → " + audio::short_name(priority[1].name);
    }
    if (priority.size() > 2) {
        value += std::format(" +{}", priority.size() - 2);
    }
    return value;
}

std::string preferred_name(const Json& settings) {
    const auto priority = saved_priority(settings);
    return priority.empty() ? std::string("Default microphone")
                            : audio::short_name(priority.front().name);
}

bool listed(const MicDevice& device) {
    return device.default_api && !device.system_alias;
}

std::optional<MicDevice> match(const DeviceKey& entry, const Devices& devices) {
    if (!devices || devices->empty()) {
        return std::nullopt;
    }
    std::vector<audio::InputDevice> inputs;
    inputs.reserve(devices->size());
    for (const auto& device : *devices) {
        inputs.push_back(input_device(device));
    }
    const auto found = audio::match_entry(entry, inputs);
    if (!found) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i] == *found) {
            return (*devices)[i];
        }
    }
    return std::nullopt;
}

std::pair<std::string, bool> entry_label(const DeviceKey& entry, const Devices& devices) {
    const auto device = match(entry, devices);
    if (!device) {
        return {audio::display_name(entry.name), !devices.has_value()};
    }
    std::string label = device->display();
    if (!device->default_api) {
        label += " · " + audio::hostapi_label(device->hostapi);
    }
    return {label, true};
}

std::string_view preferred_description(bool has_priority) {
    return has_priority ? "Used for dictation and meetings."
                        : "Used for dictation and meetings. Choose one to set backups for when "
                          "it's unplugged.";
}

PreferredCombo preferred_combo(std::span<const DeviceKey> priority, const Devices& devices) {
    PreferredCombo combo;
    combo.items.push_back({"System default", std::string(kSystemDefault)});
    std::vector<std::string> tokens{std::string(kSystemDefault)};
    const auto add_once = [&](std::string text, std::string token) {
        if (std::ranges::find(tokens, token) != tokens.end()) {
            return;
        }
        tokens.push_back(token);
        combo.items.push_back({std::move(text), std::move(token)});
    };
    if (devices) {
        for (const auto& device : *devices) {
            if (!listed(device)) {
                continue;
            }
            const auto saved = std::ranges::find_if(
                priority, [&](const DeviceKey& entry) { return match(entry, devices) == device; });
            add_once(device.display(),
                     entry_token(saved != priority.end() ? *saved : device.key()));
        }
    }
    for (const auto& entry : priority) {
        const std::string token = entry_token(entry);
        if (std::ranges::find(tokens, token) != tokens.end()) {
            continue;
        }
        auto [label, connected] = entry_label(entry, devices);
        add_once(connected ? label : label + " · Not connected", token);
    }
    const std::string wanted =
        priority.empty() ? std::string(kSystemDefault) : entry_token(priority.front());
    for (std::size_t i = 0; i < combo.items.size(); ++i) {
        if (combo.items[i].token == wanted) {
            combo.current = i;
            break;
        }
    }
    return combo;
}

AddCombo add_combo(std::span<const DeviceKey> priority, const Devices& devices) {
    AddCombo combo;
    if (!devices) {
        combo.items.push_back({"Looking for microphones…", std::nullopt});
    } else {
        std::vector<MicDevice> taken;
        for (const auto& entry : priority) {
            if (auto device = match(entry, devices)) {
                taken.push_back(std::move(*device));
            }
        }
        std::vector<const MicDevice*> choices;
        for (const auto& device : *devices) {
            if (listed(device) && std::ranges::find(taken, device) == taken.end()) {
                choices.push_back(&device);
            }
        }
        combo.items.push_back(
            {choices.empty() ? "No other microphones found" : "Add a microphone…", std::nullopt});
        for (const MicDevice* device : choices) {
            combo.items.push_back({device->display(), entry_token(device->key())});
        }
    }
    combo.enabled = combo.items.size() > 1;
    return combo;
}

std::vector<BackupRow> backup_rows(std::span<const DeviceKey> priority, const Devices& devices) {
    std::vector<BackupRow> rows;
    if (priority.size() < 2) {
        return rows;
    }
    const std::size_t backups = priority.size() - 1;
    for (std::size_t offset = 0; offset < backups; ++offset) {
        const DeviceKey& entry = priority[offset + 1];
        auto [label, connected] = entry_label(entry, devices);
        rows.push_back(BackupRow{
            .label = std::move(label),
            .note = connected ? "" : "Not connected",
            .state = connected ? "" : "missing",
            .token = entry_token(entry),
            .index = offset + 1,
            .up = offset > 0,
            .down = offset + 1 < backups,
        });
    }
    return rows;
}

MicSave priority_save(std::vector<DeviceKey> priority) {
    MicSave save;
    save.updates[std::string(keys::audio_input_priority.key())] = audio::priority_json(priority);
    save.priority = std::move(priority);
    return save;
}

std::optional<MicSave> choose_preferred(std::string_view token, std::span<const DeviceKey> priority,
                                        const Devices& devices) {
    std::vector<DeviceKey> chosen;
    if (token != kSystemDefault) {
        const auto entry = token_entry(token);
        if (!entry) {
            return std::nullopt;
        }
        chosen.push_back(*entry);
        for (const auto& saved : priority) {
            if (saved != *entry) {
                chosen.push_back(saved);
            }
        }
    }
    const auto device = chosen.empty() ? std::nullopt : match(chosen.front(), devices);
    MicSave save = priority_save(std::move(chosen));
    if (device && device->default_api) {
        save.updates[std::string(keys::audio_input_device.key())] = device->index;
    } else {
        save.drops.emplace_back(keys::audio_input_device.key());
    }
    return save;
}

std::optional<std::vector<DeviceKey>> move(std::span<const DeviceKey> priority, std::size_t index,
                                           int offset) {
    const auto size = static_cast<std::int64_t>(priority.size());
    const auto from = static_cast<std::int64_t>(index);
    const std::int64_t target = from + offset;
    if (!(1 <= from && from < size && 1 <= target && target < size)) {
        return std::nullopt;
    }
    std::vector<DeviceKey> moved(priority.begin(), priority.end());
    std::swap(moved[static_cast<std::size_t>(from)], moved[static_cast<std::size_t>(target)]);
    return moved;
}

std::optional<std::vector<DeviceKey>> remove(std::span<const DeviceKey> priority,
                                             std::size_t index) {
    if (index < 1 || index >= priority.size()) {
        return std::nullopt;
    }
    std::vector<DeviceKey> kept(priority.begin(), priority.end());
    kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
    return kept;
}

std::optional<std::vector<DeviceKey>> add(std::span<const DeviceKey> priority,
                                          std::string_view token) {
    const auto entry = token_entry(token);
    if (!entry || ranked(priority, *entry) || priority.empty()) {
        return std::nullopt;
    }
    std::vector<DeviceKey> added(priority.begin(), priority.end());
    added.push_back(*entry);
    return added;
}

} // namespace fv::appcore::mic
