#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "automation/action_runtime.h"
#include "process/launcher.h"
#include "process/run_once.h"
#include "support/action_support.h"
#include "support/hook_support.h"

// What a Cancel reaches and what it says it did (tests/test_automation_fix2_cancel.py), runs whose
// work already finished (test_automation_r3_cancel_finished.py) and runs that can no longer be
// stopped (test_automation_r4_cancel_outcome.py), then the package's acceptance: a program killed
// and a link already opening.

using namespace fv;
using namespace fv::automation;
using namespace fv::automation::test;
using namespace std::chrono_literals;

namespace {

using Notices = std::vector<std::pair<std::string, bool>>;
using Outcome = std::tuple<bool, std::string, std::string>;

const Action kTranslate =
    an_action("w1", "Translate", ActionKind::WEBHOOK,
              Json{{"url", "http://127.0.0.1:9/hook"}, {"output", "scratchpad"}});
const Action kLink = an_action("l1", "Ask Claude", ActionKind::OPEN_LINK,
                               Json{{"template", "claude://claude.ai/new?q={text}"}});
const Action kQuick =
    an_action("q1", "Quick answer", ActionKind::AGENT,
              Json{{"agent", "claude_code"}, {"mode", "ask"}, {"output", "notice"}});
const Action kTicket = an_action("p1", "Make a ticket", ActionKind::PROGRAM,
                                 Json{{"program", "C:/nowhere/ticket.exe"},
                                      {"args", Json::array()},
                                      {"timeout_s", 5},
                                      {"output", "notice"}});

RunOnceResult cancellable_program(const std::vector<std::string>& argv, std::string_view input,
                                  double timeout_s, const CancelToken& cancel,
                                  const std::map<std::string, std::string>& extra_env) {
    if (can_start_programs()) {
        return fv::run_once(argv, input, timeout_s, cancel, {}, extra_env);
    }
    // Core-only builds exercise the same Cancel contract without a native launcher.
    if (cancel.wait_for(5s)) {
        return RunOnceResult{.error = std::string(fv::CANCELED), .canceled = true};
    }
    return RunOnceResult{.error = timed_out_message(timeout_s), .timed_out = true};
}

// (ok, message, output) of each action.ran.
std::vector<Outcome> outcomes(const Harness& h) {
    std::vector<Outcome> found;
    for (const Json& ran : h.ran()) {
        found.emplace_back(ran["ok"].get<bool>(), ran["message"].get<std::string>(),
                           ran["output"].get<std::string>());
    }
    return found;
}

// A webhook action whose POST is under way until released (`posting`).
struct Posting {
    explicit Posting(Harness& h, DeliveryRecord record = delivery(true, 200),
                     std::optional<std::string> body = R"({"text": "Hallo Welt"})") {
        h.post.set([held = post, record, body](const std::string&, const Json&, const std::string&,
                                               const SignedPost&) {
            held.enter_and_wait();
            return std::pair{record, body};
        });
        h.add(kTranslate);
    }
    ~Posting() { post.release(); }
    Blocking post;
};

// A Quick answer still thinking until released (`thinking`).
struct Thinking {
    explicit Thinking(Harness& h) {
        h.ask.set([blocked = ask](const std::string&, const std::string& text, const std::string&,
                                  const CancelToken& cancel) { return blocked(text, cancel); });
        h.add(kQuick);
        h.rt->deliver(action_job("q1"), "what's the capital of Peru", "", "", "");
        REQUIRE(wait_until([&] { return ask.started(1); }, 5s));
    }
    ~Thinking() { ask.release(); }
    BlockedAsk ask;
};

// ApplicationController.cancel: a Cancel cancels the recording or job in progress, and only
// that; with neither, the actions still running (the controller's routing is M5.22's).
void users_cancel(Harness& h, bool canceled_a_job) {
    if (!canceled_a_job) {
        h.rt->cancel();
    }
}

bool has_notice(const Harness& h, const std::pair<std::string, bool>& notice) {
    const auto notices = h.host.notices_now();
    return std::ranges::find(notices, notice) != notices.end();
}

} // namespace

// -- a run too far along to stop reports what really happened (test_automation_fix2_cancel.py) --

