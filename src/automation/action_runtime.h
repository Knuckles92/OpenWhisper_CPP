#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agents/agent_actions.h"
#include "automation/action_model.h"
#include "automation/action_result.h"
#include "automation/event_bus.h"
#include "automation/programs.h"
#include "automation/webhooks.h"
#include "base/cancel.h"
#include "base/events.h"
#include "base/executor.h"
#include "base/json.h"
#include "credentials/store.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/snapshot.h"
#include "http/client.h"
#include "settings/store.h"

// Runs an action once its words are ready (services/actions/runtime.py).
//
// ActionRuntime::deliver runs on the UI thread and returns at once. Copying, "Send to my scripts"
// and the Claude Code channel finish there. Opening a link, a webhook, a program and an agent
// block, so they run on the runtime's own 4-wide strand rather than the transcription jobs
// strand: the dictation's job slot frees as soon as its history is saved, however long an agent
// thinks. Their results come back to the UI executor through one relay; a worker only ever hands
// it a token and a result, and never holds the runtime. At most MAX_PENDING of them run or wait
// at once; more are refused. cancel drops the runs still waiting and tells agents and programs
// under way to stop (killed with what they started); each one's result then says whether it was
// stopped. A run that wasn't (it can't be stopped, or its work was already done) finishes,
// reports what happened, and its answer goes nowhere.
//
// A run the user started that fails leaves the words on the clipboard, so nothing said is lost.
// One a program started over the live API or MCP never touches the clipboard or types into an
// app: an answer it would paste or copy goes to the Scratchpad, and a failure is only reported
// (status, notice and action.ran). Dictated text, links, program paths and arguments, selections,
// secrets and replies are never logged.
namespace fv::automation {

namespace runtime {
// EventBus::subscribers label of a Claude Code session's channel bridge.
inline constexpr std::string_view CHANNEL_LISTENER_LABEL = "claude-channel";
// Labels of the listeners "Send to my scripts" reaches: live event streams, webhooks, programs
// and MCP Events.
inline constexpr std::string_view SSE_LABEL = "sse";
inline constexpr std::string_view WEBHOOKS_LABEL = "webhooks";
inline constexpr std::string_view PROGRAMS_LABEL = "programs";
inline constexpr std::string_view MCP_EVENTS_LABEL = "mcp-events";
// Blocking runs at once; more wait their turn.
inline constexpr int MAX_WORKERS = 4;
// Blocking runs running or waiting; beyond this a run is refused.
inline constexpr std::size_t MAX_PENDING = 8;
// How long a webhook action waits for its receiver, in seconds.
inline constexpr double WEBHOOK_TIMEOUT_S = 5.0;
// How much of an answer a notice shows, in code points.
inline constexpr std::size_t NOTICE_MAX_CHARS = 140;
// The fixed frame around the selection an agent action passes as context; the opening tag names
// identity().product (context_open()).
inline constexpr std::string_view CONTEXT_CLOSE = "</context>";

inline constexpr std::string_view MISSING_MESSAGE = "That action no longer exists";
inline constexpr std::string_view DISABLED_MESSAGE = "That action is turned off";
inline constexpr std::string_view NO_LISTENER_MESSAGE = "No Claude Code session is listening";
inline constexpr std::string_view NO_SCRIPTS_MESSAGE = "Nothing is listening for your scripts";
inline constexpr std::string_view BUSY_MESSAGE = "Too many actions are still running";
inline constexpr std::string_view CANCELED_MESSAGE = "Canceled";
inline constexpr std::string_view PROGRAM_WORDS_MESSAGE =
    "Words from a program need “Send words to my coding agents”";
inline constexpr std::string_view COPIED_INSTEAD = " — copied instead";
inline constexpr std::string_view FULL_ANSWER_COPIED = " — full answer copied";
} // namespace runtime

// '<context source="OpenWhisper_CPP">': CONTEXT_OPEN with identity().product.
std::string context_open();
// "OpenWhisper_CPP is closing": CLOSING_MESSAGE with identity().product.
std::string closing_message();

// The `"text"` string of a JSON reply, or nullopt (no body, not JSON, not an object, or no
// string at "text").
std::optional<std::string> reply_text(const std::optional<std::string>& body);

// `text` followed by the selection and app the job started on, when known. The selection is UI
// Automation's read from the job's focus capture, never a copy; nothing is added when it is
// unknown, refused or empty. The frame's own tags inside the selection or the app name are made
// inert ("<" as "&lt;"), so neither can end the frame early and pose as the request after it.
std::string prompt_with_context(std::string_view text,
                                const std::optional<focus::FocusSnapshot>& snapshot);
// The same with the job's capture, waiting at most focus::CONTEXT_CAPTURE_DEADLINE for one still
// running: a worker thread, never the UI thread.
std::string prompt_with_context(std::string_view text, const dictation::DictationJob& job);

// Whether `action` may get words a program supplied or changed: an agent or Claude Code channel
// action hands its words to an agent that acts on them, so it takes a program's words only with
// "Send words to my coding agents" (permissions::AGENT_PROMPTS). Delivery applies it to every
// job; the live API and MCP before starting one; leased text filters leave alone an action job
// it would refuse.
bool program_words_allowed(const Action& action, const Json& settings);
bool program_words_allowed(std::string_view kind, const Json& settings);

// _short: whitespace runs as one space, cut to `limit` code points with "…" (trailing whitespace
// before it removed).
std::string short_text(std::string_view text, std::size_t limit = runtime::NOTICE_MAX_CHARS);

// What a webhook action POSTs: {"v": 1, "action": {id, name}, job_id, text, raw_text (null unless
// it differs from text), language, app}, with the app and language the job's capture knows now
// (history_fields, live).
Json webhook_payload(const dictation::DictationJob& job, const Action& action,
                     std::string_view text, std::string_view raw_text, const Json& settings);

// Open subscribers of the bus actions publish on, by label (EventBus::subscribers).
using ListenerCount = std::function<std::size_t(std::string_view label)>;

// Whether anything outside the app would act on action.ran now: a live event stream, an enabled
// webhook or program hook that takes action.ran, or an MCP Events subscription
// (`mcp_subscriptions`, consulted only while MCP Events listens on the bus). Never blocks.
bool scripts_listening(const Json& settings, const ListenerCount& subscribers,
                       const std::function<std::size_t()>& mcp_subscriptions);
bool scripts_listening(const Json& settings, const EventBus& bus,
                       std::function<std::size_t()> mcp_subscriptions);

// What the runtime asks of the app's UI: implemented by the controller. Every call comes on the
// UI executor.
// Affinity: UI thread.
class ActionHost {
public:
    virtual ~ActionHost() = default;
    virtual void set_status(std::string text) = 0;
    virtual void show_notice(std::string text, bool done) = 0;
    virtual bool show_in_scratchpad(std::string text) = 0;
    // `html` is the rich-text twin of `text`, or "" for plain text only.
    virtual bool copy_to_clipboard(std::string text, std::string html = {}) = 0;
    // TranscriptionRuntime::paste_text_now: pastes `text` at the caret for the job it answers.
    virtual bool paste_text_now(std::string text, const dictation::DictationJob* job) = 0;
    // dictation::paste_target with the app's capture service.
    virtual dictation::PasteTarget paste_target(const dictation::DictationJob& job) = 0;
    // A routed dictation took a clipboard snapshot for a paste that will not happen.
    virtual void discard_clipboard_prefetch() = 0;
};

// The blocking work and the listener counts the runtime reaches; tests replace them. Each empty
// member means the real one.
struct ActionServices {
    // launch.open_uri; throws LaunchError (or NoAnswer) with the sentence to show.
    std::function<void(std::string_view uri)> open_uri;
    // agents.handoff: what to show; throws agents::HandoffError with the sentence to show.
    std::function<std::string(const agents::HandoffRequest& request)> handoff;
    // agents.ask with its default time limit.
    std::function<agents::AskResult(std::string_view agent_id, std::string_view text,
                                    std::string_view model, const CancelToken& cancel)>
        ask;
    // webhooks.post_signed_reply; the real one posts through `http`.
    std::function<std::pair<DeliveryRecord, std::optional<std::string>>(
        std::string_view url, const Json& payload, std::string_view secret,
        const SignedPost& options)>
        post_signed_reply;
    // The client of the real post_signed_reply; null: a curl client of the runtime's own.
    std::shared_ptr<http::Client> http;
    // programs.run_once; the real one starts the program through `launcher` (null: the
    // process-wide launcher).
    RunOnceFn run_once;
    ProcessLauncher* launcher = nullptr;
    // Open subscribers by label of the bus `events` publishes on; empty: the process-wide bus().
    ListenerCount subscribers;
    // MCP Events subscriptions (events_ext.subscription_count); empty: none.
    std::function<std::size_t()> mcp_subscriptions;
    // The pool whose "action-run" strand (MAX_WORKERS wide) runs the blocking work; null: a pool
    // of the runtime's own with MAX_WORKERS threads.
    ThreadPool* pool = nullptr;
};

// Delivers an action job's words; one per controller. Results of blocking work reach the UI
// actor through `ui`; tokens of runs dropped by close are ignored when their results arrive.
// The host, stores, sink and executor must outlive the runtime; the destructor closes it and
// waits for the work still running (cancelled when it can be).
// Affinity: UI actor (accepting: any thread).
class ActionRuntime final : public ActionTester {
public:
    ActionRuntime(ActionHost& host, SettingsStore& settings, CredentialStore& credentials,
                  EventSink& events, Executor& ui, ActionServices services = {});
    ~ActionRuntime() override;
    ActionRuntime(const ActionRuntime&) = delete;
    ActionRuntime& operator=(const ActionRuntime&) = delete;

