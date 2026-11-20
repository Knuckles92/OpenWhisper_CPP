#include "settings/keys.h"

#include <string>
#include <unordered_map>

namespace fv::keys {

const SettingBase* by_key(std::string_view key) {
    static const auto table = [] {
        std::unordered_map<std::string_view, const SettingBase*> map;
        for (const SettingBase* setting : all()) {
            map.emplace(setting->key(), setting);
        }
        return map;
    }();
    const auto found = table.find(key);
    return found == table.end() ? nullptr : found->second;
}

const Json& defaults_table() {
    static const Json table = [] {
        Json map = Json::object();
        for (const SettingBase* setting : all()) {
            if (setting->in_table()) {
                map[std::string(setting->key())] = setting->default_json();
            }
        }
        return map;
    }();
    return table;
}

} // namespace fv::keys