TEST_CASE("test_a_webhook_already_posting_finishes_and_reports_what_happened",
          "[test_automation_fix2_cancel]") {
    Harness h;
    Posting posting(h);
    h.rt->deliver(action_job("w1"), "hello world", "", "", "");
    REQUIRE(wait_until([&] { return posting.post.started(); }, 5s));

    CHECK(h.rt->cancel() == 1);
    CHECK(h.ran().empty()); // not "Canceled": the receiver may already be acting on it
    const Notices notices = h.host.notices;

    posting.post.release();
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));
    CHECK(outcomes(h) == std::vector<Outcome>{{true, "Sent to Translate", "none"}});
    // Its answer is dropped: nothing reaches the Scratchpad, the clipboard or a notice.
    CHECK(h.host.scratchpad.empty());
    CHECK(h.host.copied.empty());
    CHECK(h.host.notices == notices);
}

TEST_CASE("test_a_failure_canceled_too_late_is_reported_without_copying_the_words",
          "[test_automation_fix2_cancel]") {
    Harness h;
    Posting posting(h, delivery(false, 500), std::string());
    h.rt->deliver(action_job("w1"), "hello world", "", "", "");
    REQUIRE(wait_until([&] { return posting.post.started(); }, 5s));

    h.rt->cancel();
    posting.post.release();
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));

    CHECK(outcomes(h) ==
          std::vector<Outcome>{{false, "Translate: the address answered 500", "none"}});
    CHECK(h.host.copied.empty());
}

TEST_CASE("test_runs_still_waiting_never_start_and_say_canceled", "[test_automation_fix2_cancel]") {
    Harness h;
    Blocking opened;
    h.open_uri.set([opened](const std::string&) { opened.enter_and_wait(); });
    h.add(kLink);
    const int total = runtime::MAX_WORKERS + 2;
    for (int n = 0; n < total; ++n) {
        h.rt->deliver(action_job("l1"), "question " + std::to_string(n), "", "", "");
    }
    REQUIRE(wait_until([&] { return opened.started(runtime::MAX_WORKERS); }, 5s));

    CHECK(h.rt->cancel() == total);

    // The two that were waiting: canceled at once, and never opened.
    CHECK(outcomes(h) == std::vector<Outcome>(2, {false, "Canceled", "none"}));
    opened.release();
    REQUIRE(h.pump([&] { return h.ran().size() == static_cast<std::size_t>(total); }));
    CHECK(opened.calls() == runtime::MAX_WORKERS);
    const auto all = outcomes(h);
    CHECK(std::vector<Outcome>(all.begin() + 2, all.end()) ==
          std::vector<Outcome>(runtime::MAX_WORKERS, {true, "Sent to Ask Claude", "none"}));
    CHECK(h.rt->accepting());
}

TEST_CASE("test_a_canceled_try_it_still_learns_what_happened", "[test_automation_fix2_cancel]") {
    Harness h;
    Posting posting(h);
    std::vector<ActionResult> results;
    h.rt->test(kTranslate, "sample", [&](ActionResult r) { results.push_back(std::move(r)); });
    REQUIRE(wait_until([&] { return posting.post.started(); }, 5s));

    CHECK(h.rt->cancel(true) == 1);
    h.ui.run_all();
    CHECK(results.empty());

    posting.post.release();
    REQUIRE(h.pump([&] { return !results.empty(); }));
    CHECK(results == std::vector<ActionResult>{result(true, "Sent to Translate", "Hallo Welt")});
}

TEST_CASE("test_a_second_cancel_leaves_a_run_already_reached_alone",
          "[test_automation_fix2_cancel]") {
    Harness h;
    Posting posting(h);
    h.rt->deliver(action_job("w1"), "hello world", "", "", "");
    REQUIRE(wait_until([&] { return posting.post.started(); }, 5s));
    CHECK(h.rt->cancel() == 1);
    const Notices said = h.host.notices;

    // Still finishing, so the second press reaches it, but cancels and says nothing again.
    CHECK(h.rt->cancel() == 1);
    CHECK(h.host.notices == said);
    CHECK(h.ran().empty());
}

// -- a Cancel cancels what is in progress, and says what it canceled -----------------------------

TEST_CASE("test_canceling_a_new_dictation_leaves_the_quick_answer_thinking",
          "[test_automation_fix2_cancel]") {
    Harness h;
    Thinking thinking(h);

    users_cancel(h, true);

    const auto calls = thinking.ask.calls();
    REQUIRE(calls.size() == 1);
    CHECK_FALSE(calls[0].second.cancelled());
    thinking.ask.release();
    CHECK(h.pump([&] { return has_notice(h, {"the answer: what's the capital of Peru", true}); }));
    CHECK(h.ran().back()["ok"] == true);
}

