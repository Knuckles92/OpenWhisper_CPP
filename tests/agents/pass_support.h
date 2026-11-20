#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "agent_actions_support.h"
#include "agents/agent_tasks.h"
#include "agents/detail/driver_impl.h"
#include "agents/drivers.h"
#include "agents/loopback_mcp.h"
#include "agents_test_support.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "net/reactor.h"
#include "process/launcher.h"
#include "pyport/json.h"
#include "pyport/text.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// What the pass tests share (tests/test_installed_agent_drivers.py and
// test_installed_agent_core.py): the meeting tools OpenWhisper's tests pass, a recording tool
// handler, requests, and the agents as fv_fake_agent copies run by the real launcher, with a
// loopback tool server.
namespace fv::agents_test {

// OpenWhisper's meeting tools (mcp_tool_definitions() at the pinned commit), as the golden
// vectors of tool_contract carry them.
inline const Json& meeting_tools() {
    static const Json tools = golden::load("meeting.agent.installed.drivers")
                                  .at("functions")
                                  .at("tool_contract")
                                  .at(0)
                                  .at("args")
                                  .at(0);
    return tools;
}

// OpenWhisper's spelling of a name, as the vectors and transcripts were recorded.
inline std::string legacy_server() {
    return py::lower(legacy_identity().product);
}

// golden::rebrand, then the server name in lower case (mcp__openwhisper__ ->
// mcp__openwhisper_cpp__), which golden::rebrand leaves alone.
inline std::string rebrand_text(std::string_view text) {
    auto out = golden::rebrand(text);
    const auto from = legacy_server();
    const auto to = agents::server_name();
    for (auto at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size())) {
        out.replace(at, from.size(), to);
    }
    return out;
}

inline Json rebranded(const Json& value) {
    if (value.is_string()) {
        return rebrand_text(value.get<std::string>());
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(rebranded(item));
        }
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[rebrand_text(key)] = rebranded(item);
        }
        return out;
    }
    return value;
}

// The tool handler of the Python tests: records each call and answers {"results": [{"ok":
// true}]}.
// Affinity: thread-safe (the server's request threads call it).
class Recorder {
public:
    using Call = std::tuple<std::string, Json, Json>;

    agents::ToolHandler handler() {
        return [state = state_](std::string_view name, const Json& args, const Json& meta) {
            std::lock_guard lock(state->mutex);
            state->calls.emplace_back(std::string(name), args, meta);
            return std::pair<std::string, bool>(
                py::dumps(Json{{"results", Json::array({Json{{"ok", true}}})}}), false);
        };
    }
    std::vector<Call> calls() const {
        std::lock_guard lock(state_->mutex);
        return state_->calls;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& call : calls()) {
            out.push_back(std::get<0>(call));
        }
        return out;
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<Call> calls;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

// PATCH_CALL of the Python tests.
inline Json patch_call() {
    return Json{{"name", "patch_state"},
                {"arguments",
                 {{"ops", Json::array({Json{{"op", "set_topic"},
                                            {"text", "Beta"},
                                            {"evidence", Json::array({"sg_1"})}}})}}}};
}

// _request of the Python tests: charter, transcript, the meeting tools, haiku, low effort, 30 s
// and a 20 s stall.
inline agents::PassRequest pass_request(agents::ToolHandler handler) {
    agents::PassRequest request;
    request.system_prompt = "charter";
    request.user_prompt = "transcript";
    request.tools = meeting_tools();
    request.handler = std::move(handler);
    request.model = "haiku";
    request.effort = "low";
    request.timeout_s = 30;
    request.stall_s = 20;
    return request;
}

// CLAUDE_HELP of the Python tests.
inline constexpr std::string_view CLAUDE_HELP =
    "--mcp-config --strict-mcp-config --tools --system-prompt --system-prompt-file "
    "--permission-prompts --no-session-persistence --disable-slash-commands --effort "
    "--include-partial-messages";

// A HelpText that always answers `text`.
inline agents::detail::HelpText canned_help(std::string text) {
    return
        [text = std::move(text)](const agents::InstalledAgent&, const std::vector<std::string>&) {
            return text;
        };
}

inline bool ends_mcp_list(const std::vector<std::string>& args) {
    return args.size() >= 3 && args[args.size() - 3] == "mcp" && args[args.size() - 2] == "list" &&
           args.back() == "--json";
}

// The value after `flag` in argv, or "<missing>".
inline std::string after_flag(const std::vector<std::string>& argv, std::string_view flag) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag) {
            return argv[i + 1];
        }
    }
    return "<missing>";
}

inline std::vector<std::string> values_of(const std::vector<std::string>& argv,
                                          std::string_view flag) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag) {
            out.push_back(argv[i + 1]);
        }
    }
    return out;
}

inline bool has_arg(const std::vector<std::string>& argv, std::string_view arg) {
    return std::find(argv.begin(), argv.end(), arg) != argv.end();
}

inline std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#ifdef _WIN32
inline bool process_alive(unsigned long pid) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}
inline void kill_process(unsigned long pid) {
    if (HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        TerminateProcess(handle, 1);
        CloseHandle(handle);
    }
}
#define FV_PASS_PROCESSES()
#else
inline bool process_alive(unsigned long) {
    return false;
}
inline void kill_process(unsigned long) {}
#define FV_PASS_PROCESSES() SKIP("programs are started on Windows only")
#endif

