#include "remote_host/registry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

#include "base/ids.h"
#include "pyport/errors.h"
#include "remote/protocol.h"

namespace fv::remote_host {

namespace {

// last_seen is persisted at most this often per device (host.py _LAST_SEEN_PERSIST_S).
constexpr double kLastSeenPersistS = 60.0;

const char* const kUnnamed = "Unnamed computer";

std::string string_field(const Json& entry, std::string_view key) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

// The entry without its token digest, as Python's {k: v for k, v in entry.items() if k !=
// "token_sha256"} returns it.
Json without_digest(const Json& entry) {
    Json out = Json::object();
    for (const auto& [key, value] : entry.items()) {
        if (key != "token_sha256") {
            out[key] = value;
        }
    }
    return out;
}

double steady_seconds(const Clock& clock) {
    return std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
}

// Python's bool(value) for a JSON value: null, false, 0, "" and empty containers are false.
bool truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return false;
    case Json::value_t::boolean:
        return value.get<bool>();
    case Json::value_t::number_integer:
        return value.get<std::int64_t>() != 0;
    case Json::value_t::number_unsigned:
        return value.get<std::uint64_t>() != 0;
    case Json::value_t::number_float:
        return value.get<double>() != 0.0;
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
    case Json::value_t::binary:
        return !value.empty();
    }
    return false;
}

bool ascii(std::string_view text) {
    unsigned char seen = 0;
    for (const char ch : text) {
        seen |= static_cast<unsigned char>(ch);
    }
    return (seen & 0x80U) == 0;
}

// hmac.compare_digest over two str: py::TypeError when either is not ASCII. As CPython's _tscmp,
// the loop runs over b's length whatever a holds, and every byte is compared, so the time tells
// nothing about where the strings differ.
bool compare_digest(std::string_view a, std::string_view b) {
    if (!ascii(a) || !ascii(b)) {
        throw py::TypeError("comparing strings with non-ASCII characters is not supported");
    }
    const std::string_view left = a.size() == b.size() ? a : b;
    unsigned char result = a.size() == b.size() ? 0 : 1;
    for (std::size_t i = 0; i < b.size(); ++i) {
        result |= static_cast<unsigned char>(static_cast<unsigned char>(left[i]) ^
                                             static_cast<unsigned char>(b[i]));
    }
    return result == 0;
}

