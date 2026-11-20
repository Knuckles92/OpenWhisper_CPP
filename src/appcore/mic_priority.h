#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio/backend.h"
#include "base/json.h"

namespace fv::audio {
struct InputDevice;
}

// The rules of Settings → Recording → Microphone (ui_qt/dialogs/settings_microphones.py):
// audio_input_priority is the single source of truth, its first entry the preferred microphone
// and the rest the backups above a fixed "System default" row. Combo items carry an entry token,
// json.dumps([hostapi, name]), never a device index, because indexes shift whenever PortAudio
// re-reads its devices. The section widget (M2.19) lists devices off the GUI thread and renders
// what these functions decide; `devices` is nullopt until that first list arrives, and until
// then nothing is called "Not connected".
// Affinity: thread-safe (pure functions).
namespace fv::appcore::mic {

using audio::DeviceKey;

inline constexpr std::string_view kSystemDefault = "";
inline constexpr std::string_view kOutcomeRefreshed = "refreshed";
inline constexpr std::string_view kOutcomeBusy = "busy";
inline constexpr std::string_view kInUse =
    "Microphones are in use. Refresh when recording or the meeting ends.";

// What the rules need of a listed input (services.audio_devices.InputDevice).
struct MicDevice {
    int index = -1;
    std::string name;
    std::string hostapi;
    bool default_api = false;  // on the platform's default host API
    bool system_alias = false; // audio::is_system_alias: Sound Mapper, Primary Sound Capture
    std::string label;         // the untruncated name when MME cut it, else ""

    DeviceKey key() const { return {name, hostapi}; }
    std::string display() const; // audio::display_name(label or name)
    bool operator==(const MicDevice&) const = default;
};

// The MicDevice of a listed audio::InputDevice.
MicDevice mic_device(const audio::InputDevice& device);

using Devices = std::optional<std::vector<MicDevice>>;

// json.dumps([hostapi, name]), ASCII-escaped as Python writes it.
std::string entry_token(const DeviceKey& entry);
// The entry a combo token names, or nullopt for anything json.loads cannot read or that does not
// unpack into two strings with a non-empty name. Like Python's tuple unpacking, a two-character
// JSON string and a two-key object unpack too.
std::optional<DeviceKey> token_entry(std::string_view token);

// audio_devices.normalize_priority(audio_input_priority).
std::vector<DeviceKey> saved_priority(const Json& settings);
// The preferred entry's token, or kSystemDefault.
std::string preferred_token(const Json& settings);
// The Recording rail value: "System default", or the first two short names joined by " → "
// with " +N" for the rest.
std::string rail_value(const Json& settings);
// The preferred microphone's short name, or "Default microphone".
std::string preferred_name(const Json& settings);
// _listed: one row per microphone, on the default host API and not a system alias.
bool listed(const MicDevice& device);

// _match: the current device a saved entry names (audio::match_entry); nullopt while the
// devices are unknown or none are listed.
std::optional<MicDevice> match(const DeviceKey& entry, const Devices& devices);
// _label: what to call a saved entry, and whether it may be connected. A matched device shows
// its display name (" · <host API label>" when not on the default host API); an unmatched one
// shows display_name(name) and counts as connected only while the devices are unknown.
std::pair<std::string, bool> entry_label(const DeviceKey& entry, const Devices& devices);

// The Recording tile's description.
std::string_view preferred_description(bool has_priority);

struct ComboItem {
    std::string text;
    std::optional<std::string> token; // nullopt for a placeholder (Python's None)
    bool operator==(const ComboItem&) const = default;
};

// _fill_preferred: "System default", one row per listed device (carrying the token of the
// saved entry that matches it, else the device's own), then each saved entry not yet listed
// ("<label> · Not connected" when it cannot be). `current` is the row the combo selects.
struct PreferredCombo {
    std::vector<ComboItem> items;
    std::size_t current = 0;
};
PreferredCombo preferred_combo(std::span<const DeviceKey> priority, const Devices& devices);

// _fill_add_combo: a placeholder ("Looking for microphones…" while the devices are unknown,
// "Add a microphone…" or "No other microphones found"), then each listed device no saved entry
// matches. The combo is enabled when it holds more than the placeholder.
struct AddCombo {
    std::vector<ComboItem> items;
    bool enabled = false;
};
AddCombo add_combo(std::span<const DeviceKey> priority, const Devices& devices);

// One row of the backups list (_fill_rows). Backups are the entries after the first; `index` is
// the entry's position in the priority. The top backup has no Up and the bottom no Down.
struct BackupRow {
    std::string label;
    std::string note;  // "" or "Not connected"
    std::string state; // "" or "missing"
    std::string token;
    std::size_t index = 0;
    bool up = false;
    bool down = false;
};
// The backups, without the fixed last row (kFixedRowLabel, kFixedRowNote, state "fixed").
// The backups tile is shown only while a microphone is chosen.
std::vector<BackupRow> backup_rows(std::span<const DeviceKey> priority, const Devices& devices);
inline constexpr std::string_view kFixedRowLabel = "System default";
inline constexpr std::string_view kFixedRowNote = "Always last";

// A save the section makes: the new order, and the settings patch and removals that store it.
struct MicSave {
    std::vector<DeviceKey> priority;
    Json updates = Json::object();
    std::vector<std::string> drops;
};
// The patch of an order alone: {"audio_input_priority": [...]}.
MicSave priority_save(std::vector<DeviceKey> priority);

// choose_preferred: kSystemDefault clears the order; any other token moves its entry to the
// front of the saved order (nullopt for an unreadable token: nothing is saved). The legacy
// audio_input_device index is kept on the preferred microphone when it is on the default host
// API (an older version reads it after a downgrade), and removed otherwise.
std::optional<MicSave> choose_preferred(std::string_view token, std::span<const DeviceKey> priority,
                                        const Devices& devices);
// _move: swaps backups; the preferred entry (index 0) never moves. nullopt when out of range.
std::optional<std::vector<DeviceKey>> move(std::span<const DeviceKey> priority, std::size_t index,
                                           int offset);
// _remove: drops one backup; nullopt for index 0 or out of range.
std::optional<std::vector<DeviceKey>> remove(std::span<const DeviceKey> priority,
                                             std::size_t index);
// _on_add_chosen: appends the entry a token names, unless it is unreadable, already ranked or
// no preferred microphone is set (nullopt).
std::optional<std::vector<DeviceKey>> add(std::span<const DeviceKey> priority,
                                          std::string_view token);

} // namespace fv::appcore::mic
