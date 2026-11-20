#include "agents/acp.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>

#include "agents/detail/pyvalue.h"
#include "agents/installed_agents.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "process/agent_process.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::agents {

namespace {

constexpr log::Logger logger("meeting.agent.installed.acp");

constexpr std::size_t STDERR_TAIL_LINES = 40;
constexpr std::int64_t STDERR_LINE_MAX = 400; // characters
constexpr std::chrono::milliseconds POLL{250};

using Clock = std::chrono::steady_clock;

std::chrono::steady_clock::duration seconds(double value) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(value));
}

// "{value:.0f}" as Python formats it: the nearest integer, ties to even.
std::string whole_seconds(double value) {
    return std::format("{:.0f}", value);
}

std::vector<Json> select_options(const Json& option) {
    std::vector<Json> flat;
    for (const auto& entry : detail::iterate(detail::get(option, "options"))) {
        if (!entry.is_object()) {
            continue;
        }
        const auto& nested = detail::get(entry, "options");
        if (nested.is_array()) {
            for (const auto& item : nested) {
                if (item.is_object()) {
                    flat.push_back(item);
                }
            }
        } else {
            flat.push_back(entry);
        }
    }
    return flat;
}

// value.get(...) of a value Python would call .get on: an AttributeError for anything that is
// not a dict.
const Json& require_dict(const Json& value, std::string_view what) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", std::string(what) + " has no attribute 'get'");
    }
    return value;
}

} // namespace

Json acp_client_info() {
    return Json{{"name", py::lower(identity().product)},
                {"title", std::string(identity().product)},
                {"version", "1"}};
}

Json acp_client_capabilities() {
    return Json{{"fs", Json{{"readTextFile", false}, {"writeTextFile", false}}},
                {"terminal", false}};
}

// ---- AcpConnection ----

struct AcpConnection::Shared {
    struct Pending {
        bool answered = false;
        Json result;
        std::optional<AcpError> error;
    };

    AcpNotificationHandler on_notification;
    AcpRequestHandler on_request;
    AcpDiagnostics diagnostics = AcpDiagnostics::standard;

    std::mutex mutex; // pending, ids, stderr tail
    std::condition_variable changed;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending;
    std::int64_t next_id = 1;
    std::deque<std::string> stderr_tail;
    bool reader_stopped = false;

    // The process while the connection holds it; writes are serialized under this lock.
    std::mutex process_mutex;
    Process* process = nullptr;

    std::atomic<bool> closed{false};

    bool alive() {
        std::lock_guard lock(process_mutex);
        return process != nullptr && !process->exit_code() && !closed.load();
    }

    std::string tail() {
        std::lock_guard lock(mutex);
        std::string out;
        for (const auto& line : stderr_tail) {
            if (!out.empty()) {
                out += '\n';
            }
            out += line;
        }
        return out;
    }

    void write(const Json& message) {
        const auto line = py::dumps(message, {.ensure_ascii = false}) + "\n";
        std::lock_guard lock(process_mutex);
        if (process == nullptr || process->exit_code()) {
            throw AcpError("the agent is not running");
        }
        process->write_stdin(line);
    }

    std::string exit_message() {
        std::optional<int> code;
        {
            std::lock_guard lock(process_mutex);
            if (process != nullptr) {
                code = process->exit_code();
            }
        }
        // stderr_tail().strip().splitlines()[-3:]
        auto kept = detail::splitlines(py::strip(tail()));
        if (kept.size() > 3) {
            kept.erase(kept.begin(), kept.end() - 3);
        }
        std::string detail;
        for (const auto& line : kept) {
            detail += detail.empty() ? ": " + line : " / " + line;
        }
        return "the agent exited (code " + (code ? std::to_string(*code) : std::string("None")) +
               ")" + detail;
    }

    void fail_pending(const std::string& reason) {
        {
            std::lock_guard lock(mutex);
            for (auto& [id, entry] : pending) {
                if (!entry->error && !entry->answered) {
                    entry->error = AcpError(reason);
                }
                entry->answered = true;
            }
        }
        changed.notify_all();
    }

