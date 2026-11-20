#pragma once

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/process_env.h"
#include "pyport/text.h"

namespace fv {

// An os.environ that lives in memory: Windows' rules (names stored upper-cased and matched
// without regard to case, putenv's refusals) or POSIX's. Variables keep the order they were
// first set in, as a dict does.
// Affinity: thread-safe.
class MapEnv final : public MutableEnv {
public:
    explicit MapEnv(bool windows = ProcessEnv::kWindows,
                    const std::vector<std::pair<std::string, std::string>>& initial = {})
        : windows_(windows) {
        for (const auto& [name, value] : initial) {
            set(name, value);
        }
    }

    std::optional<std::string> get(std::string_view name) const override {
        std::lock_guard lock(mutex_);
        const auto it = find(key(name));
        if (it == items_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void set(std::string_view name, std::string_view value) override {
        check_env_assignment(name, value, windows_);
        std::lock_guard lock(mutex_);
        auto stored = key(name);
        const auto it = find(stored);
        if (it == items_.end()) {
            items_.emplace_back(std::move(stored), std::string(value));
        } else {
            it->second = std::string(value);
        }
    }

    void unset(std::string_view name) override {
        std::lock_guard lock(mutex_);
        const auto it = find(key(name));
        if (it != items_.end()) {
            items_.erase(it);
        }
    }

    std::vector<std::pair<std::string, std::string>> items() const override {
        std::lock_guard lock(mutex_);
        return items_;
    }

private:
    std::string key(std::string_view name) const {
        return windows_ ? py::upper(name) : std::string(name);
    }
    std::vector<std::pair<std::string, std::string>>::iterator find(const std::string& stored) {
        return std::find_if(items_.begin(), items_.end(),
                            [&](const auto& item) { return item.first == stored; });
    }
    std::vector<std::pair<std::string, std::string>>::const_iterator
    find(const std::string& stored) const {
        return std::find_if(items_.begin(), items_.end(),
                            [&](const auto& item) { return item.first == stored; });
    }

    bool windows_;
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> items_;
};

} // namespace fv
