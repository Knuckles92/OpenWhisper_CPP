// InstalledAgentCore and its cadence (tests/test_installed_agent_core.py), over agents::FakeDriver
// where Python patches make_driver: the driver plays each pass by calling the pass's tools.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agents/drivers.h"
#include "agents/installed_agents.h"
#include "agents/testing/fake_driver.h"
#include "agents/testing/fake_installed_agents.h"
#include "base/testing/fake_clock.h"
#include "meeting_agent/installed_core.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_specs.h"
#include "net/reactor.h"
#include "scheduler_support.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::agents::FakeDriver;
using fv::agents::InstalledAgent;
using fv::agents::PassOutcome;
using namespace fv::meeting_agent;
using namespace std::chrono_literals;
using fv::meeting_agent::testing::FakeToolHost;

namespace {

// A driver the core owns that forwards to a FakeDriver the test keeps.
class Shared final : public fv::agents::Driver {
public:
    explicit Shared(std::shared_ptr<fv::agents::Driver> fake) : fake_(std::move(fake)) {}
    std::string_view name() const override { return fake_->name(); }
    void start(fv::agents::LoopbackMcpServer& server, std::map<std::string, std::string> personas,
               const Json& tools) override {
        fake_->start(server, std::move(personas), tools);
    }
    bool healthy() const override { return fake_->healthy(); }
    void close() override { fake_->close(); }
    PassOutcome run_pass(const fv::agents::PassRequest& request) override {
        return fake_->run_pass(request);
    }
    std::string tool_prefix() const override { return fake_->tool_prefix(); }

private:
    std::shared_ptr<fv::agents::Driver> fake_;
};

FakeDriver::Options driver_options() {
    FakeDriver::Options options;
    options.tool_prefix = "mcp__fiorivoce__";
    options.calls = {
        {"patch_state", Json{{"ops", Json::array({Json{{"op", "set_topic"},
                                                       {"text", "Beta"},
                                                       {"evidence", Json::array({"sg_1"})}}})}}}};
    return options;
}

// The `agent` fixture: Claude Code 2.1.281, signed in to Claude Team, as installed_agents()
// finds it for the length of a test; a reactor for the tool server.
struct Rig {
    fv::testkit::TempDataRoot temp;
    std::shared_ptr<fv::agents::FakeInstalledAgents> agents =
        std::make_shared<fv::agents::FakeInstalledAgents>(fv::agents::ScanResult{}, temp.path(),
                                                          temp.path());
    fv::net::Reactor reactor{1};
    Json settings = Json::object();
    fv::FakeClock clock;

