#include "settings/resolve.h"

#include <stdexcept>
#include <string>

#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv {

namespace {

// SETTING_DEFAULTS[key], whose KeyError message is the key's repr.
const Json& table_default(std::string_view key) {
    const Json& table = keys::defaults_table();
    const auto found = table.find(key);
    if (found == table.end()) {
        throw std::out_of_range(py::repr(key));
    }
    return *found;
}

} // namespace

Json setting_value(std::string_view key, const Json& snapshot) {
    // Python evaluates the default first, so a key outside the table fails even when saved.
    const Json& fallback = table_default(key);
    if (snapshot.is_object()) {
        const auto found = snapshot.find(key);
        if (found != snapshot.end()) {
            return *found;
        }
    }
    return fallback;
}

bool resolve_bool_setting(std::string_view key, const Json& snapshot) {
    const SettingBase* setting = keys::by_key(key);
    if (setting == nullptr) {
        throw std::out_of_range(py::repr(key));
    }
    if (setting->kind() != SettingKind::Bool) {
        throw std::invalid_argument(std::string(key) + " is not an on/off setting");
    }
    return setting->read_json(snapshot).get<bool>();
}

Json resolve_choice_setting(std::string_view key, std::span<const Json> choices,
                            const Json& snapshot) {
    Json raw = setting_value(key, snapshot);
    for (const Json& choice : choices) {
        if (py::py_equal(raw, choice)) {
            return raw;
        }
    }
    return table_default(key);
}

} // namespace fv
