#include "automation/filters.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <limits>
#include <utility>

#include "automation/action_model.h"
#include "automation/catalog.h"
#include "automation/detail/filter_verdict.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/url.h"
#include "automation/detail/worker_threads.h"
#include "automation/hook_http.h"
#include "automation/hooks_model.h"
#include "automation/permissions.h"
#include "base/ids.h"
#include "base/log.h"
#include "dictation/events.h"
#include "dictation/pipeline.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.automation.filters"};

using SteadyTime = std::chrono::steady_clock::time_point;

// How often a wait on a filter looks at whether the dictation was cancelled.
constexpr auto kCancelPoll = std::chrono::milliseconds(20);

std::chrono::steady_clock::duration seconds(double value) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(value));
}

double seconds_between(SteadyTime from, SteadyTime to) {
    return std::chrono::duration<double>(to - from).count();
}

std::string type_name(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    return "RuntimeError";
}

// The threads filters are waited on with. A filter that is abandoned (timed out or cancelled)
// finishes on its own; these are joined when the process ends, after their requests were told
// to stop.
detail::WorkerThreads& filter_workers() {
    static detail::WorkerThreads workers;
    return workers;
}

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c';
}

const std::string& require_string(const Json& value) {
    return value.get_ref<const std::string&>();
}

// _lease_timeout.
std::int64_t lease_timeout(const Json& given) {
    const auto refuse = [] {
        return py::ValueError(std::format("Use a timeout_ms from {} to {}.",
                                          hooks::FILTER_TIMEOUT_MIN_MS,
                                          hooks::FILTER_TIMEOUT_MAX_MS));
    };
    std::int64_t value = 0;
    if (given.is_number_float()) {
        const double number = given.get<double>();
        // float.is_integer(), then int(): a value beyond the limits fails below either way.
        if (!std::isfinite(number) || number != std::floor(number) || number < -1e18 ||
            number > 1e18) {
            throw refuse();
        }
        value = static_cast<std::int64_t>(number);
    } else if (given.is_number_integer() && !given.is_boolean()) {
        if (given.is_number_unsigned() &&
            given.get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw refuse();
        }
        value = given.get<std::int64_t>();
    } else {
        throw refuse();
    }
    if (value < hooks::FILTER_TIMEOUT_MIN_MS || value > hooks::FILTER_TIMEOUT_MAX_MS) {
        throw refuse();
    }
    return value;
}

// _lease_modes.
std::vector<std::string> lease_modes(const Json& given) {
    const py::ValueError refuse("Give modes as a list: dictation, action or both.");
    if (!given.is_array()) {
        throw refuse; // a str, a mapping or a scalar is not a sequence of modes
    }
    std::vector<std::string> cleaned;
    bool unknown = false;
    std::vector<Json> seen;
    for (const Json& item : given) {
        if (item.is_array() || item.is_object()) {
            throw py::TypeError(
                std::format("unhashable type: '{}'", item.is_array() ? "list" : "dict"));
        }
        if (std::ranges::any_of(seen,
                                [&](const Json& other) { return py::py_equal(other, item); })) {
            continue;
        }
        seen.push_back(item);
        if (!item.is_string() ||
            std::ranges::find(filters::MODES, require_string(item)) == filters::MODES.end()) {
            unknown = true;
            continue;
        }
        cleaned.push_back(item.get<std::string>());
    }
    if (seen.empty() || unknown) {
        throw refuse;
    }
    return cleaned;
}

double unix_seconds(const Clock& clock) {
    return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
}

} // namespace

// -- leases
// ----------------------------------------------------------------------------------------

Json Lease::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["url"] = url;
    out["stage"] = stage;
    out["timeout_ms"] = timeout_ms;
    out["modes"] = modes;
    out["expires_at"] = expires_at;
    out["ttl_s"] = filters::LEASE_TTL_S;
    return out;
}

FilterLeases::FilterLeases(const Clock& clock, double ttl_s) : clock_(clock), ttl_s_(ttl_s) {}

