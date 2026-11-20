#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/launch.h"

namespace fv {

// The platform calls of process/launch.h, recorded: association lookups answer from a table
// (NO_ASSOCIATION for anything else), Explorer and ShellExecute in this process record what
// they were given and answer true unless told otherwise, terminals are recorded specs, and
// time moves only when told. Install with set_launch_backend.
// Affinity: thread-safe.
class FakeLaunchBackend final : public LaunchBackend {
public:
    using Opener = std::function<bool(std::string_view file, std::string_view args)>;

    void set_assoc(std::string scheme, AssocString what, std::uint32_t result,
                   std::string value = {}) {
        std::lock_guard lock(mutex_);
        assoc_[{std::move(scheme), what}] = AssocAnswer{result, std::move(value)};
    }
    void clear_assoc() {
        std::lock_guard lock(mutex_);
        assoc_.clear();
    }
    // Replaces the lookup altogether (for one that throws, say).
    void set_assoc_function(std::function<AssocAnswer(std::string_view, AssocString)> lookup) {
        std::lock_guard lock(mutex_);
        assoc_function_ = std::move(lookup);
    }
    // Called on the explorer thread instead of recording and answering true.
    void set_explorer(Opener opener) {
        std::lock_guard lock(mutex_);
        explorer_ = std::move(opener);
    }
    void set_start_here(Opener opener) {
        std::lock_guard lock(mutex_);
        start_here_ = std::move(opener);
    }
    void set_console_result(bool started) {
        std::lock_guard lock(mutex_);
        console_result_ = started;
    }
    void set_explorer_wait_s(double seconds) {
        std::lock_guard lock(mutex_);
        explorer_wait_s_ = seconds;
    }
    void advance(std::chrono::steady_clock::duration by) {
        std::lock_guard lock(mutex_);
        now_ += by;
    }

    AssocAnswer assoc_query(std::string_view scheme, AssocString what) override {
        std::function<AssocAnswer(std::string_view, AssocString)> lookup;
        {
            std::lock_guard lock(mutex_);
            lookups_.emplace_back(scheme, what);
            lookup = assoc_function_;
            if (!lookup) {
                const auto it = assoc_.find({std::string(scheme), what});
                return it == assoc_.end() ? AssocAnswer{} : it->second;
            }
        }
        return lookup(scheme, what);
    }

    bool explorer_open(std::string_view file, std::string_view args) override {
        Opener opener;
        {
            std::lock_guard lock(mutex_);
            opener = explorer_;
            if (!opener) {
                explorer_opened_.emplace_back(file, args);
                return true;
            }
        }
        return opener(file, args);
    }

    bool start_here(std::string_view file, std::string_view args) override {
        Opener opener;
        {
            std::lock_guard lock(mutex_);
            opener = start_here_;
            if (!opener) {
                opened_here_.emplace_back(file, args);
                return true;
            }
        }
        return opener(file, args);
    }

    bool start_console(const ProcessSpec& spec) override {
        std::lock_guard lock(mutex_);
        consoles_.push_back(spec);
        return console_result_;
    }

    bool reveal(const std::filesystem::path& path) override {
        std::lock_guard lock(mutex_);
        revealed_.push_back(path);
        return true;
    }

    std::chrono::steady_clock::time_point now() const override {
        std::lock_guard lock(mutex_);
        return now_;
    }

    double explorer_wait_s() const override {
        std::lock_guard lock(mutex_);
        return explorer_wait_s_;
    }

    std::vector<std::pair<std::string, AssocString>> lookups() const {
        std::lock_guard lock(mutex_);
        return lookups_;
    }
    std::vector<std::pair<std::string, std::string>> explorer_opened() const {
        std::lock_guard lock(mutex_);
        return explorer_opened_;
    }
    std::vector<std::pair<std::string, std::string>> opened_here() const {
        std::lock_guard lock(mutex_);
        return opened_here_;
    }
    std::vector<ProcessSpec> consoles() const {
        std::lock_guard lock(mutex_);
        return consoles_;
    }
    std::vector<std::filesystem::path> revealed() const {
        std::lock_guard lock(mutex_);
        return revealed_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, AssocString>, AssocAnswer> assoc_;
    std::function<AssocAnswer(std::string_view, AssocString)> assoc_function_;
    Opener explorer_;
    Opener start_here_;
    bool console_result_ = true;
    double explorer_wait_s_ = EXPLORER_WAIT_S;
    std::chrono::steady_clock::time_point now_{std::chrono::hours(1000)};
    std::vector<std::pair<std::string, AssocString>> lookups_;
    std::vector<std::pair<std::string, std::string>> explorer_opened_;
    std::vector<std::pair<std::string, std::string>> opened_here_;
    std::vector<ProcessSpec> consoles_;
    std::vector<std::filesystem::path> revealed_;
};

} // namespace fv
