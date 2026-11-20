#include "agents/agent_actions.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>

#include "agents/acp.h"
#include "agents/detail/headless.h"
#include "agents/detail/home.h"
#include "agents/detail/opencode_cleanup.h"
#include "agents/detail/pyvalue.h"
#include "agents/detail/shim.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::agents {

namespace {

constexpr log::Logger logger("services.actions.agents");

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

constexpr std::string_view CHOOSE_AGENT = "Choose Claude Code, Codex or OpenCode for this action.";

constexpr std::array<std::string_view, 9> SIGNED_OUT_WORDS{
    "not logged in", "login",        "log in", "sign in",         "signed out",
    "authenticat",   "unauthorized", "401",    "invalid api key",
};

// AGENT_NAMES of services/actions/model.py: the agents an action can use are the installed-agent
// specs, under the same names.
std::optional<std::string> agent_name(std::string_view agent_id) {
    if (const auto* spec = find_agent_spec(agent_id)) {
        return std::string(spec->name);
    }
    return std::nullopt;
}

std::string name_of(std::string_view agent_id) {
    return std::string(agent_spec(agent_id).name);
}

// urllib.parse.quote(text, safe=""): every UTF-8 byte but letters, digits and "_.-~" as %XX.
std::string quote(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-' ||
            byte == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0xF];
        }
    }
    return out;
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// os.path.isabs as Python 3.12 has it: on Windows a path that starts with a slash counts.
bool is_abs(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return true;
    }
    if constexpr (kWindows) {
        const auto text = path_to_utf8(path);
        return !text.empty() && (text.front() == '\\' || text.front() == '/');
    }
    return false;
}

bool any_space(std::string_view text) {
    const auto points = py::to_code_points(text);
    return std::any_of(points.begin(), points.end(), [](char32_t cp) { return py::is_space(cp); });
}

// tempfile.mkdtemp(prefix=prefix, dir=parent): a new folder with eight random characters after
// the prefix. Throws filesystem_error when none can be made.
std::filesystem::path make_temp_dir(const std::filesystem::path& parent, std::string_view prefix) {
    static constexpr std::string_view characters = "abcdefghijklmnopqrstuvwxyz0123456789_";
    static std::mutex mutex;
    static std::mt19937_64 random(std::random_device{}());
    for (int attempt = 0; attempt < 10'000; ++attempt) {
        std::string name(prefix);
        {
            std::lock_guard lock(mutex);
            for (int i = 0; i < 8; ++i) {
                name += characters[random() % characters.size()];
            }
        }
        const auto path = parent / path_from_utf8(name);
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
            // os.path.abspath. The folder exists either way: a failure here keeps the path as
            // made rather than handing back an empty one.
            auto absolute = std::filesystem::absolute(path, error);
            return error ? path : absolute;
        }
        if (error) {
            throw std::filesystem::filesystem_error("mkdtemp", path, error);
        }
    }
    throw std::filesystem::filesystem_error("mkdtemp", parent,
                                            std::make_error_code(std::errc::file_exists));
}

// The user's home folder: the set instance's, else this process's.
std::filesystem::path home_folder() {
    if (const auto agents = installed_agents_if_set()) {
        return agents->home();
    }
    return path_from_utf8(detail::home_folder(system_env()));
}

// A durable folder per handoff, outside the app's source and data folders. A visible session can
// keep working and be resumed after the app exits, so it is never removed as a temporary
// workspace. Its name holds only the time and a random suffix, never the words.
std::filesystem::path new_handoff_folder() {
    const auto home = installed_agents().home();
    if (home.empty()) {
        throw std::filesystem::filesystem_error(
            "no home folder", std::make_error_code(std::errc::no_such_file_or_directory));
    }
    const auto root = home / path_from_utf8(std::string(identity().product) + " Agent Tasks");
    std::filesystem::create_directories(root);
    const auto& clock = system_clock();
    const auto now = std::chrono::floor<std::chrono::seconds>(clock.utc());
    std::string prefix;
    if (const auto* zone = clock.zone()) {
        prefix = std::format("task-{:%Y-%m-%d-%H%M%S}-", zone->to_local(now));
    } else {
        prefix = std::format("task-{:%Y-%m-%d-%H%M%S}-", now);
    }
    return make_temp_dir(root, prefix);
}

