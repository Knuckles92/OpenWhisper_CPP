// The Claude Code channel on real sockets and pipes.
// from tests/test_automation_channel_e2e.py
//
// openwhisper-cpp-cli --channel runs as a child on pipes, as Claude Code starts it, against the
// live API's own routes (agentapi::LiveApi in an McpApp on a loopback port, with a FakeAppControl)
// or against FakeApp, the live API in miniature whose answers a test sets. The windowed tests run
// openwhisper-cpp.exe --channel, which hands the command to openwhisper-cpp-cli before anything
// opens.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentapi/history_server.h"
#include "agentapi/live_api.h"
#include "automation/catalog.h"
#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "automation/testing/fake_app_control.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "channel_support.h"
#include "cli/pe_imports.h"
#include "cli_process.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using namespace fv::clitest;
namespace automation = fv::automation;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string product() {
    return std::string(fv::identity().product);
}

std::string prefix() {
    return std::string(fv::identity().env_prefix);
}

Json message_data(const std::string& text = "rename the helper to load_config") {
    Json data = Json::parse(R"({"job_id": "0123456789abcdef", "source": "hotkey",
        "origin": "dictated", "action_id": "claude_channel", "text": "", "app": {"id": "code",
        "name": "Visual Studio Code", "category": "editor"}})");
    data["text"] = text;
    return data;
}