    void answer(const Json& id, const std::string& method, const Json& params) {
        Json reply{{"jsonrpc", "2.0"}, {"id", id}};
        try {
            if (!on_request) {
                throw AcpError(method + " is not supported", -32601);
            }
            reply["result"] = on_request(method, params);
        } catch (const AcpError& error) {
            const auto code = error.code().value_or(0);
            reply["error"] = Json{{"code", code != 0 ? code : -32601}, {"message", error.what()}};
        } catch (const std::exception& error) {
            if (diagnostics == AcpDiagnostics::standard) {
                logger.error("ACP request handler failed for {}", method);
            }
            reply["error"] = Json{{"code", -32603}, {"message", error.what()}};
        }
        try {
            write(reply);
        } catch (const AcpError&) {
        }
    }

    void dispatch(const Json& message) {
        const auto& method = detail::get(message, "method");
        const auto& raw_params = detail::get(message, "params");
        const Json params = raw_params.is_object() ? raw_params : Json::object();
        if (method.is_string()) {
            const auto name = method.get<std::string>();
            if (message.contains("id")) {
                answer(message.at("id"), name, params);
            } else if (on_notification) {
                try {
                    on_notification(name, params);
                } catch (const std::exception&) {
                    if (diagnostics == AcpDiagnostics::standard) {
                        logger.error("ACP notification handler failed for {}", name);
                    }
                }
            }
            return;
        }
        const auto& id = detail::get(message, "id");
        if (!detail::is_py_int(id)) {
            return;
        }
        const std::int64_t key =
            id.is_boolean() ? (id.get<bool>() ? 1 : 0) : id.get<std::int64_t>();
        {
            std::lock_guard lock(mutex);
            const auto it = pending.find(key);
            if (it == pending.end()) {
                return;
            }
            auto& entry = *it->second;
            const auto& error = detail::get(message, "error");
            if (error.is_object()) {
                const auto& text = detail::get(error, "message");
                const auto& code = detail::get(error, "code");
                std::optional<std::int64_t> number;
                if (code.is_number_integer() || code.is_number_unsigned()) {
                    number = code.get<std::int64_t>();
                }
                entry.error = AcpError(detail::truthy(text) ? detail::py_str(text) : "error",
                                       number, detail::get(error, "data"));
            } else {
                entry.result = detail::get(message, "result");
            }
            entry.answered = true;
        }
        changed.notify_all();
    }

    void read_line(const std::string& raw) {
        {
            std::lock_guard lock(mutex);
            if (reader_stopped) {
                return;
            }
        }
        const auto line = py::strip(raw);
        if (line.empty()) {
            return;
        }
        Json message;
        try {
            message = py::loads(line);
        } catch (const py::PyError&) {
            logger.debug("ACP agent wrote a non-JSON line ({} bytes)", py::len(line));
            return;
        }
        if (!message.is_object()) {
            return;
        }
        try {
            dispatch(message);
        } catch (const std::exception&) {
            // A dispatch bug would otherwise end the connection without a word.
            if (closed.load()) {
                logger.debug("ACP reader stopped after close");
            } else {
                logger.warning("ACP reader stopped unexpectedly");
            }
            {
                std::lock_guard lock(mutex);
                reader_stopped = true;
            }
            fail_pending(exit_message());
        }
    }

    void stdout_ended() {
        {
            std::lock_guard lock(mutex);
            if (reader_stopped) {
                return;
            }
            reader_stopped = true;
        }
        fail_pending(exit_message());
    }

    void read_stderr(const std::string& raw) {
        if (diagnostics == AcpDiagnostics::private_data) {
            return;
        }
        const auto text = py::rstrip(raw);
        if (text.empty()) {
            return;
        }
        const auto kept = py::slice(text, 0, STDERR_LINE_MAX);
        {
            std::lock_guard lock(mutex);
            stderr_tail.push_back(kept);
            while (stderr_tail.size() > STDERR_TAIL_LINES) {
                stderr_tail.pop_front();
            }
        }
        logger.debug("agent stderr: {}", kept);
    }
};