std::string no_app_message(std::string_view agent_id, std::string_view text) {
    if (agent_id == CLAUDE_CODE) {
        if (py::len(text) > CLAUDE_APP_TEXT_MAX) {
            return "That's too long for the Claude app. Choose Terminal for long requests.";
        }
        return "Claude Code can't open in an app here. Install the Claude app, or choose "
               "Terminal.";
    }
    if (agent_id == CODEX) {
        return "Codex can't open in an app here. Install the ChatGPT app, or choose Terminal.";
    }
    return "OpenCode has no app link. Choose Terminal instead.";
}

std::string handoff_terminal(std::string_view agent_id, std::string_view text,
                             const std::filesystem::path& folder) {
    const auto name = name_of(agent_id);
    const auto agent = detail::find_installed(agent_id);
    if (!agent) {
        throw HandoffError(name + " isn't installed.");
    }
    auto& agents = installed_agents();
    try {
        open_in_terminal(terminal_argv(agent_id, terminal_command(*agent), text),
                         folder.empty() ? agents.home() : folder, agents.agent_child_env());
    } catch (const LaunchError& error) {
        throw HandoffError(error.what());
    }
    logger.info("Handed off to {} in a terminal", agent_id);
    return name + " in a terminal";
}

std::string handoff_session(std::string_view agent_id, std::string_view text,
                            const std::filesystem::path& folder, std::string_view surface) {
    const auto name = name_of(agent_id);
    if (surface != SURFACE_TERMINAL) {
        std::string failure;
        for (const auto& uri : app_links(agent_id, text, folder)) {
            const auto scheme = uri.substr(0, uri.find(':'));
            const auto found = handler_available(scheme);
            if (found == false || (!found && surface == SURFACE_AUTO)) {
                continue;
            }
            try {
                open_uri(uri);
            } catch (const NoAnswer& error) {
                // Windows may still open this link late; another link or a terminal now could
                // start the agent twice.
                throw HandoffError(error.what());
            } catch (const LaunchError& error) {
                failure = error.what();
                logger.info("Agent link for {} didn't open", agent_id);
                continue;
            }
            logger.info("Handed off to {} through its {} link", agent_id, scheme);
            return "Opened " + name;
        }
        if (surface == SURFACE_APP) {
            throw HandoffError(failure.empty() ? no_app_message(agent_id, text) : failure);
        }
    }
    return "Opened " + handoff_terminal(agent_id, text, folder);
}

// ---- asks ----

AskResult refused(std::string message) {
    return AskResult{.ok = false, .message = std::move(message)};
}