Json* find_entry(Json& entries, std::string_view id) {
    for (auto& entry : entries) {
        if (entry.at("id").get_ref<const std::string&>() == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

std::string now_iso(Clock& clock) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(clock.utc());
    return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", seconds);
}

std::string clean_device_name(std::string_view name) {
    std::string cleaned = remote::clean_name(name);
    return cleaned.empty() ? std::string(kUnnamed) : cleaned;
}

std::string clean_device_name(const Json& name) {
    std::string cleaned = remote::clean_name(name);
    return cleaned.empty() ? std::string(kUnnamed) : cleaned;
}

bool valid_record_owner(std::string_view owner) {
    if (owner.empty() || owner.size() > 64) {
        return false;
    }
    for (const char ch : owner) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

remote::PairedDevice paired_device(const Json& entry) {
    remote::PairedDevice device;
    device.id = string_field(entry, "id");
    device.name = string_field(entry, "name");
    device.paired_at = string_field(entry, "paired_at");
    device.last_seen = string_field(entry, "last_seen");
    device.via = string_field(entry, "via");
    device.paired_name = string_field(entry, "paired_name");
    if (const auto it = entry.find("record_owner_ids"); it != entry.end() && it->is_array()) {
        for (const auto& owner : *it) {
            if (owner.is_string()) {
                device.record_owner_ids.push_back(owner.get<std::string>());
            }
        }
    }
    return device;
}

DeviceRegistry::DeviceRegistry(std::function<Json()> load, std::function<void(const Json&)> save,
                               Clock& clock)
    : load_(std::move(load)), save_(std::move(save)), clock_(clock) {}

Json DeviceRegistry::entries_locked() {
    const Json raw = load_();
    Json entries = Json::array();
    if (!raw.is_array()) {
        return entries;
    }
    for (const auto& entry : raw) {
        if (!entry.is_object()) {
            continue;
        }
        const auto id = entry.find("id");
        const auto digest = entry.find("token_sha256");
        if (id == entry.end() || !id->is_string() || digest == entry.end() ||
            !digest->is_string()) {
            continue;
        }
        Json copy = entry;
        if (const auto owners = copy.find("record_owner_ids"); owners != copy.end()) {
            Json kept = Json::array();
            if (owners->is_array()) {
                for (const auto& owner : *owners) {
                    if (!owner.is_string() ||
                        !valid_record_owner(owner.get_ref<const std::string&>())) {
                        continue;
                    }
                    bool seen = false;
                    for (const auto& earlier : kept) {
                        seen = seen || earlier == owner;
                    }
                    if (!seen) {
                        kept.push_back(owner);
                    }
                }
            }
            *owners = std::move(kept);
        }
        entries.push_back(std::move(copy));
    }
    return entries;
}

Json DeviceRegistry::entries() {
    std::lock_guard lock(mutex_);
    return entries_locked();
}

std::vector<remote::PairedDevice> DeviceRegistry::list() {
    std::lock_guard lock(mutex_);
    std::vector<remote::PairedDevice> devices;
    for (const auto& entry : entries_locked()) {
        devices.push_back(paired_device(entry));
    }
    return devices;
}

std::pair<remote::PairedDevice, std::string> DeviceRegistry::add(std::string_view name,
                                                                 std::string_view via) {
    std::string token = token_urlsafe(32);
    Json entry = Json::object();
    entry["id"] = uuid4_hex();
    entry["name"] = clean_device_name(name);
    entry["token_sha256"] = remote::token_digest(token);
    entry["paired_at"] = now_iso(clock_);
    entry["last_seen"] = now_iso(clock_);
    entry["via"] = std::string(via);
    {
        std::lock_guard lock(mutex_);
        Json entries = entries_locked();
        entries.push_back(entry);
        save_(entries);
    }
    return {paired_device(without_digest(entry)), std::move(token)};
}

std::optional<remote::PairedDevice> DeviceRegistry::rename(std::string_view id,
                                                           std::string_view name) {
    std::lock_guard lock(mutex_);
    Json entries = entries_locked();
    Json* entry = find_entry(entries, id);
    if (entry == nullptr) {
        return std::nullopt;
    }
    // entry.pop("paired_name", None) or entry.get("name")
    Json previous = nullptr;
    if (const auto it = entry->find("paired_name"); it != entry->end()) {
        previous = std::move(*it);
        entry->erase(it);
    }
    if (!truthy(previous)) {
        const auto current = entry->find("name");
        previous = current == entry->end() ? Json(nullptr) : *current;
    }
    const std::string paired_name = clean_device_name(previous);
    std::string chosen = remote::clean_name(name);
    if (chosen.empty()) {
        chosen = paired_name;
    }
    (*entry)["name"] = chosen;
    if (chosen != paired_name) {
        (*entry)["paired_name"] = paired_name;
    }
    save_(entries);
    return paired_device(without_digest(*entry));
}

bool DeviceRegistry::remove(std::string_view id) {
    std::lock_guard lock(mutex_);
    const Json entries = entries_locked();
    Json kept = Json::array();
    for (const auto& entry : entries) {
        if (entry.at("id").get_ref<const std::string&>() != id) {
            kept.push_back(entry);
        }
    }
    if (kept.size() == entries.size()) {
        return false;
    }
    save_(kept);
    return true;
}

void DeviceRegistry::attach_record_owner(std::string_view id, std::string_view owner) {
    if (!valid_record_owner(owner)) {
        throw py::ValueError("Invalid stored record owner.");
    }
    std::lock_guard lock(mutex_);
    Json entries = entries_locked();
    Json* target = find_entry(entries, id);
    if (target == nullptr) {
        throw py::ValueError("Pair the receiving computer again before recovering its records.");
    }
    for (const auto& entry : entries) {
        bool owned = entry.at("id").get_ref<const std::string&>() == owner;
        if (const auto owners = entry.find("record_owner_ids"); owners != entry.end()) {
            for (const auto& held : *owners) {
                owned = owned || held.get_ref<const std::string&>() == owner;
            }
        }
        if (owned) {
            throw py::ValueError("These records already belong to a paired computer.");
        }
    }
    Json owners = Json::array();
    if (const auto it = target->find("record_owner_ids"); it != target->end()) {
        owners = *it;
    }
    owners.push_back(std::string(owner));
    (*target)["record_owner_ids"] = std::move(owners);
    save_(entries);
}

std::optional<remote::PairedDevice> DeviceRegistry::authenticate(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const std::string digest = remote::token_digest(token);
    std::lock_guard lock(mutex_);
    std::optional<Json> match;
    // Compare against every entry so timing reveals nothing.
    for (const auto& entry : entries_locked()) {
        if (compare_digest(entry.at("token_sha256").get_ref<const std::string&>(), digest)) {
            match = entry;
        }
    }
    if (!match) {
        return std::nullopt;
    }
    touch_locked(match->at("id").get<std::string>());
    return paired_device(without_digest(*match));
}

void DeviceRegistry::touch_locked(const std::string& id) {
    const double now = steady_seconds(clock_);
    const auto seen = persisted_seen_.find(id);
    const double last = seen == persisted_seen_.end() ? -kLastSeenPersistS : seen->second;
    if (now - last < kLastSeenPersistS) {
        return;
    }
    Json entries = entries_locked();
    for (auto& entry : entries) {
        if (entry.at("id").get_ref<const std::string&>() == id) {
            entry["last_seen"] = now_iso(clock_);
        }
    }
    save_(entries);
    persisted_seen_[id] = now;
}

} // namespace fv::remote_host
