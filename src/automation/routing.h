#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/action_model.h"
#include "base/cancel.h"
#include "base/events.h"
#include "base/json.h"
#include "llm/judge.h"

// Route by voice: which saved action the words name, decided by a Fast judgments judge
// (services/actions/routing.py). Two gates. Code decides whether an action could have been
// addressed: a plain dictation is judged only when it starts with an action's name or one of
// its spoken names, after filler words and a few lead-ins, a misheard name included; words
// recorded with Route by voice skip that, every enabled action being a candidate. Then the judge
// answers two questions in one request: which action the words are for (or none), and which
// code-proposed span of the words is meant for it. Only a confident choice routes; anything
// else, a failure or a timeout included, leaves the words to be pasted. Only words that passed
// the gate, with each candidate's name, spoken names, description and kind, are sent (never a
// link, address, program or folder), and nothing here logs the words.
// Every function is thread-safe.
namespace fv::automation {

// Seconds the dictation waits for Jev.
inline constexpr double ROUTE_TIMEOUT_S = 1.5;
// Seconds when the user's text model judges: it writes the two answers token by token instead
// of returning them in Jev's 0.2 s; a slower answer pastes the words as a Jev timeout does.
inline constexpr double TEXT_MODEL_ROUTE_TIMEOUT_S = 4.0;
// A route needs the judge's confidence in its chosen action at least this high (measured on
// benchmarks/voice_routing: no false route at 0.5 or above, with a margin for real speech).
inline constexpr double ROUTE_CONFIDENCE = 0.5;

// DictationJob::route values.
inline constexpr std::string_view ROUTE_HOTKEY = "hotkey";
inline constexpr std::string_view ROUTE_AUTO = "auto";
// The "not for an action" option of the route question, and the "none of these spans" option of
// the payload question.
inline constexpr std::string_view ROUTE_NONE = "none";
// The `hotkeys` entry of the Route by voice shortcut.
inline constexpr std::string_view ROUTE_HOTKEY_NAME = "route_record";

inline constexpr std::string_view PAYLOAD_QUESTION =
    "Which option is the part of `dictation` meant for the action: the request itself, without "
    "the words at its start or end that only name or address the action?";
inline constexpr std::string_view PAYLOAD_ADDRESSING =
    "an action name, with words such as \"hey\", \"ask\", \"tell\", \"send this to\" or \"add it "
    "to my\"";
inline constexpr std::string_view PAYLOAD_NONE = "None of these; use the whole dictation as said.";

// RoutingStatus::fix: Fast judgments are off, or the chosen route has no key.
inline constexpr std::string_view FIX_FAST_JUDGMENTS = "fast_judgments";
// No actions, or every action is turned off.
inline constexpr std::string_view FIX_ACTIONS = "actions";
// Nothing starts it: no Route by voice shortcut, and normal dictations aren't listened to.
inline constexpr std::string_view FIX_TRIGGER = "trigger";

// RouteDecision::failure: no answer within route_budget.
inline constexpr std::string_view FAILURE_TIMEOUT = "timeout";
// No usable answer: Fast judgments off or without a key at that moment, a network or HTTP
// error, a malformed answer.
inline constexpr std::string_view FAILURE_UNREACHABLE = "unreachable";

// Where an utterance names an action, in word indices. Leading matches start the utterance
// (after fillers and lead-ins) and the words for the action follow name_end; trailing matches
// (Route by voice only) end it and the words for the action come before `first`.
// Affinity: value type.
struct GateMatch {
    std::string action_id;
    std::size_t first = 0;
    std::size_t name_start = 0;
    std::size_t name_end = 0;
    bool leading = false;
    friend bool operator==(const GateMatch&, const GateMatch&) = default;
};

// Actions `text` names, most completely named first. A plain dictation (`route` "auto") names
// an action only at its start; with Route by voice ("hotkey") a name said last counts too.
std::vector<GateMatch> gate(std::string_view text, const std::vector<Action>& actions,
                            std::string_view route);

// What the judge answered, before the confidence threshold.
// Affinity: value type.
struct Judgment {
    // The action it chose; nullopt for none.
    std::optional<std::string> action_id;
    double confidence = 0.0;
    // Probability of the chosen option.
    double probability = 0.0;
    std::string payload;
    friend bool operator==(const Judgment&, const Judgment&) = default;
};

// One routing request: the candidates and the questions asked about them.
// Affinity: value type.
struct RoutePlan {
    // The dictation, stripped.
    std::string text;
    std::string route;
    // Action ids offered to the judge, named ones first.
    std::vector<std::string> candidates;
    // Action ids the gate found named.
    std::vector<std::string> matched;
    Json state = Json::object();
    Json questions = Json::object();
    // Payload option -> the span of `text` it stands for, in option order; empty when no
    // payload question is asked.
    std::vector<std::pair<std::string, std::string>> spans;