TEST_CASE("test_cancel_with_nothing_recording_stops_the_quick_answer_and_says_so",
          "[test_automation_fix2_cancel]") {
    Harness h;
    Thinking thinking(h);

    users_cancel(h, false);

    const auto calls = thinking.ask.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].second.cancelled());
    // Said once the Quick answer reports that it stopped.
    REQUIRE(h.pump([&] { return !h.host.notices.empty(); }));
    CHECK(h.host.statuses.back() == "Canceled Quick answer");
    CHECK(h.host.notices.back() == std::pair<std::string, bool>{"Canceled Quick answer", false});
}

TEST_CASE("test_a_cancel_too_late_to_stop_a_run_says_so", "[test_automation_fix2_cancel]") {
    Harness h;
    Thinking thinking(h);
    Posting posting(h);
    h.rt->deliver(action_job("w1"), "hello world", "", "", "");
    REQUIRE(wait_until([&] { return posting.post.started(); }, 5s));

    users_cancel(h, false);

    REQUIRE(h.pump([&] { return !h.host.notices.empty(); }));
    CHECK(h.host.statuses.back() == "Canceled Quick answer \u2014 too late to stop Translate");
    CHECK(h.host.notices.back() == std::pair<std::string, bool>{h.host.statuses.back(), false});
}

// -- work that already finished (test_automation_r3_cancel_finished.py) -------------------------

namespace {

// `ticket`: a program that runs to the end and prints its ticket number.
std::shared_ptr<std::atomic<int>> ticket(Harness& h) {
    auto runs = std::make_shared<std::atomic<int>>(0);
    h.run_once.set([runs](const std::vector<std::string>&, std::string_view, double,
                          const CancelToken&, const std::map<std::string, std::string>&) {
        ++*runs;
        return program_result(0, "TICKET-42");
    });
    h.add(kTicket);
    return runs;
}

// _finished_and_queued: the run's work returned and its result waits for the UI executor.
void finished_and_queued(Harness& h, std::size_t results = 1) {
    REQUIRE(wait_until([&] { return h.ui.queued() >= results; }, 5s));
}

} // namespace

TEST_CASE("test_a_program_that_already_finished_reports_what_happened",
          "[test_automation_r3_cancel_finished]") {
    Harness h;
    const auto runs = ticket(h);
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    finished_and_queued(h);

    CHECK(h.rt->cancel() == 1);
    h.ui.run_all();

    CHECK(runs->load() == 1);
    CHECK(outcomes(h) == std::vector<Outcome>{{true, "Sent to Make a ticket", "none"}});
    // Its answer goes nowhere.
    CHECK(h.host.notices == Notices{{"Too late to stop Make a ticket", false}});
    CHECK(h.host.copied.empty());
    CHECK(h.host.scratchpad.empty());
}

TEST_CASE("test_a_quick_answer_that_already_came_back_reports_what_happened",
          "[test_automation_r3_cancel_finished]") {
    Harness h;
    h.ask.set([](const std::string&, const std::string&, const std::string&, const CancelToken&) {
        return asked(true, "Lima");
    });
    h.add(kQuick);
    h.rt->deliver(action_job("q1"), "capital of Peru", "", "", "");
    finished_and_queued(h);

    h.rt->cancel();
    h.ui.run_all();

    const auto ran = h.ran();
    REQUIRE(ran.size() == 1);
    CHECK(ran[0]["ok"] == true);
    CHECK(ran[0]["output"] == "none");
    CHECK(h.host.notices == Notices{{"Too late to stop Quick answer", false}});
}

TEST_CASE("test_a_failure_that_already_happened_is_reported_without_copying",
          "[test_automation_r3_cancel_finished]") {
    Harness h;
    h.run_once.set(answers(2, ""));
    h.add(kTicket);
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    finished_and_queued(h);

    h.rt->cancel();
    h.ui.run_all();

    CHECK(outcomes(h) ==
          std::vector<Outcome>{{false, "Make a ticket: the program stopped with code 2", "none"}});
    CHECK(h.host.copied.empty());
}

TEST_CASE("test_a_cancel_says_which_it_stopped_and_which_had_finished",
          "[test_automation_r3_cancel_finished]") {
    Harness h;
    ticket(h);
    BlockedAsk ask;
    h.ask.set([ask](const std::string&, const std::string& text, const std::string&,
                    const CancelToken& cancel) { return ask(text, cancel); });
    h.add(kQuick);
    h.rt->deliver(action_job("q1"), "capital of Peru", "", "", "");
    REQUIRE(wait_until([&] { return ask.started(1); }, 5s));
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    finished_and_queued(h);

    CHECK(h.rt->cancel() == 2);

    // One notice, once the Quick answer reports that it stopped.
    REQUIRE(h.pump([&] { return !h.host.notices.empty(); }));
    CHECK(h.host.notices ==
          Notices{{"Canceled Quick answer \u2014 too late to stop Make a ticket", false}});
    const auto calls = ask.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].second.cancelled());
    ask.release();
}