std::size_t count_of(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos;
         at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// Every log record at INFO and above for its lifetime (caplog).
class LogCapture {
public:
    LogCapture() : previous_(fv::log::level()) {
        fv::log::set_level(fv::log::Level::Info);
        fv::log::add_sink(sink_);
    }
    ~LogCapture() {
        fv::log::remove_sink(sink_);
        fv::log::set_level(previous_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    std::string text() const { return sink_->text(); }

private:
    class Sink final : public fv::log::Sink {
    public:
        void write(const fv::log::Record& record) override {
            std::lock_guard lock(mutex_);
            text_ += std::string(record.message) + "\n";
        }
        std::string text() const {
            std::lock_guard lock(mutex_);
            return text_;
        }

    private:
        mutable std::mutex mutex_;
        std::string text_;
    };
    std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();
    fv::log::Level previous_;
};

// The in-app listener's app with the live API on a loopback port, as McpRuntime serves it, with
// every request it received recorded.
class LiveApp {
public:
    LiveApp()
        : database_(root_.path() / "live.db"),
          live_(settings, [this] { return on_.load(); }, *bus, leases_, io_) {
        fs::copy_file(fv::testkit::source_root() / "tests" / "fixtures" / "agentapi" / "history.db",
                      database_, fs::copy_options::overwrite_existing);
        fv::agentapi::McpAppOptions options;
        options.database = database_;
        options.token = kChannelToken;
        options.enabled = [this] {
            return on_.load();
        };
        options.settings = &settings;
        app_ = std::make_shared<fv::agentapi::McpApp>(std::move(options));
        live_.install(app_->live_routes());
        automation::set_app_control(control);
    }
    ~LiveApp() {
        stop();
        live_.close_streams();
        bus->close();
        automation::set_app_control(nullptr);
    }
    LiveApp(const LiveApp&) = delete;
    LiveApp& operator=(const LiveApp&) = delete;

    void start() {
        auto server = fv::net::HttpServer::create(reactor_);
        server->set_fallback([this](const fv::net::HttpRequest& request, bool,
                                    std::shared_ptr<fv::net::Responder> out) {
            SeenRequest seen{request.method, request.path, request.query, {}, Json()};
            for (const auto& [name, value] : request.headers) {
                seen.headers.emplace(fv::py::lower(name), value);
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(seen);
            }
            auto copy = std::make_shared<fv::net::HttpRequest>(request);
            io_.post([app = app_, copy, out = std::move(out)]() mutable {
                app->serve(*copy, std::move(out));
            });
        });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto listening = server->listen({"127.0.0.1", port_, std::nullopt, false});
            if (listening) {
                port_ = listening->port;
                break;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(50ms);
        }
        server_ = std::move(server);
    }
    void stop() {
        if (server_) {
            server_->stop(0ms);
            server_.reset();
        }
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<SeenRequest> requests(const std::string& method) const {
        std::lock_guard lock(mutex_);
        std::vector<SeenRequest> out;
        std::copy_if(requests_.begin(), requests_.end(), std::back_inserter(out),
                     [&](const SeenRequest& r) { return r.method == method; });
        return out;
    }
    std::size_t channel_streams() const { return bus->subscribers(fv::agentapi::CHANNEL_LABEL); }

    fv::MemorySettingsStore settings;
    std::shared_ptr<automation::EventBus> bus = std::make_shared<automation::EventBus>();
    std::shared_ptr<automation::FakeAppControl> control =
        std::make_shared<automation::FakeAppControl>();

private:
    fv::testkit::TempDataRoot root_;
    fs::path database_;
    std::atomic<bool> on_{true};
    fv::net::Reactor reactor_{2};
    fv::ThreadPool pool_{"channel-live", 4};
    fv::Strand io_{pool_, "channel-live-io", 4, fv::ShutdownMode::CancelQueued};
    automation::FilterLeases leases_;
    fv::agentapi::LiveApi live_;
    std::shared_ptr<fv::agentapi::McpApp> app_;
    std::unique_ptr<fv::net::HttpServer> server_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::vector<SeenRequest> requests_;
};

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = kWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

std::map<std::string, std::string> channel_env(const std::string& url, const fs::path& data) {
    std::map<std::string, std::string> values{{prefix() + "TOKEN", kChannelToken},
                                              {prefix() + "DATA_DIR", fv::path_to_utf8(data)}};
    if (!url.empty()) {
        values[prefix() + "URL"] = url;
    }
    return child_env(values);
}

std::unique_ptr<ChannelProcess> spawn_cli(std::map<std::string, std::string> env) {
    return std::make_unique<ChannelProcess>(cli_executable(), std::vector<std::string>{"--channel"},
                                            std::move(env));
}

} // namespace

// --- the channel as a child ----------------------------------------------------------------------

TEST_CASE("test_channel_end_to_end", "[platform][test_automation_channel_e2e]") {
    LiveApp app;
    app.settings.save_setting("api_run_actions", true);
    app.start();
    fv::testkit::TempDataRoot data;
    auto channel = spawn_cli(channel_env(app.url(), data.path()));

    // Claude Code's v2 runtime probes for revision 2026-07-28 first; a "method not found" keeps
    // it on the handshake that carries channels.
    const Json probe = channel->request("server/discover", Json::object(), Json("probe-1"));
    CHECK(probe["error"]["code"] == -32601);

    const Json initialized = channel->handshake()["result"];
    CHECK(initialized["protocolVersion"] == "2025-11-25");
    CHECK(initialized["capabilities"]["experimental"] == Json::parse(R"({"claude/channel": {}})"));
    CHECK(initialized["capabilities"]["tools"] == Json::object());
    CHECK(initialized["serverInfo"]["name"] == fv::py::lower(product()));
    CHECK_THAT(initialized["instructions"].get<std::string>(), ContainsSubstring("reply tool"));

    const Json tools = channel->request("tools/list")["result"]["tools"];
    REQUIRE(tools.size() == 1);
    CHECK(tools[0]["name"] == "reply");
    CHECK(tools[0]["inputSchema"]["required"] == Json::array({"text"}));

    REQUIRE(wait_until([&] { return app.channel_streams() == 1; }));
    const SeenRequest first = app.requests("GET").front();
    CHECK(first.path == "/v1/events");
    CHECK(first.query == fv::net::Query{{"types", "channel.message"}});
    CHECK(first.headers.at("authorization") == "Bearer " + kChannelToken);
    CHECK(first.headers.at(fv::py::lower(automation::channel_client_header())) == "claude-channel");
    CHECK(first.headers.at("accept") == "text/event-stream");
    CHECK_FALSE(first.headers.contains("last-event-id"));

    app.bus->publish_event(automation::catalog::RECORDING_STARTED,
                           Json::object({{"job_id", "aa11"}, {"mode", "dictation"}}));
    app.bus->publish_event(automation::catalog::CHANNEL_MESSAGE,
                           message_data("rename the helper to load_config, café ✓"));
    const Json pushed = channel->notification("notifications/claude/channel");
    CHECK(pushed["params"] == Json::parse(R"({"content":
        "rename the helper to load_config, café ✓", "meta": {"origin": "dictated",
        "job_id": "0123456789abcdef", "action": "claude_channel",
        "app": "Visual Studio Code"}})"));

    const Json reply =
        channel->request("tools/call", Json::parse(R"({"name": "reply", "arguments": {
            "text": "Renamed in 3 files.", "title": "Claude"}})"));
    CHECK(reply["result"]["isError"] == false);
    REQUIRE(wait_until([&] { return !app.control->calls().empty(); }));
    CHECK(app.control->calls().front() ==
          automation::FakeAppControl::Call{"notify", {"Renamed in 3 files.", "Claude"}});
    const SeenRequest posted = app.requests("POST").front();
    CHECK(posted.path == "/v1/notify");
    CHECK(posted.headers.at("authorization") == "Bearer " + kChannelToken);
    CHECK(posted.headers.at(fv::py::lower(automation::channel_client_header())) ==
          "claude-channel");

    // The app restarts its listener; a message sent meanwhile is replayed.
    app.stop();
    const std::int64_t missed = app.bus
                                    ->publish_event(automation::catalog::CHANNEL_MESSAGE,
                                                    message_data("run the tests again"))
                                    .seq;
    app.start();
    REQUIRE(wait_until([&] { return app.requests("GET").size() >= 2; }));
    CHECK(app.requests("GET").back().headers.at("last-event-id") == std::to_string(missed - 1));
    const Json replayed = channel->notification("notifications/claude/channel");
    CHECK(replayed["params"]["content"] == "run the tests again");

    CHECK(channel->close_stdin() == 0);
    for (const std::string& line : channel->raw_lines()) {
        CHECK_FALSE(line.ends_with("\r\n"));
    }
    const std::string log = channel->stderr_text();
    CHECK_THAT(log, ContainsSubstring("Connected to " + product()));
    CHECK_THAT(log, !ContainsSubstring(kChannelToken));
    CHECK_THAT(log, !ContainsSubstring("rename the helper"));
    CHECK_THAT(log, !ContainsSubstring("Renamed in 3 files"));
}

TEST_CASE("test_reply_needs_the_run_actions_permission",
          "[platform][test_automation_channel_e2e]") {
    LiveApp app; // running actions is off by default
    app.start();
    fv::testkit::TempDataRoot data;
    auto channel = spawn_cli(channel_env(app.url(), data.path()));
    channel->handshake();
    const Json result = channel->request(
        "tools/call",
        Json::parse(R"({"name": "reply", "arguments": {"text": "Done."}})"))["result"];
    CHECK(result["isError"] == true);
    CHECK_THAT(
        result["content"][0]["text"].get<std::string>(),
        ContainsSubstring("allow running actions in " + product() + " Settings > API & MCP"));
    CHECK(app.control->calls().empty());
    CHECK(channel->close_stdin() == 0);
}

TEST_CASE("test_reply_says_when_openwhisper_is_not_running",
          "[platform][test_automation_channel_e2e]") {
    fv::testkit::TempDataRoot data;
    auto channel =
        spawn_cli(channel_env("http://127.0.0.1:" + std::to_string(closed_port()), data.path()));
    channel->handshake();
    const Json result = channel->request(
        "tools/call",
        Json::parse(R"({"name": "reply", "arguments": {"text": "Done."}})"))["result"];
    CHECK(result["isError"] == true);
    CHECK_THAT(result["content"][0]["text"].get<std::string>(),
               ContainsSubstring(product() + " isn't running"));
    CHECK(channel->close_stdin() == 0);
    // Retries while the app is away log one line, not one per attempt.
    CHECK(count_of(channel->stderr_text(), product() + " isn't running") == 1);
}

TEST_CASE("test_channel_reads_the_saved_port_and_never_loads_qt",
          "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    fv::testkit::TempDataRoot data;
    std::ofstream(data.path() / fv::path_from_utf8(fv::identity().settings_file), std::ios::binary)
        << fv::py::dumps(Json::object({{"mcp_enabled", true}, {"mcp_port", app.port()}}));
    auto channel = spawn_cli(channel_env("", data.path()));
    channel->handshake();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
#ifdef __linux__
    const auto loaded = mapped_modules(channel->pid());
    REQUIRE_FALSE(loaded.empty());
    for (const auto& name : loaded) {
        CHECK_FALSE(name.starts_with("libqt"));
    }
#endif
    CHECK(channel->close_stdin() == 0);
#ifdef _WIN32
    // No Qt library is in the executable's import table, so none loads with it.
    const std::optional<std::vector<std::string>> imports = fv::cli::pe_imports(cli_executable());
    REQUIRE(imports.has_value());
    for (const std::string& name : *imports) {
        INFO(name);
        CHECK_FALSE((name.starts_with("Qt") || name.starts_with("qt")));
    }
#endif
}

TEST_CASE("test_windowed_interpreter_speaks_over_inherited_pipes",
          "[platform][test_automation_channel_e2e]") {
    if (app_executable().empty()) {
        SKIP("this build has no openwhisper-cpp executable");
    }
    FakeApp app;
    app.start();
    fv::testkit::TempDataRoot data;
    ChannelProcess channel(app_executable(), {"--channel"}, channel_env(app.url(), data.path()));
    CHECK(channel.handshake()["result"]["capabilities"]["experimental"] ==
          Json::parse(R"({"claude/channel": {}})"));
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    app.publish(message_data("from a windowed process"));
    CHECK(channel.notification("notifications/claude/channel")["params"]["content"] ==
          "from a windowed process");
    CHECK(channel.close_stdin() == 0);
    for (const std::string& line : channel.raw_lines()) {
        CHECK(line.ends_with("\n"));
        CHECK_FALSE(line.ends_with("\r\n"));
    }
    CHECK_THAT(channel.stderr_text(), ContainsSubstring("Connected to " + product()));
}

TEST_CASE("test_an_empty_stdin_ends_the_channel_at_once",
          "[platform][test_automation_channel_e2e]") {
    fv::testkit::TempDataRoot data;
    const CliResult done =
        run_cli({"--channel"},
                {{prefix() + "URL", "http://127.0.0.1:" + std::to_string(closed_port())},
                 {prefix() + "TOKEN", kChannelToken},
                 {prefix() + "DATA_DIR", fv::path_to_utf8(data.path())}},
                60s);
    CHECK(done.exit_code == 0u);
    CHECK(done.out.empty());
}

TEST_CASE("test_a_windowed_start_without_pipes_exits_quietly",
          "[platform][test_automation_channel_e2e]") {
#if defined(_WIN32)
    if (app_executable().empty()) {
        SKIP("this build has no openwhisper-cpp executable");
    }
    // A double-clicked shortcut gives the windowed app no stdio: no error dialog, just exit 2.
    fv::testkit::TempDataRoot data;
    std::wstring environment;
    for (const auto& [name, value] :
         channel_env("http://127.0.0.1:" + std::to_string(closed_port()), data.path())) {
        environment += fv::path_from_utf8(name + "=" + value).native();
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    const auto run = [&](const std::wstring& arguments) {
        std::wstring command = L"\"" + app_executable().native() + L"\" " + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                               nullptr, &startup, &info));
        CloseHandle(info.hThread);
        const DWORD waited = WaitForSingleObject(info.hProcess, 60000);
        DWORD code = 0;
        if (waited != WAIT_OBJECT_0) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 10000);
        }
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        CHECK(waited == WAIT_OBJECT_0);
        return code;
    };
    CHECK(run(L"--channel") == 2u);
    // main(argv) looked at its arguments before the pipes: help is still 0, anything else 2.
    CHECK(run(L"--channel --help") == 0u);
    CHECK(run(L"--channel --port 1") == 2u);
