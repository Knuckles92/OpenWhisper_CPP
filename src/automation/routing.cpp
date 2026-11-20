#include "automation/routing.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "automation/catalog.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/route_words.h"
#include "automation/detail/thread_name.h"
#include "base/log.h"
#include "dictation/job.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.actions.routing"};

using detail::RouteWord;

// Long dictations are shown to the judge by their start and end, which is where an action is
// addressed; the request stays well inside TypeSafe's state budget however long someone talks.
// Code points.
constexpr std::size_t kStateHead = 1500;
constexpr std::size_t kStateTail = 400;
constexpr std::size_t kSpanHead = 200;
constexpr std::size_t kSpanTail = 80;

constexpr std::string_view kLeadingJunk = " \t\r\n,.;:!?-–—";
constexpr std::string_view kTrailingJunk = " \t\r\n,;:-–—";

// Worded as measured in benchmarks/voice_routing; rewording needs a live re-run.
constexpr std::string_view kAutoInstructions =
    "`dictation` starts with the name of one of the user's saved actions (speech recognition may "
    "mishear a name, such as \"cloud\" for \"Claude\"). Is the user speaking to that action, the "
    "way you address an assistant (\"Claude, ...\", \"ask Claude to ...\", \"todo: ...\"), to "
    "give it a request? Choose the action. Choose none when the name is the subject or object of "
    "an ordinary sentence meant to be typed, such as \"Claude said ...\" or \"I asked Claude "
    "...\".";
constexpr std::string_view kHotkeyInstructions =
    "The user recorded `dictation` with a shortcut that sends the words to one of their saved "
    "actions. Which action is it meant for? Usually the user says the action's name or a spoken "
    "name (speech recognition may mishear a name, such as \"cloud\" for \"Claude\"); otherwise "
    "choose the action whose purpose clearly fits the request. Choose none when no action fits.";

Json none_criteria(std::string_view route) {
    Json criteria = Json::object();
    if (route == ROUTE_AUTO) {
        criteria["what"] = "Ordinary dictation to type where the cursor is.";
        criteria["includes"] = "Sentences in which an action's name is just a word, such as "
                               "\"Claude said the build is green.\"";
    } else {
        criteria["what"] =
            "No action fits: ordinary dictation, or a request none of the actions handles.";
        criteria["includes"] = "Sentences that only mention an action's name or talk about it.";
    }
    return criteria;
}

constexpr std::size_t kMaxPayloadNames = 12;
// The whole request stays under this many characters of JSON, below llm::kMaxStateChars, by
// describing each action more briefly when there are many: (purpose code points, spoken names).
constexpr std::size_t kRequestChars = 20'000;
constexpr std::pair<std::size_t, std::size_t> kDescriptionLevels[] = {
    {300, 8}, {160, 4}, {80, 2}, {0, 0}};

// How often the wait for the judge looks at whether the dictation was cancelled.
constexpr std::chrono::milliseconds kCancelPoll{20};
constexpr std::string_view kCanceled = "canceled";

// _WHERE_FAST: Fast judgments live on another page than the status line.
constexpr std::string_view kWhereFast = "Settings → Meeting Mode → Fast judgments";

std::string kind_phrase(const Action& action) {
    if (action.kind == ActionKind::AGENT) {
        const Json agent_id = action.param("agent");
        std::string agent = "coding agent";
        if (agent_id.is_string()) {
            if (const auto name = agent_display_name(agent_id.get<std::string>())) {
                agent = std::string(*name);
            }
        }
        const Json mode = action.param("mode");
        if (mode.is_string() && mode.get<std::string>() == AgentMode::ASK) {
            return "Asks " + agent + " the words as a question and shows the answer.";
        }
        return "Starts a " + agent + " session with the words as the task.";
    }
    // Links only prefill (a new chat, a search); the user sends.
    if (action.kind == ActionKind::OPEN_LINK) {
        return "Opens an app or website with the words filled in, ready for the user to send.";
    }
    if (action.kind == ActionKind::CHANNEL) {
        return "Sends the words into a running Claude Code session.";
    }
    if (action.kind == ActionKind::WEBHOOK) {
        return "Sends the words to a web service.";
    }
    if (action.kind == ActionKind::PROGRAM) {
        return "Runs a program on this computer with the words.";
    }
    if (action.kind == ActionKind::CLIPBOARD) {
        return "Copies the words without pasting them.";
    }
    if (action.kind == ActionKind::SCRIPTS) {
        return "Sends the words to the user's own scripts.";
    }
    return "";
}

