#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "settings/setting.h"

namespace fv {

// Every settings key, with Python's spelling (keys::minimize_tray), generated from settings.py
// by tools/gen/settings_keys.py: OpenWhisper's 186 keys in declaration order, then FioriVoce's
// fv_ keys. keys::choices::<key> and keys::int_choices::<key> are a choice setting's values.
namespace keys {
#include "settings/keys.inc" // e.g. inline constexpr BoolSetting minimize_tray{...};

// ALL_SETTINGS: every declaration, in declaration order.
constexpr std::span<const SettingBase* const> all() {
    return declared;
}

// SETTINGS_BY_KEY: the declaration of a bare key, or nullptr when none is declared.
const SettingBase* by_key(std::string_view key);

// SETTING_DEFAULTS: the default of every in_table setting, in declaration order. Read-only;
// Setting::default_value() and default_json() are the fresh copies.
const Json& defaults_table();
} // namespace keys

} // namespace fv