    Rig() {
        InstalledAgent found{"claude_code", "claude", "2.1.281"};
        found.account = "Claude Team";
        found.signed_in = true;
        agents->set_agent("claude_code", found);
        fv::agents::set_installed_agents(agents);
    }
    ~Rig() { fv::agents::set_installed_agents(nullptr); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // _core: a core whose driver is `driver`, initialized for meeting m1 with model haiku.
    std::unique_ptr<InstalledAgentCore> core(std::shared_ptr<fv::agents::Driver> driver,
                                             std::shared_ptr<FakeToolHost> tools) {
        auto made = std::make_unique<InstalledAgentCore>(
            "claude_code", [this] { return settings; }, reactor,
            std::make_shared<fv::agents::RefusingLauncher>(),
            [driver](const InstalledAgent&) { return std::make_unique<Shared>(driver); }, clock);
        AgentConfig config;
        config.meeting_id = "m1";
        config.provider = "claude_code";
        config.model = "haiku";
        config.system_prompt = "COPILOT CHARTER";
        made->initialize(config, std::move(tools));
        return made;
    }
};

// _payload: one segment, an empty dashboard.
CheckpointPayload payload(bool notes = false, bool consolidation = false) {
    CheckpointPayload out;
    out.request_id = "r1";
    out.state_snapshot = Json{{"cards", Json::object()}};
    out.new_segments = {Json{
        {"id", "sg_1"}, {"start_s", 1.0}, {"text", "Let's plan the beta."}, {"channel", "mic"}}};
    out.is_notes = notes;
    out.is_consolidation = consolidation;
    return out;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// A driver whose passes `play` the request's events, as a streaming agent reports them.
class StreamingDriver final : public fv::agents::Driver {
public:
    explicit StreamingDriver(std::function<void(const fv::agents::PassRequest&)> play)
        : play_(std::move(play)) {}
    std::string_view name() const override { return "Streaming"; }
    void start(fv::agents::LoopbackMcpServer&, std::map<std::string, std::string>,
               const Json&) override {}
    bool healthy() const override { return true; }
    void close() override {}
    PassOutcome run_pass(const fv::agents::PassRequest& request) override {
        play_(request);
        PassOutcome outcome;
        outcome.ok = true;
        return outcome;
    }

private:
    std::function<void(const fv::agents::PassRequest&)> play_;
};

} // namespace

// from tests/test_installed_agent_core.py::test_a_pass_applies_the_agents_tool_calls
TEST_CASE("test_a_pass_applies_the_agents_tool_calls", "[test_installed_agent_core]") {
    Rig rig;
    auto driver = std::make_shared<FakeDriver>(driver_options());
    auto tools = std::make_shared<FakeToolHost>();
    auto core = rig.core(driver, tools);
    std::vector<AgentActivity> activity;
    core->set_activity_callback([&](const AgentActivity& tick) { activity.push_back(tick); });
    const auto result = core->checkpoint(payload());
    CHECK(result.ok);
    REQUIRE(result.op_results.size() == 1);
    CHECK(result.op_results[0].ok);
    CHECK(tools->ops() ==
          std::vector<Json>{Json{{"op", "set_topic"}, {"text", "Beta"}, {"evidence", {"sg_1"}}}});
    REQUIRE(driver->requests().size() == 1);
    const auto request = driver->requests()[0];
    CHECK(request.persona == "build");
    CHECK(request.model == "haiku");
    CHECK(request.effort == "low");
    CHECK(request.system_prompt.starts_with("COPILOT CHARTER"));
    CHECK(contains(request.system_prompt, "mcp__fiorivoce__patch_state"));
    std::set<std::string> kinds;
    for (const auto& tick : activity) {
        kinds.insert(tick.kind);
    }
    CHECK(kinds.contains("start"));
    CHECK(kinds.contains("tool"));
    CHECK(kinds.contains("settled"));
    CHECK(result.usage == Json{{"total_tokens", 10}});
    // The driver was started with the meeting tools (an empty list would leave it tool-free).
    CHECK(driver->start_tools() == mcp_tool_definitions());
    core->shutdown();
    CHECK(driver->closed());
    CHECK_FALSE(core->is_healthy());
}

// from tests/test_installed_agent_core.py::test_a_notes_pass_is_the_note_taker_and_keeps_to_notes
TEST_CASE("test_a_notes_pass_is_the_note_taker_and_keeps_to_notes", "[test_installed_agent_core]") {
    Rig rig;
    auto driver = std::make_shared<FakeDriver>(driver_options());
    auto tools = std::make_shared<FakeToolHost>();
    auto core = rig.core(driver, tools);
    const auto result = core->checkpoint(payload(true));
    REQUIRE(driver->requests().size() == 1);
    const auto request = driver->requests()[0];
    CHECK(request.persona == "plan");
    const auto personas = driver->personas();
    REQUIRE(personas.has_value());
    CHECK(request.system_prompt == personas->at("plan"));
    CHECK(personas->at("plan") != personas->at("build"));
    // set_topic is outside a notes pass's job: refused by the shared policy.
    REQUIRE(result.op_results.size() == 1);
    CHECK(result.op_results[0].reason == std::optional<std::string>("notes_only"));
    CHECK(tools->ops().empty());
}

// from
// tests/test_installed_agent_core.py::test_streamed_progress_reaches_the_card_only_when_it_changes
TEST_CASE("test_streamed_progress_reaches_the_card_only_when_it_changes",
          "[test_installed_agent_core]") {
    Rig rig;
    bool heartbeat = false;
    auto driver = std::make_shared<StreamingDriver>([&](const fv::agents::PassRequest& request) {
        for (int i = 0; i < 50; ++i) { // a streaming agent reports every token
            request.emit("writing");
        }
        if (heartbeat) {
            // The same line again once the heartbeat interval has passed.
            rig.clock.advance(1s);
            request.emit("writing");
        }
        request.emit("settled");
    });
    auto core = rig.core(driver, std::make_shared<FakeToolHost>());
    std::vector<std::string> lines;
    core->set_progress_callback([&](const std::string& line) { lines.push_back(line); });
    CHECK(core->consolidate(payload(false, true)).ok);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] != lines[1]);