void FilterLeases::purge_expired() const {
    const SteadyTime now = clock_.steady();
    for (std::size_t i = leases_.size(); i-- > 0;) {
        if (deadlines_[i] <= now) {
            leases_.erase(leases_.begin() + static_cast<std::ptrdiff_t>(i));
            deadlines_.erase(deadlines_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

Lease FilterLeases::register_lease(std::string_view url, std::string_view stage,
                                   std::optional<Json> timeout_ms, std::optional<Json> modes) {
    const std::string cleaned_url = py::strip(url);
    hooks::validate_url(cleaned_url);
    const std::string host = detail::urlsplit(cleaned_url).hostname().value_or("");
    if (std::ranges::find(filters::LOOPBACK_HOSTS, py::lower(host)) ==
        filters::LOOPBACK_HOSTS.end()) {
        throw py::ValueError("A filter added over the API must listen on this computer "
                             "(127.0.0.1, ::1 or localhost).");
    }
    if (std::ranges::find(hooks::STAGES, stage) == std::end(hooks::STAGES)) {
        throw py::ValueError(
            std::format("Use the stage {} or {}.", hooks::STAGES[0], hooks::STAGES[1]));
    }
    const std::int64_t timeout =
        lease_timeout(timeout_ms.value_or(Json(hooks::FILTER_TIMEOUT_DEFAULT_MS)));
    Json default_modes = Json::array();
    for (const auto mode : filters::MODES) {
        default_modes.push_back(std::string(mode));
    }
    std::vector<std::string> cleaned_modes = lease_modes(modes.value_or(default_modes));
    std::lock_guard lock(mutex_);
    purge_expired();
    if (leases_.size() >= filters::MAX_LEASES) {
        throw py::ValueError(std::format("{} filters are already registered. Remove one first.",
                                         filters::MAX_LEASES));
    }
    Lease lease{"flt_" + random_hex(16),  cleaned_url,
                std::string(stage),       timeout,
                std::move(cleaned_modes), unix_seconds(clock_) + ttl_s_};
    leases_.push_back(lease);
    deadlines_.push_back(clock_.steady() + seconds(ttl_s_));
    return lease;
}

std::optional<Lease> FilterLeases::renew_lease(std::string_view id) {
    std::lock_guard lock(mutex_);
    purge_expired();
    for (std::size_t i = 0; i < leases_.size(); ++i) {
        if (leases_[i].id == id) {
            leases_[i].expires_at = unix_seconds(clock_) + ttl_s_;
            deadlines_[i] = clock_.steady() + seconds(ttl_s_);
            return leases_[i];
        }
    }
    return std::nullopt;
}

bool FilterLeases::drop_lease(std::string_view id) {
    std::lock_guard lock(mutex_);
    purge_expired();
    for (std::size_t i = 0; i < leases_.size(); ++i) {
        if (leases_[i].id == id) {
            leases_.erase(leases_.begin() + static_cast<std::ptrdiff_t>(i));
            deadlines_.erase(deadlines_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

int FilterLeases::drop_all_leases() {
    std::lock_guard lock(mutex_);
    purge_expired();
    const int count = static_cast<int>(leases_.size());
    leases_.clear();
    deadlines_.clear();
    return count;
}

std::vector<Lease> FilterLeases::leases() const {
    std::lock_guard lock(mutex_);
    purge_expired();
    return leases_;
}

// -- one filter
// ------------------------------------------------------------------------------------

namespace detail {

Verdict interpret(std::string_view raw) {
    const bool blank = std::ranges::all_of(raw, is_ascii_space);
    if (blank) {
        return {VerdictKind::Keep, {}, {}};
    }
    if (!encodes_as_utf8(raw)) {
        return {VerdictKind::Failed, {}, "The reply wasn't JSON"};
    }
    Json reply;
    try {
        reply = py::loads(raw);
    } catch (const py::ValueError&) {
        return {VerdictKind::Failed, {}, "The reply wasn't JSON"};
    }
    if (!reply.is_object()) {
        return {VerdictKind::Failed, {}, "The reply wasn't a JSON object"};
    }
    const auto cancel = reply.find("cancel");
    if (cancel != reply.end()) {
        if (!cancel->is_boolean()) {
            return {VerdictKind::Failed, {}, "\"cancel\" wasn't true or false"};
        }
        if (cancel->get<bool>()) {
            return {VerdictKind::Cancel, {}, {}};
        }
    }
    const auto text = reply.find("text");
    if (text == reply.end()) {
        return {VerdictKind::Keep, {}, {}};
    }
    if (!text->is_string()) {
        return {VerdictKind::Failed, {}, "\"text\" wasn't a string"};
    }
    // JSON may escape half of a surrogate pair ("\ud83d"); such text cannot be pasted, saved or
    // published.
    if (!encodes_as_utf8(std::string_view(require_string(*text)))) {
        return {VerdictKind::Failed, {}, "\"text\" wasn't valid Unicode"};
    }
    return {VerdictKind::Replace, text->get<std::string>(), {}};
}

Verdict await_verdict(std::function<Verdict(const CancelToken& abandoned)> work,
                      std::optional<double> timeout_s, const CancelToken& cancel,
                      std::string_view failed_copy) {
    struct Waiting {
        std::mutex mutex;
        std::condition_variable answered;
        std::optional<Verdict> answer;
    };
    auto waiting = std::make_shared<Waiting>();
    CancelSource abandon;
    filter_workers().spawn("text-filter", [waiting, work = std::move(work), token = abandon.token(),
                                           failed_copy = std::string(failed_copy)] {
        Verdict verdict;
        try {
            verdict = work(token);
        } catch (const std::exception& error) {
            verdict = {
                VerdictKind::Failed, {}, std::format("{} ({})", failed_copy, type_name(error))};
        }
        std::lock_guard lock(waiting->mutex);
        waiting->answer = std::move(verdict);
        waiting->answered.notify_all();
    });
    std::optional<SteadyTime> deadline;
    if (timeout_s) {
        deadline = std::chrono::steady_clock::now() + seconds(*timeout_s);
    }
    std::unique_lock lock(waiting->mutex);
    while (true) {
        // A cancel wins over an answer that arrived with it: a program filter sees the same
        // cancel and answers that it was stopped.
        if (cancel.cancelled()) {
            abandon.cancel();
            return {VerdictKind::Canceled, {}, {}};
        }
        if (waiting->answer) {
            return *waiting->answer;
        }
        std::optional<std::chrono::steady_clock::duration> wait;
        if (deadline) {
            wait = *deadline - std::chrono::steady_clock::now();
            if (*wait <= std::chrono::steady_clock::duration::zero()) {
                abandon.cancel();
                return {VerdictKind::Failed, {}, hook_http::no_answer_copy(*timeout_s)};
            }
        }
        // Only a token that can fire needs looking at; Python polls whenever a cancel event is
        // given, which every live job has.
        wait = wait ? std::min<std::chrono::steady_clock::duration>(*wait, kCancelPoll)
                    : std::chrono::steady_clock::duration(kCancelPoll);
        waiting->answered.wait_for(lock, *wait);
    }
}

Verdict program_verdict(const FilterRunOnce& run_once, const std::vector<std::string>& argv,
                        std::string_view body, double timeout_s, const CancelToken& cancel) {
    std::string input(body);
    input += '\n';
    const RunOnceResult result = run_once ? run_once(argv, input, timeout_s, cancel)
                                          : fv::run_once(argv, input, timeout_s, cancel);
    if (!result.error.empty()) {
        return {VerdictKind::Failed, {}, result.error};
    }
    // Python's `code != 0` also holds for a code of None, which run_once gives only with an error.
    if (result.exit_code.value_or(-1) != 0) {
        return {VerdictKind::Failed,
                {},
                result.exit_code ? std::format("Exited with code {}", *result.exit_code)
                                 : std::string("Exited with code None")};
    }
    return interpret(result.output);
}

} // namespace detail

// -- a stage
// -----------------------------------------------------------------------------------------

struct StageRun::Step {
    std::string id;
    std::string name;
    std::string kind;
    std::string url;
    std::vector<std::string> argv;
    double timeout_s = 0.0;
    // Leased over the live API by a program, rather than saved by the user.
    bool leased = false;
};

namespace {

// runtime.program_words_allowed: an agent or channel action takes a program's words only with
// "Send words to my coding agents".
bool program_words_allowed(const Action& action, const Json& settings) {
    const bool agentic = action.kind == ActionKind::AGENT || action.kind == ActionKind::CHANNEL;
    return !agentic || permissions::granted(settings, permissions::AGENT_PROMPTS);
}

// _leases_apply: leases need the text filters permission, and an action job whose action would
// not take a program's words skips them, so the user's own words still reach it.
bool leases_apply(const dictation::DictationJob& job, const Json& settings) {
    if (!permissions::granted(settings, permissions::TEXT_FILTERS)) {
        return false;
    }
    if (job.mode != dictation::JobMode::Action) {
        return true;
    }
    const auto action = find_action(settings, job.action_id);
    return !action || program_words_allowed(*action, settings);
}

// _filter_request: what every filter of the stage gets; "text" changes per step.
Json filter_request(std::string_view stage, const dictation::DictationJob& job,
                    const Json& settings, std::string_view text) {
    Json language = nullptr;
    Json app = nullptr;
    try {
        const Json fields = dictation::history_fields(&job, std::nullopt, true, settings);
        const auto found = fields.find("language");
        if (found != fields.end() && found->is_string() && !require_string(*found).empty()) {
            language = *found;
        }
        app = dictation::events::app_from_fields(fields);
    } catch (const std::exception& error) {
        kLog.warning("Filter context unavailable: {}", type_name(error));
        language = nullptr;
        app = nullptr;
    }
    Json request = Json::object();
    request["v"] = filters::REQUEST_VERSION;
    request["stage"] = std::string(stage);
    request["job_id"] = job.id;
    request["mode"] = std::string(dictation::to_string(job.mode));
    request["action_id"] = job.action_id.empty() ? Json(nullptr) : Json(job.action_id);
    request["text"] = std::string(text);
    request["language"] = language;
    request["app"] = app;
    return request;
}

// _post.
detail::Verdict post(const std::shared_ptr<http::Client>& client, const std::string& url,
                     std::string body, double timeout_s, const CancelToken& abandoned) {
    const hook_http::Result result = hook_http::exchange(
        *client, url, std::move(body), {{"Content-Type", std::string(hook_http::CONTENT_TYPE)}},
        timeout_s, filters::MAX_REPLY_BYTES, std::chrono::steady_clock::now() + seconds(timeout_s),
        abandoned);
    if (!result.ok) {
        return {detail::VerdictKind::Failed, {}, result.error};
    }
    if (result.status == 204) {
        return {detail::VerdictKind::Keep, {}, {}};
    }
    return detail::interpret(result.body);
}

} // namespace

StageRun::StageRun(std::string stage, std::optional<Json> settings, FilterContext context,
                   CancelToken cancel)
    : stage_(std::move(stage)), settings_(std::move(settings)), context_(std::move(context)),
      cancel_(std::move(cancel)) {}

FilterOutcome StageRun::saved(std::string text, const dictation::DictationJob& job) {
    return run(std::move(text), &job, true, false);
}

FilterOutcome StageRun::leased(std::string text, const dictation::DictationJob& job) {
    return run(std::move(text), &job, false, true);
}

FilterOutcome StageRun::apply(std::string text, const dictation::DictationJob& job) {
    return run(std::move(text), &job, true, true);
}

std::vector<StageRun::Step> StageRun::steps(const dictation::DictationJob& job, bool saved,
                                            bool leased) const {
    std::vector<Step> steps;
    const Json& settings = *settings_;
    if (saved) {
        for (const auto& item : hooks::load_filters(settings)) {
            if (!item.enabled || item.stage != stage_) {
                continue;
            }
            std::vector<std::string> argv{item.program};
            argv.insert(argv.end(), item.args.begin(), item.args.end());
            steps.push_back(Step{item.id, item.name, item.kind, item.url, std::move(argv),
                                 static_cast<double>(item.timeout_ms) / 1000.0, false});
        }
    }
    if (leased && leases_apply(job, settings)) {
        const std::string_view mode = dictation::to_string(job.mode);
        for (const auto& lease : context_.leases.leases()) {
            if (lease.stage == stage_ &&
                std::ranges::find(lease.modes, mode) != lease.modes.end()) {
                steps.push_back(Step{lease.id,
                                     std::string(filters::LEASED_NAME),
                                     std::string(hooks::FILTER_HTTP),
                                     lease.url,
                                     {},
                                     static_cast<double>(lease.timeout_ms) / 1000.0,
                                     true});
            }
        }
    }
    return steps;
}

FilterOutcome StageRun::run(std::string text, const dictation::DictationJob* job, bool saved,
                            bool leased) {
    if (job == nullptr) {
        return FilterOutcome{std::move(text)};
    }
    const std::string_view mode = dictation::to_string(job->mode);
    if (std::ranges::find(filters::MODES, mode) == filters::MODES.end() ||
        py::strip(text).empty() || cancel_.cancelled()) {
        return FilterOutcome{std::move(text)};
    }
    if (!settings_) {
        settings_ = context_.load_settings ? context_.load_settings() : Json::object();
    }
    const std::vector<Step> found = steps(*job, saved, leased);
    if (found.empty()) {
        return FilterOutcome{std::move(text)};
    }
    const SteadyTime deadline = std::chrono::steady_clock::now() + seconds(budget_s_);
    struct Spend {
        StageRun& run;
        SteadyTime deadline;
        // What a later part of this stage has left.
        ~Spend() {
            run.budget_s_ =
                std::max(0.0, seconds_between(std::chrono::steady_clock::now(), deadline));
        }
    } spend{*this, deadline};
    return run_steps(found, text, *job, deadline);
}

FilterOutcome StageRun::run_steps(const std::vector<Step>& steps, const std::string& text,
                                  const dictation::DictationJob& job, SteadyTime deadline) {
    Json request = filter_request(stage_, job, *settings_, text);
    // Saved filters run before leases: what they leave is still the user's.
    std::string current = text;
    std::string users_text = text;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const Step& step = steps[index];
        if (cancel_.cancelled()) {
            return FilterOutcome{text};
        }
        const double remaining = seconds_between(std::chrono::steady_clock::now(), deadline);
        if (remaining < filters::MIN_SLICE_S) {
            for (std::size_t skipped = index; skipped < steps.size(); ++skipped) {
                failed(steps[skipped],
                       std::format("Skipped: this stage's {:g} s for filters ran out",
                                   filters::STAGE_BUDGET_S));
            }
            break;
        }
        request["text"] = current;
        std::string body =
            py::dumps(request, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"});
        const double timeout = std::min(step.timeout_s, remaining);
        detail::Verdict verdict;
        try {
            if (step.kind == hooks::FILTER_HTTP) {
                verdict = detail::await_verdict(
                    [client = context_.http, url = step.url, body,
                     timeout](const CancelToken& abandoned) {
                        return post(client, url, body, timeout, abandoned);
                    },
                    timeout, cancel_, "The request failed");
            } else {
                verdict = detail::await_verdict(
                    [run_once = context_.run_once, argv = step.argv, body, timeout,
                     cancel = cancel_](const CancelToken&) {
                        return detail::program_verdict(run_once, argv, body, timeout, cancel);
                    },
                    std::nullopt, cancel_, "The filter couldn't run");
            }
        } catch (const std::exception& error) {
            verdict = {detail::VerdictKind::Failed,
                       {},
                       std::format("The filter couldn't run ({})", type_name(error))};
        }
        switch (verdict.kind) {
        case detail::VerdictKind::Canceled:
            return FilterOutcome{text};
        case detail::VerdictKind::Failed:
            failed(step, verdict.reason);
            break;
        case detail::VerdictKind::Cancel:
            return FilterOutcome{current, true, current != text, current != users_text};
        case detail::VerdictKind::Replace:
            if (py::strip(verdict.text).empty()) {
                return FilterOutcome{current, true, current != text, current != users_text};
            }
            current = std::move(verdict.text);
            if (!step.leased) {
                users_text = current;
            }
            break;
        case detail::VerdictKind::Keep:
            break;
        }
    }
    if (cancel_.cancelled()) {
        return FilterOutcome{text};
    }
    return FilterOutcome{current, false, current != text, current != users_text};
}

void StageRun::failed(const Step& step, std::string_view reason) {
    kLog.info("Text filter {} at {} kept the text: {}", step.id, stage_, reason);
    context_.events.publish(catalog::FILTER_FAILED,
                            catalog::filter_failed(step.id, step.name, stage_, reason));
}

FilterOutcome apply(std::string_view stage, std::string text, const dictation::DictationJob* job,
                    std::optional<Json> settings, FilterContext context,
                    const CancelToken& cancel) {
    StageRun run(std::string(stage), std::move(settings), std::move(context), cancel);
    if (job == nullptr) {
        return FilterOutcome{std::move(text)};
    }
    return run.apply(std::move(text), *job);
}

} // namespace fv::automation
