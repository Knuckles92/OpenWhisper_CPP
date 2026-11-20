// The ACP client (meeting/agent/installed/acp.py) against a scripted agent on fake stdio: the
// handshake, a session, a request from the agent the client must answer, notifications, error
// replies, and an agent that dies with requests pending.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agents/acp.h"
#include "agents/installed_agents.h"
#include "agents_test_support.h"
#include "base/cancel.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/json.h"
#include "pyport/text.h"

using fv::Json;
using fv::agents::AcpConnection;
using fv::agents::AcpError;
using fv::agents::OPENCODE;
using fv::agents_test::AgentsFixture;
using fv::agents_test::exe;
using fv::agents_test::make_program;

namespace {

// What a FakeAcpAgent was told, readable after the process is gone.
// Affinity: thread-safe.
struct AgentLog {
    mutable std::mutex mutex;
    std::vector<Json> messages;
    bool stdin_closed = false;
    std::atomic<int> kills{0};

    std::vector<Json> received() const {
        std::lock_guard lock(mutex);
        return messages;
    }
    std::vector<Json> received(std::string_view method) const {
        std::vector<Json> found;
        for (const auto& message : received()) {
            if (message.contains("method") && message["method"] == std::string(method)) {
                found.push_back(message);
            }
        }
        return found;
    }
    bool closed() const {
        std::lock_guard lock(mutex);
        return stdin_closed;
    }
};

// An agent speaking JSON-RPC on fake pipes. Each line the client writes goes to `handler`, which
// answers through send(); output is delivered in order on the agent's own thread.
// Affinity: thread-safe.
class FakeAcpAgent final : public fv::Process {
public:
    using Handler = std::function<void(const Json& message, FakeAcpAgent& agent)>;

    FakeAcpAgent(Handler handler, bool exit_on_eof, std::shared_ptr<AgentLog> log)
        : handler_(std::move(handler)), exit_on_eof_(exit_on_eof), log_(std::move(log)) {}
    ~FakeAcpAgent() override {
        kill_tree();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // ---- the agent's side ----
    void send(const Json& message) { push({Item::Out, fv::py::dumps(message) + "\n"}); }
    void send_raw(std::string bytes) { push({Item::Out, std::move(bytes)}); }
    void stderr_line(const std::string& line) { push({Item::Err, line + "\r\n"}); }
    void exit(int code) { push({Item::Exit, {}, code}); }

    // ---- fv::Process ----
    std::uint32_t pid() const override { return 9001; }
    void write_stdin(std::string_view data) override {
        std::vector<Json> messages;
        {
            std::lock_guard lock(log_->mutex);
            if (log_->stdin_closed) {
                return;
            }
            stdin_.append(data);
            for (auto end = stdin_.find('\n'); end != std::string::npos; end = stdin_.find('\n')) {
                messages.push_back(fv::py::loads(stdin_.substr(0, end)));
                stdin_.erase(0, end + 1);
            }
            log_->messages.insert(log_->messages.end(), messages.begin(), messages.end());
        }
        for (const auto& message : messages) {
            if (handler_) {
                handler_(message, *this);
            }
        }
    }
    void close_stdin() override {
        {
            std::lock_guard lock(log_->mutex);
            log_->stdin_closed = true;
        }
        if (exit_on_eof_) {
            exit(0);
        }
    }
    void on_line(fv::LineSink, fv::LineSink) override {}
    void on_bytes(fv::ByteSink out, fv::ByteSink err) override {
        worker_ =
            std::thread([this, out = std::move(out), err = std::move(err)] { run(out, err); });
    }
    std::optional<int> wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return exit_.has_value(); });
        return exit_;
    }
    std::optional<int> exit_code() const override {
        std::lock_guard lock(mutex_);
        return exit_;
    }
    void kill_tree() override {
        ++log_->kills;
        {
            std::lock_guard lock(mutex_);
            if (!exit_) {
                exit_ = 1;
            }
        }
        changed_.notify_all();
        exit(1);
    }
    void disown() override {}
    void release_pipes(std::chrono::milliseconds) override {}