    heartbeat = true;
    lines.clear();
    core->consolidate(payload(false, true));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == lines[1]);
}

// from tests/test_installed_agent_core.py::test_the_final_pass_thinks_harder_and_waits_longer
TEST_CASE("test_the_final_pass_thinks_harder_and_waits_longer", "[test_installed_agent_core]") {
    Rig rig;
    auto driver = std::make_shared<FakeDriver>(driver_options());
    auto core = rig.core(driver, std::make_shared<FakeToolHost>());
    core->consolidate(payload(false, true));
    REQUIRE(driver->requests().size() == 1);
    const auto request = driver->requests()[0];
    CHECK(request.effort == "medium");
    CHECK(request.timeout_s >= 600);
    CHECK(request.timeout_s == 900.0);
    CHECK(request.stall_s == 300.0);
}

TEST_CASE("live, polish and consolidation passes get their own budgets", "[installed_core]") {
    Rig rig;
    auto driver = std::make_shared<FakeDriver>(driver_options());
    auto core = rig.core(driver, std::make_shared<FakeToolHost>());
    core->checkpoint(payload());
    auto polish = payload();
    polish.is_polish = true;
    core->checkpoint(polish);
    // A consolidation payload handed to checkpoint() runs as the final pass.
    core->checkpoint(payload(false, true));
    const auto requests = driver->requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].timeout_s == 120.0);
    CHECK(requests[0].stall_s == 75.0);
    CHECK(requests[1].timeout_s == 180.0);
    CHECK(requests[1].stall_s == 75.0);
    CHECK(requests[2].timeout_s == 900.0);
    CHECK(requests[2].effort == "medium");
}

// from tests/test_installed_agent_core.py::test_saved_thinking_levels_run_at_what_the_agent_takes
TEST_CASE("test_saved_thinking_levels_run_at_what_the_agent_takes", "[test_installed_agent_core]") {
    Rig rig;
    rig.settings = Json{{"meeting_live_thinking", "off"}, {"meeting_final_thinking", "ultra"}};
    auto driver = std::make_shared<FakeDriver>(driver_options());
    auto core = rig.core(driver, std::make_shared<FakeToolHost>());
    core->checkpoint(payload());
    core->consolidate(payload(false, true));
    // Claude Code has no Off and goes up to Max.
    const auto requests = driver->requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].effort == "low");
    CHECK(requests[1].effort == "max");
}

// from tests/test_installed_agent_core.py::test_tool_calls_after_the_pass_have_no_authority
TEST_CASE("test_tool_calls_after_the_pass_have_no_authority", "[test_installed_agent_core]") {
    Rig rig;
    auto options = driver_options();
    options.calls.clear();
    auto driver = std::make_shared<FakeDriver>(options);
    auto tools = std::make_shared<FakeToolHost>();
    auto core = rig.core(driver, tools);
    core->checkpoint(payload());
    REQUIRE(driver->requests().size() == 1);
    const auto late = driver->requests()[0].handler(
        "patch_state",
        Json{{"ops",
              Json::array({Json{
                  {"op", "set_topic"}, {"text", "x"}, {"evidence", Json::array({"sg_1"})}}})}},
        Json::object());
    CHECK(late.second);
    CHECK(late.first == "This meeting pass was canceled.");
    CHECK(tools->ops().empty());
}

