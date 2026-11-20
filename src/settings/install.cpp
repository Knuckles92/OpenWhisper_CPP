#include "settings/install.h"

#include <exception>
#include <string>

#include "base/log.h"
#include "settings/keys.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.settings"};

} // namespace

bool seed_new_install_settings(SettingsStore& store) {
    if (!store.is_new_install()) {
        return false;
    }
    bool seeded = false;
    try {
        store.mutate([&](Json& settings) {
            const std::string key(keys::app_style_tones.key());
            if (settings.contains(key)) {
                seeded = false;
                return;
            }
            Json tones = Json::object();
            for (const auto& [category, tone] : kNewInstallAppStyleTones) {
                tones[std::string(category)] = std::string(tone);
            }
            settings[key] = std::move(tones);
            seeded = true;
        });
        return seeded;
    } catch (const std::exception& error) {
        kLog.warning("Could not seed new-install settings: {}", error.what());
        return false;
    }
}

bool keep_unchosen_engine(SettingsStore& store) {
    if (!store.is_new_install()) {
        return false;
    }
    const std::string key(keys::selected_model.key());
    if (store.snapshot()->contains(key)) {
        return false;
    }
    const std::string engine = keys::selected_model.default_value();
    bool kept = false;
    try {
        store.mutate([&](Json& settings) {
            if (settings.contains(key)) {
                kept = false;
                return;
            }
            settings[key] = engine;
            kept = true;
        });
        return kept;
    } catch (const std::exception& error) {
        kLog.warning("Could not save the install's engine: {}", error.what());
        return false;
    }
}

} // namespace fv