private:
    struct Item {
        enum Kind { Out, Err, Exit } kind;
        std::string bytes;
        int code = 0;
    };

    void push(Item item) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(item));
        }
        changed_.notify_all();
    }

    void run(const fv::ByteSink& out, const fv::ByteSink& err) {
        for (;;) {
            Item item;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return !queue_.empty(); });
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            if (item.kind == Item::Exit) {
                {
                    std::lock_guard lock(mutex_);
                    if (!exit_) {
                        exit_ = item.code;
                    }
                }
                changed_.notify_all();
                err(std::span<const char>());
                out(std::span<const char>());
                return;
            }
            const auto& sink = item.kind == Item::Out ? out : err;
            sink(std::span<const char>(item.bytes.data(), item.bytes.size()));
        }
    }

    Handler handler_;
    bool exit_on_eof_;
    std::shared_ptr<AgentLog> log_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    std::string stdin_;
    std::optional<int> exit_;
    std::thread worker_;
};

Json reply(const Json& request, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", std::move(result)}};
}

std::string method_of(const Json& message) {
    return message.contains("method") ? message["method"].get<std::string>() : "";
}

// A launcher whose every start is a FakeAcpAgent recording into `log`.
struct AcpFixture : AgentsFixture {
    std::shared_ptr<AgentLog> log = std::make_shared<AgentLog>();

    void serve(FakeAcpAgent::Handler handler, bool exit_on_eof = true) {
        launcher->set_factory([handler, exit_on_eof, log = log](const fv::ProcessSpec&) {
            return std::make_unique<FakeAcpAgent>(handler, exit_on_eof, log);
        });
    }

    std::unique_ptr<AcpConnection> connect(fv::agents::AcpNotificationHandler on_notification = {},
                                           fv::agents::AcpRequestHandler on_request = {}) {
        auto connection = std::make_unique<AcpConnection>(
            launcher, std::vector<std::string>{fv::path_to_utf8(temp.path() / "agent.exe"), "acp"},
            std::map<std::string, std::string>{{"X", "1"}}, temp.path(), std::move(on_notification),
            std::move(on_request));
        connection->start();
        return connection;
    }
};

std::string error_of(const std::function<void()>& call) {
    try {
        call();
    } catch (const AcpError& error) {
        return error.what();
    }
    return "(no AcpError)";
}

} // namespace

TEST_CASE("an ACP handshake lists OpenCode's models and deletes its session", "[acp]") {
    AcpFixture fx;
    const auto program = make_program(exe(fx.path_dir, "opencode"));
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        const auto method = method_of(message);
        if (method == "initialize") {
            agent.send(
                reply(message, Json{{"protocolVersion", 1},
                                    {"agentCapabilities",
                                     {{"sessionCapabilities", {{"delete", Json::object()}}}}}}));
        } else if (method == "session/new") {
            agent.send(Json{{"jsonrpc", "2.0"},
                            {"method", "session/update"},
                            {"params", {{"sessionId", "s1"}}}});
            // A request of the agent's own, which the client must refuse.
            agent.send(Json{{"jsonrpc", "2.0"},
                            {"id", "perm-1"},
                            {"method", "session/request_permission"},
                            {"params", {{"sessionId", "s1"}}}});
            agent.send(reply(
                message,
                Json{{"sessionId", "s1"},
                     {"configOptions",
                      Json::array(
                          {Json{{"id", "mode"}, {"options", {{{"value", "build"}}}}},
                           Json{{"id", "model"},
                                {"type", "select"},
                                {"options",
                                 Json::array(
                                     {Json{{"group", "OpenRouter"},
                                           {"options",
                                            Json::array(
                                                {Json{{"value", "openrouter/a"}, {"name", "A"}},
                                                 Json{{"value", "openrouter/b"}}, "junk"})}},
                                      Json{{"value", "anthropic/c"}, {"name", "C"}},
                                      Json{{"value", ""}, {"name", "Empty"}}})}}})}}));
        } else if (method == "session/delete") {
            agent.send(reply(message, nullptr));
        }
    });
    auto agents = fx.make();
    const fv::agents::InstalledAgent opencode{
        .id = std::string(OPENCODE), .path = program, .version = "2.0.18"};

    const auto models = agents->list_models(opencode);
    const std::vector<fv::agents::AgentModel> expected{{"", "OpenCode default"},
                                                       {"openrouter/a", "A"},
                                                       {"openrouter/b", "openrouter/b"},
                                                       {"anthropic/c", "C"}};
    CHECK(models == expected);

    const auto& agent = *fx.log;
    const auto initialize = agent.received("initialize");
    REQUIRE(initialize.size() == 1);
    CHECK(initialize[0]["params"] ==
          Json{{"protocolVersion", 1},
               {"clientCapabilities",
                {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}}, {"terminal", false}}},
               {"clientInfo",
                {{"name", fv::py::lower(fv::identity().product)},
                 {"title", std::string(fv::identity().product)},
                 {"version", "1"}}}});
    const auto workspace = agents->agent_workspace_dir();
    const auto session = agent.received("session/new");
    REQUIRE(session.size() == 1);
    CHECK(session[0]["params"] ==
          Json{{"cwd", fv::path_to_utf8(workspace)}, {"mcpServers", Json::array()}});
    const auto deleted = agent.received("session/delete");
    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0]["params"] == Json{{"sessionId", "s1"}});

    // The permission request got its refusal.
    bool refused = false;
    for (const auto& message : agent.received()) {
        if (message.contains("id") && message["id"] == "perm-1") {
            refused = true;
            CHECK(message == Json{{"jsonrpc", "2.0"},
                                  {"id", "perm-1"},
                                  {"error",
                                   {{"code", -32601},
                                    {"message", "session/request_permission is not supported"}}}});
        }
    }
    CHECK(refused);
    CHECK(agent.closed());
    CHECK(agent.kills >= 1);

    const auto specs = fx.launcher->specs();
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].program == program);
    CHECK(specs[0].args == std::vector<std::string>{"acp"});
    CHECK(specs[0].cwd == workspace);
    CHECK(specs[0].env.at("OPENCODE_DISABLE_PROJECT_CONFIG") == "1");
    CHECK(specs[0].pipes);
    CHECK(specs[0].own_tree);
    fx.check_launcher_stayed_inside();
}

