#include "agents/detail/headless.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <system_error>
#include <tuple>

#include "agents/detail/probe.h"
#include "agents/detail/pyvalue.h"
#include "base/log.h"
#include "base/utf8.h"
#include "process/agent_process.h"
#include "pyport/json.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::agents::detail {

namespace {

constexpr log::Logger logger("services.actions.agents");

using Clock = std::chrono::steady_clock;

// How long, after the tree is killed, the pipe readers get to reach the end of their pipes.
constexpr std::chrono::seconds PIPE_JOIN{2};

struct HelpCache {
    std::mutex mutex;
    std::map<std::tuple<std::string, std::string, std::vector<std::string>>, std::string> entries;
};

HelpCache& help_cache() {
    static auto* cache = new HelpCache();
    return *cache;
}

// open(path, "w", encoding="utf-8").write(text); throws filesystem_error as OSError would rise.
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

bool has_text(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// What a headless run's pipe readers gather.
struct Gathered {
    std::mutex mutex;
    std::condition_variable ended;
    int open = 2;
    std::string out;
    std::size_t out_size = 0;
    std::string out_line;
    std::string err_line;
    std::deque<std::string> err;

    // _read_stdout: whole lines are kept until STDOUT_MAX_BYTES is reached; the rest is read and
    // dropped.
    void keep_out(std::string_view line) {
        if (out_size < STDOUT_MAX_BYTES) {
            out.append(line);
            out_size += line.size();
        }
    }
    // _read_stderr: each non-empty line, right-stripped and cut to STDERR_LINE_MAX characters.
    void keep_err(std::string_view raw) {
        const auto line = py::rstrip(decode_utf8_replace(raw));
        if (line.empty()) {
            return;
        }
        err.push_back(py::slice(line, 0, STDERR_LINE_MAX));
        while (err.size() > STDERR_LINES) {
            err.pop_front();
        }
    }
};

// A ByteSink that splits a binary pipe at "\n", as iterating a binary file does, handing each
// line with its "\n" (and the unterminated rest at the end) to `keep` under the lock.
ByteSink line_sink(std::shared_ptr<Gathered> gathered, std::string Gathered::*partial,
                   void (Gathered::*keep)(std::string_view)) {
    return [gathered = std::move(gathered), partial, keep](std::span<const char> bytes) {
        {
            std::lock_guard lock(gathered->mutex);
            auto& pending = (*gathered).*partial;
            if (bytes.empty()) {
                if (!pending.empty()) {
                    ((*gathered).*keep)(pending);
                    pending.clear();
                }
                --gathered->open;
            } else {
                // What is pending holds no "\n" (each whole line was handed on), so only the new
                // bytes are searched: a long line arriving in many chunks is not rescanned.
                const auto searched = pending.size();
                pending.append(bytes.data(), bytes.size());
                std::size_t start = 0;
                for (auto end = pending.find('\n', searched); end != std::string::npos;
                     end = pending.find('\n', start)) {
                    ((*gathered).*keep)(std::string_view(pending).substr(start, end + 1 - start));
                    start = end + 1;
                }
                pending.erase(0, start);
            }
        }
        if (bytes.empty()) {
            gathered->ended.notify_all();
        }
    };
}

} // namespace

const std::vector<std::string_view>& codex_tool_feature_names() {
    static const std::vector<std::string_view> names{
        "shell_tool",
        "unified_exec",
        "code_mode",
        "code_mode_host",
        "apps",
        "browser_use",
        "browser_use_external",
        "browser_use_full_cdp_access",
        "in_app_browser",
        "computer_use",
        "image_generation",
        "view_image",
        "multi_agent",
        "multi_agent_v2",
        "plugins",
        "remote_plugin",
        "goals",
        "skill_search",
        "tool_suggest",
        "skill_mcp_dependency_install",
        "workspace_dependencies",
        "hooks",
        "memories",
        "worktrees",
        "sleep_tool",
        "request_permissions_tool",
        "standalone_web_search",
        "web_search_request",
        "web_search_cached",
        "js_repl",
    };
    return names;
}

bool valid_model(std::string_view model) {
    static const py::Regex pattern(R"re([\w.:/@\[\]+-]{1,200})re");
    return pattern.fullmatch(model).has_value();
}

std::string help(InstalledAgents& agents, const InstalledAgent& agent,
                 const std::vector<std::string>& args) {
    auto key = std::make_tuple(path_to_utf8(agent.path), agent.version, args);
    auto& cache = help_cache();
    {
        std::lock_guard lock(cache.mutex);
        if (const auto it = cache.entries.find(key); it != cache.entries.end()) {
            return it->second;
        }
    }
    std::string text;
    try {
        std::vector<std::string> argv{path_to_utf8(agent.path)};
        argv.insert(argv.end(), args.begin(), args.end());
        text = run_probe(*agents.launcher(), argv, HELP_TIMEOUT_S, agents.agent_workspace_dir(),
                         agents.agent_child_env())
                   .text;
    } catch (const std::exception&) {
        text.clear();
    }
    if (!text.empty()) {
        std::lock_guard lock(cache.mutex);
        cache.entries.insert_or_assign(std::move(key), text);
    }
    return text;
}

void forget_help() {
    auto& cache = help_cache();
    std::lock_guard lock(cache.mutex);
    cache.entries.clear();
}

std::vector<std::string> claude_ask_command(InstalledAgents& agents, const InstalledAgent& agent,
                                            std::string_view model,
                                            const std::filesystem::path& workdir,
                                            const CleanupPrompt* cleanup) {
    const auto help_text = help(agents, agent, {"--help"});
    if (!help_text.empty() && !has_text(help_text, "--tools")) {
        throw TooOld(
            std::format("Claude Code {} is too old to answer questions here. Run `claude update`.",
                        agent.version));
    }
    if (cleanup &&
        (!has_text(help_text, "--tools") || !has_text(help_text, "--strict-mcp-config") ||
         !has_text(help_text, "--settings") || !has_text(help_text, "--setting-sources") ||
         !has_text(help_text, "--no-session-persistence") ||
         !has_text(help_text, "--disable-slash-commands") ||
         !has_text(help_text, "--system-prompt-file") ||
         (!cleanup->effort.empty() && !has_text(help_text, "--effort")))) {
        throw TooOld("Claude Code cannot isolate this cleanup request.");
    }
    const auto mcp_file = workdir / "mcp.json";
    const auto settings_file = workdir / "settings.json";
    write_text(mcp_file, py::dumps(Json{{"mcpServers", Json::object()}}));
    Json settings{{"disableAllHooks", true}};
    if (!cleanup || cleanup->effort == "low") {
        settings["alwaysThinkingEnabled"] = false;
    }
    write_text(settings_file, py::dumps(settings));
    std::vector<std::string> argv{path_to_utf8(agent.path),
                                  "-p",
                                  "--output-format",
                                  "json",
                                  "--tools",
                                  "",
                                  "--mcp-config",
                                  path_to_utf8(mcp_file),
                                  "--strict-mcp-config",
                                  "--settings",
                                  path_to_utf8(settings_file)};
    if (cleanup) {
        const auto instructions = workdir / "instructions.md";
        write_text(instructions, cleanup->system_prompt);
        argv.insert(argv.end(),
                    {"--system-prompt-file", path_to_utf8(instructions), "--setting-sources", ""});
        if (!cleanup->effort.empty()) {
            argv.insert(argv.end(), {"--effort", std::string(cleanup->effort)});
        }
    } else {
        argv.insert(argv.end(), {"--system-prompt", std::string(ASK_INSTRUCTION)});
    }
    for (const std::string_view flag : {"--no-session-persistence", "--disable-slash-commands"}) {
        if (help_text.empty() || has_text(help_text, flag)) {
            argv.emplace_back(flag);
        }
    }
    std::string chosen(model);
    if (cleanup && chosen.empty()) {
        chosen = agents.config().configured_default_model(CLAUDE_CODE);
        if (!chosen.empty() && !valid_model(chosen)) {
            throw TooOld("Claude Code's configured default model cannot be preserved.");
        }
    }
    if (!chosen.empty()) {
        argv.emplace_back("--model");
        argv.push_back(std::move(chosen));
    }
    return argv;
}

std::vector<std::string> codex_tool_features(InstalledAgents& agents, const InstalledAgent& agent) {
    const auto listing = help(agents, agent, {"features", "list"});
    std::set<std::string> known;
    for (const auto& line : splitlines(listing)) {
        const auto words = py::split(line);
        if (!words.empty()) {
            known.insert(words.front());
        }
    }
    std::vector<std::string> features;
    for (const auto name : codex_tool_feature_names()) {
        if (known.contains(std::string(name))) {
            features.emplace_back(name);
        }
    }
    if (features.empty()) {
        // A run that fails on the flag is better than one that has a shell.
        features.emplace_back("shell_tool");
    }
    return features;
}

std::vector<std::string> codex_ask_command(InstalledAgents& agents, const InstalledAgent& agent,
                                           std::string_view model,
                                           const std::filesystem::path& workdir,
                                           const CleanupPrompt* cleanup) {
    const auto exec_help = help(agents, agent, {"exec", "--help"});
    if (!exec_help.empty() && !has_text(exec_help, "--ignore-user-config")) {
        throw TooOld(std::format("Codex {} is too old to answer questions here. {}", agent.version,
                                 agent_spec(CODEX).update_hint));
    }
    const auto instructions = workdir / "instructions.md";
    if (cleanup &&
        (!has_text(exec_help, "--ignore-user-config") || !has_text(exec_help, "--ephemeral"))) {
        throw TooOld("Codex cannot isolate this cleanup request.");
    }
    write_text(instructions, cleanup ? cleanup->system_prompt : ASK_INSTRUCTION);
    std::vector<std::string> argv{path_to_utf8(agent.path), "exec", "--json"};
    if (exec_help.empty() || has_text(exec_help, "--ephemeral")) {
        argv.emplace_back("--ephemeral");
    }
    // The user's config brings their MCP servers, hooks and notify program; ignoring it keeps
    // sign-in, which lives beside it.
    for (std::string arg :
         {std::string("--skip-git-repo-check"), std::string("--ignore-user-config"),
          std::string("-s"), std::string("read-only"), std::string("-C"),
          path_to_utf8(cleanup ? workdir : agents.agent_workspace_dir())}) {
        argv.push_back(std::move(arg));
    }
    auto instructions_text = path_to_utf8(instructions);
#ifdef _WIN32
    std::replace(instructions_text.begin(), instructions_text.end(), '\\', '/');
#endif
    for (const auto& override_ :
         {std::string("approval_policy=never"), std::string("web_search=disabled"),
          std::string("project_doc_max_bytes=0"), "model_instructions_file=" + instructions_text}) {
        argv.emplace_back("-c");
        argv.push_back(override_);
    }
    if (!cleanup || !cleanup->effort.empty()) {
        argv.emplace_back("-c");
        argv.push_back("model_reasoning_effort=" + std::string(cleanup ? cleanup->effort : "low"));
    }
    auto features = codex_tool_features(agents, agent);
    if (cleanup && std::ranges::find(features, "shell_tool") == features.end()) {
        throw TooOld("Codex did not advertise a shell tool switch for transcript cleanup.");
    }
    for (auto& feature : features) {
        argv.emplace_back("--disable");
        argv.push_back(std::move(feature));
    }
    std::string chosen(model);
    if (chosen.empty()) {
        const auto configured = agents.config().configured_default_model(CODEX);
        if (cleanup && !configured.empty() && !valid_model(configured)) {
            throw TooOld("Codex's configured default model cannot be preserved.");
        }
        chosen = valid_model(configured) ? configured : "";
    }
    if (!chosen.empty()) {
        argv.emplace_back("-m");
        argv.push_back(chosen);
    }
    argv.emplace_back("-");
    return argv;
}

HeadlessRun run_headless(InstalledAgents& agents, const std::vector<std::string>& argv,
                         std::string_view prompt, Clock::time_point deadline,
                         const CancelToken& cancel, const std::filesystem::path& cwd) {
    ProcessSpec spec;
    try {
        spec.program = path_from_utf8(argv.at(0));
        spec.args.assign(argv.begin() + 1, argv.end());
        spec.cwd = cwd.empty() ? agents.agent_workspace_dir() : cwd;
        spec.env = agents.agent_child_env();
    } catch (const std::exception&) {
        logger.info("An agent couldn't start (no working folder)");
        return {.status = "unstartable"};
    }
    spec.own_tree = true;
    spec.pipes = true;
    if (cancel.cancelled() || Clock::now() >= deadline) {
        return {.status = cancel.cancelled() ? "canceled" : "timeout"};
    }
    auto started = agents.launcher()->start(spec);
    if (!started) {
        logger.info("An agent couldn't start ({})", started.error());
        return {.status = "unstartable"};
    }
    auto& process = **started;
    auto gathered = std::make_shared<Gathered>();
    process.on_bytes(line_sink(gathered, &Gathered::out_line, &Gathered::keep_out),
                     line_sink(gathered, &Gathered::err_line, &Gathered::keep_err));
    // Binary: a text-mode stdin would turn the prompt's line breaks into CRLF on Windows.
    if (cancel.cancelled() || Clock::now() >= deadline) {
        kill_process_tree(process);
        process.release_pipes(std::chrono::milliseconds(0));
        return {.status = cancel.cancelled() ? "canceled" : "timeout"};
    }
    process.write_stdin(prompt);
    process.close_stdin();

    std::string status;
    for (;;) {
        if (process.wait(EXIT_POLL)) {
            status = "done";
            break;
        }
        if (cancel.cancelled()) {
            status = "canceled";
            break;
        }
        if (Clock::now() >= deadline) {
            status = "timeout";
            break;
        }
    }
    kill_process_tree(process);
    {
        std::unique_lock lock(gathered->mutex);
        gathered->ended.wait_for(lock, PIPE_JOIN, [&] { return gathered->open <= 0; });
    }
    process.release_pipes(std::chrono::milliseconds(0));

    HeadlessRun run{.status = status, .returncode = process.exit_code()};
    std::lock_guard lock(gathered->mutex);
    run.stdout_text = decode_utf8_replace(gathered->out);
    for (std::size_t i = 0; i < gathered->err.size(); ++i) {
        if (i > 0) {
            run.stderr_text += '\n';
        }
        run.stderr_text += gathered->err[i];
    }
    return run;
}

} // namespace fv::agents::detail