#else
    SKIP("the windowed executable is a Windows question");
#endif
}

// --- the event stream in process -----------------------------------------------------------------

namespace {

class Collector {
public:
    void operator()(const Json& params) {
        std::lock_guard lock(mutex_);
        messages_.push_back(params);
    }
    std::vector<Json> wait(std::size_t count, std::chrono::milliseconds timeout = kWait) {
        REQUIRE(wait_until(
            [&] {
                std::lock_guard lock(mutex_);
                return messages_.size() >= count;
            },
            timeout));
        return messages();
    }
    std::vector<Json> messages() {
        std::lock_guard lock(mutex_);
        return messages_;
    }

private:
    std::mutex mutex_;
    std::vector<Json> messages_;
};

automation::AppLinkOptions link_to(const std::string& url, std::string token = kChannelToken) {
    automation::AppLinkOptions options;
    options.url = url;
    options.token = std::move(token);
    options.read_settings = [] {
        return Json::object();
    };
    options.read_token = [] {
        return std::optional<std::string>();
    };
    return options;
}

automation::EventStreamOptions quick(
    std::function<bool()> update_check = [] { return false; },
    std::function<void()> on_update = {}) {
    automation::EventStreamOptions options;
    options.backoff_first = 20ms;
    options.backoff_max = 80ms;
    options.update_check = std::move(update_check);
    options.on_update = std::move(on_update);
    return options;
}

} // namespace

