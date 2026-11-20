#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "remote/control.h"

// The computers paired with this host (services/remote_asr/host.py DeviceRegistry), persisted
// through `load` and `save` (remote_host_devices in the app, host_settings.h). Each entry keeps
// the SHA-256 of the device's token, never the token, so reading the settings file does not let
// anyone connect.
namespace fv::remote_host {

// "YYYY-MM-DDTHH:MM:SS+00:00": `clock`'s UTC time without its fraction (host.py _now_iso).
std::string now_iso(Clock& clock);

// A computer's name for the registry: protocol::clean_name, or "Unnamed computer" when nothing
// printable is left.
std::string clean_device_name(std::string_view name);
inline std::string clean_device_name(const std::string& name) {
    return clean_device_name(std::string_view(name));
}
inline std::string clean_device_name(const char* name) {
    return clean_device_name(std::string_view(name));
}
// The same over str(name or "") of a JSON value.
std::string clean_device_name(const Json& name);

// Whether `owner` may stand for a device whose records a host keeps: 1 to 64 of A-Z, a-z, 0-9,
// "_" and "-".
bool valid_record_owner(std::string_view owner);

// An entry as `list` shows it, without its token digest. A field that is not a string in the
// stored entry (a hand-edited file) reads as "".
remote::PairedDevice paired_device(const Json& entry);

// Affinity: thread-safe. One lock covers every read-modify-write, `load` and `save` included,
// so a last_seen write never puts back a device another thread removed meanwhile.
class DeviceRegistry {
public:
    // `load` returns the stored list (anything else reads as no devices); `save` stores the list
    // it is given. Both are called with the registry's lock held.
    DeviceRegistry(std::function<Json()> load, std::function<void(const Json&)> save, Clock& clock);
    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    // The entries, for display.
    std::vector<remote::PairedDevice> list();
    // Pairs a computer: a new entry (uuid4 hex id, clean_device_name, paired and last seen now)
    // and the token it connects with (token_urlsafe(32)), which only the client keeps.
    std::pair<remote::PairedDevice, std::string> add(std::string_view name,
                                                     std::string_view via = "code");
    // Calls a device `name` here; an empty name (after clean_name) goes back to the one it paired
    // with, which `paired_name` keeps while the device goes by another. nullopt when the device
    // isn't paired.
    std::optional<remote::PairedDevice> rename(std::string_view id, std::string_view name);
    bool remove(std::string_view id);
    // Hands records an old pairing left behind to a paired device. A host-owner action, never a
    // client's. py::ValueError "Invalid stored record owner.", "Pair the receiving computer again
    // before recovering its records." or "These records already belong to a paired computer.".
    void attach_record_owner(std::string_view id, std::string_view owner);
    // The device holding `token`, compared in constant time against every entry (the last match
    // wins, as nothing exits early); nullopt for an empty or unknown token. last_seen is written
    // at most once per device every 60 s of steady time; the entry returned is as it was read,
    // before that write. py::TypeError when a stored digest is not ASCII (hmac.compare_digest).
    std::optional<remote::PairedDevice> authenticate(std::string_view token);

    // The stored entries, sanitized as every method reads them: dicts with string "id" and
    // "token_sha256", record_owner_ids (where the key exists) reduced to unique valid owners.
    // Token digests included; for the host's own bookkeeping and tests.
    Json entries();

private:
    Json entries_locked();
    void touch_locked(const std::string& id);

    std::function<Json()> load_;
    std::function<void(const Json&)> save_;
    Clock& clock_;
    std::mutex mutex_;
    // Steady seconds of each device's last persisted last_seen.
    std::map<std::string, double, std::less<>> persisted_seen_;
};

} // namespace fv::remote_host