// Removes a folder and everything in it when the scope ends, ignoring errors.
struct RemoveTree {
    std::filesystem::path path;
    ~RemoveTree() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

AskResult ask_headless(const InstalledAgent& agent, std::string_view text, std::string_view model,
                       Clock::time_point deadline, const CancelToken& cancel,
                       const detail::CleanupPrompt* cleanup = nullptr) {
    const auto name = name_of(agent.id);
    auto& agents = installed_agents();
    // In the temporary folder the agent workspace is in, as both are tempfile's.
    const auto temp = agents.agent_workspace_dir().parent_path();
    detail::HeadlessRun run;
    {
        const RemoveTree workdir{make_temp_dir(temp, py::lower(identity().product) + "-ask-")};
        try {
            const auto argv =
                agent.id == CLAUDE_CODE
                    ? detail::claude_ask_command(agents, agent, model, workdir.path, cleanup)
                    : detail::codex_ask_command(agents, agent, model, workdir.path, cleanup);
            if (cleanup && !agents.config().supports_isolated_cleanup(agent.id)) {
                return refused("The agent's configured destination cannot be preserved.");
            }
            run = detail::run_headless(agents, argv, text, deadline, cancel,
                                       cleanup ? workdir.path : std::filesystem::path{});
        } catch (const detail::TooOld& error) {
            return refused(error.what());
        }
    }
    if (run.status == "canceled") {
        return AskResult{.ok = false, .message = "Stopped asking " + name + ".", .stopped = true};
    }
    if (run.status == "timeout") {
        return refused(name + " didn't answer in time.");
    }
    if (run.status == "unstartable") {
        return refused(name + " couldn't be started.");
    }
    if (agent.id == CLAUDE_CODE) {
        return detail::claude_answer(run);
    }
    return detail::codex_answer(run);
}

double seconds_until(Clock::time_point deadline) {
    return std::chrono::duration<double>(deadline - Clock::now()).count();
}

// Cancels the turn in flight and says why the ask ended.
AskResult opencode_failure(AcpConnection& connection, const std::string& session_id,
                           const AcpError& error, Clock::time_point deadline,
                           const CancelToken& cancel) {
    const auto name = name_of(OPENCODE);
    if (!session_id.empty()) {
        connection.notify("session/cancel", Json{{"sessionId", session_id}});
    }
    if (cancel.cancelled()) {
        return AskResult{.ok = false, .message = "Stopped asking " + name + ".", .stopped = true};
    }
    if (Clock::now() >= deadline) {
        return refused(name + " didn't answer in time.");
    }
    const std::string reason = error.what();
    if (py::lower(reason).find("auth") != std::string::npos) {
        return refused(detail::signed_out(OPENCODE));
    }
    return refused(detail::failure(OPENCODE, reason));
}

void close_opencode(AcpConnection& connection, const std::string& session_id, bool can_delete) {
    if (!session_id.empty() && can_delete) {
        try {
            // Keeps asks out of the user's OpenCode history.
            connection.request("session/delete", Json{{"sessionId", session_id}}, 3.0);
        } catch (const AcpError&) {
            logger.debug("Couldn't delete the OpenCode ask session");
        }
    }
    connection.close(1.0);
}

// value.get(...) of a value Python calls .get on: an AttributeError for anything but a dict.
const Json& require_dict(const Json& value) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return value;
}

// (value or {}): a falsy value reads as an empty dict.
Json or_empty(const Json& value) {
    return detail::truthy(value) ? value : Json::object();
}

