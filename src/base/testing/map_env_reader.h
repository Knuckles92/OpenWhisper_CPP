#pragma once

#include <filesystem>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "base/env.h"

namespace fv {

// An environment held in a map, like the dict Python's tests pass. Names match exactly (case
// counts), and a name set to "" is set.
// Affinity: thread-safe.
class MapEnvReader final : public EnvReader {
public:
    MapEnvReader() = default;
    MapEnvReader(std::initializer_list<std::pair<const std::string, std::string>> values)
        : values_(values) {}

    std::optional<std::string> get(std::string_view name) const override {
        std::lock_guard lock(mutex_);
        const auto it = values_.find(std::string(name));
        if (it == values_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void set(std::string name, std::string value) {
        std::lock_guard lock(mutex_);
        values_[std::move(name)] = std::move(value);
    }

    void unset(const std::string& name) {
        std::lock_guard lock(mutex_);
        values_.erase(name);
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;
};

// Points the four variables a Paths finds the per-user folder and the home folder by
// (LOCALAPPDATA, XDG_DATA_HOME, HOME, USERPROFILE) at `folder`, so that local_app_dir(),
// components_root(), speech_models_root() and openwhisper_roots() of an environment a test pins
// stay in the test's own folder. Pinned without them, "~" is left as written (Paths never reads
// the password database for a pinned environment): a folder named "~" in the working directory.
inline void pin_per_user_folders(MapEnvReader& env, const std::filesystem::path& folder) {
    // Not base/utf8.h's path_to_utf8: a test that also includes engine/platform.h would then
    // find two fv::path_to_utf8 (base's sits in an inline namespace). u8string is UTF-8 too.
    const std::u8string utf8 = folder.u8string();
    const std::string text(utf8.begin(), utf8.end());
    for (const char* name : {"LOCALAPPDATA", "XDG_DATA_HOME", "HOME", "USERPROFILE"}) {
        env.set(name, text);
    }
}

} // namespace fv