Json criterion(const Action& action, std::size_t purpose_chars, std::size_t spoken) {
    Json entry = Json::object();
    entry["name"] = action.name;
    if (spoken != 0 && !action.spoken_names.empty()) {
        Json names = Json::array();
        for (std::size_t i = 0; i < action.spoken_names.size() && i < spoken; ++i) {
            names.push_back(action.spoken_names[i]);
        }
        entry["spoken_names"] = std::move(names);
    }
    if (purpose_chars != 0 && !action.description.empty()) {
        entry["purpose"] =
            py::slice(action.description, 0, static_cast<std::int64_t>(purpose_chars));
    }
    entry["does"] = kind_phrase(action);
    return entry;
}

struct Found {
    std::size_t first;
    std::size_t name_start;
    std::size_t name_end;
};

std::vector<std::string> norms(std::span<const RouteWord> words) {
    std::vector<std::string> out;
    out.reserve(words.size());
    for (const RouteWord& word : words) {
        out.push_back(word.norm);
    }
    return out;
}

std::optional<Found> leading_match(const std::vector<RouteWord>& words,
                                   const std::vector<detail::NameVariant>& variants) {
    const std::size_t start = detail::skip_fillers(words);
    const std::span<const RouteWord> all(words);
    for (std::size_t skip = 0; skip <= detail::MAX_LEAD_INS; ++skip) {
        const std::size_t at = start + skip;
        if (at >= words.size() || (skip != 0 && !detail::is_lead_in(words[at - 1].norm))) {
            return std::nullopt;
        }
        const std::vector<std::string> said = norms(all.subspan(start, at - start));
        std::size_t best = 0;
        for (const detail::NameVariant& variant : variants) {
            const std::size_t n = detail::match_len(words, at, variant.words);
            if (n > best && variant.follows(said, all.subspan(at, n))) {
                best = n;
            }
        }
        if (best != 0) {
            return Found{start, at, at + best};
        }
    }
    return std::nullopt;
}

std::optional<Found> trailing_match(const std::vector<RouteWord>& words,
                                    const std::vector<detail::NameVariant>& variants) {
    const std::size_t end = words.size() - detail::polite_tail(words);
    const std::span<const RouteWord> all(words);
    for (const detail::NameVariant& variant : variants) {
        const std::size_t lowest =
            end > variant.words.size() + 2 ? end - variant.words.size() - 2 : 0;
        for (std::size_t at = lowest; at < end; ++at) {
            if (at == 0 || detail::match_len(words, at, variant.words) != end - at) {
                continue;
            }
            std::size_t first = at;
            while (first > 0 && at - first < detail::MAX_LEAD_INS &&
                   detail::is_lead_in(words[first - 1].norm)) {
                --first;
            }
            if (first < at && !variant.follows(norms(all.subspan(first, at - first)),
                                               all.subspan(at, end - at))) {
                first = at;
            }
            // Said last on its own, a name is set off by a pause ("Lock the front door, home."),
            // not the end of a sentence about it.
            if (first == 0 || (first == at && !words[at - 1].pause)) {
                continue;
            }
            return Found{first, at, end};
        }
    }
    return std::nullopt;
}