namespace {

// The UI executor with each worker held just after it hands over its result (_HeldAfterSending):
// the run's work is over and its result on the way, but its worker is not done yet.
class HeldAfterSending final : public Executor {
public:
    explicit HeldAfterSending(Executor& inner) : inner_(inner) {}

    void post(Task task) override {
        inner_.post(std::move(task));
        sent.release();
        hold.wait({}, 5s);
    }
    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        return inner_.post_after(delay, std::move(task));
    }
    bool is_current() const override { return inner_.is_current(); }

    Hold sent;
    Hold hold;

private:
    Executor& inner_;
};

} // namespace

TEST_CASE("test_a_result_already_sent_is_past_stopping_before_its_worker_is_done",
          "[test_automation_r3_cancel_finished]") {
    std::unique_ptr<HeldAfterSending> held;
    Harness h(HarnessOptions{.ui = [&](ManualExecutor& inner) -> Executor& {
        held = std::make_unique<HeldAfterSending>(inner);
        return *held;
    }});
    ticket(h);
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    REQUIRE(held->sent.wait({}, 5s));

    // The worker is still inside its task: the result is on its way, not yet delivered.
    h.rt->cancel();
    held->hold.release();
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));

    const auto all = outcomes(h);
    REQUIRE(all.size() == 1);
    CHECK(std::get<0>(all[0]) == true);
    CHECK(std::get<1>(all[0]) == "Sent to Make a ticket");
    CHECK(h.host.notices == Notices{{"Too late to stop Make a ticket", false}});
}

// -- runs a Cancel can no longer stop (test_automation_r4_cancel_outcome.py) --------------------

namespace {

// `drained`: run_once for a program that ran to its end; its output is still being read when the
// cancel comes, and the cancel changes nothing.
struct Drained {
    explicit Drained(Harness& h) {
        h.run_once.set([this](const std::vector<std::string>&, std::string_view, double,
                              const CancelToken& cancel,
                              const std::map<std::string, std::string>&) {
            exited.release();
            cancel.wait_for(5s);
            return program_result(0, "TICKET-42");
        });
        h.add(kTicket);
    }
    Hold exited;
};

} // namespace

TEST_CASE("test_a_program_that_exited_while_its_output_drains_is_too_late_to_stop",
          "[test_automation_r4_cancel_outcome]") {
    Harness h;
    Drained drained(h);
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    REQUIRE(drained.exited.wait({}, 5s));

    CHECK(h.rt->cancel() == 1);
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));

    CHECK(outcomes(h) == std::vector<Outcome>{{true, "Sent to Make a ticket", "none"}});
    CHECK(h.host.notices == Notices{{"Too late to stop Make a ticket", false}});
    // Its answer goes nowhere.
    CHECK(h.host.copied.empty());
    CHECK(h.host.scratchpad.empty());
    CHECK(h.rt->accepting());
    CHECK(h.rt->cancel() == 0);
}

TEST_CASE("test_a_try_it_program_that_already_exited_reports_its_answer",
          "[test_automation_r4_cancel_outcome]") {
    Harness h;
    Drained drained(h);
    std::vector<ActionResult> results;
    h.rt->test(kTicket, "sample", [&](ActionResult r) { results.push_back(std::move(r)); });
    REQUIRE(drained.exited.wait({}, 5s));

    CHECK(h.rt->cancel(true) == 1);
    REQUIRE(h.pump([&] { return !results.empty(); }));

    CHECK(results == std::vector<ActionResult>{result(true, "Sent to Make a ticket", "TICKET-42")});
}

TEST_CASE("test_a_program_the_cancel_kills_is_canceled",
          "[test_automation_r4_cancel_outcome][platform]") {
    Harness h;
    // A real program that would sleep for 30 s, stopped by run_once with its whole tree.
    h.run_once.set(cancellable_program);
    h.add(an_action("p1", "Make a ticket", ActionKind::PROGRAM,
                    Json{{"program", hook_child()},
                         {"args", Json::array({"sleep", "30"})},
                         {"timeout_s", 60},
                         {"output", "notice"}}));
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    REQUIRE(wait_until([&] { return !h.program_calls().empty(); }, 5s));
    sleep_ms(300); // the program is running

    const auto started = std::chrono::steady_clock::now();
    CHECK(h.rt->cancel() == 1);
    CHECK(h.program_calls().at(0).cancel.cancelled());
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));

    CHECK(seconds_since(started) < 10.0);
    CHECK(outcomes(h) == std::vector<Outcome>{{false, "Canceled", "none"}});
    CHECK(h.host.notices == Notices{{"Canceled Make a ticket", false}});
    CHECK(h.host.copied.empty());
}

