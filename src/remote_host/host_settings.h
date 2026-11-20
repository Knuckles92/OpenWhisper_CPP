#pragma once

#include <functional>
#include <memory>

#include "base/clock.h"
#include "base/json.h"
#include "remote_host/registry.h"
#include "settings/store.h"

// The host's settings as the host reads them (services/remote_asr/settings.py host readers,
// load_host_devices and save_host_devices). The readers over a settings mapping are
// remote/pairing_settings.h's; these take the store, read one snapshot, and are rechecked by the
// host for every request that needs them.
// Affinity: thread-safe; each call reads or writes the settings file, so never on the GUI thread.
namespace fv::remote_host {

bool host_enabled(const SettingsStore& settings);
int host_port(const SettingsStore& settings);
// On unless turned off: the owner's own Tailscale computers pair without a code.
bool host_tailscale_trust(const SettingsStore& settings);
// Off by default; pairing alone never grants model administration.
bool host_model_management(const SettingsStore& settings);
// Off by default; pairing alone never lets a computer store files here.
bool host_keeps_records(const SettingsStore& settings);
// Off by default; pairing alone never lets a computer run this one's MCP.
bool host_manages_mcp(const SettingsStore& settings);

// remote_host_devices, read on its own (the host checks it for every request a paired computer
// sends, and copying every setting would cost each one), and written as one setting.
std::function<Json()> host_devices_loader(const SettingsStore& settings);
std::function<void(const Json&)> host_devices_saver(SettingsStore& settings);

// The registry of the computers paired with this host, kept in remote_host_devices. The store
// and the clock outlive it.
std::unique_ptr<DeviceRegistry> settings_device_registry(SettingsStore& settings, Clock& clock);

} // namespace fv::remote_host