// One ACP session with every OpenCode tool denied, deleted afterwards. `opencode acp` runs
// in-process (no background service), and the overlay replaces both primary agents' prompts and
// permissions for this process only, as Meeting Mode does.
AskResult ask_opencode(const InstalledAgent& agent, std::string_view text, std::string_view model,
                       Clock::time_point deadline, const CancelToken& cancel) {
    const auto name = name_of(OPENCODE);
    auto& agents = installed_agents();
    auto answer = std::make_shared<detail::AcpAnswer>();
    const auto remaining = [&] {
        return std::max(0.1, seconds_until(deadline));
    };
    const auto workspace = agents.agent_workspace_dir();

    AcpConnection connection(
        agents.launcher(), {path_to_utf8(agent.path), "acp"}, detail::opencode_env(agents),
        workspace,
        [answer](const std::string& method, const Json& params) {
            answer->on_notification(method, params);
        },
        [](const std::string& method, const Json& params) {
            return detail::refuse_acp_request(method, params);
        });
    try {
        connection.start();
    } catch (const AcpError&) {
        return refused(name + " couldn't be started.");
    }
    std::string session_id;
    bool can_delete = false;
    std::optional<AskResult> outcome;
    Json result;
    {
        struct Close {
            AcpConnection& connection;
            const std::string& session_id;
            const bool& can_delete;
            ~Close() {
                try {
                    close_opencode(connection, session_id, can_delete);
                } catch (const std::exception&) {
                    logger.debug("Couldn't close the OpenCode ask");
                }
            }
        } close{connection, session_id, can_delete};
        try {
            const auto info = acp_initialize(connection, remaining());
            const auto capabilities =
                or_empty(detail::get(require_dict(or_empty(detail::get(info, "agentCapabilities"))),
                                     "sessionCapabilities"));
            can_delete = detail::contains(capabilities, "delete");
            auto session = connection.request(
                "session/new",
                Json{{"cwd", path_to_utf8(workspace)}, {"mcpServers", Json::array()}}, remaining(),
                cancel);
            if (!session.is_object()) {
                session = Json::object();
            }
            const auto& raw_id = detail::get(session, "sessionId");
            session_id = detail::truthy(raw_id) ? detail::py_str(raw_id) : "";
            if (session_id.empty()) {
                outcome = refused(name + " didn't open a session.");
            } else if (const auto wanted = detail::opencode_model(
                           session, model,
                           // `model or configured_default_model(...)`: the user's config is read
                           // only when no model was chosen.
                           model.empty() ? agents.config().configured_default_model(OPENCODE)
                                         : std::string());
                       !wanted) {
                outcome = refused(std::format(
                    "OpenCode has no model {} with your current providers. Pick another in "
                    "Settings.",
                    model));
            } else {
                if (!wanted->empty() &&
                    Json(*wanted) !=
                        detail::get(acp_config_option(session, "model"), "currentValue")) {
                    connection.request(
                        "session/set_config_option",
                        Json{{"sessionId", session_id}, {"configId", "model"}, {"value", *wanted}},
                        std::min(30.0, remaining()), cancel);
                }
                result = connection.request(
                    "session/prompt",
                    Json{{"sessionId", session_id},
                         {"prompt",
                          Json::array({Json{{"type", "text"}, {"text", std::string(text)}}})}},
                    remaining(), cancel);
            }
        } catch (const AcpError& error) {
            outcome = opencode_failure(connection, session_id, error, deadline, cancel);
        }
    }
    if (outcome) {
        return *outcome;
    }
    return detail::opencode_result(result, answer->text());
}

} // namespace

std::map<std::string, bool> available() {
    const auto agents = installed_agents_if_set();
    if (!agents) {
        return {};
    }
    const auto scan = agents->cached_scan();
    if (!scan) {
        return {};
    }
    std::map<std::string, bool> found;
    for (const auto id : agent_order()) {
        const auto it = scan->find(std::string(id));
        found.emplace(std::string(id), it != scan->end() && it->second.has_value());
    }
    return found;
}

std::string handoff(const HandoffRequest& request) {
    const auto name = agent_name(request.agent_id);
    if (!name) {
        throw HandoffError(std::string(CHOOSE_AGENT));
    }
    if (request.surface != SURFACE_AUTO && request.surface != SURFACE_APP &&
        request.surface != SURFACE_TERMINAL) {
        throw HandoffError("Choose where the agent opens: app, terminal or automatic.");
    }
    if (py::strip(request.text).empty()) {
        throw HandoffError("There's nothing to send to " + *name + ".");
    }
    auto folder = request.folder;
    std::error_code error;
    if (!request.new_folder && !folder.empty() &&
        !(is_abs(folder) && std::filesystem::is_directory(folder, error))) {
        throw HandoffError("This action's working folder doesn't exist anymore. "
                           "Choose another in Settings.");
    }
    if (request.new_folder) {
        try {
            folder = new_handoff_folder();
        } catch (const std::filesystem::filesystem_error&) {
            throw HandoffError(std::format("Couldn't create a new working folder. "
                                           "Check that ~/{} Agent Tasks is writable.",
                                           identity().product));
        }
    }
    return handoff_session(request.agent_id, request.text, folder, request.surface);
}