TEST_CASE("the model-list session stays when the agent can't delete sessions", "[acp]") {
    AcpFixture fx;
    const auto program = make_program(exe(fx.path_dir, "opencode"));
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        const auto method = method_of(message);
        if (method == "initialize") {
            agent.send(reply(message, Json{{"agentCapabilities", Json::object()}}));
        } else if (method == "session/new") {
            // The draft before configOptions.
            agent.send(reply(
                message,
                Json{{"sessionId", "s2"},
                     {"models",
                      {{"availableModels",
                        Json::array({Json{{"modelId", "x/one"}, {"name", "One"}},
                                     Json{{"modelId", "x/two"}}, Json{{"name", "no id"}}})}}}}));
        }
    });
    auto agents = fx.make();
    const auto models = fv::agents::list_opencode_models(program, *agents);
    CHECK(models == std::vector<fv::agents::AgentModel>{{"x/one", "One"}, {"x/two", "x/two"}});
    CHECK(fx.log->received("session/new").size() == 1);
    CHECK(fx.log->received("session/delete").empty());
}

TEST_CASE("OpenCode's model list falls back to its default", "[acp]") {
    AgentsFixture fx;
    auto agents = fx.make();
    const fv::agents::InstalledAgent opencode{
        .id = std::string(OPENCODE), .path = fx.path_dir / "opencode.exe", .version = "2.0.18"};
    // The agent can't start: the default stays.
    CHECK(agents->list_models(opencode) ==
          std::vector<fv::agents::AgentModel>{{"", "OpenCode default"}});
    CHECK(fx.launcher->specs().size() == 1);

    // An agent that can't run isn't asked.
    auto too_old = opencode;
    too_old.problem = "too old";
    CHECK(agents->list_models(too_old).size() == 1);
    CHECK(fx.launcher->specs().size() == 1);
}

