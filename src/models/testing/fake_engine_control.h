#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "models/engine_control.h"

// An EngineControl whose answers a test sets and whose commands it records, in place of the
// controller over the speech service.
namespace fv::models {

// Affinity: thread-safe, except on_call, which is set before the fake is shared.
class FakeEngineControl final : public EngineControl {
public:
    // Told every command after it is recorded ("reload_engine", "release_backends_using <id>").
    std::function<void(const std::string&)> on_call;

    bool job_active() const override {
        std::lock_guard lock(mutex_);
        return job_active_;
    }
    bool meeting_active() const override {
        std::lock_guard lock(mutex_);
        return meeting_active_;
    }
    std::string selected_backend() const override {
        std::lock_guard lock(mutex_);
        return selected_backend_;
    }
    std::string loaded_model_key() const override {
        std::lock_guard lock(mutex_);
        return loaded_model_;
    }
    bool with_model_unused(std::string_view model_key, const std::function<void()>& work) override {
        std::lock_guard lock(mutex_);
        if (meeting_active_ || loaded_model_ == model_key) {
            return false;
        }
        work();
        return true;
    }
    std::filesystem::path loaded_runtime_dir() const override {
        std::lock_guard lock(mutex_);
        return loaded_runtime_dir_;
    }
    void reload_engine(std::string_view note = {}) override {
        {
            std::lock_guard lock(mutex_);
            ++reloads_;
            last_note_ = std::string(note);
        }
        record("reload_engine");
    }
    std::string last_reload_note() const {
        std::lock_guard lock(mutex_);
        return last_note_;
    }
    bool engine_settled() const override {
        std::lock_guard lock(mutex_);
        return engine_settled_;
    }
    void set_engine_settled(bool value) {
        std::lock_guard lock(mutex_);
        engine_settled_ = value;
    }
    void release_backends_using(std::string_view component_id) override {
        {
            std::lock_guard lock(mutex_);
            released_.emplace_back(component_id);
        }
        record("release_backends_using " + std::string(component_id));
    }

    void set_job_active(bool value) {
        std::lock_guard lock(mutex_);
        job_active_ = value;
    }
    void set_meeting_active(bool value) {
        std::lock_guard lock(mutex_);
        meeting_active_ = value;
    }
    void set_selected_backend(std::string backend) {
        std::lock_guard lock(mutex_);
        selected_backend_ = std::move(backend);
    }
    void set_loaded_model(std::string model) {
        std::lock_guard lock(mutex_);
        loaded_model_ = std::move(model);
    }
    void set_loaded_runtime_dir(std::filesystem::path dir) {
        std::lock_guard lock(mutex_);
        loaded_runtime_dir_ = std::move(dir);
    }

    int reloads() const {
        std::lock_guard lock(mutex_);
        return reloads_;
    }
    std::vector<std::string> released() const {
        std::lock_guard lock(mutex_);
        return released_;
    }
    // Every command, in order: "reload_engine", "release_backends_using <id>".
    std::vector<std::string> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    void record(std::string call) {
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(call);
        }
        if (on_call) {
            on_call(call);
        }
    }

    mutable std::mutex mutex_;
    bool job_active_ = false;
    bool meeting_active_ = false;
    std::string selected_backend_ = "parakeet";
    std::string loaded_model_;
    std::filesystem::path loaded_runtime_dir_;
    int reloads_ = 0;
    std::string last_note_;
    bool engine_settled_ = true;
    std::vector<std::string> released_;
    std::vector<std::string> calls_;
};

} // namespace fv::models