TEST_CASE("a cancel ends a pass's authority and keeps what it applied", "[installed_core]") {
    Rig rig;
    // The agent applies one op, the pass is cancelled, and its next call is refused.
    class CancellingDriver final : public fv::agents::Driver {
    public:
        explicit CancellingDriver(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
        std::string_view name() const override { return "Cancelling"; }
        void start(fv::agents::LoopbackMcpServer&, std::map<std::string, std::string>,
                   const Json&) override {}
        bool healthy() const override { return true; }
        void close() override {}
        PassOutcome run_pass(const fv::agents::PassRequest& request) override {
            const Json ops{{"ops", Json::array({Json{{"op", "set_topic"}, {"text", "Beta"}}})}};
            request.handler("patch_state", ops, Json::object());
            cancel_();
            second = request.handler("patch_state", ops, Json::object());
            PassOutcome outcome;
            outcome.canceled = true;
            return outcome;
        }
        std::pair<std::string, bool> second;

    private:
        std::function<void()> cancel_;
    };
    InstalledAgentCore* core_ptr = nullptr;
    auto driver = std::make_shared<CancellingDriver>([&] { core_ptr->cancel(); });
    auto tools = std::make_shared<FakeToolHost>();
    auto core = rig.core(driver, tools);
    core_ptr = core.get();
    const auto result = core->checkpoint(payload());
    CHECK_FALSE(result.ok);
    CHECK(result.error == "canceled");
    CHECK(result.op_results.size() == 1);
    CHECK(tools->ops().size() == 1);
    CHECK(driver->second == std::pair<std::string, bool>{"This meeting pass was canceled.", true});
}

// from tests/test_installed_agent_core.py::test_a_failed_run_reports_its_reason
TEST_CASE("test_a_failed_run_reports_its_reason", "[test_installed_agent_core]") {
    Rig rig;
    auto options = driver_options();
    options.calls.clear();
    options.outcome = PassOutcome{.ok = false, .error = "Codex: model not supported"};
    auto driver = std::make_shared<FakeDriver>(options);
    auto core = rig.core(driver, std::make_shared<FakeToolHost>());
    const auto result = core->checkpoint(payload());
    CHECK_FALSE(result.ok);
    CHECK(result.error == "Codex: model not supported");
}

TEST_CASE("an unusable agent says why at initialize", "[installed_core]") {
    Rig rig;
    rig.agents->set_agent("claude_code", std::nullopt);
    InstalledAgentCore core(
        "claude_code", [] { return Json::object(); }, rig.reactor,
        std::make_shared<fv::agents::RefusingLauncher>(),
        [](const InstalledAgent&) { return std::make_unique<FakeDriver>(); });
    AgentConfig config;
    try {
        core.initialize(config, std::make_shared<FakeToolHost>());
        FAIL("initialize did not throw");
    } catch (const fv::agents::AgentUnavailable& error) {
        CHECK(contains(error.what(), "not installed on this computer"));
    }
    CHECK_FALSE(core.is_healthy());
    const auto result = core.checkpoint(payload());
    CHECK_FALSE(result.ok);
    CHECK(result.error == "agent_unavailable");
}

TEST_CASE("a driver that throws anything fails the pass and ends its authority",
          "[installed_core]") {
    Rig rig;
    class ThrowingDriver final : public fv::agents::Driver {
    public:
        std::string_view name() const override { return "Throwing"; }
        void start(fv::agents::LoopbackMcpServer&, std::map<std::string, std::string>,
                   const Json&) override {}
        bool healthy() const override { return true; }
        void close() override {}
        PassOutcome run_pass(const fv::agents::PassRequest& request) override {
            handler = request.handler;
            throw 42; // not a std::exception
        }
        fv::agents::ToolHandler handler;
    };
    auto driver = std::make_shared<ThrowingDriver>();
    auto tools = std::make_shared<FakeToolHost>();
    auto core = rig.core(driver, tools);
    const auto result = core->checkpoint(payload());
    CHECK_FALSE(result.ok);
    CHECK(result.error == "unknown error");
    REQUIRE(driver->handler);
    const auto late = driver->handler(
        "patch_state", Json{{"ops", Json::array({Json{{"op", "set_topic"}, {"text", "x"}}})}},
        Json::object());
    CHECK(late == std::pair<std::string, bool>{"This meeting pass was canceled.", true});
    CHECK(tools->ops().empty());
    CHECK(core->is_healthy());
}

TEST_CASE("initializing again closes the replaced driver while its server still runs",
          "[installed_core]") {
    Rig rig;
    // A driver keeps a raw pointer to its tool server until its first close().
    class ServerWatchingDriver final : public fv::agents::Driver {
    public:
        std::string_view name() const override { return "Watching"; }
        void start(fv::agents::LoopbackMcpServer& server, std::map<std::string, std::string>,
                   const Json&) override {
            server_ = &server;
        }
        bool healthy() const override { return !closed; }
        void close() override {
            if (!closed && server_ != nullptr) {
                server_running_at_close = server_->running();
            }
            closed = true;
        }
        PassOutcome run_pass(const fv::agents::PassRequest&) override {
            PassOutcome outcome;
            outcome.ok = true;
            return outcome;
        }
        bool closed = false;
        std::optional<bool> server_running_at_close;

    private:
        fv::agents::LoopbackMcpServer* server_ = nullptr;
    };
    std::vector<std::shared_ptr<ServerWatchingDriver>> made;
    InstalledAgentCore core(
        "claude_code", [] { return Json::object(); }, rig.reactor,
        std::make_shared<fv::agents::RefusingLauncher>(),
        [&made](const InstalledAgent&) {
            made.push_back(std::make_shared<ServerWatchingDriver>());
            return std::make_unique<Shared>(made.back());
        },
        rig.clock);
    AgentConfig config;
    config.model = "haiku";
    core.initialize(config, std::make_shared<FakeToolHost>());
    core.initialize(config, std::make_shared<FakeToolHost>());
    REQUIRE(made.size() == 2);
    CHECK(made[0]->closed);
    CHECK(made[0]->server_running_at_close == std::optional<bool>(true));
    CHECK_FALSE(made[1]->closed);
    CHECK(core.is_healthy());
    CHECK(core.checkpoint(payload()).ok);
    core.shutdown();
    CHECK(made[1]->server_running_at_close == std::optional<bool>(true));
}

// ---- cadence ----

// from tests/test_installed_agent_core.py::test_installed_agents_get_their_own_cadence
TEST_CASE("test_installed_agents_get_their_own_cadence", "[test_installed_agent_core]") {
    using P = SchedulerProbe;
    fv::FakeClock clock;
    fv::net::Reactor reactor{1};
    testing::FakeSchedulerHost host;
    auto codex = std::make_shared<InstalledAgentCore>(
        "codex", [] { return Json::object(); }, reactor,
        std::make_shared<fv::agents::RefusingLauncher>());
    CheckpointScheduler slow(host, codex, clock);
    CHECK(P::base_interval_s(slow) == INSTALLED_AGENT_CADENCE.base_s);
    CHECK(P::pace_notes_min(slow) == INSTALLED_AGENT_CADENCE.notes_min_s);
    CHECK(INSTALLED_AGENT_CADENCE == CheckpointCadence{45.0, 30.0, 90.0, 4, 180.0, 2, 120.0});
    CheckpointScheduler fast(host, std::make_shared<testing::FakeAgentCore>(), clock);
    CHECK(P::base_interval_s(fast) == 15.0);
    CHECK(P::pace_notes_min(fast) == 45.0);
    SchedulerOptions explicit_options;
    explicit_options.base_interval_s = 60.0;
    CheckpointScheduler pinned(host, codex, clock, {}, {}, explicit_options);
    CHECK(P::base_interval_s(pinned) == 60.0);
}

// from tests/test_installed_agent_core.py::test_a_mock_core_is_not_mistaken_for_a_cadence
TEST_CASE("test_a_mock_core_is_not_mistaken_for_a_cadence", "[test_installed_agent_core]") {
    // Python's MagicMock answers every attribute; a core here has a cadence only when it says
    // so, and FakeAgentCore says none unless told.
    using P = SchedulerProbe;
    fv::FakeClock clock;
    testing::FakeSchedulerHost host;
    CheckpointScheduler scheduler(host, std::make_shared<testing::FakeAgentCore>(), clock);
    CHECK_FALSE(P::cadence(scheduler).has_value());
    CHECK(P::base_interval_s(scheduler) == 15.0);
}