    // Runs `job`'s action with `text`; returns at once. Shows the outcome and publishes
    // action.ran (and channel.message for a channel action). The action and the settings are
    // read afresh for every run. Words a program supplied or changed reach an agent or channel
    // action only as program_words_allowed says. `raw_text` is the words before AI cleanup ("" or
    // the same text when it changed nothing); `notice` why the words aren't what the action asked
    // for, shown with the outcome.
    void deliver(std::shared_ptr<const dictation::DictationJob> job, std::string text,
                 std::string raw_text, std::string html, std::string notice);
    // Runs the immutable action and settings that the caller disclosed and approved. This avoids
    // silently switching destinations if settings change while an approval dialog is open.
    void deliver_approved(std::shared_ptr<const dictation::DictationJob> job, Action action,
                          std::shared_ptr<const Json> settings, std::string text,
                          std::string raw_text, std::string html, std::string notice);
    // Tries `action` with `sample` from Settings; `done` on the UI executor. Runs it for real but
    // saves no history and routes no answer: `done` gets the answer as `output`. A channel action
    // only reports how many sessions are listening.
    void test(const Action& action, std::string sample,
              std::function<void(ActionResult)> done) override;
    // Whether another blocking run would be taken now: false once MAX_PENDING run or wait, and
    // after close.
    bool accepting() const;
    // Cancels the runs still waiting and stops those that can be (programs and agents' quick
    // answers, killed with what they started). A run that wasn't stopped, or can't be (a link
    // opening, a webhook posting, a session handed off), finishes and reports what really
    // happened; only its answer is dropped. Unless `tests_only`, the status line and a notice say
    // what was canceled and what was too late, once every run told to stop has answered.
    // `tests_only`: only Settings' Try it runs (its page went away). Returns how many runs the
    // cancel reached, counting those an earlier cancel is still stopping.
    int cancel(bool tests_only = false);
    int cancel_tests() override;
    // Stops work in progress at quit: cancels agents and programs, drops late results, starts
    // nothing more. Returns at once.
    void close();

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace fv::automation