std::vector<GateMatch> gate_words(const std::vector<RouteWord>& words,
                                  const std::vector<Action>& actions, std::string_view route) {
    std::vector<GateMatch> matches;
    if (words.empty()) {
        return matches;
    }
    for (const Action& action : actions) {
        const std::vector<detail::NameVariant> variants =
            detail::name_variants(action.name, action.spoken_names);
        if (const auto found = leading_match(words, variants)) {
            matches.push_back(
                GateMatch{action.id, found->first, found->name_start, found->name_end, true});
            continue;
        }
        if (route == ROUTE_HOTKEY) {
            if (const auto found = trailing_match(words, variants)) {
                matches.push_back(
                    GateMatch{action.id, found->first, found->name_start, found->name_end, false});
            }
        }
    }
    // {a.id: i}: a repeated id takes its last position, as the dict comprehension does.
    std::map<std::string, std::size_t, std::less<>> order;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        order[actions[i].id] = i;
    }
    std::ranges::stable_sort(matches, [&](const GateMatch& a, const GateMatch& b) {
        if (a.leading != b.leading) {
            return a.leading;
        }
        const std::size_t a_span = a.name_end - a.first;
        const std::size_t b_span = b.name_end - b.first;
        if (a_span != b_span) {
            return a_span > b_span;
        }
        return order.at(a.action_id) < order.at(b.action_id);
    });
    return matches;
}

// float(value or 0.0)
double float_or_zero(const Json& value) {
    if (!detail::truthy(value)) {
        return 0.0;
    }
    const std::optional<double> number = py::py_float(value);
    if (!number) {
        if (value.is_string()) {
            throw py::ValueError("could not convert string to float");
        }
        throw py::TypeError("float() argument must be a string or a real number");
    }
    return *number;
}

// float(value), which refuses None.
double strict_float(const Json& value) {
    const std::optional<double> number = py::py_float(value);
    if (!number) {
        if (value.is_string()) {
            throw py::ValueError("could not convert string to float");
        }
        throw py::TypeError("float() argument must be a string or a real number");
    }
    return *number;
}

const std::string* span_for(const std::vector<std::pair<std::string, std::string>>& spans,
                            std::string_view option) {
    for (const auto& [key, span] : spans) {
        if (key == option) {
            return &span;
        }
    }
    return nullptr;
}

std::string payload_of(const RoutePlan& plan, const Json& answer) {
    const std::string full = py::strip(plan.text);
    if (plan.spans.empty() || !answer.is_object()) {
        return full;
    }
    const Json& probabilities = detail::get(answer, "probabilities");
    std::string span;
    if (probabilities.is_object() && !probabilities.empty()) {
        // "None of these" means the whole dictation, so it counts for that span.
        std::vector<std::pair<std::string, double>> weight;
        for (const auto& [option, p] : probabilities.items()) {
            const std::string* found =
                option != ROUTE_NONE ? span_for(plan.spans, option) : nullptr;
            const std::string& option_span = found != nullptr ? *found : full;
            if (p.is_number() || p.is_boolean()) {
                const double value = p.is_boolean() ? (p.get<bool>() ? 1.0 : 0.0) : p.get<double>();
                const auto at =
                    std::ranges::find(weight, option_span, &std::pair<std::string, double>::first);
                if (at == weight.end()) {
                    weight.emplace_back(option_span, value);
                } else {
                    at->second += value;
                }
            }
        }
        if (weight.empty()) {
            span = full;
        } else {
            // max() keeps the first of equal weights.
            auto best = weight.begin();
            for (auto it = weight.begin(); it != weight.end(); ++it) {
                if (it->second > best->second) {
                    best = it;
                }
            }
            span = best->first;
        }
    } else {
        const Json& choice = detail::get(answer, "choice");
        if (choice.is_array() || choice.is_object()) {
            throw py::TypeError("unhashable type");
        }
        const std::string* found =
            choice.is_string() ? span_for(plan.spans, choice.get<std::string>()) : nullptr;
        span = found != nullptr ? *found : full;
    }
    return span == full ? span : detail::sentence_start(span);
}

// What the helper thread asking the judge shares with the wait.
struct AskState {
    std::mutex mutex;
    std::condition_variable done_changed;
    std::optional<std::chrono::steady_clock::time_point> finished_at;
    std::optional<Json> answers;
    CancelSource stop;
};