std::vector<std::string> app_links(std::string_view agent_id, std::string_view text,
                                   const std::filesystem::path& folder,
                                   const std::filesystem::path& home) {
    const auto words = quote(text);
    const auto place = folder.empty() ? std::string() : quote(path_to_utf8(folder));
    std::vector<std::string> links;
    if (agent_id == CLAUDE_CODE) {
        if (py::len(text) <= CLAUDE_APP_TEXT_MAX) {
            // Claude Desktop asks before using a folder, so name one only when set.
            links.push_back("claude://code/new?q=" + words +
                            (place.empty() ? std::string() : "&folder=" + place));
        }
        if (py::len(text) <= CLAUDE_CLI_TEXT_MAX) {
            std::string cwd = place;
            if (cwd.empty()) {
                const auto user_home = home.empty() ? home_folder() : home;
                // os.path.expanduser("~") leaves "~" when there is no home.
                cwd = quote(user_home.empty() ? std::string("~") : path_to_utf8(user_home));
            }
            links.push_back("claude-cli://open?q=" + words + "&cwd=" + cwd);
        }
    } else if (agent_id == CODEX) {
        links.push_back("codex://new?prompt=" + words +
                        (place.empty() ? std::string() : "&path=" + place));
    }
    std::erase_if(links, [](const std::string& uri) { return py::len(uri) > LINK_MAX_CHARS; });
    return links;
}

std::vector<std::string> terminal_argv(std::string_view agent_id, std::vector<std::string> command,
                                       std::string_view text) {
    if (agent_id == CLAUDE_CODE) {
        // Claude Code reads a lone word naming one of its commands (update, purge, ...) as that
        // command even after "--"; words with a space never are one.
        command.emplace_back("--");
        command.push_back(any_space(text) ? std::string(text) : std::string(text) + " ");
        return command;
    }
    if (agent_id == CODEX) {
        command.emplace_back("--");
        command.emplace_back(text);
        return command;
    }
    command.push_back("--prompt=" + std::string(text));
    return command;
}

std::vector<std::string> terminal_argv(std::string_view agent_id, const std::filesystem::path& cmd,
                                       std::string_view text) {
    return terminal_argv(agent_id, std::vector<std::string>{path_to_utf8(cmd)}, text);
}

std::vector<std::string> terminal_command(const InstalledAgent& agent) {
    const auto path = path_to_utf8(agent.path);
    const auto lower = py::lower(path);
    // Windows runs these through cmd.exe, which would read the words as commands.
    if (!lower.ends_with(".cmd") && !lower.ends_with(".bat")) {
        return {path};
    }
    for (const auto& candidate : installed_agents().candidate_paths(agent.id)) {
        auto text = path_to_utf8(candidate);
        if (py::lower(text).ends_with(".exe")) {
            return {std::move(text)};
        }
    }
    if (auto command = npm_shim_command(agent.path)) {
        return std::move(*command);
    }
    return {path};
}

std::optional<std::vector<std::string>> npm_shim_command(const std::filesystem::path& shim) {
    std::optional<std::string> path_env;
    if (const auto agents = installed_agents_if_set()) {
        path_env = agents->env("PATH");
    } else {
        path_env = system_env().get("PATH");
    }
    return detail::npm_shim_command(shim, path_env);
}

AskResult ask(std::string_view agent_id, std::string_view text, std::string_view model,
              double timeout_s, const CancelToken& cancel) {
    const auto name = agent_name(agent_id);
    if (!name) {
        return refused(std::string(CHOOSE_AGENT));
    }
    if (py::strip(text).empty()) {
        return refused("There's nothing to ask " + *name + ".");
    }
    if (!model.empty() && !detail::valid_model(model)) {
        return refused("That model name can't be used. Pick another in Settings.");
    }
    const auto agent = detail::find_installed(agent_id);
    if (!agent) {
        return refused(*name + " isn't installed.");
    }
    // A year at most, so an infinite limit can't overflow the clock; NaN reads as 1 s, as max()
    // gives it in Python.
    const double limit = std::min(std::max(1.0, timeout_s), 365.0 * 24 * 3600);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(limit));
    const auto result = agent_id == OPENCODE ? ask_opencode(*agent, text, model, deadline, cancel)
                                             : ask_headless(*agent, text, model, deadline, cancel);
    logger.info("Asked {}: {}", agent_id, result.ok ? "answered" : "no answer");
    return result;
}

