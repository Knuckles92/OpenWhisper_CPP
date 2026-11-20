#include "remote_host/host_settings.h"

#include "remote/pairing_settings.h"

namespace fv::remote_host {

bool host_enabled(const SettingsStore& settings) {
    return remote::host_enabled(*settings.snapshot());
}

int host_port(const SettingsStore& settings) {
    return remote::host_port(*settings.snapshot());
}

bool host_tailscale_trust(const SettingsStore& settings) {
    return remote::host_tailscale_trust(*settings.snapshot());
}

bool host_model_management(const SettingsStore& settings) {
    return remote::host_model_management(*settings.snapshot());
}

bool host_keeps_records(const SettingsStore& settings) {
    return remote::host_keeps_records(*settings.snapshot());
}

bool host_manages_mcp(const SettingsStore& settings) {
    return remote::host_manages_mcp(*settings.snapshot());
}

std::function<Json()> host_devices_loader(const SettingsStore& settings) {
    return [&settings] {
        return remote::load_host_devices(settings);
    };
}

std::function<void(const Json&)> host_devices_saver(SettingsStore& settings) {
    return [&settings](const Json& devices) {
        remote::save_host_devices(settings, devices);
    };
}

std::unique_ptr<DeviceRegistry> settings_device_registry(SettingsStore& settings, Clock& clock) {
    return std::make_unique<DeviceRegistry>(host_devices_loader(settings),
                                            host_devices_saver(settings), clock);
}

} // namespace fv::remote_host