// _ask: (answers, ""), or (nullopt, a FAILURE_* or kCanceled).
std::pair<std::optional<Json>, std::string>
ask_judge(const RoutePlan& request, const Json& settings, llm::JudgeProvider& provider,
          const CancelToken& cancel, std::shared_ptr<const void> keep_alive) {
    const double budget = route_budget(settings);
    // Start the route's budget before dispatch. The judge can have its own deadline, which
    // starts only when the worker reaches ask().
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(budget));
    auto shared = std::make_shared<AskState>();
    std::thread([shared, &provider, keep_alive = std::move(keep_alive), settings,
                 state = request.state, questions = request.questions, budget] {
        detail::set_current_thread_name("route-by-voice");
        std::optional<Json> answers;
        try {
            // A wait that already ended (a cancel, or the budget before this thread ran) reads
            // no key and sends nothing, and leaves `provider` alone once decide has returned.
            const std::shared_ptr<llm::Judge> judge =
                shared->stop.token().cancelled() ? nullptr : provider.judge(settings);
            if (shared->stop.token().cancelled()) {
                kLog.debug("Route by voice stopped waiting before it asked");
            } else if (!judge) {
                kLog.debug("Route by voice couldn't ask: Fast judgments aren't set up");
            } else {
                answers = judge->ask(state, questions, budget, shared->stop.token());
            }
        } catch (const std::exception& error) {
            kLog.warning("Route by voice judgment failed ({})", detail::error_type(error));
        } catch (...) {
            kLog.warning("Route by voice judgment failed (unknown)");
        }
        {
            std::lock_guard lock(shared->mutex);
            shared->answers = std::move(answers);
            shared->finished_at = std::chrono::steady_clock::now();
        }
        shared->done_changed.notify_all();
    }).detach();

    std::unique_lock lock(shared->mutex);
    while (true) {
        if (cancel.cancelled()) {
            lock.unlock();
            shared->stop.cancel();
            return {std::nullopt, std::string(kCanceled)};
        }
        // Judge completion counts when published, even if the waiter wakes late.
        if (shared->finished_at && *shared->finished_at <= deadline) {
            break;
        }
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            lock.unlock();
            shared->stop.cancel();
            return {std::nullopt, std::string(FAILURE_TIMEOUT)};
        }
        shared->done_changed.wait_for(
            lock, std::min<std::chrono::steady_clock::duration>(left, kCancelPoll));
    }
    std::optional<Json> answers = std::move(shared->answers);
    if (answers) {
        return {std::move(answers), std::string()};
    }
    return {std::nullopt, std::string(FAILURE_UNREACHABLE)};
}

RouteDecision decide_now(std::string_view text, const Json& settings, std::string_view route,
                         llm::JudgeProvider& provider, EventSink& events, const CancelToken& cancel,
                         std::string_view job_id, std::shared_ptr<const void> keep_alive) {
    if ((route != ROUTE_HOTKEY && route != ROUTE_AUTO) ||
        !keys::voice_routing_enabled.read(settings)) {
        return {};
    }
    if (route == ROUTE_AUTO && !keys::voice_routing_on_dictation.read(settings)) {
        return {};
    }
    std::vector<Action> enabled;
    for (Action& action : load_actions(settings)) {
        if (action.enabled) {
            enabled.push_back(std::move(action));
        }
    }
    const std::optional<RoutePlan> request = plan(text, enabled, route);
    if (!request || cancel.cancelled()) {
        return {};
    }
    auto [answers, failure] =
        ask_judge(*request, settings, provider, cancel, std::move(keep_alive));
    if (failure == kCanceled) {
        return {};
    }
    std::optional<Judgment> judgment;
    if (failure.empty()) {
        judgment = request->read(*answers);
        if (!judgment) {
            failure = FAILURE_UNREACHABLE; // it answered, but not the route question
        }
    }
    const bool routed = judgment && judgment->action_id && judgment->confidence >= ROUTE_CONFIDENCE;
    kLog.debug("Route by voice ({}, {} candidates): {} at {}", route, request->candidates.size(),
               !failure.empty() ? failure : judgment->action_id.value_or(std::string(ROUTE_NONE)),
               judgment ? py::format_fixed(judgment->confidence, 2) : std::string("-"));
    dictation::DictationJob job;
    job.id = std::string(job_id);
    events.publish(catalog::ROUTE_DECIDED,
                   catalog::route_decided(
                       &job, routed ? *judgment->action_id : std::string(),
                       judgment ? std::optional(judgment->confidence) : std::nullopt, !routed));
    if (!failure.empty()) {
        return RouteDecision{std::nullopt, "", std::nullopt, failure};
    }
    if (!routed) {
        return {};
    }
    return RouteDecision{judgment->action_id, judgment->payload, judgment->confidence, ""};
}