bool valid_agent_model_id(std::string_view model) {
    return detail::valid_model(model);
}

AskResult ask_for_cleanup(std::string_view agent_id, std::string_view text,
                          std::string_view system_prompt, std::string_view model,
                          std::string_view effort, double timeout_s, const CancelToken& cancel) {
    if (cancel.cancelled()) {
        return AskResult{.message = "Cleanup canceled.", .stopped = true};
    }
    constexpr std::array<std::string_view, 9> efforts{"",     "off",   "minimal", "low",  "medium",
                                                      "high", "xhigh", "max",     "ultra"};
    if ((!model.empty() && !valid_agent_model_id(model)) ||
        std::ranges::find(efforts, effort) == efforts.end() || !(timeout_s > 0.0)) {
        return refused("Invalid installed agent cleanup request.");
    }
    if (agent_id != CLAUDE_CODE && agent_id != CODEX && agent_id != OPENCODE) {
        return refused("This agent is unavailable for transcript cleanup.");
    }
    const auto limit = std::min(timeout_s, 365.0 * 24 * 3600);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(limit));
    try {
        const auto agent = detail::find_installed(agent_id);
        if (!agent || !agent->usable()) {
            return refused("The installed agent is unavailable for transcript cleanup.");
        }
        if (cancel.cancelled() || Clock::now() >= deadline) {
            return {.message = "Cleanup ended before launch.", .stopped = cancel.cancelled()};
        }
        auto& agents = installed_agents();
        if (agent_id != OPENCODE && !agents.config().supports_isolated_cleanup(agent_id)) {
            return refused("The agent's configured destination cannot be preserved.");
        }
        const std::string prompt = std::string(system_prompt) + std::string(kCleanupClosing);
        const detail::CleanupPrompt instructions{prompt, effort == "off" ? "" : effort};
        const auto result =
            agent_id == OPENCODE
                ? detail::ask_opencode_cleanup(agents, *agent, text, system_prompt, model, effort,
                                               deadline, cancel)
                : ask_headless(*agent, text, model, deadline, cancel, &instructions);
        if (cancel.cancelled() || Clock::now() >= deadline || !result.ok) {
            return {.message = "Installed agent cleanup failed.", .stopped = cancel.cancelled()};
        }
        return result;
    } catch (...) {
        return {.message = "Installed agent cleanup failed.", .stopped = cancel.cancelled()};
    }
}

// ---- reading what the agents print, and the messages ----

namespace detail {

std::optional<InstalledAgent> find_installed(std::string_view agent_id) {
    auto& agents = installed_agents();
    std::optional<InstalledAgent> agent;
    if (const auto scan = agents.cached_scan()) {
        if (const auto it = scan->find(std::string(agent_id)); it != scan->end()) {
            agent = it->second;
        }
    }
    if (agent && is_file(agent->path)) {
        return agent;
    }
    return agents.find_agent(agent_id);
}

std::optional<Json> claude_result(std::string_view stdout_text) {
    std::vector<std::string> chunks{py::strip(stdout_text)};
    auto lines = splitlines(stdout_text);
    chunks.insert(chunks.end(), lines.rbegin(), lines.rend());
    for (const auto& chunk : chunks) {
        Json data;
        try {
            data = py::loads(chunk);
        } catch (const py::ValueError&) {
            continue;
        }
        const auto items = data.is_array() ? data : Json::array({data});
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->is_object() && get(*it, "type") == "result") {
                return std::optional<Json>(*it);
            }
        }
    }
    return std::nullopt;
}

