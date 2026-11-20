#include "agents/installed_agents.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "agents/acp.h"
#include "agents/detail/home.h"
#include "agents/detail/probe.h"
#include "agents/detail/pyvalue.h"
#include "agents/detail/thread_name.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "process/process_env.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::agents {

namespace {

constexpr log::Logger logger("services.installed_agents");

constexpr double VERSION_TIMEOUT_S = 6.0;
constexpr double AUTH_TIMEOUT_S = 10.0;

#ifdef _WIN32
constexpr bool kWindows = true;
constexpr char kPathSeparator = ';';
#else
constexpr bool kWindows = false;
constexpr char kPathSeparator = ':';
#endif
#ifdef __APPLE__
constexpr bool kMac = true;
#else
constexpr bool kMac = false;
#endif

const std::vector<AgentSpec>& specs_table() {
    static const std::vector<AgentSpec> specs{
        AgentSpec{
            .id = CLAUDE_CODE,
            .name = "Claude Code",
            .vendor = "Anthropic",
            .commands = {"claude"},
            .transport = "headless",
            .install_url = "https://docs.anthropic.com/en/docs/claude-code/setup",
            .install_hint = "npm install -g @anthropic-ai/claude-code",
            .monogram = "CC",
            .accent = "#D97757",
            .min_version = {2, 0, 0},
            .update_hint = "Run `claude update`.",
        },
        AgentSpec{
            .id = CODEX,
            .name = "Codex",
            .vendor = "OpenAI",
            .commands = {"codex"},
            .transport = "headless",
            .install_url = "https://developers.openai.com/codex/cli",
            .install_hint = "npm install -g @openai/codex",
            .monogram = "Cx",
            .accent = "#10A37F",
            .min_version = {0, 120, 0},
            .update_hint = "Run `npm install -g @openai/codex` or update the Codex app.",
        },
        AgentSpec{
            .id = OPENCODE,
            .name = "OpenCode",
            .vendor = "OpenCode",
            .commands = {"opencode"},
            .transport = "acp",
            .install_url = "https://opencode.ai/docs/",
            .install_hint = "npm install -g opencode-ai",
            .monogram = "OC",
            .accent = "#6D5DD3",
            .min_version = {2, 0, 0},
            .update_hint = "Run `opencode upgrade`.",
        },
    };
    return specs;
}

// int() of a run of Unicode decimal digits (\d matched them).
std::int64_t digits_value(std::string_view digits) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    std::int64_t value = 0;
    for (const char32_t cp : py::to_code_points(digits)) {
        const int digit = py::decimal_value(cp).value_or(0);
        if (value > (max - digit) / 10) {
            return max;
        }
        value = value * 10 + digit;
    }
    return value;
}