AcpConnection::AcpConnection(std::shared_ptr<ProcessLauncher> launcher,
                             std::vector<std::string> argv, std::map<std::string, std::string> env,
                             std::filesystem::path cwd, AcpNotificationHandler on_notification,
                             AcpRequestHandler on_request, AcpDiagnostics diagnostics)
    : launcher_(std::move(launcher)), argv_(std::move(argv)), env_(std::move(env)),
      cwd_(std::move(cwd)), shared_(std::make_shared<Shared>()) {
    shared_->on_notification = std::move(on_notification);
    shared_->on_request = std::move(on_request);
    shared_->diagnostics = diagnostics;
}

AcpConnection::~AcpConnection() {
    if (process_ && !shared_->closed.load()) {
        close();
    }
    {
        std::lock_guard lock(shared_->process_mutex);
        shared_->process = nullptr;
    }
    process_.reset();
}

void AcpConnection::start() {
    if (argv_.empty() || !launcher_) {
        throw AcpError("could not start the agent: no command");
    }
    ProcessSpec spec;
    spec.program = path_from_utf8(argv_.front());
    spec.args.assign(argv_.begin() + 1, argv_.end());
    spec.cwd = cwd_;
    spec.env = env_;
    spec.own_tree = true;
    spec.pipes = true;
    auto started = launcher_->start(spec);
    if (!started) {
        throw AcpError("could not start the agent: " + started.error());
    }
    process_ = std::move(*started);
    {
        std::lock_guard lock(shared_->process_mutex);
        shared_->process = process_.get();
    }
    auto shared = shared_;
    process_->on_bytes(detail::lines([shared](std::string line) { shared->read_line(line); },
                                     [shared] { shared->stdout_ended(); }),
                       detail::lines([shared](std::string line) { shared->read_stderr(line); }));
}

bool AcpConnection::alive() const {
    return shared_->alive();
}

std::string AcpConnection::stderr_tail() const {
    return shared_->tail();
}