std::string route_shortcut(const Json& settings) {
    const Json hotkeys = keys::hotkeys.read(settings);
    if (!hotkeys.is_object()) {
        return "";
    }
    const Json& value = detail::get(hotkeys, ROUTE_HOTKEY_NAME);
    return value.is_string() ? py::strip(value.get<std::string>()) : std::string();
}

RoutingStatus off_status() {
    return {false, false, "Route by voice is off. Turn it on above.", ""};
}

RoutingStatus needs_fast_judgments_status() {
    return {true, false,
            "Route by voice needs Fast judgments. Turn them on in " + std::string(kWhereFast) + ".",
            std::string(FIX_FAST_JUDGMENTS)};
}

RoutingStatus status_now(const Json& settings, llm::JudgeProvider& provider) {
    if (!keys::voice_routing_enabled.read(settings)) {
        return off_status();
    }
    const llm::JudgeRoute judge_route = provider.route(settings);
    if (!keys::typesafe_enabled.read(settings)) {
        return needs_fast_judgments_status();
    }
    if (!provider.key_present(settings)) {
        return {true, false,
                "Route by voice needs " + judge_route.key_phrase() + ". Add one in " +
                    std::string(kWhereFast) + ".",
                std::string(FIX_FAST_JUDGMENTS)};
    }
    const std::vector<Action> actions = load_actions(settings);
    if (actions.empty()) {
        return {true, false, "Route by voice is on, but there are no actions yet. Add one above.",
                std::string(FIX_ACTIONS)};
    }
    if (std::ranges::none_of(actions, &Action::enabled)) {
        return {true, false,
                "Route by voice is on, but every action is turned off. Turn one on above.",
                std::string(FIX_ACTIONS)};
    }
    const bool shortcut = !route_shortcut(settings).empty();
    const bool on_dictation = keys::voice_routing_on_dictation.read(settings);
    if (!shortcut && !on_dictation) {
        return {true, false,
                "Route by voice is on, but has nothing to listen to yet. Set a Route by voice "
                "shortcut below, or turn on Also listen to normal dictations.",
                std::string(FIX_TRIGGER)};
    }
    const std::string to =
        (judge_route.is_local() ? "checked on this computer by " + judge_route.name
                                : "sent to " + judge_route.name) +
        " to choose the action.";
    std::string sent;
    if (shortcut && on_dictation) {
        sent = "Everything you record with the Route by voice shortcut, and dictations that start "
               "with an action's name, are " +
               to;
    } else if (shortcut) {
        sent = "Everything you record with the Route by voice shortcut is " + to;
    } else {
        sent = "Dictations that start with an action's name are " + to;
    }
    return {true, true, "Route by voice is on. " + sent, ""};
}

} // namespace

namespace detail {

std::vector<std::string> payload_spans(std::string_view text, const std::vector<RouteWord>& words,
                                       const std::vector<GateMatch>& matches) {
    std::vector<std::string> spans;
    const auto add = [&](std::string_view span) {
        std::string stripped = py::strip(span);
        if (!route_words(stripped).empty() && std::ranges::find(spans, stripped) == spans.end()) {
            spans.push_back(std::move(stripped));
        }
    };
    for (const GateMatch& match : matches) {
        if (match.leading) {
            if (match.name_end >= words.size()) {
                continue;
            }
            const std::string after = py::rstrip(
                py::lstrip(text.substr(words[match.name_end - 1].byte_end), kLeadingJunk));
            std::size_t linked = match.name_end;
            while (linked + 1 < words.size() && linked - match.name_end < MAX_LINK_WORDS &&
                   is_link_word(words[linked].norm)) {
                ++linked;
            }
            const std::string unlinked =
                linked > match.name_end ? py::rstrip(text.substr(words[linked].byte_start)) : after;
            add(after);
            add(unlinked);
            add(without_polite_end(after));
            add(without_polite_end(unlinked));
        } else {
            add(py::rstrip(text.substr(0, words[match.first].byte_start), kTrailingJunk));
        }
    }
    add(py::strip(text));
    return spans;
}

} // namespace detail