    // The judgment in `answers`, or nullopt when they don't answer the route question. Throws a
    // py::PyError (ValueError, TypeError, or AttributeError for answers that are neither falsy
    // nor an object) where Python's float(), a dict lookup or .get would (decide catches it).
    std::optional<Judgment> read(const Json& answers) const;
};

// The request that decides where `text` goes, or nullopt when there is nothing to ask.
// `actions` are the enabled actions. With "auto" only the actions the words start with are
// candidates, and none at all means no request; with "hotkey" every action is one, named ones
// first. The request is described more briefly until its JSON is at most 20,000 characters.
std::optional<RoutePlan> plan(std::string_view text, const std::vector<Action>& actions,
                              std::string_view route);

// Where the words go. action_id set: the action the words are for, with the words meant for it
// and the judge's confidence. action_id and failure empty: paste the words as a dictation.
// failure set: the judge couldn't answer (FAILURE_TIMEOUT, FAILURE_UNREACHABLE), so the paste
// can say so instead of "No action matched".
// Affinity: value type.
struct RouteDecision {
    std::optional<std::string> action_id;
    std::string payload;
    std::optional<double> confidence;
    std::string failure;
    friend bool operator==(const RouteDecision&, const RouteDecision&) = default;
};

// How long a dictation waits for the judge Settings > Fast judgments names (seconds).
double route_budget(const Json& settings);

// Where `text` of the job `job_id`, recorded with `route` ("hotkey", "auto" or "" for none),
// goes; worker thread, never throws. The judge `provider` names for `settings` is asked on a
// helper thread, so neither a slow answer nor reading its key holds the caller past
// route_budget; `cancel` ends the wait at once (polled every 20 ms), and nothing is sent once it
// is set. Publishes route.decided on `events` whenever the words were to be judged and the
// dictation wasn't cancelled: with the action when they were routed, else fallback true (and
// the judge's confidence in its choice, when it answered). A helper still asking when the wait
// ends is cancelled and finishes on its own, asking nothing once it sees that; `provider` must
// outlive a helper still inside provider.judge, as the app's process-wide provider does.
RouteDecision decide(std::string_view text, const Json& settings, std::string_view route,
                     llm::JudgeProvider& provider, EventSink& events, const CancelToken& cancel,
                     std::string_view job_id);
// The same, with `keep_alive` held by the helper until it finishes: whatever `provider` needs
// (the provider itself, its environment) when it is not process-wide.
RouteDecision decide(std::string_view text, const Json& settings, std::string_view route,
                     llm::JudgeProvider& provider, EventSink& events, const CancelToken& cancel,
                     std::string_view job_id, std::shared_ptr<const void> keep_alive);

// Whether Route by voice can run, what it still needs, and what it sends.
// Affinity: value type.
struct RoutingStatus {
    bool enabled = false;
    bool ready = false;
    std::string message;
    // FIX_FAST_JUDGMENTS, FIX_ACTIONS or FIX_TRIGGER; "" when ready or off.
    std::string fix;
    friend bool operator==(const RoutingStatus&, const RoutingStatus&) = default;
};

// The status line of the Actions page; never throws (a failure reads "Route by voice isn't
// available right now."). Reads the credential store through `provider`: never on the GUI
// thread.
RoutingStatus status(const Json& settings, llm::JudgeProvider& provider);

// The part of status() that needs no credential store, for the GUI thread: Route by voice off,
// or Fast judgments off; nullopt when the answer depends on the key (status() then). Never
// throws (nullopt when the settings cannot be read).
std::optional<RoutingStatus> status_without_key(const Json& settings);

} // namespace fv::automation
