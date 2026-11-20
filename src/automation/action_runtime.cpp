#include "automation/action_runtime.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "automation/catalog.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/result_relay.h"
#include "automation/hooks_model.h"
#include "automation/permissions.h"
#include "automation/secrets.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "process/launch.h"
#include "process/run_once.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::automation {

using namespace runtime;

namespace {

constexpr log::Logger kLog{"services.actions.runtime"};

// The "<" of a tag naming one of the frame's own elements, open or close.
const py::Regex& frame_tag() {
    static const py::Regex pattern(R"(<(?=\s*/?\s*(?:selected_text|context|app)\b))",
                                   py::IGNORECASE);
    return pattern;
}

std::string inside_frame(std::string_view value) {
    return frame_tag().sub("&lt;", value);
}

// message[:1].upper() + message[1:].
std::string upper_first(std::string_view message) {
    return py::upper(py::slice(message, 0, 1)) + py::slice(message, 1);
}

// " ".join(text.split()).
std::string flattened(std::string_view text) {
    std::string flat;
    for (const std::string& word : py::split(text)) {
        if (!flat.empty()) {
            flat += " ";
        }
        flat += word;
    }
    return flat;
}

// 14000 as "14,000" (Python's f"{n:,}").
std::string with_thousands(std::size_t number) {
    std::string digits = std::to_string(number);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

// A parameter's text: the string itself, "" for anything falsy or not a string (Python's
// `param(...) or ""` on the strings load_actions keeps).
std::string param_text(const Action& action, std::string_view key) {
    const Json value = action.param(key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

bool is_ask(const Action& action) {
    const Json mode = action.param("mode");
    return mode.is_string() && mode.get<std::string_view>() == AgentMode::ASK;
}

// _stoppable: a run under way that takes its cancel: a program, or an agent's quick answer
// (both killed with everything they started). Whether it was stopped only its result can say.
bool stoppable(const Action& action) {
    return action.kind == ActionKind::PROGRAM ||
           (action.kind == ActionKind::AGENT && is_ask(action));
}

// _users_own: only the user's runs copy to the clipboard or paste into an app.
bool users_own(const dictation::DictationJob& job) {
    return dictation::user_started(job.source);
}

std::string listening(std::size_t count) {
    if (count == 0) {
        return std::string(NO_LISTENER_MESSAGE);
    }
    if (count == 1) {
        return "1 Claude Code session is listening";
    }
    return std::format("{} Claude Code sessions are listening", count);
}

// bytes.decode("utf-8", errors="replace"): each maximal invalid subpart becomes one U+FFFD.
std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    const auto byte = [&](std::size_t at) {
        return static_cast<unsigned char>(bytes[at]);
    };
    const auto continuation = [&](std::size_t at, unsigned char low, unsigned char high) {
        return at < bytes.size() && byte(at) >= low && byte(at) <= high;
    };
    while (i < bytes.size()) {
        const unsigned char lead = byte(i);
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead < 0x80) {
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            low = lead == 0xE0 ? 0xA0 : 0x80;
            high = lead == 0xED ? 0x9F : 0xBF;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            low = lead == 0xF0 ? 0x90 : 0x80;
            high = lead == 0xF4 ? 0x8F : 0xBF;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t good = 1;
        if (continuation(i + 1, low, high)) {
            good = 2;
            while (good < length && continuation(i + good, 0x80, 0xBF)) {
                ++good;
            }
        }
        if (good == length) {
            out.append(bytes.substr(i, length));
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += good;
    }
    return out;
}

ActionResult stopped_result() {
    return ActionResult{false, std::string(CANCELED_MESSAGE), {}, true};
}

// -- blocking work (worker threads; never touches the runtime or the host) ---------------------

ActionResult open_link(const std::function<void(std::string_view)>& open_uri,
                       const std::string& uri, const std::string& words) {
    // The Claude app quietly cuts longer words short.
    const auto scheme = py::split_on(uri, ":", 1);
    if (py::lower(scheme.front()) == "claude" && py::len(words) > CLAUDE_APP_TEXT_MAX) {
        return ActionResult{
            false, std::format("That's too long for the Claude app ({} characters at most)",
                               with_thousands(CLAUDE_APP_TEXT_MAX))};
    }
    try {
        open_uri(uri);
    } catch (const LaunchError& error) {
        return ActionResult{false, error.what()};
    }
    return ActionResult{true, ""};
}

using PostSigned = std::function<std::pair<DeliveryRecord, std::optional<std::string>>(
    std::string_view, const Json&, std::string_view, const SignedPost&)>;

ActionResult post_webhook(const PostSigned& post, CredentialStore& credentials,
                          const std::string& url, const Json& payload,
                          const std::string& action_id) {
    std::string secret;
    try {
        secret = ensure_action_secret(credentials, action_id);
    } catch (const CredentialStoreError& error) {
        return ActionResult{false,
                            std::format("couldn't keep its signing secret ({})", error.what())};
    } catch (const py::ValueError& error) {
        return ActionResult{false,
                            std::format("couldn't keep its signing secret ({})", error.what())};
    }
    SignedPost options;
    options.timeout_s = WEBHOOK_TIMEOUT_S;
    const auto [record, body] = post(url, payload, secret, options);
    if (!record.ok) {
        if (!record.error.empty()) {
            return ActionResult{false, record.error};
        }
        if (record.status && *record.status != 0) {
            return ActionResult{false, std::format("the address answered {}", *record.status)};
        }
        return ActionResult{false, "the address didn't answer"};
    }
    return ActionResult{true, "", reply_text(body).value_or("")};
}

ActionResult run_program(const RunOnceFn& run_once, const std::vector<std::string>& argv,
                         const std::string& input, double timeout_s,
                         const std::map<std::string, std::string>& extra_env,
                         const CancelToken& cancel) {
    const RunOnceResult result = run_once(argv, input, timeout_s, cancel, extra_env);
    if (result.error == fv::CANCELED) {
        // Only a program still running is stopped: one that already exited ran to its end,
        // whatever cancel says while its output drains.
        return stopped_result();
    }
    if (!result.error.empty()) {
        return ActionResult{false, result.error};
    }
    if (!result.exit_code) {
        return ActionResult{false, "the program didn't finish"};
    }
    if (*result.exit_code != 0) {
        return ActionResult{false,
                            std::format("the program stopped with code {}", *result.exit_code)};
    }
    return ActionResult{true, "", py::strip(decode_utf8_replace(result.output))};
}

struct AgentParams {
    std::string agent;
    bool ask = false;
    std::string surface;
    std::string folder;
    bool new_folder = false;
    bool include_selection = false;
    std::string model;
};

ActionResult run_agent(const ActionServices& services, const AgentParams& params, std::string text,
                       const dictation::DictationJob& job, const CancelToken& cancel) {
    if (params.include_selection) {
        // Only a job the user started has the selection (begin_job). Its capture ran while they
        // spoke; a worker can afford to wait for one still running.
        text = prompt_with_context(text, job);
    }
    if (params.ask) {
        // The ask says whether its cancel stopped it; one that answered, or failed on its own,
        // wasn't.
        const agents::AskResult answer = services.ask(params.agent, text, params.model, cancel);
        return ActionResult{answer.ok, answer.message, answer.ok ? answer.answer : std::string(),
                            answer.stopped};
    }
    agents::HandoffRequest request;
    request.agent_id = params.agent;
    request.text = std::move(text);
    request.surface = params.surface;
    request.folder = path_from_utf8(params.folder);
    request.new_folder = params.new_folder;
    try {
        return ActionResult{true, services.handoff(request)};
    } catch (const agents::HandoffError& error) {
        return ActionResult{false, error.what()};
    }
}

// One run of an action, from its words to its outcome.
struct Run {
    std::shared_ptr<const dictation::DictationJob> job;
    Action action;
    std::string text;
    std::string raw_text;
    std::string html;
    std::shared_ptr<const Json> settings;
    // Settings' Try it: the outcome goes to this callback, not the user.
    std::function<void(ActionResult)> done;
    // Stops an agent or a program early (cancel, or the app closing).
    CancelSource cancel;
    // Why the words aren't what the user asked for; shown with the outcome.
    std::string notice;
};
using RunPtr = std::shared_ptr<Run>;

// One cancel: what it did, said once every run it is stopping has answered.
struct CancelRecord {
    // Settings' Try it runs only: their page shows what happened.
    bool quiet = false;
    // Runs that never started, or were stopped.
    std::vector<RunPtr> stopped;
    // Runs that finished all the same.
    std::vector<RunPtr> late;
    // Programs and quick answers told to stop, whose results aren't back.
    int waiting = 0;
};

// A run's place on the strand: waiting for a worker, taken by one, or canceled before it was.
enum Phase : int { kWaiting = 0, kStarted = 1, kCanceled = 2 };

// A run on the strand, until its result is back or it is canceled.
struct Pending {
    RunPtr run;
    std::shared_ptr<std::atomic<int>> phase;
    // The cancel that reached it under way; its result says whether the work was stopped.
    std::shared_ptr<CancelRecord> canceled_by;
};

std::string runs_text(const std::vector<RunPtr>& runs) {
    return runs.size() == 1 ? runs.front()->action.name : std::format("{} actions", runs.size());
}

} // namespace

// -- free functions ------------------------------------------------------------------------------

std::string context_open() {
    return std::format("<context source=\"{}\">", identity().product);
}

std::string closing_message() {
    return std::format("{} is closing", identity().product);
}

std::optional<std::string> reply_text(const std::optional<std::string>& body) {
    if (!body || body->empty()) {
        return std::nullopt;
    }
    if (!detail::encodes_as_utf8(std::string_view(*body))) {
        return std::nullopt; // json.loads raises UnicodeDecodeError, a ValueError
    }
    Json data;
    try {
        data = py::loads(*body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!data.is_object()) {
        return std::nullopt;
    }
    const Json& text = detail::get(data, "text");
    return text.is_string() ? std::optional<std::string>(text.get<std::string>()) : std::nullopt;
}

std::string prompt_with_context(std::string_view text,
                                const std::optional<focus::FocusSnapshot>& snapshot) {
    if (!snapshot || !snapshot->text) {
        return std::string(text);
    }
    const focus::TextContext& context = *snapshot->text;
    if (context.blocked || !context.selection_known) {
        return std::string(text);
    }
    if (py::strip(context.selected).empty()) {
        return std::string(text);
    }
    std::string app;
    if (snapshot->identity && !snapshot->identity->name.empty()) {
        app = snapshot->identity->name;
    }
    std::vector<std::string> lines{context_open()};
    if (!app.empty()) {
        lines.push_back("<app>" + inside_frame(app) + "</app>");
    }
    lines.push_back("<selected_text>");
    lines.push_back(inside_frame(context.selected));
    lines.push_back("</selected_text>");
    lines.emplace_back(CONTEXT_CLOSE);
    std::string prompt(text);
    prompt += "\n\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            prompt += "\n";
        }
        prompt += lines[i];
    }
    return prompt;
}

std::string prompt_with_context(std::string_view text, const dictation::DictationJob& job) {
    return prompt_with_context(text, job.snapshot(focus::CONTEXT_CAPTURE_DEADLINE));
}

bool program_words_allowed(std::string_view kind, const Json& settings) {
    const bool agent_kind = kind == ActionKind::AGENT || kind == ActionKind::CHANNEL;
    return !agent_kind || permissions::granted(settings, permissions::AGENT_PROMPTS);
}

bool program_words_allowed(const Action& action, const Json& settings) {
    return program_words_allowed(action.kind, settings);
}

std::string short_text(std::string_view text, std::size_t limit) {
    const std::string flat = flattened(text);
    if (py::len(flat) <= limit) {
        return flat;
    }
    return py::rstrip(py::slice(flat, 0, static_cast<std::int64_t>(limit) - 1)) + "…";
}

Json webhook_payload(const dictation::DictationJob& job, const Action& action,
                     std::string_view text, std::string_view raw_text, const Json& settings) {
    const Json fields = dictation::history_fields(&job, std::nullopt, true, settings);
    const Json& language = detail::get(fields, "language");
    Json payload = Json::object();
    payload["v"] = 1;
    payload["action"] = Json{{"id", action.id}, {"name", action.name}};
    payload["job_id"] = job.id;
    payload["text"] = std::string(text);
    payload["raw_text"] =
        !raw_text.empty() && raw_text != text ? Json(std::string(raw_text)) : Json(nullptr);
    payload["language"] = detail::truthy(language) ? language : Json("");
    payload["app"] = catalog::app_from_fields(fields);
    return payload;
}

bool scripts_listening(const Json& settings, const ListenerCount& subscribers,
                       const std::function<std::size_t()>& mcp_subscriptions) {
    if (subscribers(SSE_LABEL) > 0) {
        return true;
    }
    const auto takes_action_ran = [](const auto& hook) {
        return hook.enabled &&
               std::ranges::find(hook.events, catalog::ACTION_RAN) != hook.events.end();
    };
    if (subscribers(WEBHOOKS_LABEL) > 0 &&
        std::ranges::any_of(hooks::load_webhooks(settings), takes_action_ran)) {
        return true;
    }
    if (subscribers(PROGRAMS_LABEL) > 0 &&
        std::ranges::any_of(hooks::load_programs(settings), takes_action_ran)) {
        return true;
    }
    if (subscribers(MCP_EVENTS_LABEL) > 0) {
        // Subscribed while MCP Events is on, whether or not an agent is.
        return mcp_subscriptions && mcp_subscriptions() > 0;
    }
    return false;
}

bool scripts_listening(const Json& settings, const EventBus& bus,
                       std::function<std::size_t()> mcp_subscriptions) {
    return scripts_listening(
        settings, [&bus](std::string_view label) { return bus.subscribers(label); },
        mcp_subscriptions);
}

// -- the relay -----------------------------------------------------------------------------------

namespace detail {

std::uint64_t ResultRelay::next_token() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1);
}

void ResultRelay::expect(std::uint64_t token, Callback callback) {
    std::lock_guard lock(mutex_);
    waiting_[token] = std::move(callback);
}

void ResultRelay::drop(const std::vector<std::uint64_t>& tokens) {
    std::vector<Callback> dropped; // destroyed outside the lock
    std::lock_guard lock(mutex_);
    for (const std::uint64_t token : tokens) {
        if (auto node = waiting_.extract(token)) {
            dropped.push_back(std::move(node.mapped()));
        }
    }
}

void ResultRelay::finished(std::uint64_t token, ActionResult result) {
    ui_.post([self = shared_from_this(), token, value = std::move(result)]() mutable {
        self->deliver(token, std::move(value));
    });
}

std::size_t ResultRelay::waiting() const {
    std::lock_guard lock(mutex_);
    return waiting_.size();
}

void ResultRelay::deliver(std::uint64_t token, ActionResult result) {
    Callback callback;
    {
        std::lock_guard lock(mutex_);
        auto node = waiting_.extract(token);
        if (!node) {
            return;
        }
        callback = std::move(node.mapped());
    }
    try {
        callback(std::move(result));
    } catch (const std::exception& error) {
        kLog.error("An action's outcome could not be shown: {}", error_type(error));
    } catch (...) {
        kLog.error("An action's outcome could not be shown: unknown error");
    }
}

} // namespace detail