TEST_CASE("test_stream_rereads_a_rejected_token", "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    std::vector<std::string> tokens{"stale-token-0123456789abcdef0123456789", kChannelToken};
    std::atomic<std::size_t> reads{0};
    automation::AppLinkOptions options = link_to(app.url(), "");
    options.read_token = [&] {
        return std::optional<std::string>(tokens.at(reads++));
    };
    automation::AppLink link(options);
    Collector collected;
    LogCapture logs;
    automation::EventStream stream(link, std::ref(collected), quick());
    stream.start();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    app.publish(message_data("after the token changed"));
    CHECK(collected.wait(1)[0]["content"] == "after the token changed");
    stream.stop();
    CHECK(reads.load() == 2);
    CHECK_THAT(logs.text(), ContainsSubstring("rejected the access token"));
    CHECK_THAT(logs.text(), !ContainsSubstring(kChannelToken));
    CHECK_THAT(logs.text(), !ContainsSubstring("after the token changed"));
}

TEST_CASE("test_stream_waits_quietly_while_live_events_are_refused",
          "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.events_status = 403;
    app.start();
    automation::AppLink link(link_to(app.url()));
    Collector collected;
    LogCapture logs;
    automation::EventStream stream(link, std::ref(collected), quick());
    stream.start();
    REQUIRE(app.wait_for([&] { return app.stream_requests().size() >= 3; }));
    stream.stop();
    CHECK(stream.state() == "forbidden");
    CHECK(count_of(logs.text(), "refused the event stream") == 1);
}