AskResult claude_answer(const HeadlessRun& run) {
    const auto result = claude_result(run.stdout_text);
    if (!result) {
        return refused(stopped(CLAUDE_CODE, run));
    }
    const auto& raw = get(*result, "result");
    const auto text = truthy(raw) ? py_str(raw) : std::string();
    const auto& subtype = get(*result, "subtype");
    if (truthy(get(*result, "is_error")) || !(subtype.is_null() || subtype == "success")) {
        return refused(failure(CLAUDE_CODE, text.empty() ? py_str(subtype) : text));
    }
    return answered(CLAUDE_CODE, text);
}

std::vector<Json> codex_events(std::string_view stdout_text) {
    std::vector<Json> events;
    for (const auto& line : splitlines(stdout_text)) {
        Json event;
        try {
            event = py::loads(line);
        } catch (const py::ValueError&) {
            continue;
        }
        if (event.is_object()) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

AskResult codex_answer(const HeadlessRun& run) {
    std::string answer;
    std::string error;
    bool done = false;
    for (const auto& event : codex_events(run.stdout_text)) {
        const auto& kind = get(event, "type");
        const auto& raw_item = get(event, "item");
        const auto item = raw_item.is_object() ? raw_item : Json::object();
        if (kind == "item.completed" && get(item, "type") == "agent_message") {
            const auto& text = get(item, "text");
            answer = truthy(text) ? py_str(text) : "";
        } else if (kind == "turn.completed") {
            done = true;
        } else if (kind == "turn.failed") {
            const auto error_dict = or_empty(get(event, "error"));
            const auto& message = get(require_dict(error_dict), "message");
            error = truthy(message) ? py_str(message) : "the turn failed";
        } else if (kind == "error") {
            const auto& message = get(event, "message");
            error = truthy(message) ? py_str(message) : "error";
        }
    }
    if (done && !py::strip(answer).empty()) {
        return answered(CODEX, answer); // an earlier error was a retried hiccup
    }
    if (!error.empty()) {
        return refused(failure(CODEX, codex_message(error)));
    }
    if (done) {
        return answered(CODEX, answer);
    }
    return refused(stopped(CODEX, run));
}

std::string codex_message(std::string_view message) {
    Json data;
    try {
        data = py::loads(message);
    } catch (const py::ValueError&) {
        return std::string(message);
    }
    if (!data.is_object()) {
        return std::string(message);
    }
    const auto error_dict = or_empty(get(data, "error"));
    const auto& inner = get(require_dict(error_dict), "message");
    if (inner.is_string() && !inner.get_ref<const std::string&>().empty()) {
        return inner.get<std::string>();
    }
    return std::string(message);
}

void AcpAnswer::on_notification(const std::string& method, const Json& params) {
    if (method != "session/update") {
        return;
    }
    const auto& update = get(params, "update");
    const auto& kind = get(update, "sessionUpdate");
    std::lock_guard lock(mutex_);
    if (kind == "agent_message_chunk") {
        const auto& chunk = get(get(update, "content"), "text");
        if (chunk.is_string()) {
            chunks_.push_back(chunk.get<std::string>());
        }
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        chunks_.clear(); // narration before a tool call isn't the answer
    }
}

std::string AcpAnswer::text() const {
    std::lock_guard lock(mutex_);
    std::string joined;
    for (const auto& chunk : chunks_) {
        joined += chunk;
    }
    return joined;
}

std::string opencode_overlay() {
    const Json deny_all =
        Json::array({Json{{"action", "*"}, {"resource", "*"}, {"effect", "deny"}}});
    Json personas = Json::object();
    for (const auto* persona : {"build", "plan"}) {
        personas[persona] =
            Json{{"system", std::string(ASK_INSTRUCTION)}, {"permissions", deny_all}};
    }
    return py::dumps(Json{{"agents", personas}});
}

std::map<std::string, std::string> opencode_env(const InstalledAgents& agents) {
    return agents.agent_child_env({
        {"OPENCODE_CONFIG_CONTENT", opencode_overlay()},
        {"OPENCODE_DISABLE_PROJECT_CONFIG", "1"},
        {"OPENCODE_DISABLE_AUTOUPDATE", "1"},
        {"OPENCODE_DISABLE_FILEWATCHER", "1"},
    });
}

Json refuse_acp_request(const std::string& method, const Json& params) {
    if (method == "session/request_permission") {
        for (const auto& option : iterate(get(params, "options"))) {
            if (!option.is_object()) {
                continue;
            }
            const auto kind = option.contains("kind") ? py_str(option.at("kind")) : std::string();
            if (kind.starts_with("reject")) {
                return Json{{"outcome",
                             Json{{"outcome", "selected"}, {"optionId", get(option, "optionId")}}}};
            }
        }
        return Json{{"outcome", Json{{"outcome", "cancelled"}}}};
    }
    throw AcpError(method + " isn't available when asking", -32601);
}

std::optional<std::string> opencode_model(const Json& session, std::string_view model,
                                          std::string_view configured) {
    std::string wanted(model.empty() ? configured : model);
    if (!wanted.empty()) {
        const auto choices = acp_model_choices(session);
        const bool offered = std::any_of(choices.begin(), choices.end(),
                                         [&](const AgentModel& m) { return m.value == wanted; });
        if (!offered) {
            if (!model.empty()) {
                return std::nullopt;
            }
            wanted.clear();
        }
    }
    return wanted;
}

AskResult opencode_result(const Json& result, std::string_view answer) {
    const auto name = name_of(OPENCODE);
    const auto reason = result.is_object() ? get(result, "stopReason") : Json();
    if (reason == "cancelled") {
        return AskResult{.ok = false, .message = "Stopped asking " + name + ".", .stopped = true};
    }
    if (reason == "refusal") {
        return refused("OpenCode's model wouldn't answer that.");
    }
    return answered(OPENCODE, answer);
}

AskResult answered(std::string_view agent_id, std::string_view answer) {
    const auto name = name_of(agent_id);
    auto text = py::strip(answer);
    if (text.empty()) {
        return refused(name + " didn't answer.");
    }
    return AskResult{.ok = true, .answer = std::move(text), .message = name + " answered"};
}

std::string signed_out(std::string_view agent_id) {
    return py::strip(name_of(agent_id) + " is signed out. " + sign_in_hint(agent_id));
}

namespace {

bool says_signed_out(std::string_view text) {
    const auto lower = py::lower(text);
    return std::any_of(
        SIGNED_OUT_WORDS.begin(), SIGNED_OUT_WORDS.end(),
        [&](std::string_view word) { return lower.find(word) != std::string::npos; });
}

} // namespace

std::string failure(std::string_view agent_id, std::string_view error) {
    if (says_signed_out(error)) {
        return signed_out(agent_id);
    }
    std::string joined;
    for (const auto& word : py::split(error)) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += word;
    }
    const auto shown = py::slice(joined, 0, 200);
    const auto name = name_of(agent_id);
    return shown.empty() ? name + " couldn't answer." : name + " couldn't answer: " + shown;
}

std::string stopped(std::string_view agent_id, const HeadlessRun& run) {
    std::vector<std::string> lines;
    for (auto& line : splitlines(run.stderr_text)) {
        if (!py::strip(line).empty()) {
            lines.push_back(std::move(line));
        }
    }
    if (!lines.empty() && says_signed_out(lines.back())) {
        return signed_out(agent_id);
    }
    const auto last = lines.empty() ? std::string(".") : ": " + py::slice(lines.back(), 0, 200);
    return name_of(agent_id) + " stopped before answering" + last;
}

} // namespace detail

} // namespace fv::agents