// -- the runtime ---------------------------------------------------------------------------------

struct ActionRuntime::Impl : std::enable_shared_from_this<ActionRuntime::Impl> {
    Impl(ActionHost& host_, SettingsStore& settings_, CredentialStore& credentials_,
         EventSink& events_, Executor& ui_, ActionServices services_)
        : host(host_), settings(settings_), credentials(credentials_), events(events_), ui(ui_),
          services(std::move(services_)), relay(std::make_shared<detail::ResultRelay>(ui_)) {
        if (!services.open_uri) {
            services.open_uri = [](std::string_view uri) {
                fv::open_uri(uri);
            };
        }
        if (!services.handoff) {
            services.handoff = [](const agents::HandoffRequest& request) {
                return agents::handoff(request);
            };
        }
        if (!services.ask) {
            services.ask = [](std::string_view agent_id, std::string_view text,
                              std::string_view model, const CancelToken& cancel) {
                return agents::ask(agent_id, text, model, agents::ASK_TIMEOUT_S, cancel);
            };
        }
        if (!services.post_signed_reply) {
            std::shared_ptr<http::Client> client =
                services.http ? services.http : http::make_curl_client();
            services.post_signed_reply = [client](std::string_view url, const Json& payload,
                                                  std::string_view secret,
                                                  const SignedPost& options) {
                return automation::post_signed_reply(*client, url, payload, secret, options);
            };
        }
        if (!services.run_once) {
            ProcessLauncher* launcher = services.launcher;
            services.run_once = [launcher](const std::vector<std::string>& argv,
                                           std::string_view input, double timeout_s,
                                           const CancelToken& cancel,
                                           const std::map<std::string, std::string>& extra_env) {
                return fv::run_once(argv, input, timeout_s, cancel, {}, extra_env, launcher);
            };
        }
        if (!services.subscribers) {
            services.subscribers = [](std::string_view label) {
                return bus().subscribers(label);
            };
        }
        if (!services.mcp_subscriptions) {
            services.mcp_subscriptions = [] {
                return std::size_t{0};
            };
        }
    }

