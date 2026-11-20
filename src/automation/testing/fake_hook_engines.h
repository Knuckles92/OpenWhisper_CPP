#pragma once

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/programs.h"
#include "automation/secrets.h"
#include "automation/webhooks.h"

// Scripted stand-ins for the webhook and program engines, for tests of what Settings > Hooks
// shows: records come from a script, tests answer at once (or when the test releases them), and
// every call is counted.
namespace fv::automation {

// Affinity: thread-safe.
class FakeWebhookControl final : public WebhookControl {
public:
    // The records deliveries(id) gives, newest first.
    void set_deliveries(const std::string& hook_id, std::vector<DeliveryRecord> records) {
        std::lock_guard lock(mutex_);
        deliveries_[hook_id] = std::move(records);
    }
    // The answer send_test gives; held answers wait for release_tests().
    void set_test_answer(DeliveryRecord record, bool hold = false) {
        std::lock_guard lock(mutex_);
        test_answer_ = std::move(record);
        hold_ = hold;
    }
    // Answers the held tests, in the order they were asked.
    void release_tests() {
        std::deque<std::function<void(DeliveryRecord)>> waiting;
        DeliveryRecord answer;
        {
            std::lock_guard lock(mutex_);
            waiting.swap(held_);
            answer = test_answer_;
        }
        for (auto& done : waiting) {
            if (done) {
                done(answer);
            }
        }
    }
    // ensure_secret and rotate_secret throw SecretUnavailable with `message` while set.
    void fail_secrets(std::optional<std::string> message) {
        std::lock_guard lock(mutex_);
        secret_failure_ = std::move(message);
    }

    std::vector<std::string> tests_sent() const {
        std::lock_guard lock(mutex_);
        return tests_sent_;
    }
    std::vector<std::string> secrets_deleted() const {
        std::lock_guard lock(mutex_);
        return deleted_;
    }
    int reloads() const {
        std::lock_guard lock(mutex_);
        return reloads_;
    }

    std::vector<DeliveryRecord> deliveries(std::string_view hook_id) const override {
        std::lock_guard lock(mutex_);
        const auto found = deliveries_.find(std::string(hook_id));
        return found == deliveries_.end() ? std::vector<DeliveryRecord>{} : found->second;
    }
    void send_test(std::string hook_id, std::function<void(DeliveryRecord)> done) override {
        DeliveryRecord answer;
        {
            std::lock_guard lock(mutex_);
            tests_sent_.push_back(hook_id);
            if (hold_) {
                held_.push_back(std::move(done));
                return;
            }
            answer = test_answer_;
        }
        if (done) {
            done(std::move(answer));
        }
    }
    std::string ensure_secret(std::string_view hook_id) override {
        std::lock_guard lock(mutex_);
        if (secret_failure_) {
            throw SecretUnavailable(*secret_failure_);
        }
        auto& secret = secrets_[std::string(hook_id)];
        if (secret.empty()) {
            secret = "whsec_" + std::string(hook_id) + std::to_string(++made_);
        }
        return secret;
    }
    std::string rotate_secret(std::string_view hook_id) override {
        std::lock_guard lock(mutex_);
        if (secret_failure_) {
            throw SecretUnavailable(*secret_failure_);
        }
        auto& secret = secrets_[std::string(hook_id)];
        secret = "whsec_" + std::string(hook_id) + std::to_string(++made_);
        return secret;
    }
    void delete_secret(std::string_view hook_id) override {
        std::lock_guard lock(mutex_);
        secrets_.erase(std::string(hook_id));
        deleted_.emplace_back(hook_id);
    }
    void reload() override {
        std::lock_guard lock(mutex_);
        ++reloads_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<DeliveryRecord>> deliveries_;
    DeliveryRecord test_answer_{"evt_test",
                                std::string(webhooks::TEST_EVENT_TYPE),
                                "2026-01-01T00:00:00.000Z",
                                true,
                                200,
                                120,
                                "",
                                1};
    bool hold_ = false;
    std::deque<std::function<void(DeliveryRecord)>> held_;
    std::optional<std::string> secret_failure_;
    std::map<std::string, std::string> secrets_;
    int made_ = 0;
    std::vector<std::string> tests_sent_;
    std::vector<std::string> deleted_;
    int reloads_ = 0;
};

// Affinity: thread-safe.
class FakeProgramControl final : public ProgramControl {
public:
    void set_runs(const std::string& program_id, std::vector<RunRecord> records) {
        std::lock_guard lock(mutex_);
        runs_[program_id] = std::move(records);
    }
    void set_test_answer(RunRecord record, bool hold = false) {
        std::lock_guard lock(mutex_);
        test_answer_ = std::move(record);
        hold_ = hold;
    }
    void release_tests() {
        std::deque<std::function<void(RunRecord)>> waiting;
        RunRecord answer;
        {
            std::lock_guard lock(mutex_);
            waiting.swap(held_);
            answer = test_answer_;
        }
        for (auto& done : waiting) {
            if (done) {
                done(answer);
            }
        }
    }
    std::vector<std::string> tests_run() const {
        std::lock_guard lock(mutex_);
        return tests_run_;
    }
    int reloads() const {
        std::lock_guard lock(mutex_);
        return reloads_;
    }

    std::vector<RunRecord> runs(std::string_view program_id) const override {
        std::lock_guard lock(mutex_);
        const auto found = runs_.find(std::string(program_id));
        return found == runs_.end() ? std::vector<RunRecord>{} : found->second;
    }
    void run_test(std::string program_id, std::function<void(RunRecord)> done) override {
        RunRecord answer;
        {
            std::lock_guard lock(mutex_);
            tests_run_.push_back(program_id);
            if (hold_) {
                held_.push_back(std::move(done));
                return;
            }
            answer = test_answer_;
        }
        if (done) {
            done(std::move(answer));
        }
    }
    void reload() override {
        std::lock_guard lock(mutex_);
        ++reloads_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<RunRecord>> runs_;
    RunRecord test_answer_{"evt_test", "2026-01-01T00:00:00.000Z", true, 0, 80, ""};
    bool hold_ = false;
    std::deque<std::function<void(RunRecord)>> held_;
    std::vector<std::string> tests_run_;
    int reloads_ = 0;
};

} // namespace fv::automation
