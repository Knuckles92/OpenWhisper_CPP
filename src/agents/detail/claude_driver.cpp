#include <array>
#include <format>
#include <fstream>
#include <system_error>

#include "agents/detail/driver_impl.h"
#include "agents/detail/pyvalue.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::agents::detail {

namespace {

// Flags a Claude Code must list in --help to run passes.
constexpr std::array<std::string_view, 4> REQUIRED{"--mcp-config", "--strict-mcp-config", "--tools",
                                                   "--system-prompt"};
// The usage fields that count as the prompt.
constexpr std::array<std::string_view, 3> PROMPT_TOKEN_KEYS{
    "input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"};

// value.get(key) where Python would raise AttributeError for a value that is not a dict.
const Json& dict_get(const Json& value, std::string_view key) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return get(value, key);
}

// `value or fallback` for a JSON value.
Json or_else(const Json& value, const Json& fallback) {
    return truthy(value) ? value : fallback;
}

// int(value), raising as Python's int() does.
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

// open(path, "w", encoding="utf-8").write(text)
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

void on_system(const Json& event, RunState& state, const PassRequest& request) {
    const auto& raw_subtype = dict_get(event, "subtype");
    const auto subtype = truthy(raw_subtype) ? py_str(raw_subtype) : std::string();
    if (subtype == "init") {
        const Json none = Json::array();
        Json ours = nullptr;
        for (const auto& server : iterate(or_else(dict_get(event, "mcp_servers"), none))) {
            if (server.is_object() && get(server, "name") == server_name()) {
                ours = server;
                break;
            }
        }
        const auto& raw_status = ours.is_object() ? get(ours, "status") : Json(nullptr);
        const auto status = truthy(raw_status) ? py_str(raw_status) : std::string("missing");
        if (truthy(request.tools) && status != "connected" && status != "pending") {
            state.fatal = std::format("Claude Code could not reach {}'s meeting tools ({}).",
                                      identity().product, status);
        }
        request.emit("start");
    } else if (contains_text(subtype, "thinking")) {
        request.emit("thinking");
    } else if (contains_text(subtype, "retry")) {
        request.emit("retry");
    }
}

} // namespace

void ClaudeCodeDriver::check_ready() {
    auto help_text = help({"--help"});
    bool missing = false;
    for (const auto flag : REQUIRED) {
        missing = missing || !contains_text(help_text, flag);
    }
    if (!help_text.empty() && missing) {
        throw AgentUnavailable(
            std::format("Claude Code {} is too old for meeting insights. Run `claude update`.",
                        agent().version));
    }
    flags_ = std::move(help_text);
}

bool ClaudeCodeDriver::has(std::string_view flag) const {
    return contains_text(flags_, flag);
}

bool ClaudeCodeDriver::supports_read_folder() const {
    return has("--restricted") && has("--add-dir");
}

std::string ClaudeCodeDriver::tool_prefix() const {
    return "mcp__" + server_name() + "__";
}

std::pair<std::vector<std::string>, std::map<std::string, std::string>>
ClaudeCodeDriver::build(const PassRequest& request, std::string_view url, std::string_view token,
                        const std::filesystem::path& workdir) {
    const auto system_file = workdir / "system.md";
    const auto mcp_file = workdir / "mcp.json";
    const auto settings_file = workdir / "settings.json";
    write_text(system_file, request.system_prompt);
    write_text(
        mcp_file,
        py::dumps(Json{{"mcpServers",
                        {{server_name(),
                          {{"type", "http"},
                           {"url", url},
                           {"headers", {{"Authorization", std::format("Bearer {}", token)}}}}}}}}));
    Json settings{{"disableAllHooks", true}};
    if (request.effort == "low") {
        // Live passes are small edits against a rolling state; extended thinking tripled their
        // latency without better ops.
        settings["alwaysThinkingEnabled"] = false;
    }
    // The user's coding hooks have no business in a pass.
    write_text(settings_file, py::dumps(settings));

    std::vector<std::string> argv{path_to_utf8(agent().path), "-p", "--output-format",
                                  "stream-json", "--verbose"};
    if (has("--include-partial-messages")) {
        argv.emplace_back("--include-partial-messages");
    }
    if (has("--system-prompt-file")) {
        argv.emplace_back("--system-prompt-file");
        argv.push_back(path_to_utf8(system_file));
    } else {
        argv.emplace_back("--system-prompt");
        argv.push_back(request.system_prompt);
    }
    if (!request.read_folder.empty()) {
        // --restricted confines the file tools to the working folders, which --add-dir names;
        // without it Read reaches every file the user can (checked with Claude Code 2.1.306). A
        // short 8.3 path is refused, so the caller passes the folder's long one.
        for (std::string arg :
             {std::string("--restricted"), std::string("--tools"), std::string("Read"),
              std::string("--add-dir"), path_to_utf8(request.read_folder)}) {
            argv.push_back(std::move(arg));
        }
    } else {
        argv.emplace_back("--tools");
        argv.emplace_back("");
    }
    for (std::string arg : {std::string("--mcp-config"), path_to_utf8(mcp_file),
                            std::string("--strict-mcp-config")}) {
        argv.push_back(std::move(arg));
    }
    if (truthy(request.tools)) {
        std::string allowed;
        for (const auto& tool : iterate(request.tools)) {
            allowed += std::format("{}mcp__{}__{}", allowed.empty() ? "" : ",", server_name(),
                                   py_str(dict_get(tool, "name")));
        }
        argv.emplace_back("--allowedTools");
        argv.push_back(allowed);
    }
    argv.emplace_back("--settings");
    argv.push_back(path_to_utf8(settings_file));
    if (has("--permission-prompts")) {
        argv.emplace_back("--permission-prompts");
        argv.emplace_back("none");
    }
    for (const std::string_view flag : {"--no-session-persistence", "--disable-slash-commands"}) {
        if (has(flag)) {
            argv.emplace_back(flag);
        }
    }
    if (!request.model.empty()) {
        argv.emplace_back("--model");
        argv.push_back(request.model);
    }
    if (!request.effort.empty() && has("--effort")) {
        argv.emplace_back("--effort");
        argv.push_back(request.effort);
    }
    return {std::move(argv), {}};
}