    ~Impl() {
        // The strand first: its destructor waits for the work still running on the pool.
        strand.reset();
        own_pool.reset();
    }

    ActionHost& host;
    SettingsStore& settings;
    CredentialStore& credentials;
    EventSink& events;
    Executor& ui;
    ActionServices services;
    std::shared_ptr<detail::ResultRelay> relay;

    mutable std::mutex mutex;
    // Runs on the strand by relay token, in the order they were submitted.
    std::map<std::uint64_t, Pending> pending;
    bool closed = false;
    std::unique_ptr<ThreadPool> own_pool;
    std::unique_ptr<Strand> strand;

    // -- entry points ----------------------------------------------------------------------------

    void deliver(std::shared_ptr<const dictation::DictationJob> job, std::string text,
                 std::string raw_text, std::string html, std::string notice,
                 std::optional<Action> approved = std::nullopt,
                 std::shared_ptr<const Json> approved_settings = {}) {
        if (!job) {
            job = std::make_shared<const dictation::DictationJob>();
        }
        // A routed dictation took a clipboard snapshot for a paste that will not happen.
        host.discard_clipboard_prefetch();
        std::shared_ptr<const Json> snapshot =
            approved_settings ? std::move(approved_settings) : settings.snapshot();
        const std::optional<Action> action =
            approved ? std::move(approved) : find_action(*snapshot, job->action_id);
        auto run = std::make_shared<Run>();
        run->job = job;
        if (action) {
            run->action = *action;
        } else {
            run->action.id = job->action_id;
        }
        run->text = std::move(text);
        run->raw_text = std::move(raw_text);
        run->html = std::move(html);
        run->settings = snapshot;
        run->notice = std::move(notice);
        if (!action || !action->enabled) {
            finish(run,
                   ActionResult{false, std::string(action ? DISABLED_MESSAGE : MISSING_MESSAGE)});
            return;
        }
        if (job->program_words() && !program_words_allowed(*action, *snapshot)) {
            // Not sent; the user's own run keeps the words on the clipboard.
            finish(run, ActionResult{false, std::string(PROGRAM_WORDS_MESSAGE)});
            return;
        }
        start(run);
    }

