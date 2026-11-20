#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/testing/fake_clock.h"
#include "update/gates.h"

// Named mutexes for tests: which names exist is a set the test (or a scripted `exists`) decides,
// create() takes a free name and fails on a taken one, and wait() moves a FakeClock instead of
// sleeping. Every call is recorded.
// Affinity: one test thread.
namespace fv::update::testing {

class FakeMutexApi final : public MutexApi {
public:
    FakeMutexApi() = default;
    explicit FakeMutexApi(FakeClock& clock) : clock_(&clock) {}

    // Names another process holds: exists() is true and create() fails for them.
    std::set<std::string> held_elsewhere;
    // Replaces the existence check of names this fake did not create (a scripted setup that
    // ends after a few polls).
    std::function<bool(std::string_view)> scripted_exists;

    std::optional<MutexHandle> create(std::string_view name) override {
        created_calls.emplace_back(name);
        const std::string key(name);
        if (exists_elsewhere(key) || owned_.contains(key)) {
            return std::nullopt;
        }
        const MutexHandle handle = next_++;
        owned_[key] = handle;
        return handle;
    }

    bool exists(std::string_view name) override {
        exists_calls.emplace_back(name);
        const std::string key(name);
        return owned_.contains(key) || exists_elsewhere(key);
    }

    void release(MutexHandle handle) override {
        released.push_back(handle);
        for (auto it = owned_.begin(); it != owned_.end(); ++it) {
            if (it->second == handle) {
                owned_.erase(it);
                return;
            }
        }
    }

    void wait(std::chrono::milliseconds delay) override {
        waits.push_back(delay);
        if (clock_ != nullptr) {
            clock_->advance(delay);
        }
    }

    // Names this fake created and has not released.
    std::vector<std::string> owned() const {
        std::vector<std::string> out;
        for (const auto& [name, handle] : owned_) {
            out.push_back(name);
        }
        return out;
    }

    std::vector<std::string> created_calls;
    std::vector<std::string> exists_calls;
    std::vector<MutexHandle> released;
    std::vector<std::chrono::milliseconds> waits;

private:
    bool exists_elsewhere(const std::string& name) const {
        if (held_elsewhere.contains(name)) {
            return true;
        }
        return scripted_exists ? scripted_exists(name) : false;
    }

    FakeClock* clock_ = nullptr;
    std::map<std::string, MutexHandle> owned_;
    MutexHandle next_ = 100;
};

} // namespace fv::update::testing