TEST_CASE("test_stream_steps_aside_for_an_update_once_the_app_is_gone",
          "[platform][test_automation_channel_e2e]") {
    automation::AppLink link(link_to("http://127.0.0.1:" + std::to_string(closed_port())));
    std::atomic<bool> stepped_aside{false};
    Collector collected;
    automation::EventStream stream(link, std::ref(collected),
                                   quick([] { return true; }, [&] { stepped_aside = true; }));
    stream.start();
    CHECK(wait_until([&] { return stepped_aside.load(); }));
    CHECK(wait_until([&] { return !stream.running(); }));
}

TEST_CASE("test_stream_ignores_an_update_while_the_app_answers",
          "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    automation::AppLink link(link_to(app.url()));
    std::atomic<bool> stepped_aside{false};
    Collector collected;
    automation::EventStream stream(link, std::ref(collected),
                                   quick([] { return true; }, [&] { stepped_aside = true; }));
    stream.start();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    app.publish(message_data("still here"));
    collected.wait(1);
    stream.stop();
    CHECK_FALSE(stepped_aside.load());
}

TEST_CASE("test_stop_ends_a_quiet_stream_promptly", "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    automation::AppLink link(link_to(app.url()));
    Collector collected;
    automation::EventStream stream(link, std::ref(collected), quick());
    stream.start();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    const auto started = std::chrono::steady_clock::now();
    stream.stop();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK_FALSE(stream.running());
}

