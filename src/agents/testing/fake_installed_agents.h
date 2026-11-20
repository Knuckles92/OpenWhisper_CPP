#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/installed_agents.h"
#include "process/launcher.h"

namespace fv::agents {

// A launcher that starts nothing, so a fake can never reach a real agent.
// Affinity: thread-safe.
class RefusingLauncher final : public ProcessLauncher {
public:
    std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec&) override {
        return std::unexpected(std::string("The program wasn't found."));
    }
};

// InstalledAgents over a fixed answer: find_agent gives the agent set for an id (with its
// sign-in already filled in), with_sign_in keeps it, and list_models gives the set list (else
// the default entry, "{Name} default"). Scans, lookups, known_agent and the listeners work as
// the real class's do. Configuration files are read from `home` (empty: nowhere);
// agent_workspace_dir() is under `temp_dir` (empty: the system's temp folder, so a test that
// asks for the workspace passes its own temporary folder).
// Affinity: thread-safe.
class FakeInstalledAgents final : public InstalledAgents {
public:
    explicit FakeInstalledAgents(ScanResult agents = {}, std::filesystem::path home = {},
                                 std::filesystem::path temp_dir = {})
        : InstalledAgents(
              std::make_shared<RefusingLauncher>(),
              [] { return std::map<std::string, std::string>{}; }, std::move(home),
              std::move(temp_dir)),
          agents_(std::move(agents)) {}
    ~FakeInstalledAgents() override { wait_for_lookups(); }

    void set_agent(std::string_view id, std::optional<InstalledAgent> agent) {
        std::lock_guard lock(mutex_);
        agents_[std::string(id)] = std::move(agent);
    }
    void set_models(std::string_view id, std::vector<AgentModel> models) {
        std::lock_guard lock(mutex_);
        models_[std::string(id)] = std::move(models);
    }
    // How many times find_agent ran (a scan runs it once per agent).
    int lookups() const { return lookups_.load(); }

    std::optional<InstalledAgent> find_agent(std::string_view id) override {
        ++lookups_;
        std::lock_guard lock(mutex_);
        const auto it = agents_.find(std::string(id));
        return it == agents_.end() ? std::nullopt : it->second;
    }
    InstalledAgent with_sign_in(const InstalledAgent& agent) override { return agent; }
    std::vector<AgentModel> list_models(const InstalledAgent& agent, double = 20.0) override {
        std::lock_guard lock(mutex_);
        if (const auto it = models_.find(agent.id); it != models_.end()) {
            return it->second;
        }
        return {AgentModel{"", std::string(agent_spec(agent.id).name) + " default"}};
    }

private:
    mutable std::mutex mutex_;
    ScanResult agents_;
    std::map<std::string, std::vector<AgentModel>> models_;
    std::atomic<int> lookups_{0};
};

} // namespace fv::agents