    void test(const Action& action, std::string sample, std::function<void(ActionResult)> done) {
        auto job = std::make_shared<dictation::DictationJob>();
        job->mode = dictation::JobMode::Action;
        job->action_id = action.id;
        job->source = "ui";
        auto run = std::make_shared<Run>();
        run->job = std::move(job);
        run->action = action;
        run->text = std::move(sample);
        run->settings = settings.snapshot();
        run->done = std::move(done);
        if (action.kind == ActionKind::CHANNEL) {
            const std::size_t count = services.subscribers(CHANNEL_LISTENER_LABEL);
            run->done(ActionResult{count > 0, listening(count)});
            return;
        }
        start(run);
    }

    bool accepting() const {
        std::lock_guard lock(mutex);
        return !closed && pending.size() < MAX_PENDING;
    }

    int cancel(bool tests_only) {
        auto record = std::make_shared<CancelRecord>();
        record->quiet = tests_only;
        std::vector<std::pair<std::uint64_t, RunPtr>> canceled;
        std::vector<RunPtr> told;
        int earlier = 0;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending.begin(); it != pending.end();) {
                Pending& item = it->second;
                if (tests_only && !item.run->done) {
                    ++it;
                    continue;
                }
                if (item.canceled_by) {
                    ++earlier; // an earlier cancel reached it; its result is on the way
                    ++it;
                    continue;
                }
                // A run no worker has taken yet never will.
                int expected = kWaiting;
                if (item.phase->compare_exchange_strong(expected, kCanceled)) {
                    canceled.emplace_back(it->first, item.run);
                    record->stopped.push_back(item.run);
                    it = pending.erase(it);
                    continue;
                }
                // Under way: its result says what this cancel did.
                item.canceled_by = record;
                if (stoppable(item.run->action)) {
                    told.push_back(item.run);
                    ++record->waiting;
                } else {
                    record->late.push_back(item.run);
                }
                ++it;
            }
        }
        for (const RunPtr& run : told) {
            run->cancel.cancel();
        }
        if (!canceled.empty()) {
            std::vector<std::uint64_t> tokens;
            for (const auto& entry : canceled) {
                tokens.push_back(entry.first);
            }
            relay->drop(tokens);
        }
        for (const auto& [token, run] : canceled) {
            run->cancel.cancel();
            publish_ran(*run, false, CANCELED_MESSAGE, Output::NONE);
            if (run->done) {
                run->done(ActionResult{false, std::string(CANCELED_MESSAGE)});
            }
        }
        const int reached = static_cast<int>(canceled.size()) + record->waiting +
                            static_cast<int>(record->late.size());
        if (reached > 0) {
            kLog.info("Canceled {} action run(s) before they started; stopping {}; {} can't be "
                      "stopped",
                      canceled.size(), record->waiting, record->late.size());
        }
        tell(*record);
        return reached + earlier;
    }

    // Says what a cancel did, once every run it is stopping has answered.
    void tell(const CancelRecord& record) {
        if (record.quiet || record.waiting > 0 || (record.stopped.empty() && record.late.empty())) {
            return;
        }
        std::string message;
        if (!record.stopped.empty()) {
            message = "Canceled " + runs_text(record.stopped);
        }
        if (!record.late.empty()) {
            if (!message.empty()) {
                message += " — ";
            }
            message += "too late to stop " + runs_text(record.late);
        }
        message = upper_first(message);
        host.set_status(message);
        host.show_notice(message, false);
    }

    void close() {
        std::map<std::uint64_t, Pending> dropped;
        {
            std::lock_guard lock(mutex);
            closed = true;
            dropped.swap(pending);
        }
        std::vector<std::uint64_t> tokens;
        for (auto& [token, item] : dropped) {
            item.run->cancel.cancel();
            tokens.push_back(token);
        }
        if (!tokens.empty()) {
            relay->drop(tokens);
        }
        if (strand) {
            strand->shutdown(std::chrono::milliseconds(0));
        }
    }

    // -- running ---------------------------------------------------------------------------------

    void start(const RunPtr& run) {
        const std::string& kind = run->action.kind;
        if (kind == ActionKind::CLIPBOARD) {
            start_clipboard(run);
        } else if (kind == ActionKind::SCRIPTS) {
            start_scripts(run);
        } else if (kind == ActionKind::CHANNEL) {
            finish(run, send_to_channel(*run));
        } else if (kind == ActionKind::OPEN_LINK) {
            start_link(run);
        } else if (kind == ActionKind::WEBHOOK) {
            start_webhook(run);
        } else if (kind == ActionKind::PROGRAM) {
            start_program(run);
        } else if (kind == ActionKind::AGENT) {
            start_agent(run);
        } else {
            finish(run, ActionResult{false, std::format("{} can't run this kind of action",
                                                        identity().product)});
        }
    }

    void start_clipboard(const RunPtr& run) {
        if (!users_own(*run->job)) {
            // Like an answer a program's run would copy (output_of).
            host.show_in_scratchpad(run->text);
            finish(run, ActionResult{true, "Put in the Scratchpad"});
            return;
        }
        const bool copied = copy(run->text, run->html);
        finish(run, ActionResult{copied, copied ? "Copied" : "Couldn't copy the words"});
    }

    void start_scripts(const RunPtr& run) {
        // action.ran carries the words to whatever listens.
        if (scripts_listening(*run->settings, services.subscribers, services.mcp_subscriptions)) {
            finish(run, ActionResult{true, "Sent to your scripts"});
        } else {
            finish(run, ActionResult{false, std::string(NO_SCRIPTS_MESSAGE)});
        }
    }

    void start_link(const RunPtr& run) {
        std::string uri;
        try {
            uri = expand_link(param_text(run->action, "template"), run->text);
        } catch (const py::ValueError& error) {
            finish(run, ActionResult{false, error.what()});
            return;
        }
        submit(run, [open = services.open_uri, link = std::move(uri), words = run->text] {
            return open_link(open, link, words);
        });
    }

    void start_webhook(const RunPtr& run) {
        const Action& action = run->action;
        Json payload = webhook_payload(*run->job, action, run->text, run->raw_text, *run->settings);
        submit(run, [post = services.post_signed_reply, store = &credentials,
                     url = param_text(action, "url"), body = std::move(payload),
                     id = action.id] { return post_webhook(post, *store, url, body, id); });
    }

    void start_program(const RunPtr& run) {
        const Action& action = run->action;
        std::vector<std::string> argv{param_text(action, "program")};
        const Json args = action.param("args");
        if (args.is_array()) {
            for (const Json& arg : args) {
                argv.push_back(arg.is_string() ? arg.get<std::string>() : detail::py_str(arg));
            }
        }
        const std::optional<double> timeout_s = py::py_float(action.param("timeout_s"));
        if (!timeout_s) {
            throw py::ValueError("could not convert the program's time limit to float");
        }
        // run_once gives the program the user's environment minus the app's own keys and
        // tokens; scripts written for OpenWhisper read its names beside ours.
        std::map<std::string, std::string> extra;
        for (const std::string_view prefix :
             {identity().env_prefix, legacy_identity().env_prefix}) {
            extra[std::format("{}ACTION_ID", prefix)] = action.id;
            extra[std::format("{}ACTION_NAME", prefix)] = action.name;
            extra[std::format("{}JOB_ID", prefix)] = run->job->id;
        }
        submit(run,
               [start = services.run_once, command = std::move(argv), input = run->text,
                timeout = *timeout_s, environment = std::move(extra), stop = run->cancel.token()] {
                   return run_program(start, command, input, timeout, environment, stop);
               });
    }

    void start_agent(const RunPtr& run) {
        const Action& action = run->action;
        AgentParams params;
        params.agent = param_text(action, "agent");
        params.ask = is_ask(action);
        params.surface = param_text(action, "surface");
        params.folder = param_text(action, "folder");
        params.new_folder = detail::truthy(action.param("new_folder"));
        params.include_selection = detail::truthy(action.param("include_selection"));
        params.model = param_text(action, "model");
        submit(run,
               [seams = services, agent = std::move(params), text = run->text, job = run->job,
                stop = run->cancel.token()] { return run_agent(seams, agent, text, *job, stop); });
    }

    // Runs `work` on the strand and finishes `run` with its result on the UI executor.
    void submit(const RunPtr& run, std::function<ActionResult()> work) {
        const std::uint64_t token = detail::ResultRelay::next_token();
        auto phase = std::make_shared<std::atomic<int>>(kWaiting);
        std::string refusal;
        Strand* executor = nullptr;
        {
            std::lock_guard lock(mutex);
            if (closed) {
                refusal = closing_message();
            } else if (pending.size() >= MAX_PENDING) {
                refusal = std::string(BUSY_MESSAGE);
            } else {
                if (!strand) {
                    ThreadPool* pool = services.pool;
                    if (pool == nullptr) {
                        own_pool = std::make_unique<ThreadPool>("action-run", MAX_WORKERS);
                        pool = own_pool.get();
                    }
                    strand = std::make_unique<Strand>(*pool, "action-run", MAX_WORKERS,
                                                      ShutdownMode::CancelQueued);
                }
                executor = strand.get();
                pending[token] = Pending{run, phase, nullptr};
                std::weak_ptr<Impl> weak = weak_from_this();
                relay->expect(token, [weak, token, run](ActionResult result) {
                    if (auto self = weak.lock()) {
                        self->finish_from_worker(token, run, std::move(result));
                    }
                });
            }
        }
        if (executor == nullptr) {
            finish(run, ActionResult{false, refusal});
            return;
        }
        if (!run->done) {
            host.set_status(std::format("{} · working…", run->action.name));
        }
        // Bound here: the worker never reaches back into the runtime.
        executor->post([results = relay, token, phase, stop = run->cancel.token(),
                        kind = run->action.kind, task = std::move(work)] {
            int expected = kWaiting;
            if (!phase->compare_exchange_strong(expected, kStarted)) {
                return; // canceled before a worker took it
            }
            if (stop.cancelled()) {
                // Stopped as a worker took it (or the app closed: its token is gone).
                results->finished(token, stopped_result());
                return;
            }
            ActionResult result;
            try {
                result = task();
            } catch (const std::exception& error) {
                kLog.warning("A {} action failed: {}", kind, detail::error_type(error));
                result = ActionResult{false, "something went wrong"};
            } catch (...) {
                kLog.warning("A {} action failed: unknown error", kind);
                result = ActionResult{false, "something went wrong"};
            }
            results->finished(token, std::move(result));
        });
    }

    void finish_from_worker(std::uint64_t token, const RunPtr& run, ActionResult result) {
        std::shared_ptr<CancelRecord> canceled_by;
        {
            std::lock_guard lock(mutex);
            if (auto node = pending.extract(token)) {
                canceled_by = node.mapped().canceled_by;
            }
        }
        if (canceled_by) {
            finish_canceled(run, std::move(result), *canceled_by);
            return;
        }
        if (!result.ok && !run->done) {
            result.message = std::format("{}: {}", run->action.name, result.message);
        }
        finish(run, std::move(result));
    }

    // The outcome of a run `cancel` reached under way. Its result says whether the cancel stopped
    // it; if not, it is too late: what happened is reported, and its answer goes nowhere.
    void finish_canceled(const RunPtr& run, ActionResult result, CancelRecord& record) {
        if (result.stopped) {
            publish_ran(*run, false, CANCELED_MESSAGE, Output::NONE);
            if (run->done) {
                run->done(ActionResult{false, std::string(CANCELED_MESSAGE)});
            }
        } else if (run->done) {
            finish(run, result); // Try it learns what happened
        } else {
            const std::string& name = run->action.name;
            const std::string message =
                !result.ok ? py::rstrip(std::format("{}: {}", name, result.message), ".")
                           : (result.message.empty() ? "Sent to " + name : result.message);
            publish_ran(*run, result.ok, message, Output::NONE);
        }
        if (stoppable(run->action)) {
            // The cancel was waiting to hear how this one went.
            --record.waiting;
            (result.stopped ? record.stopped : record.late).push_back(run);
            tell(record);
        }
    }

    // Shows the outcome, routes an answer and publishes action.ran.
    void finish(const RunPtr& run, ActionResult result) {
        const std::string& name = run->action.name;
        if (run->done) {
            std::string message = result.message.empty() ? "Sent to " + name : result.message;
            message = upper_first(message);
            publish_ran(*run, result.ok, message, Output::NONE);
            run->done(ActionResult{result.ok, message, result.output});
            return;
        }
        if (!result.ok) {
            std::string message = py::rstrip(result.message, ".");
            std::string_view where = Output::NONE;
            // Words the user spoke or typed are kept on the clipboard; a program's are not its to
            // overwrite the clipboard with. A copy action's failure is the copy itself.
            if (run->action.kind != ActionKind::CLIPBOARD && users_own(*run->job) &&
                copy(run->text, run->html)) {
                message += COPIED_INSTEAD;
                where = Output::CLIPBOARD;
            }
            say(*run, message, false);
            publish_ran(*run, false, message, where);
            return;
        }
        const auto [ok, message, where] = route(*run, result);
        publish_ran(*run, ok, message, where);
    }

    // _output_of: where a run's answer goes. A program's run never copies or pastes: an answer
    // it would put on the clipboard or paste goes to the Scratchpad.
    std::string_view output_of(const Run& run) const {
        const Action& action = run.action;
        const bool answers = action.kind == ActionKind::WEBHOOK ||
                             action.kind == ActionKind::PROGRAM ||
                             (action.kind == ActionKind::AGENT && is_ask(action));
        if (!answers) {
            return Output::NONE;
        }
        const Json value = action.param("output");
        if (!value.is_string()) {
            return Output::NONE;
        }
        const auto output = value.get<std::string>();
        const auto known = std::ranges::find(Output::ALL, output);
        if (known == Output::ALL.end()) {
            return Output::NONE;
        }
        if ((*known == Output::PASTE || *known == Output::CLIPBOARD) && !users_own(*run.job)) {
            return Output::SCRATCHPAD;
        }
        return *known;
    }

    // Puts the answer where the action sends it: (ok, message, where it went).
    std::tuple<bool, std::string, std::string_view> route(const Run& run,
                                                          const ActionResult& result) {
        const std::string& name = run.action.name;
        const std::string_view output = output_of(run);
        const std::string answer = py::strip(result.output);
        if (output == Output::NONE || answer.empty()) {
            std::string message = result.message.empty() ? "Sent to " + name : result.message;
            if (output != Output::NONE) {
                message = std::format("Sent to {} — nothing came back", name);
            }
            say(run, message, true);
            return {true, message, Output::NONE};
        }
        if (output == Output::PASTE) {
            return paste(run, answer);
        }
        if (output == Output::SCRATCHPAD) {
            host.show_in_scratchpad(answer);
            std::string message = std::format("{} answered in the Scratchpad", name);
            say(run, message, true);
            return {true, message, Output::SCRATCHPAD};
        }
        if (output == Output::NOTICE) {
            std::string shown = short_text(answer);
            std::string message = std::format("Showed {}'s answer", name);
            // A notice holds only the start of a long answer; the clipboard keeps all of it,
            // for the user's own runs.
            if (py::len(flattened(answer)) > NOTICE_MAX_CHARS && users_own(*run.job) &&
                copy(answer)) {
                shown += FULL_ANSWER_COPIED;
                message += FULL_ANSWER_COPIED;
            }
            say(run, shown, true);
            return {true, message, Output::NOTICE};
        }
        if (copy(answer)) {
            std::string message = std::format("Copied {}'s answer", name);
            say(run, message, true);
            return {true, message, Output::CLIPBOARD};
        }
        std::string message = std::format("Couldn't copy {}'s answer", name);
        say(run, message, false);
        return {false, message, Output::NONE};
    }

    // Pastes `answer` only into the app the job started in, else copies it. Only the user's runs
    // get here (output_of).
    std::tuple<bool, std::string, std::string_view> paste(const Run& run,
                                                          const std::string& answer) {
        const std::string& name = run.action.name;
        const dictation::PasteTarget target = host.paste_target(*run.job);
        std::string reason;
        if (target == dictation::PasteTarget::Same) {
            if (host.paste_text_now(answer, run.job.get())) {
                std::string message = std::format("Pasted {}'s answer", name);
                say(run, message, true);
                return {true, message, Output::PASTE};
            }
            reason = "couldn't paste it";
        } else if (target == dictation::PasteTarget::Changed) {
            reason = "the app changed";
        } else {
            reason = "couldn't confirm the app";
        }
        if (copy(answer)) {
            std::string message = std::format("Copied — {}", reason);
            say(run, message, true);
            return {true, message, Output::CLIPBOARD};
        }
        std::string message = std::format("Couldn't paste or copy {}'s answer", name);
        say(run, message, false);
        return {false, message, Output::NONE};
    }

    // -- kinds that finish on the UI thread ------------------------------------------------------

    ActionResult send_to_channel(const Run& run) {
        if (services.subscribers(CHANNEL_LISTENER_LABEL) == 0) {
            return ActionResult{false, std::string(NO_LISTENER_MESSAGE)};
        }
        const Json fields =
            dictation::history_fields(run.job.get(), std::nullopt, true, *run.settings);
        events.publish(catalog::CHANNEL_MESSAGE,
                       catalog::channel_message(run.job.get(), run.action, run.text,
                                                catalog::app_from_fields(fields)));
        return ActionResult{true, "Sent to " + run.action.name};
    }

    // -- showing ---------------------------------------------------------------------------------

    bool copy(const std::string& text, const std::string& html = {}) {
        return host.copy_to_clipboard(text, html);
    }

    // The outcome in the status line and near the pointer, with the run's notice.
    void say(const Run& run, std::string message, bool ok) {
        if (!run.notice.empty()) {
            message = std::format("{} — {}", message, run.notice);
        }
        host.set_status(message);
        host.show_notice(message, ok);
    }

    void publish_ran(const Run& run, bool ok, std::string_view message, std::string_view where) {
        // "Send to my scripts" exists to hand its listeners the words.
        std::optional<std::string> text;
        if (run.action.kind == ActionKind::SCRIPTS) {
            text = run.text;
        }
        events.publish(catalog::ACTION_RAN,
                       catalog::action_ran(run.job.get(), run.action, ok, message, where, text));
    }
};