TEST_CASE("notifications reach the handler and the client answers the agent's requests", "[acp]") {
    AcpFixture fx;
    std::mutex mutex;
    std::vector<std::pair<std::string, Json>> notes;
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        if (method_of(message) == "go") {
            agent.send(Json{{"jsonrpc", "2.0"}, {"method", "note"}, {"params", {{"n", 1}}}});
            agent.send(Json{{"jsonrpc", "2.0"}, {"method", "note"}, {"params", "not an object"}});
            for (const auto* name : {"allow", "refuse", "deny", "crash"}) {
                agent.send(Json{{"jsonrpc", "2.0"}, {"id", name}, {"method", name}});
            }
            agent.send(reply(message, "done"));
        }
    });
    auto connection = fx.connect(
        [&](const std::string& method, const Json& params) {
            std::lock_guard lock(mutex);
            notes.emplace_back(method, params);
        },
        [](const std::string& method, const Json&) -> Json {
            if (method == "allow") {
                return Json{{"outcome", "selected"}};
            }
            if (method == "refuse") {
                throw AcpError("no", 0);
            }
            if (method == "deny") {
                throw AcpError("denied", -32000);
            }
            throw std::runtime_error("boom");
        });

    CHECK(connection->request("go", Json::object(), 5.0) == "done");
    {
        std::lock_guard lock(mutex);
        CHECK(notes == std::vector<std::pair<std::string, Json>>{{"note", Json{{"n", 1}}},
                                                                 {"note", Json::object()}});
    }
    std::map<std::string, Json> answers;
    for (const auto& message : fx.log->received()) {
        if (message.contains("id") && message["id"].is_string()) {
            answers[message["id"].get<std::string>()] = message;
        }
    }
    CHECK(answers["allow"] ==
          Json{{"jsonrpc", "2.0"}, {"id", "allow"}, {"result", {{"outcome", "selected"}}}});
    CHECK(answers["refuse"]["error"] == Json{{"code", -32601}, {"message", "no"}});
    CHECK(answers["deny"]["error"] == Json{{"code", -32000}, {"message", "denied"}});
    CHECK(answers["crash"]["error"] == Json{{"code", -32603}, {"message", "boom"}});
}

TEST_CASE("an error reply is an AcpError with its message, code and data", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        agent.send(
            Json{{"jsonrpc", "2.0"},
                 {"id", message["id"]},
                 {"error", {{"code", -32602}, {"message", "bad params"}, {"data", {{"x", 1}}}}}});
    });
    auto connection = fx.connect();
    try {
        connection->request("session/prompt", Json::object(), 5.0);
        FAIL("no error");
    } catch (const AcpError& error) {
        CHECK(std::string(error.what()) == "bad params");
        CHECK(error.code() == -32602);
        CHECK(error.data() == Json{{"x", 1}});
    }
}

TEST_CASE("non-JSON lines and replies nobody waits for are ignored", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        agent.send_raw("not json\n\n   \n[1, 2]\n");
        agent.send(Json{{"jsonrpc", "2.0"}, {"id", 99}, {"result", "stray"}});
        agent.send_raw("  " + fv::py::dumps(reply(message, {{"ok", true}})) + "  \r\n");
    });
    auto connection = fx.connect();
    CHECK(connection->request("x", Json::object(), 5.0) == Json{{"ok", true}});
}

TEST_CASE("a dropped agent fails pending requests with its stderr tail", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json&, FakeAcpAgent& agent) {
        for (const auto* line : {"one", "two   ", "", "three", "four"}) {
            agent.stderr_line(line);
        }
        agent.exit(3);
    });
    auto connection = fx.connect();
    CHECK(error_of([&] { connection->request("session/prompt", Json::object(), 5.0); }) ==
          "the agent exited (code 3): two / three / four");
    CHECK(connection->stderr_tail() == "one\ntwo\nthree\nfour");
    CHECK_FALSE(connection->alive());
    CHECK(error_of([&] { connection->request("again", Json::object(), 5.0); }) ==
          "the agent is not running");
    // A notification to a dead agent is dropped.
    connection->notify("session/cancel", Json::object());
}

TEST_CASE("the exit message splits the stderr tail as str.splitlines does", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json&, FakeAcpAgent& agent) {
        // A form feed and U+2028 end lines for splitlines, though not for the pipe's reader.
        for (const auto* line : {"zero", "one\x0ctwo",
                                 "three\xE2\x80\xA8"
                                 "four"}) {
            agent.stderr_line(line);
        }
        agent.exit(3);
    });
    auto connection = fx.connect();
    CHECK(error_of([&] { connection->request("x", Json::object(), 5.0); }) ==
          "the agent exited (code 3): two / three / four");
}