void ClaudeCodeDriver::handle_event(const Json& event, RunState& state,
                                    const PassRequest& request) {
    const auto& kind = get(event, "type");
    if (kind == "system") {
        on_system(event, state, request);
    } else if (kind == "stream_event") {
        emit_stream_block(event, request);
    } else if (kind == "assistant") {
        const Json none = Json::object();
        const auto& message = or_else(get(event, "message"), none);
        for (const auto& block : iterate(or_else(dict_get(message, "content"), Json::array()))) {
            if (block.is_object() && get(block, "type") == "tool_use") {
                const auto& raw_name = get(block, "name");
                const auto name = truthy(raw_name) ? py_str(raw_name) : std::string();
                const auto parts = py::split_on(name, "__");
                request.emit("tool", parts.back());
            }
        }
    } else if (kind == "result") {
        on_claude_result(event, state, request);
    }
}

PassOutcome ClaudeCodeDriver::finish(const RunState& state, std::optional<int> returncode,
                                     std::string_view stderr_tail) {
    if (!state.error.empty()) {
        return PassOutcome{.ok = false, .error = claude_error(state.error), .usage = state.usage};
    }
    if (!state.done) {
        return PassOutcome{
            .ok = false,
            .error = std::format("Claude Code exited (code {}) before finishing the pass{}",
                                 returncode ? std::to_string(*returncode) : std::string("None"),
                                 tail_detail(stderr_tail)),
            .usage = state.usage};
    }
    return PassOutcome{.ok = true, .text = state.text, .usage = state.usage};
}

void emit_stream_block(const Json& event, const PassRequest& request) {
    const Json none = Json::object();
    const auto& inner = or_else(get(event, "event"), none);
    const auto& content_block = dict_get(inner, "content_block");
    const auto& block = truthy(content_block) ? content_block : or_else(get(inner, "delta"), none);
    const auto& raw_type = dict_get(block, "type");
    const auto block_type = truthy(raw_type) ? py_str(raw_type) : std::string();
    if (contains_text(block_type, "thinking")) {
        request.emit("thinking");
    } else if (contains_text(block_type, "text")) {
        request.emit("writing");
    } else if (contains_text(block_type, "tool") || contains_text(block_type, "json")) {
        const auto& raw_name = get(block, "name");
        request.emit("tool", truthy(raw_name) ? py_str(raw_name) : std::string());
    }
}

Json claude_usage(const Json& event) {
    const Json none = Json::object();
    const auto& usage = or_else(dict_get(event, "usage"), none);
    std::int64_t prompt = 0;
    for (const auto key : PROMPT_TOKEN_KEYS) {
        const auto& value = dict_get(usage, key);
        prompt += truthy(value) ? to_int(value) : 0;
    }
    const auto& output = dict_get(usage, "output_tokens");
    const std::int64_t completion = truthy(output) ? to_int(output) : 0;
    const auto& turns = get(event, "num_turns");
    Json result{{"prompt_tokens", prompt},
                {"completion_tokens", completion},
                {"total_tokens", prompt + completion},
                {"requests", truthy(turns) ? to_int(turns) : 1}};
    // isinstance(cost, (int, float)): a bool is an int too.
    const auto& cost = get(event, "total_cost_usd");
    if (cost.is_number() || cost.is_boolean()) {
        result["cost_usd"] =
            cost.is_boolean() ? (cost.get<bool>() ? 1.0 : 0.0) : cost.get<double>();
    }
    return result;
}

void on_claude_result(const Json& event, RunState& state, const PassRequest& request) {
    state.done = true;
    state.usage = claude_usage(event);
    const auto& raw_text = get(event, "result");
    const auto text = truthy(raw_text) ? py_str(raw_text) : std::string();
    const auto& subtype = get(event, "subtype");
    if (truthy(get(event, "is_error")) || !(subtype.is_null() || subtype == "success")) {
        state.error = !text.empty() ? text : (truthy(subtype) ? py_str(subtype) : "error");
    } else {
        state.text = text;
    }
    request.emit("settled");
}

std::string claude_error(std::string_view message) {
    const auto text = py::strip(message);
    const auto lowered = py::lower(text);
    if (contains_text(lowered, "login") || contains_text(lowered, "not logged in") ||
        contains_text(lowered, "authentication")) {
        return "Claude Code is not signed in. Run `claude` once and sign in.";
    }
    return "Claude Code: " + py::slice(text, 0, 300);
}

} // namespace fv::agents::detail