inline bool gone_soon(unsigned long pid, std::chrono::seconds limit = std::chrono::seconds(5)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (process_alive(pid) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !process_alive(pid);
}

// The three agents as fv_fake_agent copies run by the real launcher, a loopback tool server on a
// reactor of its own, and the files the fake writes, all in a temporary folder; installed as
// installed_agents() for the length of a test.
struct PassAgents {
    TempDir temp;
    std::filesystem::path bin = temp.path() / "bin";
    std::filesystem::path tmp = temp.path() / "tmp";
    std::filesystem::path record = temp.path() / "record.json";
    std::filesystem::path helper_pid = temp.path() / "helper.pid";
    std::filesystem::path acp_log = temp.path() / "acp.jsonl";
    std::shared_ptr<std::map<std::string, std::string>> env =
        std::make_shared<std::map<std::string, std::string>>();
    std::shared_ptr<ProcessLauncher> launcher = make_process_launcher();
    std::shared_ptr<TableAgents> table;
    std::unique_ptr<ScopedAgents> scoped;
    agents::InstalledAgent claude;
    agents::InstalledAgent codex;
    agents::InstalledAgent opencode;
    net::Reactor reactor{2};
    agents::LoopbackMcpServer server{reactor};

    PassAgents() {
        std::filesystem::create_directories(bin);
        std::filesystem::create_directories(tmp);
        for (const auto* name : {"claude", "codex", "opencode"}) {
            std::filesystem::copy_file(FV_FAKE_AGENT, exe(bin, name));
        }
        const auto home = temp.path() / "home";
        std::filesystem::create_directories(home);
        *env = {
            {"USERPROFILE", path_to_utf8(home)},
            {"HOME", path_to_utf8(home)},
            {"LOCALAPPDATA", path_to_utf8(temp.path() / "local")},
            {"APPDATA", path_to_utf8(temp.path() / "roaming")},
            {"CODEX_HOME", path_to_utf8(temp.path() / "codex-home")},
            {"CLAUDE_CONFIG_DIR", path_to_utf8(temp.path() / "claude-config")},
            {"XDG_CONFIG_HOME", path_to_utf8(temp.path() / "xdg")},
            {"XDG_DATA_HOME", path_to_utf8(temp.path() / "xdg-data")},
            {"PATH", path_to_utf8(bin)},
            {"FV_FAKE_AGENT_RECORD", path_to_utf8(record)},
            {"FV_FAKE_AGENT_HELPER_PID", path_to_utf8(helper_pid)},
            {"FV_FAKE_AGENT_ACP_LOG", path_to_utf8(acp_log)},
        };
        // Windows programs, and Winsock, expect these.
        for (const auto* name : {"SystemRoot", "SYSTEMDRIVE", "windir"}) {
            if (const auto value = env_utf8(name)) {
                env->insert_or_assign(name, *value);
            }
        }
        table = std::make_shared<TableAgents>(launcher, [env = env] { return *env; }, home, tmp);
        claude = {"claude_code", exe(bin, "claude"), "9.0.0"};
        codex = {"codex", exe(bin, "codex"), "9.0.0"};
        opencode = {"opencode_cli", exe(bin, "opencode"), "2.0.18"};
        for (const auto* agent : {&claude, &codex, &opencode}) {
            table->set(*agent);
        }
        scoped = std::make_unique<ScopedAgents>(table);
        agents::detail::forget_driver_help();
        server.start();
    }
    ~PassAgents() {
        server.stop();
        if (const auto pid = helper(); pid != 0 && process_alive(pid)) {
            kill_process(pid); // a test that failed may leave the helper; never leave it running
        }
    }
    PassAgents(const PassAgents&) = delete;
    PassAgents& operator=(const PassAgents&) = delete;

    void set(const std::string& name, const std::string& value) {
        env->insert_or_assign(name, value);
    }
    void mode(const std::string& value) { set("FV_FAKE_AGENT_MODE", value); }
    void tool_call(const Json& call) { set("FV_FAKE_AGENT_TOOL_CALL", call.dump()); }

    Json recorded() const { return py::loads(read_all(record)); }
    // The ACP messages the fake received, after its first line (the overlay it was started with).
    std::vector<Json> acp() const {
        std::vector<Json> entries;
        for (const auto& line : py::split_on(read_all(acp_log), "\n")) {
            if (!line.empty()) {
                entries.push_back(py::loads(line));
            }
        }
        return entries;
    }
    std::vector<Json> acp_requests(std::string_view method) const {
        std::vector<Json> out;
        for (const auto& entry : acp()) {
            if (const auto found = entry.value("method", Json());
                found.is_string() && found.get<std::string>() == method) {
                out.push_back(entry.at("params"));
            }
        }
        return out;
    }
    unsigned long helper() const {
        const auto text = read_all(helper_pid);
        return text.empty() ? 0 : std::stoul(text);
    }
    // No openwhisper-cpp-pass-* folder is left in the temporary folder.
    bool pass_folders_removed() const {
        const auto prefix = py::lower(identity().product) + "-pass-";
        for (const auto& entry : std::filesystem::directory_iterator(tmp)) {
            if (path_to_utf8(entry.path().filename()).starts_with(prefix)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace fv::agents_test
