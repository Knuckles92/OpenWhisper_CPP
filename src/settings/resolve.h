#pragma once

#include <span>
#include <string_view>

#include "settings/setting.h"

// The generic resolvers of services/settings.py:1470-1508, for code that holds a key name.
// Resolvers of particular settings live with their consumers (architecture 3.3).
namespace fv {

// The stored value of `key`, unvalidated, or its table default when unset. Throws
// std::out_of_range for a key not in keys::defaults_table(), set or not (Python's KeyError).
Json setting_value(std::string_view key, const Json& snapshot);

// `key`'s stored bool, or its default when unset or not a bool. Throws std::out_of_range for an
// undeclared key and std::invalid_argument "<key> is not an on/off setting" for another kind.
bool resolve_bool_setting(std::string_view key, const Json& snapshot);

// `key`'s stored value when it is one of `choices` (Python ==), else its table default.
Json resolve_choice_setting(std::string_view key, std::span<const Json> choices,
                            const Json& snapshot);

} // namespace fv