// os.path.normcase(os.path.realpath(path)).
std::string real_key(const std::filesystem::path& path) {
    std::error_code error;
    auto real = std::filesystem::canonical(path, error);
    if (error) {
        real = std::filesystem::absolute(path, error).lexically_normal();
    }
    auto text = path_to_utf8(real);
    if constexpr (kWindows) {
        std::replace(text.begin(), text.end(), '/', '\\');
        return py::lower(text);
    }
    return text;
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool is_executable(const std::filesystem::path& path) {
    if constexpr (kWindows) {
        return true;
    } else {
        std::error_code error;
        using std::filesystem::perms;
        const auto mode = std::filesystem::status(path, error).permissions();
        return !error &&
               (mode & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
    }
}

std::vector<std::string> split_list(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const auto end = value.find(separator, start);
        parts.emplace_back(value.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

std::string ascii_upper(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::mutex& instance_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<InstalledAgents>& instance_slot() {
    static std::shared_ptr<InstalledAgents> agents;
    return agents;
}

// Calls fn with the set instance's configuration (held while fn runs), else this process's.
template <class Fn> auto with_config(Fn&& fn) {
    if (auto agents = installed_agents_if_set()) {
        return fn(agents->config());
    }
    return fn(AgentConfig::system());
}

// re.sub(r"\s*\(.*\)$", "", label)
std::string without_parenthetical(std::string_view label) {
    static const py::Regex trailing("\\s*\\(.*\\)$");
    return trailing.sub("", label);
}

// model.rsplit("/", 1)[-1]
std::string last_segment(std::string_view model) {
    const auto slash = model.rfind('/');
    return std::string(slash == std::string_view::npos ? model : model.substr(slash + 1));
}

} // namespace

const std::vector<AgentSpec>& agent_specs() {
    return specs_table();
}

const AgentSpec* find_agent_spec(std::string_view id) {
    for (const auto& spec : specs_table()) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

const AgentSpec& agent_spec(std::string_view id) {
    if (const auto* spec = find_agent_spec(id)) {
        return *spec;
    }
    throw std::out_of_range("no agent " + std::string(id));
}

std::vector<std::string_view> agent_order() {
    return {CLAUDE_CODE, CODEX, OPENCODE};
}

ParsedVersion parse_version(std::string_view text) {
    static const py::Regex pattern("(\\d+)\\.(\\d+)(?:\\.(\\d+))?(?:-([0-9A-Za-z.\\-]+))?");
    const auto match = pattern.search(text);
    if (!match) {
        return {};
    }
    ParsedVersion parsed;
    parsed.key = std::array<std::int64_t, 4>{
        digits_value(match->group(1)), digits_value(match->group(2)),
        match->matched(3) ? digits_value(match->group(3)) : 0, match->matched(4) ? 0 : 1};
    parsed.text = match->group(0);
    return parsed;
}

std::pair<std::optional<bool>, std::string> claude_sign_in_from(std::optional<int> /*code*/,
                                                                std::string_view output) {
    const auto start = output.find('{');
    if (start == std::string_view::npos) {
        return {std::nullopt, ""};
    }
    // output[start:output.rfind("}") + 1]: empty when there is no "}" after the "{".
    const auto close = output.rfind('}');
    const auto end = close == std::string_view::npos ? 0 : close + 1;
    const auto slice = end > start ? output.substr(start, end - start) : std::string_view();
    Json status;
    try {
        status = py::loads(slice);
    } catch (const py::PyError&) {
        return {std::nullopt, ""};
    }
    if (!status.is_object()) {
        return {std::nullopt, ""};
    }
    const auto& logged_in = detail::get(status, "loggedIn");
    if (logged_in.is_boolean() && !logged_in.get<bool>()) {
        return {false, ""};
    }
    const auto& provider_value = detail::get(status, "apiProvider");
    const auto provider = detail::truthy(provider_value) ? detail::py_str(provider_value) : "";
    if (provider == "bedrock") {
        return {true, "Amazon Bedrock"};
    }
    if (provider == "vertex") {
        return {true, "Google Vertex AI"};
    }
    if (provider == "foundry") {
        return {true, "Microsoft Foundry"};
    }
    const auto& plan_value = detail::get(status, "subscriptionType");
    const auto plan = py::lower(detail::truthy(plan_value) ? detail::py_str(plan_value) : "");
    static const std::vector<std::pair<std::string_view, std::string_view>> plans{
        {"pro", "Claude Pro"},   {"max", "Claude Max"},
        {"team", "Claude Team"}, {"enterprise", "Claude Enterprise"},
        {"free", "Claude"},
    };
    for (const auto& [key, label] : plans) {
        if (plan == key) {
            return {true, std::string(label)};
        }
    }
    const auto& method_value = detail::get(status, "authMethod");
    const auto method = py::lower(detail::truthy(method_value) ? detail::py_str(method_value) : "");
    if (method.find("key") != std::string::npos) {
        return {true, "API key"};
    }
    if (detail::truthy(logged_in)) {
        return {true, "Claude account"};
    }
    return {std::nullopt, ""};
}

std::pair<std::optional<bool>, std::string> codex_sign_in_from(std::optional<int> code,
                                                               std::string_view output) {
    const auto text = py::lower(output);
    if (!code) {
        return {std::nullopt, ""};
    }
    if (text.find("not logged in") != std::string::npos || *code != 0) {
        return {false, ""};
    }
    if (text.find("chatgpt") != std::string::npos) {
        return {true, "ChatGPT"};
    }
    if (text.find("api key") != std::string::npos) {
        return {true, "API key"};
    }
    return {true, ""};
}

std::string sign_in_hint(std::string_view agent_id) {
    if (agent_id == CLAUDE_CODE) {
        return "Run `claude` once and sign in, or `claude auth login`.";
    }
    if (agent_id == CODEX) {
        return "Run `codex login`.";
    }
    if (agent_id == OPENCODE) {
        return "Run `opencode auth login`.";
    }
    return "";
}

// ---- InstalledAgents ----

InstalledAgents::InstalledAgents(std::shared_ptr<ProcessLauncher> launcher,
                                 std::function<std::map<std::string, std::string>()> base_env,
                                 std::filesystem::path home, std::filesystem::path temp_dir)
    : launcher_(std::move(launcher)), base_env_(std::move(base_env)), home_(std::move(home)),
      temp_dir_(std::move(temp_dir)),
      config_([this](std::string_view name) { return env(name); }, home_) {}

InstalledAgents::~InstalledAgents() {
    stop_lookups();
}

void InstalledAgents::stop_lookups() {
    stopping_ = true;
    // Ends the probes running now too (a scan's on io as well as the lookups joined below).
    stop_.cancel();
    wait_for_lookups();
}

std::optional<std::string> InstalledAgents::env(std::string_view name) const {
    if (!base_env_) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    for (const auto& [key, value] : base_env_()) {
        const EnvNameLess less;
        if (!less(key, name) && !less(name, key)) {
            found = value;
        }
    }
    return found;
}

std::map<std::string, std::string>
InstalledAgents::agent_child_env(const std::map<std::string, std::string>& extra) const {
    EnvMap env;
    if (base_env_) {
        for (const auto& [name, value] : base_env_()) {
            env.insert_or_assign(name, value);
        }
    }
    for (const auto& name : internal_env_names()) {
        env.erase(name);
    }
    for (const auto& [name, value] : extra) {
        env.erase(name);
        env.emplace(name, value);
    }
    return to_spec_env(env);
}

std::filesystem::path InstalledAgents::agent_workspace_dir() const {
    const auto temp = temp_dir_.empty() ? std::filesystem::temp_directory_path() : temp_dir_;
    const auto path = temp / path_from_utf8(py::lower(identity().product) + "-agent-workspace");
    std::filesystem::create_directories(path);
    return path;
}

ProbeResult InstalledAgents::run(const std::vector<std::string>& argv, double timeout_s) const {
    if (stopping_) {
        // Shutting down: what is left of a lookup ends without starting another probe.
        return {};
    }
    try {
        return detail::run_probe(*launcher_, argv, timeout_s, agent_workspace_dir(),
                                 agent_child_env(), stop_.token());
    } catch (const std::exception&) {
        logger.debug("Agent probe failed: {}", argv.size() > 1 ? argv[1] : std::string());
        return {};
    }
}

std::optional<std::filesystem::path> InstalledAgents::which(std::string_view command) const {
    // shutil.which(command) without its search of the current folder: this app's working
    // folder is not the user's.
    auto path = env("PATH");
    if (!path) {
        path = kWindows ? ".;C:\\bin" : "/bin:/usr/bin";
    }
    if (path->empty()) {
        return std::nullopt;
    }
    std::vector<std::string> files;
    if constexpr (kWindows) {
        auto pathext_source = env("PATHEXT").value_or("");
        if (pathext_source.empty()) {
            pathext_source = ".COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC";
        }
        std::vector<std::string> pathext;
        for (auto& ext : split_list(pathext_source, ';')) {
            if (!ext.empty()) {
                pathext.push_back(std::move(ext));
            }
        }
        for (const auto& ext : pathext) {
            files.push_back(std::string(command) + ext);
        }
        const auto upper = ascii_upper(command);
        if (std::any_of(pathext.begin(), pathext.end(),
                        [&](const auto& ext) { return upper.ends_with(ascii_upper(ext)); })) {
            files.insert(files.begin(), std::string(command));
        }
    } else {
        files.emplace_back(command);
    }
    std::set<std::string> seen;
    for (const auto& dir : split_list(*path, kPathSeparator)) {
        if (dir.empty()) {
            continue;
        }
        auto normdir = dir;
        if constexpr (kWindows) {
            std::replace(normdir.begin(), normdir.end(), '/', '\\');
            normdir = py::lower(normdir);
        }
        if (!seen.insert(normdir).second) {
            continue;
        }
        for (const auto& file : files) {
            const auto name = path_from_utf8(dir) / path_from_utf8(file);
            std::error_code error;
            const auto status = std::filesystem::status(name, error);
            if (!error && std::filesystem::exists(status) &&
                !std::filesystem::is_directory(status) && is_executable(name)) {
                return name;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> InstalledAgents::candidate_paths(std::string_view id) const {
    const auto& spec = agent_spec(id);
    const auto home = home_;
    const auto local_text = env("LOCALAPPDATA").value_or("");
    const auto local = path_from_utf8(local_text);
    std::vector<std::filesystem::path> found;
    for (const auto command : spec.commands) {
        if (auto path = which(command)) {
            found.push_back(*path);
        }
    }

    std::vector<std::filesystem::path> dirs{home / ".local" / "bin"};
    std::vector<std::filesystem::path> globbed;
    if (id == CLAUDE_CODE) {
        dirs.push_back(home / ".claude" / "local");
    } else if (id == CODEX) {
        if (kWindows && !local_text.empty()) {
            // The Codex desktop app keeps versioned CLIs under bin\<hash>\.
            const auto bin = local / "OpenAI" / "Codex" / "bin";
            std::error_code error;
            for (std::filesystem::directory_iterator it(bin, error), end; !error && it != end;
                 it.increment(error)) {
                const auto name = path_to_utf8(it->path().filename());
                std::error_code is_dir_error;
                if (name.starts_with('.') || !it->is_directory(is_dir_error)) {
                    continue;
                }
                const auto candidate = it->path() / "codex.exe";
                std::error_code exists_error;
                if (std::filesystem::exists(
                        std::filesystem::symlink_status(candidate, exists_error))) {
                    globbed.push_back(candidate);
                }
            }
            std::sort(globbed.begin(), globbed.end(), [](const auto& a, const auto& b) {
                return path_to_utf8(a) < path_to_utf8(b);
            });
            dirs.push_back(local / "OpenAI" / "Codex" / "bin");
            dirs.push_back(local / "Programs" / "OpenAI" / "Codex" / "bin");
        } else if constexpr (kMac) {
            for (const auto& root :
                 {std::filesystem::path("/Applications"), home / "Applications"}) {
                dirs.push_back(root / "Codex.app" / "Contents" / "Resources");
            }
        }
    } else if (id == OPENCODE) {
        dirs.push_back(home / ".opencode" / "bin");
        if constexpr (kWindows) {
            dirs.push_back(home / "scoop" / "shims");
            if (!local_text.empty()) {
                dirs.push_back(local / "Microsoft" / "WinGet" / "Links");
            }
        }
    }
    if constexpr (kWindows) {
        if (auto appdata = env("APPDATA"); appdata && !appdata->empty()) {
            dirs.push_back(path_from_utf8(*appdata) / "npm");
        }
    } else {
        if (auto prefix = env("NPM_CONFIG_PREFIX"); prefix && !prefix->empty()) {
            dirs.push_back(path_from_utf8(*prefix) / "bin");
        }
        dirs.push_back(home / ".npm-global" / "bin");
        dirs.push_back(home / ".bun" / "bin");
        dirs.push_back("/opt/homebrew/bin");
        dirs.push_back("/usr/local/bin");
    }

    for (const auto& directory : dirs) {
        for (const auto command : spec.commands) {
            if constexpr (kWindows) {
                found.push_back(directory / path_from_utf8(std::string(command) + ".exe"));
                found.push_back(directory / path_from_utf8(std::string(command) + ".cmd"));
            } else {
                found.push_back(directory / path_from_utf8(command));
            }
        }
    }
    found.insert(found.end(), globbed.begin(), globbed.end());

    std::vector<std::filesystem::path> unique;
    std::set<std::string> seen;
    for (const auto& path : found) {
        if (path.empty() || !is_file(path) || !is_executable(path)) {
            continue;
        }
        if (seen.insert(real_key(path)).second) {
            std::error_code error;
            unique.push_back(std::filesystem::absolute(path, error).lexically_normal());
        }
    }
    return unique;
}

std::optional<InstalledAgent> InstalledAgents::find_agent(std::string_view id) {
    const auto& spec = agent_spec(id);
    std::optional<std::array<std::int64_t, 4>> best_key;
    std::string best_text;
    std::filesystem::path best_path;
    for (const auto& path : candidate_paths(id)) {
        const auto probe = run({path_to_utf8(path), "--version"}, VERSION_TIMEOUT_S);
        if (probe.code != 0) {
            continue;
        }
        const auto parsed = parse_version(probe.text);
        if (!parsed.key) {
            continue;
        }
        if (!best_key || *parsed.key > *best_key) {
            best_key = parsed.key;
            best_text = parsed.text;
            best_path = path;
        }
    }
    if (!best_key) {
        return std::nullopt;
    }
    InstalledAgent agent{.id = std::string(id), .path = best_path, .version = best_text};
    const std::array<std::int64_t, 3> have{(*best_key)[0], (*best_key)[1], (*best_key)[2]};
    const std::array<std::int64_t, 3> wanted{spec.min_version[0], spec.min_version[1],
                                             spec.min_version[2]};
    if (have < wanted) {
        agent.problem =
            py::strip(std::string(identity().product) + " needs " + std::string(spec.name) + " " +
                      std::to_string(wanted[0]) + "." + std::to_string(wanted[1]) + "." +
                      std::to_string(wanted[2]) + " or newer. " + std::string(spec.update_hint));
    }
    return agent;
}

InstalledAgent InstalledAgents::with_sign_in(const InstalledAgent& agent) {
    if (!agent.problem.empty()) {
        return agent;
    }
    std::pair<std::optional<bool>, std::string> sign_in{std::nullopt, ""};
    if (agent.id == CLAUDE_CODE) {
        const auto probe = run({path_to_utf8(agent.path), "auth", "status"}, AUTH_TIMEOUT_S);
        sign_in = claude_sign_in_from(probe.code, probe.text);
    } else if (agent.id == CODEX) {
        const auto probe = run({path_to_utf8(agent.path), "login", "status"}, AUTH_TIMEOUT_S);
        sign_in = codex_sign_in_from(probe.code, probe.text);
    }
    auto signed_in = agent;
    signed_in.account = sign_in.second;
    signed_in.signed_in = sign_in.first;
    return signed_in;
}

Subscription InstalledAgents::add_lookup_listener(Executor& executor,
                                                  std::function<void(std::string)> listener) {
    return changed_.connect(executor, [listener = std::move(listener)](std::string id) {
        try {
            listener(std::move(id));
        } catch (const std::exception&) {
            logger.warning("Agent lookup listener failed");
        }
    });
}

void InstalledAgents::notify(const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        changed_.emit(id);
    }
}

ScanResult InstalledAgents::scan_agents(bool refresh) {
    assert_not_gui_thread();
    {
        std::lock_guard lock(mutex_);
        if (scan_result_ && !refresh) {
            return *scan_result_;
        }
    }
    const auto scan_one = [this](const std::string& id) -> std::optional<InstalledAgent> {
        try {
            auto agent = find_agent(id);
            return agent ? std::optional(with_sign_in(*agent)) : std::nullopt;
        } catch (const std::exception&) {
            logger.warning("Scanning for {} failed", id);
            return std::nullopt;
        }
    };
    // One thread per agent, as ThreadPoolExecutor(max_workers=3, thread_name_prefix=
    // "agent-scan") runs them; own threads rather than std::async, whose pool threads would keep
    // the name.
    const auto order = agent_order();
    std::vector<std::optional<InstalledAgent>> results(order.size());
    {
        std::vector<std::jthread> running;
        for (std::size_t i = 0; i < order.size(); ++i) {
            running.emplace_back([&, i] {
                detail::set_current_thread_name("agent-scan_" + std::to_string(i));
                results[i] = scan_one(std::string(order[i]));
            });
        }
    }
    ScanResult found;
    for (std::size_t i = 0; i < order.size(); ++i) {
        found[std::string(order[i])] = std::move(results[i]);
    }
    {
        std::lock_guard lock(mutex_);
        scan_result_ = found;
        looked_up_.clear();
    }
    std::vector<std::string> ids;
    std::string summary;
    for (const auto id : agent_order()) {
        ids.emplace_back(id);
        if (!summary.empty()) {
            summary += ", ";
        }
        const auto& agent = found[std::string(id)];
        summary += std::string(id) + "=" + (agent ? agent->version : "-");
    }
    notify(ids);
    logger.info("Installed agents: {}", summary);
    return found;
}

std::optional<ScanResult> InstalledAgents::cached_scan() const {
    std::lock_guard lock(mutex_);
    return scan_result_;
}

std::pair<bool, std::optional<InstalledAgent>> InstalledAgents::known_agent(std::string_view id) {
    const std::string key(id);
    bool known = false;
    std::optional<InstalledAgent> agent;
    std::thread finished;
    {
        std::lock_guard lock(mutex_);
        // Cleared by each scan, so a lookup here is the newer answer.
        if (const auto it = looked_up_.find(key); it != looked_up_.end()) {
            known = true;
            agent = it->second;
        } else if (scan_result_) {
            if (const auto found = scan_result_->find(key); found != scan_result_->end()) {
                known = true;
                agent = found->second;
            }
        }
        if ((known && agent && agent->usable()) || looking_up_.contains(key) || stopping_) {
            return {known, agent};
        }
        looking_up_.insert(key);
        if (auto previous = lookups_.find(key); previous != lookups_.end()) {
            finished = std::move(previous->second);
            lookups_.erase(previous);
        }
        lookups_[key] = std::thread([this, key] {
            detail::set_current_thread_name("agent-lookup-" + key);
            try {
                resolve_agent(key);
            } catch (const std::exception&) {
                logger.warning("Looking up {} failed", key);
            }
            std::lock_guard done(mutex_);
            looking_up_.erase(key);
        });
    }
    // The previous lookup of this agent has ended (it is no longer in looking_up_).
    if (finished.joinable()) {
        finished.join();
    }
    return {known, agent};
}

void InstalledAgents::wait_for_lookups() {
    // Again until none is left: a listener on an inline executor may call known_agent from a
    // lookup thread and start another lookup while these are joined.
    for (;;) {
        std::map<std::string, std::thread> running;
        {
            std::lock_guard lock(mutex_);
            if (lookups_.empty()) {
                return;
            }
            running.swap(lookups_);
        }
        for (auto& [id, thread] : running) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
}

std::optional<InstalledAgent> InstalledAgents::resolve_agent(std::string_view id) {
    assert_not_gui_thread();
    const std::string key(id);
    std::optional<InstalledAgent> agent;
    {
        std::lock_guard lock(mutex_);
        // _looked_up.get(id) or _scan_result.get(id): a lookup that found nothing defers to the
        // scan.
        if (const auto it = looked_up_.find(key); it != looked_up_.end() && it->second) {
            agent = it->second;
        } else if (scan_result_) {
            if (const auto found = scan_result_->find(key); found != scan_result_->end()) {
                agent = found->second;
            }
        }
    }
    if (agent && agent->usable() && is_file(agent->path)) {
        return agent;
    }
    agent = find_agent(id);
    if (agent) {
        agent = with_sign_in(*agent);
    }
    if (stopping_) {
        // The probes may have been cut short: nothing learnt here is the agent's real state.
        return agent;
    }
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = looked_up_.find(key);
        changed = it == looked_up_.end() || it->second != agent;
        looked_up_[key] = agent;
    }
    if (changed) {
        notify({key});
    }
    return agent;
}

std::vector<AgentModel> InstalledAgents::list_models(const InstalledAgent& agent,
                                                     double timeout_s) {
    std::vector<AgentModel> models{{"", config_.default_model_label(agent.id)}};
    if (agent.id == CLAUDE_CODE) {
        const auto& aliases = claude_models();
        models.insert(models.end(), aliases.begin(), aliases.end());
    } else if (agent.id == CODEX) {
        const auto listed = config_.codex_models();
        models.insert(models.end(), listed.begin(), listed.end());
    } else if (agent.id == OPENCODE && agent.usable()) {
        assert_not_gui_thread();
        try {
            const auto listed = list_opencode_models(agent.path, *this, timeout_s);
            models.insert(models.end(), listed.begin(), listed.end());
        } catch (const std::exception&) {
            logger.warning("Could not list OpenCode models");
        }
    }
    return models;
}

// ---- the app's instance ----

std::shared_ptr<InstalledAgents> make_installed_agents(std::shared_ptr<ProcessLauncher> launcher,
                                                       const EnvReader& env) {
    return std::make_shared<InstalledAgents>(
        std::move(launcher), [] { return to_spec_env(ProcessEnv::instance().external_env()); },
        path_from_utf8(detail::home_folder(env)));
}

void set_installed_agents(std::shared_ptr<InstalledAgents> agents) {
    std::lock_guard lock(instance_mutex());
    instance_slot() = std::move(agents);
}

InstalledAgents& installed_agents() {
    auto agents = installed_agents_if_set();
    if (!agents) {
        throw std::logic_error("installed_agents() before set_installed_agents()");
    }
    // The slot keeps it alive until it is replaced, which only the composition and tests do.
    return *agents;
}

std::shared_ptr<InstalledAgents> installed_agents_if_set() {
    std::lock_guard lock(instance_mutex());
    return instance_slot();
}

// ---- model names ----

const std::vector<AgentModel>& claude_models() {
    static const std::vector<AgentModel> models{
        {"haiku", "Haiku (fastest)"},
        {"sonnet", "Sonnet"},
        {"opus", "Opus"},
        {"fable", "Fable"},
    };
    return models;
}

std::string model_display_name(const AgentConfig& config, std::string_view agent_id,
                               std::string_view model, const std::vector<AgentModel>& models) {
    if (model.empty()) {
        const auto configured = config.configured_default_model(agent_id);
        return configured.empty() ? "default model" : last_segment(configured);
    }
    for (const auto& entry : models) {
        if (entry.value == model) {
            return without_parenthetical(entry.label);
        }
    }
    if (agent_id == CLAUDE_CODE) {
        for (const auto& entry : claude_models()) {
            if (entry.value == model) {
                return without_parenthetical(entry.label);
            }
        }
    }
    return last_segment(model);
}

std::string model_display_name(std::string_view agent_id, std::string_view model,
                               const std::vector<AgentModel>& models) {
    return with_config([&](const AgentConfig& config) {
        return model_display_name(config, agent_id, model, models);
    });
}

std::string describe_choice(const AgentConfig& config, std::string_view agent_id,
                            std::string_view model) {
    const auto* spec = find_agent_spec(agent_id);
    if (spec == nullptr) {
        return "";
    }
    return std::string(spec->name) + " \xC2\xB7 " + model_display_name(config, agent_id, model);
}

std::string describe_choice(std::string_view agent_id, std::string_view model) {
    return with_config(
        [&](const AgentConfig& config) { return describe_choice(config, agent_id, model); });
}

std::string default_model_label(std::string_view agent_id) {
    return with_config(
        [&](const AgentConfig& config) { return config.default_model_label(agent_id); });
}

ModelEfforts codex_model_efforts() {
    return with_config([](const AgentConfig& config) { return config.codex_model_efforts(); });
}

} // namespace fv::agents