TEST_CASE("test_a_gap_is_announced_and_keeps_the_resume_point",
          "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    automation::AppLink link(link_to(app.url()));
    Collector collected;
    automation::EventStream stream(link, std::ref(collected), quick());
    stream.start();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    const std::int64_t first = app.publish(message_data("before the gap"));
    collected.wait(1);
    app.send_gap(first + 1);
    collected.wait(2);
    CHECK(stream.last_id() == std::to_string(first));
    const std::int64_t second = app.publish(message_data("after the gap"));
    const std::vector<Json> messages = collected.wait(3);
    CHECK(wait_until([&] { return stream.last_id() == std::to_string(second); }));
    stream.stop();
    REQUIRE(messages.size() == 3);
    CHECK(messages[0]["content"] == "before the gap");
    CHECK(messages[1]["content"] == automation::gap_notice());
    CHECK(messages[2]["content"] == "after the gap");
    CHECK(messages[1]["meta"] == Json::object({{"origin", automation::channel_notice_origin()}}));
}

TEST_CASE("test_stream_skips_events_without_text", "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    automation::AppLink link(link_to(app.url()));
    Collector collected;
    automation::EventStream stream(link, std::ref(collected), quick());
    stream.start();
    REQUIRE(app.wait_for([&] { return app.open_streams() > 0; }));
    app.publish(message_data("   "));
    app.publish(Json::object({{"job_id", "x"}}));
    app.publish(message_data("the real one"));
    const std::vector<Json> messages = collected.wait(1);
    stream.stop();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["content"] == "the real one");
}

// --- replies in process
// ---------------------------------------------------------------------------

TEST_CASE("test_notify_rereads_a_rejected_token_once", "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    std::vector<std::string> tokens{"stale-token-0123456789abcdef0123456789", kChannelToken};
    std::size_t next = 0;
    automation::AppLinkOptions options = link_to(app.url(), "");
    options.read_token = [&] {
        return std::optional<std::string>(tokens.at(next++));
    };
    automation::AppLink link(options);
    (void)link.token(); // the stale one, as if read at startup
    link.notify("Done.");
    std::vector<std::string> sent;
    for (const SeenRequest& notification : app.notifications()) {
        sent.push_back(notification.headers.at("authorization"));
    }
    CHECK(sent == std::vector<std::string>{"Bearer stale-token-0123456789abcdef0123456789",
                                           "Bearer " + kChannelToken});
}

TEST_CASE("test_notify_refusals_tell_claude_what_to_do",
          "[platform][test_automation_channel_e2e]") {
    const auto [status, message, expected] = GENERATE(table<int, std::string, std::string>({
        {403, "", "allow running actions in {} Settings > API & MCP"},
        {404, "", "can't show replies. Update {}"},
        {503, "{} isn't ready for this yet.", "({} isn't ready for this yet.)"},
    }));
    const auto named = [](std::string text) {
        const std::size_t at = text.find("{}");
        return at == std::string::npos ? text : text.replace(at, 2, product());
    };
    FakeApp app;
    app.notify_status = status;
    app.set_notify_message(named(message));
    app.start();
    try {
        automation::AppLink(link_to(app.url())).notify("Done.");
        FAIL("no LinkError");
    } catch (const automation::LinkError& error) {
        CHECK_THAT(error.message(), ContainsSubstring(named(expected)));
    }
}

TEST_CASE("test_notify_sends_no_title_when_none_is_given",
          "[platform][test_automation_channel_e2e]") {
    FakeApp app;
    app.start();
    automation::AppLink(link_to(app.url())).notify("Done.");
    REQUIRE(app.notifications().size() == 1);
    CHECK(app.notifications()[0].body == Json::object({{"text", "Done."}}));
}