std::vector<GateMatch> gate(std::string_view text, const std::vector<Action>& actions,
                            std::string_view route) {
    return gate_words(detail::route_words(text), actions, route);
}

std::optional<Judgment> RoutePlan::read(const Json& answers) const {
    // (answers or {}).get(...): a falsy value reads as {}, any other value that is not a mapping
    // has no get.
    if (detail::truthy(answers) && !answers.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    const Json& route_answer = detail::get(answers, "route");
    if (!route_answer.is_object()) {
        return std::nullopt;
    }
    const Json& choice = detail::get(route_answer, "choice");
    if (!choice.is_string()) {
        return std::nullopt;
    }
    const std::string chosen = choice.get<std::string>();
    if (chosen != ROUTE_NONE && std::ranges::find(candidates, chosen) == candidates.end()) {
        return std::nullopt;
    }
    const Json& stated = detail::get(route_answer, "probabilities");
    const Json probabilities = detail::truthy(stated) ? stated : Json::object();
    const double confidence = float_or_zero(detail::get(route_answer, "confidence"));
    double probability = confidence;
    if (probabilities.is_object() && probabilities.contains(chosen)) {
        probability = strict_float(probabilities.at(chosen));
    }
    Judgment judgment;
    if (chosen != ROUTE_NONE) {
        judgment.action_id = chosen;
    }
    judgment.confidence = confidence;
    judgment.probability = probability;
    judgment.payload = payload_of(*this, detail::get(answers, "payload"));
    return judgment;
}

std::optional<RoutePlan> plan(std::string_view raw_text, const std::vector<Action>& actions,
                              std::string_view route) {
    const std::string text = py::strip(raw_text);
    const std::vector<RouteWord> words = detail::route_words(text);
    if ((route != ROUTE_AUTO && route != ROUTE_HOTKEY) || words.empty() || actions.empty()) {
        return std::nullopt;
    }
    const std::vector<GateMatch> matches = gate_words(words, actions, route);
    if (route == ROUTE_AUTO && matches.empty()) {
        return std::nullopt;
    }
    // {a.id: a}: a repeated id answers with its last action.
    std::map<std::string, const Action*, std::less<>> by_id;
    for (const Action& action : actions) {
        by_id[action.id] = &action;
    }
    RoutePlan request;
    request.text = text;
    request.route = std::string(route);
    for (const GateMatch& match : matches) {
        if (std::ranges::find(request.matched, match.action_id) == request.matched.end()) {
            request.matched.push_back(match.action_id);
        }
    }
    request.candidates = request.matched;
    if (route == ROUTE_HOTKEY) {
        for (const Action& action : actions) {
            if (std::ranges::find(request.matched, action.id) == request.matched.end()) {
                request.candidates.push_back(action.id);
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> span_options;
    for (const std::string& span : detail::payload_spans(text, words, matches)) {
        std::string option = detail::clip(span, kSpanHead, kSpanTail);
        if (span_for(span_options, option) == nullptr) {
            span_options.emplace_back(std::move(option), span);
        }
    }
    request.state = Json::object();
    request.state["dictation"] = detail::clip(text, kStateHead, kStateTail);

    Json questions;
    for (const auto& [purpose_chars, spoken] : kDescriptionLevels) {
        Json criteria = Json::object();
        for (const std::string& action_id : request.candidates) {
            criteria[action_id] = criterion(*by_id.at(action_id), purpose_chars, spoken);
        }
        criteria[std::string(ROUTE_NONE)] = none_criteria(route);
        questions = Json::object();
        Json route_question = Json::object();
        route_question["type"] = "choice";
        route_question["instructions"] =
            std::string(route == ROUTE_AUTO ? kAutoInstructions : kHotkeyInstructions);
        route_question["criteria"] = std::move(criteria);
        questions["route"] = std::move(route_question);
        if (span_options.size() > 1) {
            Json payload_criteria = Json::object();
            for (const auto& [option, span] : span_options) {
                payload_criteria[option] = nullptr;
            }
            payload_criteria[std::string(ROUTE_NONE)] = std::string(PAYLOAD_NONE);
            Json names = Json::array();
            std::vector<std::string> seen;
            for (const std::string& action_id : request.matched) {
                const Action& action = *by_id.at(action_id);
                std::vector<std::string> said{action.name};
                said.insert(said.end(), action.spoken_names.begin(), action.spoken_names.end());
                for (std::string& name : said) {
                    if (std::ranges::find(seen, name) == seen.end()) {
                        seen.push_back(std::move(name));
                    }
                }
            }
            for (std::size_t i = 0; i < seen.size() && i < kMaxPayloadNames; ++i) {
                names.push_back(seen[i]);
            }
            Json instructions = Json::object();
            instructions["question"] = std::string(PAYLOAD_QUESTION);
            instructions["action_names"] = std::move(names);
            instructions["addressing"] = std::string(PAYLOAD_ADDRESSING);
            Json payload_question = Json::object();
            payload_question["type"] = "choice";
            payload_question["instructions"] = std::move(instructions);
            payload_question["criteria"] = std::move(payload_criteria);
            questions["payload"] = std::move(payload_question);
        }
        Json whole = Json::object();
        whole["state"] = request.state;
        whole["questions"] = questions;
        // json.dumps escapes everything beyond ASCII, so its length is its byte count.
        if (py::dumps(whole).size() <= kRequestChars) {
            break;
        }
    }
    if (questions.contains("payload")) {
        request.spans = std::move(span_options);
    }
    request.questions = std::move(questions);
    return request;
}

double route_budget(const Json& settings) {
    return llm::route_from_settings(settings).profile ? TEXT_MODEL_ROUTE_TIMEOUT_S
                                                      : ROUTE_TIMEOUT_S;
}

RouteDecision decide(std::string_view text, const Json& settings, std::string_view route,
                     llm::JudgeProvider& provider, EventSink& events, const CancelToken& cancel,
                     std::string_view job_id) {
    return decide(text, settings, route, provider, events, cancel, job_id, nullptr);
}

RouteDecision decide(std::string_view text, const Json& settings, std::string_view route,
                     llm::JudgeProvider& provider, EventSink& events, const CancelToken& cancel,
                     std::string_view job_id, std::shared_ptr<const void> keep_alive) {
    try {
        return decide_now(text, settings.is_object() ? settings : Json::object(), route, provider,
                          events, cancel, job_id, std::move(keep_alive));
    } catch (const std::exception& error) {
        kLog.warning("Route by voice failed ({}); pasting the dictation",
                     detail::error_type(error));
    } catch (...) {
        kLog.warning("Route by voice failed (unknown); pasting the dictation");
    }
    return {};
}

RoutingStatus status(const Json& settings, llm::JudgeProvider& provider) {
    try {
        // settings or {}: a value that is truthy and not a mapping fails, as Python's .get would.
        if (detail::truthy(settings) && !settings.is_object()) {
            throw py::TypeError("settings are not a mapping");
        }
        return status_now(settings.is_object() ? settings : Json::object(), provider);
    } catch (const std::exception& error) {
        kLog.warning("Route by voice status unavailable: {}", detail::error_type(error));
    } catch (...) {
        kLog.warning("Route by voice status unavailable: unknown");
    }
    return {false, false, "Route by voice isn't available right now.", ""};
}

std::optional<RoutingStatus> status_without_key(const Json& settings) {
    try {
        if (!settings.is_object()) {
            return std::nullopt;
        }
        if (!keys::voice_routing_enabled.read(settings)) {
            return off_status();
        }
        if (!keys::typesafe_enabled.read(settings)) {
            return needs_fast_judgments_status();
        }
    } catch (const std::exception&) {
        // status() logs and words the failure.
    }
    return std::nullopt;
}

} // namespace fv::automation
