#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <system_error>

#include "agents/detail/driver_impl.h"
#include "agents/detail/pyvalue.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::agents::detail {

namespace {

constexpr log::Logger logger("meeting.agent.installed.drivers");

const Json& dict_get(const Json& value, std::string_view key) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return get(value, key);
}

Json or_else(const Json& value, const Json& fallback) {
    return truthy(value) ? value : fallback;
}

std::int64_t to_int(const Json& value) {
    const auto parsed = py::py_int_checked(value);
    if (!parsed) {
        if (parsed.error() == py::IntError::Type) {
            throw py::TypeError("int() argument must be a string, a bytes-like object or a real "
                                "number");
        }
        throw py::ValueError("invalid literal for int()");
    }
    return *parsed;
}

std::int64_t int_or_zero(const Json& usage, std::string_view key) {
    const auto& value = dict_get(usage, key);
    return truthy(value) ? to_int(value) : 0;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!out) {
        throw std::filesystem::filesystem_error("could not write", path,
                                                std::make_error_code(std::errc::io_error));
    }
}

bool contains_text(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::string str_or(const Json& value, std::string_view fallback) {
    return truthy(value) ? py_str(value) : std::string(fallback);
}

} // namespace

const std::vector<std::string_view>& codex_disable_names() {
    static const std::vector<std::string_view> names{
        "shell_tool",
        "unified_exec",
        "apps",
        "browser_use",
        "browser_use_external",
        "computer_use",
        "image_generation",
        "multi_agent",
        "multi_agent_v2",
        "plugins",
        "goals",
        "skill_search",
        "tool_suggest",
        "in_app_browser",
        "hooks",
        "memories",
        "workspace_dependencies",
        "skill_mcp_dependency_install",
        "personality",
        "standalone_web_search",
        "web_search_request",
        "web_search_cached",
        "js_repl",
        "remote_plugin",
        "view_image",
        "browser_use_full_cdp_access",
        "worktrees",
    };
    return names;
}

std::vector<std::string> codex_features_to_disable(std::string_view listing) {
    std::set<std::string> enabled;
    for (const auto& line : splitlines(listing)) {
        const auto parts = py::split(line);
        if (parts.size() >= 3 && parts.back() == "true" && parts[1] != "removed") {
            enabled.insert(parts[0]);
        }
    }
    if (py::strip(listing).empty()) {
        return {"shell_tool"};
    }
    std::vector<std::string> features;
    for (const auto name : codex_disable_names()) {
        if (enabled.contains(std::string(name))) {
            features.emplace_back(name);
        }
    }
    return features;
}

std::vector<std::string> codex_servers_from(std::string_view listing) {
    static const py::Regex safe_name(R"re([A-Za-z0-9_-]+)re");
    const auto unverifiable = [] {
        return AgentUnavailable("Could not verify Codex's MCP configuration. Update Codex before "
                                "using it for meeting insights.");
    };
    Json servers;
    try {
        servers = py::loads(listing);
    } catch (const py::ValueError&) {
        throw unverifiable();
    }
    if (!servers.is_array()) {
        throw unverifiable();
    }
    std::set<std::string> names;
    for (const auto& server : servers) {
        if (!server.is_object()) {
            throw unverifiable();
        }
        const auto name = str_or(get(server, "name"), "");
        const auto& enabled = get(server, "enabled");
        if (name == server_name() || (enabled.is_boolean() && !enabled.get<bool>())) {
            continue;
        }
        if (!safe_name.fullmatch(name)) {
            throw unverifiable();
        }
        names.insert(name);
    }
    return {names.begin(), names.end()};
}

void CodexDriver::check_ready() {
    auto exec_help = help({"exec", "--help"});
    if (!exec_help.empty() && !contains_text(exec_help, "--json")) {
        throw AgentUnavailable(std::format("Codex {} is too old for meeting insights. {}",
                                           agent().version, agent_spec(CODEX).update_hint));
    }
    exec_help_ = std::move(exec_help);
    disable_ = codex_features_to_disable(help({"features", "list"}));
    mcp_servers_to_disable();
}

std::vector<std::string> CodexDriver::mcp_servers_to_disable() const {
    std::vector<std::string> args;
    for (const auto& name : disable_) {
        args.emplace_back("--disable");
        args.push_back(name);
    }
    for (const auto* word : {"mcp", "list", "--json"}) {
        args.emplace_back(word);
    }
    return codex_servers_from(help(args));
}