TEST_CASE("stdout lines end in a lone carriage return too, even across chunks", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json& message, FakeAcpAgent& agent) {
        const auto note = [](int n) {
            return fv::py::dumps(
                Json{{"jsonrpc", "2.0"}, {"method", "note"}, {"params", {{"n", n}}}});
        };
        // Python's text-mode pipe reads "\n", "\r\n" and a lone "\r" as line ends.
        agent.send_raw(note(1) + "\r" + note(2) + "\r");
        agent.send_raw("\n" + fv::py::dumps(reply(message, "done")) + "\r");
        agent.send_raw("\n");
    });
    std::mutex mutex;
    std::vector<Json> notes;
    auto connection = fx.connect([&](const std::string&, const Json& params) {
        std::lock_guard lock(mutex);
        notes.push_back(params);
    });
    CHECK(connection->request("x", Json::object(), 5.0) == "done");
    std::lock_guard lock(mutex);
    CHECK(notes == std::vector<Json>{Json{{"n", 1}}, Json{{"n", 2}}});
}

TEST_CASE("the stderr tail keeps the last 40 lines of at most 400 characters", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json&, FakeAcpAgent& agent) {
        for (int i = 0; i < 45; ++i) {
            agent.stderr_line("line " + std::to_string(i));
        }
        agent.stderr_line(std::string(500, 'x'));
        agent.exit(0);
    });
    auto connection = fx.connect();
    error_of([&] { connection->request("x", Json::object(), 5.0); });
    const auto lines = fv::py::split_on(connection->stderr_tail(), "\n");
    REQUIRE(lines.size() == 40);
    CHECK(lines.front() == "line 6");
    CHECK(lines.back() == std::string(400, 'x'));
}

TEST_CASE("a request times out, stalls or is cancelled", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json&, FakeAcpAgent&) {});
    auto connection = fx.connect();

    auto started = std::chrono::steady_clock::now();
    CHECK(error_of([&] { connection->request("slow", Json::object(), 1.0); }) ==
          "slow timed out after 1s");
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::seconds(1));

    started = std::chrono::steady_clock::now();
    CHECK(error_of([&] {
              connection->request("quiet", Json::object(), 30.0, {}, 0.6, [&] { return started; });
          }) == "quiet stalled after 1s without progress");

    fv::CancelSource cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel.cancel();
    });
    CHECK(error_of([&] { connection->request("waiting", Json::object(), 30.0, cancel.token()); }) ==
          "canceled");
    canceller.join();
    CHECK(connection->alive());
}

TEST_CASE("closing fails what is still pending and stops the agent", "[acp]") {
    AcpFixture fx;
    fx.serve([](const Json&, FakeAcpAgent&) {}, false);
    auto connection = fx.connect();
    std::string error;
    std::thread waiting(
        [&] { error = error_of([&] { connection->request("x", Json::object(), 30.0); }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    connection->close(0.0);
    waiting.join();
    // The reader may see the killed agent's end first, as OpenWhisper's can.
    CHECK((error == "the agent was closed" || error == "the agent exited (code 1)"));
    CHECK(fx.log->closed());
    CHECK(fx.log->kills >= 1);
    CHECK_FALSE(connection->alive());
}

TEST_CASE("an agent that can't start, or isn't started, is an AcpError", "[acp]") {
    AgentsFixture fx;
    AcpConnection connection(fx.launcher, {fv::path_to_utf8(fx.temp.path() / "missing.exe"), "acp"},
                             {}, fx.temp.path());
    CHECK(error_of([&] { connection.request("x", Json::object(), 1.0); }) ==
          "the agent is not running");
    CHECK(error_of([&] { connection.start(); }) ==
          "could not start the agent: The program wasn't found.");
}

TEST_CASE("config options and model choices read sessions as OpenWhisper does", "[acp]") {
    const Json session{
        {"configOptions",
         Json::array(
             {"junk",
              Json{{"id", "mode"},
                   {"options", Json::array({Json{{"value", "build"}},
                                            Json{{"options", Json::array({Json{{"value", "plan"}},
                                                                          Json{{"value", 0}},
                                                                          Json{{"value", 5}}})}},
                                            Json{{"value", nullptr}}})}}})}};
    CHECK(fv::agents::acp_config_option(session, "mode")["id"] == "mode");
    CHECK(fv::agents::acp_config_option(session, "model").is_null());
    CHECK(fv::agents::acp_option_values(session, "mode") ==
          std::vector<std::string>{"build", "plan", "5"});
    CHECK(fv::agents::acp_option_values(session, "missing").empty());
    // No model option and no draft models: nothing.
    CHECK(fv::agents::acp_model_choices(session).empty());
    CHECK(fv::agents::acp_model_choices(Json{{"models", nullptr}}).empty());
}
