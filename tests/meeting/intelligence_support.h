#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "meeting_agent/interfaces.h"
#include "meeting_agent/testing/fake_agent_core.h"

// What the tests of the app's intelligence provider (MeetingIntelligence) share: the provider
// owns the core its factory returns, so the factory hands it a forwarder and the test keeps the
// scripted fake behind it (Python's tests keep the core they monkeypatch into the factory).
namespace fv::meeting::test {

class OwnedCore final : public meeting_agent::AgentCore {
public:
    // `on_initialize` runs after the fake recorded its config and tools (it may block or throw);
    // `on_cancel` after the fake's cancel.
    explicit OwnedCore(std::shared_ptr<meeting_agent::testing::FakeAgentCore> inner,
                       std::function<void()> on_initialize = {},
                       std::function<void()> on_cancel = {})
        : inner_(std::move(inner)), on_initialize_(std::move(on_initialize)),
          on_cancel_(std::move(on_cancel)) {}

    void initialize(const meeting_agent::AgentConfig& config,
                    std::shared_ptr<meeting_agent::AgentToolHost> tools) override {
        inner_->initialize(config, std::move(tools));
        if (on_initialize_) {
            on_initialize_();
        }
    }
    meeting_agent::AgentResult
    checkpoint(const meeting_agent::CheckpointPayload& payload) override {
        return inner_->checkpoint(payload);
    }
    meeting_agent::AgentResult
    consolidate(const meeting_agent::CheckpointPayload& payload) override {
        return inner_->consolidate(payload);
    }
    void cancel() override {
        inner_->cancel();
        if (on_cancel_) {
            on_cancel_();
        }
    }
    bool is_healthy() const override { return inner_->is_healthy(); }
    void shutdown() override { inner_->shutdown(); }
    std::optional<meeting_agent::CheckpointCadence> cadence() const override {
        return inner_->cadence();
    }
    void set_progress_callback(std::function<void(const std::string&)> callback) override {
        inner_->set_progress_callback(std::move(callback));
    }
    void set_activity_callback(
        std::function<void(const meeting_agent::AgentActivity&)> callback) override {
        inner_->set_activity_callback(std::move(callback));
    }

private:
    std::shared_ptr<meeting_agent::testing::FakeAgentCore> inner_;
    std::function<void()> on_initialize_;
    std::function<void()> on_cancel_;
};

} // namespace fv::meeting::test
