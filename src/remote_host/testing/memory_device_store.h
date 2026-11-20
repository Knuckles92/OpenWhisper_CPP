#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>

#include "base/json.h"

// Where a test's DeviceRegistry keeps its devices (tests/fakes/remote.py ListStore): a list in
// memory, copied on every load and save.
namespace fv::remote_host::testing {

// Affinity: thread-safe.
class MemoryDeviceStore {
public:
    explicit MemoryDeviceStore(Json devices = Json::array()) : devices_(std::move(devices)) {}

    Json load() const {
        std::lock_guard lock(mutex_);
        ++loads_;
        return devices_;
    }
    void save(const Json& devices) {
        std::lock_guard lock(mutex_);
        ++saves_;
        devices_ = devices;
    }

    // DeviceRegistry's load and save over this store, which outlives the registry.
    std::function<Json()> loader() {
        return [this] {
            return load();
        };
    }
    std::function<void(const Json&)> saver() {
        return [this](const Json& devices) {
            save(devices);
        };
    }

    // What is stored now, without counting a load.
    Json devices() const {
        std::lock_guard lock(mutex_);
        return devices_;
    }
    // An outside writer (another app instance, a hand edit).
    void set_devices(Json devices) {
        std::lock_guard lock(mutex_);
        devices_ = std::move(devices);
    }
    std::size_t loads() const {
        std::lock_guard lock(mutex_);
        return loads_;
    }
    std::size_t saves() const {
        std::lock_guard lock(mutex_);
        return saves_;
    }

private:
    mutable std::mutex mutex_;
    Json devices_;
    mutable std::size_t loads_ = 0;
    std::size_t saves_ = 0;
};

} // namespace fv::remote_host::testing