TEST_CASE("test_a_quick_answer_that_came_back_as_it_was_canceled_is_too_late",
          "[test_automation_r4_cancel_outcome]") {
    Harness h;
    auto answered = std::make_shared<Hold>();
    h.ask.set([answered](const std::string&, const std::string&, const std::string&,
                         const CancelToken& cancel) {
        answered->release();
        cancel.wait_for(5s);
        return asked(true, "Lima", "Claude Code answered");
    });
    h.add(kQuick);
    h.rt->deliver(action_job("q1"), "capital of Peru", "", "", "");
    REQUIRE(answered->wait({}, 5s));

    h.rt->cancel();
    REQUIRE(h.pump([&] { return !h.ran().empty(); }));

    CHECK(outcomes(h) == std::vector<Outcome>{{true, "Claude Code answered", "none"}});
    CHECK(h.host.notices == Notices{{"Too late to stop Quick answer", false}});
}

TEST_CASE("test_one_cancel_says_once_what_it_stopped_and_what_was_too_late",
          "[test_automation_r4_cancel_outcome]") {
    Harness h;
    Drained drained(h);
    h.ask.set(
        [](const std::string&, const std::string&, const std::string&, const CancelToken& cancel) {
            cancel.wait_for(5s);
            return asked(false, "", "Stopped asking Claude Code.", true);
        });
    h.add(kQuick);
    h.rt->deliver(action_job("p1"), "file a ticket", "", "", "");
    REQUIRE(drained.exited.wait({}, 5s));
    h.rt->deliver(action_job("q1"), "capital of Peru", "", "", "");

    CHECK(h.rt->cancel() == 2);
    REQUIRE(h.pump([&] { return h.ran().size() == 2; }));

    CHECK(h.host.notices ==
          Notices{{"Canceled Quick answer \u2014 too late to stop Make a ticket", false}});
    std::vector<std::tuple<std::string, bool, std::string>> seen;
    for (const Json& ran : h.ran()) {
        seen.emplace_back(ran["action_id"].get<std::string>(), ran["ok"].get<bool>(),
                          ran["message"].get<std::string>());
    }
    std::ranges::sort(seen);
    CHECK(seen == std::vector<std::tuple<std::string, bool, std::string>>{
                      {"p1", true, "Sent to Make a ticket"}, {"q1", false, "Canceled"}});
}

// -- acceptance ----------------------------------------------------------------------------------

TEST_CASE("a cancel kills a running program and is too late for a link already opening",
          "[action_cancel][platform]") {
    Harness h;
    h.run_once.set(cancellable_program);
    Blocking opening;
    h.open_uri.set([opening](const std::string&) { opening.enter_and_wait(); });
    h.add(an_action("log", "Log it", ActionKind::PROGRAM,
                    Json{{"program", hook_child()},
                         {"args", Json::array({"sleep", "30"})},
                         {"timeout_s", 60}}));
    h.add(kLink);
    h.rt->deliver(action_job("log"), "my words", "", "", "");
    h.rt->deliver(action_job("l1"), "my words", "", "", "");
    REQUIRE(wait_until([&] { return !h.program_calls().empty() && opening.started(); }, 5s));
    sleep_ms(300); // the program is running

    const auto started = std::chrono::steady_clock::now();
    CHECK(h.rt->cancel() == 2);
    // The link opens all the same once Explorer answers.
    opening.release();
    REQUIRE(h.pump([&] { return h.ran().size() == 2; }));

    // The program was stopped long before its 30 s, with what it started.
    CHECK(seconds_since(started) < 10.0);
    CHECK(h.host.statuses.back() == "Canceled Log it \u2014 too late to stop Ask Claude");
    CHECK(h.host.notices == Notices{{"Canceled Log it \u2014 too late to stop Ask Claude", false}});
    // Neither run's words were copied: one was stopped, the other was too late to stop.
    CHECK(h.host.copied.empty());
    std::vector<std::tuple<std::string, bool, std::string>> seen;
    for (const Json& ran : h.ran()) {
        seen.emplace_back(ran["action_id"].get<std::string>(), ran["ok"].get<bool>(),
                          ran["message"].get<std::string>());
    }
    std::ranges::sort(seen);
    CHECK(seen == std::vector<std::tuple<std::string, bool, std::string>>{
                      {"l1", true, "Sent to Ask Claude"}, {"log", false, "Canceled"}});
}