ActionRuntime::ActionRuntime(ActionHost& host, SettingsStore& settings,
                             CredentialStore& credentials, EventSink& events, Executor& ui,
                             ActionServices services)
    : impl_(std::make_shared<Impl>(host, settings, credentials, events, ui, std::move(services))) {}

ActionRuntime::~ActionRuntime() {
    impl_->close();
}

void ActionRuntime::deliver(std::shared_ptr<const dictation::DictationJob> job, std::string text,
                            std::string raw_text, std::string html, std::string notice) {
    impl_->deliver(std::move(job), std::move(text), std::move(raw_text), std::move(html),
                   std::move(notice));
}

void ActionRuntime::deliver_approved(std::shared_ptr<const dictation::DictationJob> job,
                                     Action action, std::shared_ptr<const Json> settings,
                                     std::string text, std::string raw_text, std::string html,
                                     std::string notice) {
    if (!job || !settings || action.id != job->action_id)
        throw std::invalid_argument("The approved action does not match the recording.");
    impl_->deliver(std::move(job), std::move(text), std::move(raw_text), std::move(html),
                   std::move(notice), std::move(action), std::move(settings));
}

void ActionRuntime::test(const Action& action, std::string sample,
                         std::function<void(ActionResult)> done) {
    impl_->test(action, std::move(sample), std::move(done));
}

bool ActionRuntime::accepting() const {
    return impl_->accepting();
}

int ActionRuntime::cancel(bool tests_only) {
    return impl_->cancel(tests_only);
}

int ActionRuntime::cancel_tests() {
    return impl_->cancel(true);
}

void ActionRuntime::close() {
    impl_->close();
}

} // namespace fv::automation