Json AcpConnection::request(std::string_view method, Json params, double timeout_s,
                            const CancelToken& cancel, std::optional<double> stall_s,
                            std::function<Clock::time_point()> last_activity) {
    assert_not_gui_thread();
    auto pending = std::make_shared<Shared::Pending>();
    std::int64_t id = 0;
    {
        std::lock_guard lock(shared_->mutex);
        if (!shared_->alive()) {
            throw AcpError("the agent is not running");
        }
        id = shared_->next_id++;
        shared_->pending[id] = pending;
    }
    struct Forget {
        Shared& shared;
        std::int64_t id;
        ~Forget() {
            std::lock_guard lock(shared.mutex);
            shared.pending.erase(id);
        }
    } forget{*shared_, id};

    const std::string name(method);
    if (cancel.cancelled()) {
        throw AcpError("canceled");
    }
    shared_->write(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", name}, {"params", params}});
    const auto started = Clock::now();
    const auto deadline = started + seconds(timeout_s);
    std::unique_lock lock(shared_->mutex);
    while (!shared_->changed.wait_for(lock, POLL, [&] { return pending->answered; })) {
        lock.unlock();
        const auto now = Clock::now();
        if (cancel.cancelled()) {
            throw AcpError("canceled");
        }
        if (!shared_->alive()) {
            throw AcpError(shared_->exit_message());
        }
        if (now >= deadline) {
            throw AcpError(name + " timed out after " + whole_seconds(timeout_s) + "s");
        }
        if (stall_s) {
            const auto last = last_activity ? last_activity() : started;
            if (now - std::max(last, started) >= seconds(*stall_s)) {
                throw AcpError(name + " stalled after " + whole_seconds(*stall_s) +
                               "s without progress");
            }
        }
        lock.lock();
    }
    if (pending->error) {
        throw *pending->error;
    }
    return pending->result;
}

void AcpConnection::notify(std::string_view method, Json params) {
    try {
        shared_->write(
            Json{{"jsonrpc", "2.0"}, {"method", std::string(method)}, {"params", params}});
    } catch (const AcpError&) {
        logger.debug("ACP notify {} after exit", method);
    }
}

void AcpConnection::close(double wait_s) {
    shared_->closed.store(true);
    if (!process_) {
        return;
    }
    process_->close_stdin();
    process_->wait(std::chrono::duration_cast<std::chrono::milliseconds>(seconds(wait_s)));
    kill_process_tree(*process_);
    // The tree is gone, so the pipes end; a reader still blocked after a second is cancelled.
    process_->release_pipes(std::chrono::seconds(1));
    shared_->fail_pending("the agent was closed");
}

// ---- helpers ----

Json acp_initialize(AcpConnection& connection, double timeout_s, const CancelToken& cancel) {
    auto result = connection.request("initialize",
                                     Json{{"protocolVersion", ACP_PROTOCOL_VERSION},
                                          {"clientCapabilities", acp_client_capabilities()},
                                          {"clientInfo", acp_client_info()}},
                                     timeout_s, cancel);
    return result.is_object() ? result : Json::object();
}

Json acp_config_option(const Json& session, std::string_view option_id) {
    for (const auto& option : detail::iterate(detail::get(session, "configOptions"))) {
        if (option.is_object() && detail::get(option, "id") == Json(std::string(option_id))) {
            return option;
        }
    }
    return nullptr;
}

std::vector<std::string> acp_option_values(const Json& session, std::string_view option_id) {
    const auto option = acp_config_option(session, option_id);
    std::vector<std::string> values;
    for (const auto& entry : select_options(option.is_null() ? Json::object() : option)) {
        const auto& value = detail::get(entry, "value");
        if (detail::truthy(value)) {
            values.push_back(detail::py_str(value));
        }
    }
    return values;
}

std::vector<AgentModel> acp_model_choices(const Json& session) {
    std::vector<AgentModel> choices;
    const auto option = acp_config_option(session, "model");
    if (!option.is_null()) {
        for (const auto& entry : select_options(option)) {
            const auto& value = detail::get(entry, "value");
            if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
                const auto& name = detail::get(entry, "name");
                choices.push_back({value.get<std::string>(), detail::truthy(name)
                                                                 ? detail::py_str(name)
                                                                 : value.get<std::string>()});
            }
        }
        return choices;
    }
    // The draft before configOptions.
    const auto& raw_models = detail::get(session, "models");
    const auto models = detail::truthy(raw_models) ? raw_models : Json::object();
    require_dict(models, "'models'");
    for (const auto& entry : detail::iterate(detail::get(models, "availableModels"))) {
        const auto& model_id = detail::get(entry, "modelId");
        if (entry.is_object() && model_id.is_string()) {
            const auto& name = detail::get(entry, "name");
            choices.push_back({model_id.get<std::string>(), detail::truthy(name)
                                                                ? detail::py_str(name)
                                                                : model_id.get<std::string>()});
        }
    }
    return choices;
}

std::vector<AgentModel> list_opencode_models(const std::filesystem::path& opencode,
                                             const InstalledAgents& agents, double timeout_s) {
    AcpConnection connection(agents.launcher(), {path_to_utf8(opencode), "acp"},
                             agents.agent_child_env({{"OPENCODE_DISABLE_PROJECT_CONFIG", "1"}}),
                             agents.agent_workspace_dir());
    connection.start();
    struct Close {
        AcpConnection& connection;
        ~Close() { connection.close(); }
    } close{connection};

    const auto info = acp_initialize(connection, timeout_s);
    auto session = connection.request(
        "session/new",
        Json{{"cwd", path_to_utf8(agents.agent_workspace_dir())}, {"mcpServers", Json::array()}},
        timeout_s);
    if (!session.is_object()) {
        session = Json::object();
    }
    auto choices = acp_model_choices(session);
    const auto& session_id = detail::get(session, "sessionId");
    const auto& raw_agent = detail::get(info, "agentCapabilities");
    const auto agent_capabilities = detail::truthy(raw_agent) ? raw_agent : Json::object();
    const auto& raw_session =
        detail::get(require_dict(agent_capabilities, "'agentCapabilities'"), "sessionCapabilities");
    const auto capabilities = detail::truthy(raw_session) ? raw_session : Json::object();
    if (detail::truthy(session_id) && detail::contains(capabilities, "delete")) {
        try {
            connection.request("session/delete", Json{{"sessionId", session_id}}, 10.0);
        } catch (const AcpError&) {
            logger.debug("Could not delete the model-list session");
        }
    }
    return choices;
}

} // namespace fv::agents