std::pair<std::vector<std::string>, std::map<std::string, std::string>>
CodexDriver::build(const PassRequest& request, std::string_view url, std::string_view token,
                   const std::filesystem::path& workdir) {
    const auto instructions = workdir / "instructions.md";
    write_text(instructions, request.system_prompt);
    auto instructions_text = path_to_utf8(instructions);
#ifdef _WIN32
    std::replace(instructions_text.begin(), instructions_text.end(), '\\', '/');
#endif
    const auto server = server_name();
    // Values are bare TOML-invalid literals on purpose: Codex reads them as plain strings, and no
    // quotes have to survive a Windows .cmd shim.
    std::vector<std::string> overrides{
        "notify=[]",
        "web_search=disabled",
        std::format("mcp_servers.{}.url={}", server, url),
        std::format("mcp_servers.{}.bearer_token_env_var={}", server, mcp_token_env()),
        std::format("mcp_servers.{}.default_tools_approval_mode=approve", server),
        "approval_policy=never",
        "project_doc_max_bytes=0",
        "model_instructions_file=" + instructions_text,
    };
    for (const auto& name : mcp_servers_to_disable()) {
        overrides.push_back(std::format("mcp_servers.{}.enabled=false", name));
    }
    if (!request.effort.empty()) {
        overrides.push_back("model_reasoning_effort=" + request.effort);
    }
    std::vector<std::string> argv{path_to_utf8(agent().path), "exec", "--json"};
    for (const auto& image : request.images) {
        // --image takes several values, so it goes before the next flag and never right before
        // the "-" that reads the prompt.
        argv.emplace_back("--image");
        argv.push_back(path_to_utf8(image));
    }
    if (contains_text(exec_help_, "--ephemeral")) {
        argv.emplace_back("--ephemeral");
    }
    for (std::string arg :
         {std::string("--skip-git-repo-check"), std::string("-s"), std::string("read-only"),
          std::string("-C"), path_to_utf8(installed_agents().agent_workspace_dir())}) {
        argv.push_back(std::move(arg));
    }
    for (const auto& override_ : overrides) {
        argv.emplace_back("-c");
        argv.push_back(override_);
    }
    for (const auto& feature : disable_) {
        argv.emplace_back("--disable");
        argv.push_back(feature);
    }
    if (!request.model.empty()) {
        argv.emplace_back("-m");
        argv.push_back(request.model);
    }
    argv.emplace_back("-");
    return {std::move(argv), {{mcp_token_env(), std::string(token)}}};
}

void CodexDriver::handle_event(const Json& event, RunState& state, const PassRequest& request) {
    const auto& kind = get(event, "type");
    if (kind == "thread.started") {
        request.emit("start");
    } else if (kind == "turn.started") {
        request.emit("turn");
    } else if (kind == "item.started" || kind == "item.updated" || kind == "item.completed") {
        on_codex_item(kind.get<std::string>(), or_else(get(event, "item"), Json::object()), state,
                      request);
    } else if (kind == "turn.completed") {
        state.done = true;
        state.usage = codex_usage(or_else(get(event, "usage"), Json::object()));
        request.emit("settled");
    } else if (kind == "turn.failed") {
        state.error = str_or(dict_get(or_else(get(event, "error"), Json::object()), "message"),
                             "turn failed");
    } else if (kind == "error") {
        state.error = str_or(get(event, "message"), "error");
    }
}

PassOutcome CodexDriver::finish(const RunState& state, std::optional<int> returncode,
                                std::string_view stderr_tail) {
    if (!state.error.empty()) {
        return PassOutcome{.ok = false, .error = codex_error(state.error), .usage = state.usage};
    }
    if (!state.done) {
        return PassOutcome{
            .ok = false,
            .error = std::format("Codex exited (code {}) before finishing the pass{}",
                                 returncode ? std::to_string(*returncode) : std::string("None"),
                                 tail_detail(stderr_tail)),
            .usage = state.usage};
    }
    return PassOutcome{.ok = true, .text = state.text, .usage = state.usage};
}

void on_codex_item(std::string_view kind, const Json& item, RunState& state,
                   const PassRequest& request) {
    const auto& item_type = dict_get(item, "type");
    if (item_type == "reasoning") {
        request.emit("thinking");
    } else if (item_type == "agent_message") {
        request.emit("writing");
        if (kind == "item.completed") {
            state.text = str_or(get(item, "text"), "");
        }
    } else if (item_type == "mcp_tool_call") {
        request.emit("tool", str_or(get(item, "tool"), ""));
    } else if (item_type == "error") {
        logger.info("Codex: {}", py::slice(str_or(get(item, "message"), ""), 0, 300));
    } else if (item_type == "command_execution" || item_type == "file_change" ||
               item_type == "web_search") {
        logger.warning("Codex used {} during a meeting pass", item_type.get<std::string>());
    }
}

Json codex_usage(const Json& usage) {
    const auto prompt = int_or_zero(usage, "input_tokens");
    const auto completion = int_or_zero(usage, "output_tokens");
    return Json{{"prompt_tokens", prompt},
                {"completion_tokens", completion},
                {"total_tokens", prompt + completion},
                {"requests", 1},
                {"cached_tokens", int_or_zero(usage, "cached_input_tokens")}};
}

std::string codex_error(std::string_view message) {
    auto text = py::strip(message);
    try {
        const auto data = py::loads(text);
        if (data.is_object()) {
            // (data.get("error") or {}).get("message"): an error that is not a dict raises.
            const Json inner = dict_get(or_else(get(data, "error"), Json::object()), "message");
            if (inner.is_string() && !inner.get<std::string>().empty()) {
                text = inner.get<std::string>();
            }
        }
    } catch (const py::ValueError&) {
    }
    if (contains_text(py::lower(text), "not logged in") || contains_text(text, "401")) {
        return "Codex is not signed in. Run `codex login`.";
    }
    return "Codex: " + py::slice(text, 0, 300);
}

} // namespace fv::agents::detail
