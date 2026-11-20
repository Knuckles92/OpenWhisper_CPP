#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "automation/action_model.h"
#include "automation/detail/route_words.h"
#include "automation/routing.h"
#include "golden/golden.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "routing_support.h"

// Route by voice's code gate and payload spans: test_automation_routing_gate.py and
// test_automation_fix_routing_gate.py, then the replay of tests/golden/services.actions.routing
// (the word model, names, gate, spans and the full request over every sentence of the routing
// tests and of OpenWhisper's benchmarks/voice_routing corpus).

using namespace fv::automation;
using namespace fv::automation::test;
using fv::Json;
namespace golden = fv::golden;
namespace py = fv::py;
namespace rd = fv::automation::detail;

namespace {

using Ids = std::vector<std::string>;

const std::vector<Action>& all_actions() {
    static const std::vector<Action> actions = {
        make_action("c1", "Ask Claude", "open_link",
                    Json{{"template", "claude://claude.ai/new?q={text}"}},
                    {"Claude", "hey Claude"}),
        make_action("cc", "Tell my Claude Code session", "channel", Json::object(),
                    {"Claude Code"}),
        make_action("t1", "Todo", "webhook", Json{{"url", "https://example.test/todo"}},
                    {"to-do list"}),
        make_action("s1", "Send to my scripts", "scripts"),
        make_action("g1", "ChatGPT app", "open_link",
                    Json{{"template", "https://chatgpt.com/?q={text}"}}),
        make_action("agent", "Agent hotkey", "agent", Json{{"agent", "claude_code"}}),
    };
    return actions;
}

// The gallery presets as they come (names only), plus two common actions.
const std::vector<Action>& preset_actions() {
    static const std::vector<Action> actions = [] {
        std::vector<Action> out;
        for (const Preset& preset : PRESETS()) {
            out.push_back(make_action(preset.id, preset.name, preset.kind, preset.params));
        }
        out.push_back(make_action("todo", "Todo", "webhook", Json{{"url", "http://127.0.0.1/x"}}));
        out.push_back(
            make_action("notes", "Notes", "webhook", Json{{"url", "http://127.0.0.1/x"}}));
        return out;
    }();
    return actions;
}

// The same actions with their names typed in lowercase ("ask claude").
const std::vector<Action>& lowercase_actions() {
    static const std::vector<Action> actions = [] {
        std::vector<Action> out = preset_actions();
        for (Action& action : out) {
            action.name = py::lower(action.name);
        }
        return out;
    }();
    return actions;
}

std::vector<std::string> plan_spans(std::string_view text,
                                    const std::vector<Action>& actions = all_actions(),
                                    std::string_view route = ROUTE_AUTO) {
    std::vector<std::string> spans;
    if (const auto request = plan(text, actions, route)) {
        for (const auto& [option, span] : request->spans) {
            spans.push_back(span);
        }
    }
    return spans;
}

std::vector<std::string> question_names(const Json& questions) {
    std::vector<std::string> names;
    for (const auto& [name, question] : questions.items()) {
        names.push_back(name);
    }
    return names;
}

constexpr std::string_view kBothRoutes[] = {ROUTE_AUTO, ROUTE_HOTKEY};

} // namespace

// -- test_automation_routing_gate.py -------------------------------------------------------

TEST_CASE("test_names_at_the_start_after_fillers_and_lead_ins_pass",
          "[test_automation_routing_gate]") {
    const std::vector<std::pair<std::string, Ids>> cases = {
        {"Claude, what's the capital of Peru?", {"c1"}},
        {"Ask Claude what the capital of Peru is", {"c1"}},
        {"Hey Claude, um, what's a mutex?", {"c1"}},
        {"Um, okay so, hey Claude, what's a mutex?", {"c1"}},
        {"All right Claude, draft a reply", {"c1"}},
        {"Can you ask Claude how to center a div", {"c1"}},
        {"Send this to my scripts: rotate the logs", {"s1"}},
        {"Send to my scripts rotate the logs", {"s1"}},
        {"Todo: buy milk", {"t1"}},
        {"To-do list, call the plumber", {"t1"}},
        {"Agent, add a dark mode toggle", {"agent"}},
        {"ChatGPT, translate this", {"g1"}},
    };
    for (const auto& [text, expected] : cases) {
        INFO(text);
        CHECK(named(text, all_actions()) == expected);
    }
}

TEST_CASE("test_a_name_may_drop_its_verb_and_generic_words", "[test_automation_routing_gate]") {
    CHECK(named("Claude Code, run the tests", all_actions()) == Ids{"cc", "c1"});
    const std::vector<Action> claude_only = {
        make_action("x", "Claude", "open_link", Json{{"template", "claude://x?q={text}"}})};
    CHECK(named("Ask Claude to summarize this", claude_only) == Ids{"x"});
}

TEST_CASE("test_speech_recognition_spacing_and_mishearings_pass",
          "[test_automation_routing_gate]") {
    const std::vector<std::pair<std::string, Ids>> cases = {
        {"chat GPT, give me a packing list", {"g1"}}, // speech recognition splits a word
        {"To do, email the landlord", {"t1"}},
        {"Cloud, what time zone is Tokyo in?", {"c1"}}, // misheard names
        {"Claud, what's the boiling point?", {"c1"}},
        {"Hey clod, three lunch ideas", {"c1"}},
        {"Clawed code, run the linter", {"cc", "c1"}},
    };
    for (const auto& [text, expected] : cases) {
        INFO(text);
        CHECK(named(text, all_actions()) == expected);
    }
}

TEST_CASE("test_ordinary_sentences_stop_at_the_gate", "[test_automation_routing_gate]") {
    for (std::string_view text : {
             "I told Claude yesterday that the plan changed.", // not at the start
             "Today I went to the store.", // sounds like "todo", too short to tell
             "Claude's answer was wrong.", // a possessive, not an address
             "Agents are the hot topic this year.",
             "Hey team, Claude Code shipped a release.",    // "team" is not a lead-in
             "Please can you ask and tell the Claude team", // more than four lead-ins
             "",
             "Um, uh.",
         }) {
        INFO(text);
        CHECK(named(text, all_actions()).empty());
    }
}

TEST_CASE("test_a_name_said_last_counts_only_with_route_by_voice",
          "[test_automation_routing_gate]") {
    const std::string_view text = "Buy oat milk, add it to my to-do list.";
    CHECK(named(text, all_actions()).empty());
    const auto matches = gate(text, all_actions(), ROUTE_HOTKEY);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].action_id == "t1");
    CHECK_FALSE(matches[0].leading);
}

TEST_CASE("test_longest_name_goes_first", "[test_automation_routing_gate]") {
    CHECK(named("Ask Claude Code why the tests fail", all_actions(), ROUTE_AUTO) ==
          Ids{"cc", "c1"});
}

TEST_CASE("test_spans_drop_the_name_link_words_and_politeness_and_end_with_everything",
          "[test_automation_routing_gate]") {
    const std::string text = "Hey Claude, um, can you explain recursion simply, please.";
    CHECK(plan_spans(text) == Ids{
                                  "um, can you explain recursion simply, please.",
                                  "explain recursion simply, please.",
                                  "um, can you explain recursion simply.",
                                  "explain recursion simply.",
                                  text,
                              });
}

TEST_CASE("test_spans_keep_the_words_verbatim", "[test_automation_routing_gate]") {
    CHECK(plan_spans("Send this to my scripts: deploy 'staging' now!") ==
          Ids{"deploy 'staging' now!", "Send this to my scripts: deploy 'staging' now!"});
}

TEST_CASE("test_a_name_said_last_proposes_the_words_before_it", "[test_automation_routing_gate]") {
    CHECK(plan_spans("What's the weather in Lisbon? Ask Claude.", all_actions(), ROUTE_HOTKEY) ==
          Ids{"What's the weather in Lisbon?", "What's the weather in Lisbon? Ask Claude."});
}

TEST_CASE("test_each_named_action_proposes_its_own_remainder", "[test_automation_routing_gate]") {
    CHECK(
        plan_spans("Ask Claude Code why the tests fail") ==
        Ids{"why the tests fail", "Code why the tests fail", "Ask Claude Code why the tests fail"});
}

TEST_CASE("test_nothing_to_strip_means_no_payload_question", "[test_automation_routing_gate]") {
    const auto request = plan("Remind me to call the dentist", all_actions(), ROUTE_HOTKEY);
    REQUIRE(request);
    CHECK(request->spans.empty());
    CHECK(question_names(request->questions) == Ids{"route"});
}

TEST_CASE("test_the_name_alone_leaves_only_the_whole_text", "[test_automation_routing_gate]") {
    const auto request = plan("Claude.", all_actions(), ROUTE_AUTO);
    REQUIRE(request);
    CHECK(request->spans.empty());
    CHECK(question_names(request->questions) == Ids{"route"});
}

// The benchmark's labels come from OpenWhisper's benchmarks/voice_routing/cases.py through the
// vectors (function "benchmark"); the gate runs here, in C++, on each sentence.
namespace {

struct BenchmarkCase {
    std::string id, action_set, route, text;
    Json expect;
    std::vector<std::string> tags;

    bool tagged(std::string_view tag) const { return std::ranges::find(tags, tag) != tags.end(); }
    bool meant_for_an_action() const {
        return std::ranges::none_of(expect, [](const Json& alias) { return alias.is_null(); });
    }
    const std::vector<Action>& actions() const { return golden_set("benchmark:" + action_set); }
};

const Json& benchmark_data() {
    return golden::load(kRoutingModule).at("functions").at("benchmark").at(0).at("result");
}

std::vector<BenchmarkCase> benchmark() {
    std::vector<BenchmarkCase> cases;
    for (const Json& row : benchmark_data().at("cases")) {
        BenchmarkCase c;
        c.id = row.at(0).get<std::string>();
        c.action_set = row.at(1).get<std::string>();
        c.route = row.at(2).get<std::string>();
        c.text = row.at(3).get<std::string>();
        c.expect = row.at(4);
        c.tags = row.at(5).get<std::vector<std::string>>();
        cases.push_back(std::move(c));
    }
    return cases;
}

} // namespace

TEST_CASE("test_benchmark_is_large_enough_and_covers_both_route_modes",
          "[test_automation_routing_gate]") {
    const std::vector<BenchmarkCase> cases = benchmark();
    CHECK(cases.size() >= 80);
    std::vector<std::string> ids;
    std::vector<std::string> routes;
    for (const BenchmarkCase& c : cases) {
        if (std::ranges::find(ids, c.id) == ids.end()) {
            ids.push_back(c.id);
        }
        if (std::ranges::find(routes, c.route) == routes.end()) {
            routes.push_back(c.route);
        }
    }
    CHECK(ids.size() == cases.size());
    std::ranges::sort(routes);
    CHECK(routes == Ids{std::string(ROUTE_AUTO), std::string(ROUTE_HOTKEY)});
    for (std::string_view tag :
         {"name", "spoken", "filler", "asr", "mention", "ambiguous", "trailing", "purpose"}) {
        INFO(tag);
        CHECK(std::ranges::any_of(cases, [&](const BenchmarkCase& c) { return c.tagged(tag); }));
    }
    const Json& aliases = benchmark_data().at("aliases");
    for (const BenchmarkCase& c : cases) {
        for (const Json& alias : c.expect) {
            INFO(c.id);
            CHECK((alias.is_null() || std::ranges::find(aliases.at(c.action_set), alias) !=
                                          aliases.at(c.action_set).end()));
        }
    }
}

TEST_CASE("test_benchmark_plain_dictations_reach_the_judge_only_when_named",
          "[test_automation_routing_gate]") {
    for (const BenchmarkCase& c : benchmark()) {
        if (c.route != ROUTE_AUTO) {
            continue;
        }
        INFO(c.id);
        const bool passed = plan(c.text, c.actions(), c.route).has_value();
        if (c.meant_for_an_action() && !c.tagged("gate-miss")) {
            CHECK(passed); // a plain dictation addressed to an action must reach the judge
        }
        if (!passed) {
            CHECK((!c.meant_for_an_action() || c.tagged("gate-miss")));
        }
    }
}

// -- test_automation_fix_routing_gate.py ---------------------------------------------------

TEST_CASE("test_ordinary_sentences_from_the_review_stop_at_the_gate",
          "[test_automation_fix_routing_gate]") {
    for (const auto* actions : {&preset_actions(), &lowercase_actions()}) {
        for (std::string_view text : {
                 "Could you send me the report by Friday?",
                 "Could we move the meeting to three?",
                 "Called the bank this morning, they said it's fine.",
                 "Cold brew is cheaper if you make it at home.",
                 "Child care pickup is at five today.",
                 "Cloudy skies all week, bring a jacket.",
                 "Clued in the team about the outage.",
                 "Killed the stale jobs on the build box.",
                 "Okay so could you take a look at the PR?",
                 "So to do that we need a new index.",
                 "Now to do this properly we should refactor first.",
                 "To do that later we'd need a second server.",
                 "To do it right we need more time.",
                 "Chat about it with the team tomorrow.",
                 "Send the scripts to Bob by Friday.",
                 "Send my scripts to Bob by Friday.",
                 "Use the webhook from the staging config.",
                 "Use my webhook settings from staging.",
                 "Open the app and click settings.",
             }) {
            INFO(text);
            CHECK(named(text, *actions).empty());
            CHECK_FALSE(plan(text, *actions, ROUTE_AUTO));
        }
    }
}

TEST_CASE("test_benchmark_sentences_that_must_be_pasted_never_reach_the_judge",
          "[test_automation_fix_routing_gate]") {
    std::size_t stopped = 0;
    for (const BenchmarkCase& c : benchmark()) {
        if (!c.tagged("gate-stop")) {
            continue;
        }
        INFO(c.id);
        ++stopped;
        CHECK_FALSE(plan(c.text, c.actions(), c.route));
    }
    CHECK(stopped > 0);
}

TEST_CASE("test_names_said_to_an_action_still_pass", "[test_automation_fix_routing_gate]") {
    const std::vector<std::pair<std::string, Ids>> cases = {
        {"Cloud, what time zone is Tokyo in?", {"claude-chat"}}, // misheard at the start
        {"Ask cloud to translate good morning.", {"claude-chat"}},
        {"Hey clod, three lunch ideas", {"claude-chat"}},
        {"Claud, what's the boiling point?", {"claude-chat"}},
        {"Clawed code, run the linter", {"claude-code-session", "claude-chat"}},
        {"Tell Claude to summarize this thread.", {"claude-chat"}}, // a proper noun after lead-ins
        {"Can you ask ChatGPT for a packing list?", {"chatgpt-app"}},
        {"Send this to my scripts: rotate the logs.", {"scripts"}}, // the name's own words
        {"Send it to my scripts, rotate the logs.", {"scripts"}},
        {"Webhook, the build finished.", {"webhook"}}, // a generic name, said first
        {"Agent, add a dark mode toggle.", {"agent"}},
        {"To do, email the landlord.", {"todo"}}, // a split name before a pause
        {"To-do: call the plumber.", {"todo"}},
        {"Todo buy milk", {"todo"}},
    };
    for (const auto& [text, expected] : cases) {
        INFO(text);
        CHECK(named(text, preset_actions()) == expected);
    }
}

TEST_CASE("test_mishearings_sound_alike", "[test_automation_fix_routing_gate]") {
    for (const auto& [heard, name] : std::vector<std::pair<std::string, std::string>>{
             {"cloud", "claude"},
             {"clod", "claude"},
             {"claud", "claude"},
             {"clawed", "claude"},
             {"clawd", "claude"},
             {"codecs", "codex"},
         }) {
        INFO(heard << " / " << name);
        CHECK(rd::alike(heard, name));
    }
}

TEST_CASE("test_other_words_do_not_sound_alike", "[test_automation_fix_routing_gate]") {
    for (const auto& [heard, name] : std::vector<std::pair<std::string, std::string>>{
             {"could", "claude"},
             {"called", "claude"},
             {"cold", "claude"},
             {"child", "claude"},
             {"killed", "claude"},
             {"cloudy", "claude"},
             {"clued", "claude"},
             {"claudes", "claude"},
             {"today", "todo"},
             {"agents", "agent"},
             {"urgent", "agent"},
         }) {
        INFO(heard << " / " << name);
        CHECK_FALSE(rd::alike(heard, name));
    }
}

TEST_CASE("test_a_name_heard_as_several_words_names_it_when_a_request_follows",
          "[test_automation_fix_routing_gate]") {
    const std::vector<std::pair<std::string, Ids>> cases = {
        {"chat gpt what's 15 percent of 80", {"chatgpt-app"}},
        {"Chat GPT what's 15% of 80", {"chatgpt-app"}},
        {"chat gpt can you translate this", {"chatgpt-app"}},
        {"To do buy milk", {"todo"}},
        {"To do email the landlord", {"todo"}},
        {"To do, email the landlord", {"todo"}},
        {"To do list, call the plumber", {"todo"}},
    };
    for (std::string_view route : kBothRoutes) {
        for (const auto& [text, expected] : cases) {
            INFO(text << " / " << route);
            CHECK(named(text, preset_actions(), route) == expected);
            CHECK(named(text, lowercase_actions(), route) == expected);
        }
    }
}

TEST_CASE("test_a_name_heard_as_several_words_is_ordinary_speech_when_the_phrase_goes_on",
          "[test_automation_fix_routing_gate]") {
    for (std::string_view text : {
             "So to do that we need a new index.",
             "To do that later we'd need a second server.",
             "To do it right we need more time.",
             "Now to do this properly we should refactor first.",
             "To do with the budget, ask finance.",
         }) {
        INFO(text);
        CHECK(named(text, preset_actions()).empty());
        CHECK(named(text, preset_actions(), ROUTE_HOTKEY).empty());
    }
}

TEST_CASE("test_a_pause_still_sets_off_a_name_heard_as_several_words",
          "[test_automation_fix_routing_gate]") {
    CHECK(named("To do: that thing for the landlord", preset_actions()) == Ids{"todo"});
    CHECK(named("Buy milk, add it to my to do", preset_actions(), ROUTE_HOTKEY) == Ids{"todo"});
    CHECK(named("Buy milk, add it to my to do please", preset_actions(), ROUTE_HOTKEY) ==
          Ids{"todo"});
}

TEST_CASE("test_a_name_typed_in_lowercase_is_heard_as_a_name",
          "[test_automation_fix_routing_gate]") {
    // Whether a shortened name may follow lead-ins ("tell" isn't the name's "ask") comes from
    // how it was heard, not only from how it was typed.
    const std::vector<std::pair<std::string, Ids>> cases = {
        {"Tell Claude to write a haiku.", {"claude-chat"}},
        {"Can you tell Claude what time it is?", {"claude-chat"}},
        {"Tell Claude Code to fix the failing test.", {"claude-code-session", "claude-chat"}},
        {"Ask my Claude Code session to run the tests.", {"claude-code-session", "claude-chat"}},
        {"Open ChatGPT and ask for a lasagna recipe.", {"chatgpt-app"}},
        {"Ask claude to summarize this.", {"claude-chat"}}, // the name as typed
        {"Send this to my scripts: rotate the logs.", {"scripts"}},
    };
    for (std::string_view route : kBothRoutes) {
        for (const auto& [text, expected] : cases) {
            INFO(text << " / " << route);
            CHECK(named(text, lowercase_actions(), route) == expected);
            CHECK(named(text, preset_actions(), route) == expected);
        }
    }
}

TEST_CASE("test_a_common_noun_heard_in_lowercase_after_lead_ins_still_names_nothing",
          "[test_automation_fix_routing_gate]") {
    for (const auto* actions : {&preset_actions(), &lowercase_actions()}) {
        CHECK(named("Send my scripts to Bob by Friday.", *actions).empty());
        CHECK(named("Use agent mode for this one.", *actions).empty());
        CHECK(named("Use my Webhook settings from staging.", *actions).empty()); // only generic
    }
}

TEST_CASE("test_words_are_written_as_names_by_their_capitals_not_a_sentence_start",
          "[test_automation_fix_routing_gate]") {
    const auto words = rd::route_words("Tell Claude. Claude, ChatGPT and okay? Scripts");
    std::vector<std::pair<std::string, bool>> seen;
    for (const auto& word : words) {
        seen.emplace_back(word.norm, word.name);
    }
    CHECK(seen == std::vector<std::pair<std::string, bool>>{{"tell", false},
                                                            {"claude", true},
                                                            {"claude", false},
                                                            {"chatgpt", true},
                                                            {"and", false},
                                                            {"okay", false},
                                                            {"scripts", false}});
    CHECK(named("Use it. Scripts are in the shared drive.", lowercase_actions()).empty());
}

TEST_CASE("test_a_generic_name_counts_after_lead_ins_only_when_said_in_full",
          "[test_automation_fix_routing_gate]") {
    CHECK(named("Use my webhook to post this.", preset_actions()).empty());
    const std::vector<Action> hook = {
        make_action("h", "Post to my webhook", "webhook", Json{{"url", "http://127.0.0.1/x"}})};
    CHECK(named("Can you post to my webhook: the build is green", hook) == Ids{"h"});
}

TEST_CASE("test_a_name_said_last_needs_lead_ins_or_a_pause_before_it",
          "[test_automation_fix_routing_gate]") {
    const std::vector<Action> home = {make_action("home", "Home Assistant", "webhook",
                                                  Json{{"url", "http://127.0.0.1/x"}}, {"home"})};
    CHECK(named("Lock the front door, home.", home, ROUTE_HOTKEY) == Ids{"home"});
    CHECK(named("Lock the front door and head home.", home, ROUTE_HOTKEY).empty());
    CHECK(named("Restart the webhook.", preset_actions(), ROUTE_HOTKEY).empty());
}

// -- golden vectors --------------------------------------------------------------------------

namespace {

Json words_json(const std::vector<rd::RouteWord>& words) {
    Json out = Json::array();
    for (const rd::RouteWord& word : words) {
        out.push_back(Json::array({word.norm, word.start, word.end, word.pause, word.name}));
    }
    return out;
}

Json sound_json(const std::optional<rd::WordSound>& sound) {
    if (!sound) {
        return nullptr;
    }
    return Json::array({sound->onset, sound->vowel, sound->coda, sound->syllables});
}

} // namespace

TEST_CASE("routing word model replays OpenWhisper", "[golden]") {
    golden::for_each(kRoutingModule, "_words", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), words_json(rd::route_words(c.arg<std::string>(0)))));
    });
    golden::for_each(kRoutingModule, "_sound", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), sound_json(rd::word_sound(c.arg<std::string>(0)))));
    });
    golden::for_each(kRoutingModule, "_alike", [](const golden::Case& c) {
        CHECK(rd::alike(c.arg<std::string>(0), c.arg<std::string>(1)) == c.result<bool>());
    });
    golden::for_each(kRoutingModule, "sounds", [](const golden::Case& c) {
        Json sounds = Json::array();
        for (const std::string& word : c.arg<std::vector<std::string>>(0)) {
            sounds.push_back(sound_json(rd::word_sound(word)));
        }
        CHECK(golden::equal(c.result<Json>(), sounds));
    });
    golden::for_each(kRoutingModule, "alike_pairs", [](const golden::Case& c) {
        Json pairs = Json::array();
        const auto names = c.arg<std::vector<std::string>>(1);
        for (const std::string& heard : c.arg<std::vector<std::string>>(0)) {
            for (const std::string& name : names) {
                if (rd::alike(heard, name)) {
                    pairs.push_back(Json::array({heard, name}));
                }
            }
        }
        CHECK(golden::equal(c.result<Json>(), pairs));
    });
}

TEST_CASE("routing name variants replay OpenWhisper", "[golden]") {
    golden::for_each(kRoutingModule, "_name_variants", [](const golden::Case& c) {
        Json variants = Json::array();
        for (const rd::NameVariant& variant :
             rd::name_variants(c.arg<std::string>(0), c.arg<std::vector<std::string>>(1))) {
            variants.push_back(Json::array({variant.words, variant.dropped, variant.after_lead_ins,
                                            variant.after_lead_ins_as_name}));
        }
        CHECK(golden::equal(c.result<Json>(), variants));
    });
}

TEST_CASE("routing gate replays OpenWhisper", "[golden]") {
    golden::for_each(kRoutingModule, "gate", [](const golden::Case& c) {
        Json matches = Json::array();
        for (const GateMatch& match :
             gate(c.arg<std::string>(0), actions_arg(c, 1), c.arg<std::string>(2))) {
            matches.push_back(Json::array(
                {match.action_id, match.first, match.name_start, match.name_end, match.leading}));
        }
        CHECK(golden::equal(c.result<Json>(), matches));
    });
}

TEST_CASE("routing payload spans replay OpenWhisper", "[golden]") {
    golden::for_each(kRoutingModule, "_spans", [](const golden::Case& c) {
        const std::string text = c.arg<std::string>(0);
        const auto words = rd::route_words(text);
        const auto matches = gate(text, actions_arg(c, 1), c.arg<std::string>(2));
        CHECK(rd::payload_spans(text, words, matches) == c.result<std::vector<std::string>>());
    });
    golden::for_each(kRoutingModule, "_clip", [](const golden::Case& c) {
        CHECK(rd::clip(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::size_t>(2)) ==
              c.result<std::string>());
    });
    golden::for_each(kRoutingModule, "_sentence_start", [](const golden::Case& c) {
        CHECK(rd::sentence_start(c.arg<std::string>(0)) == c.result<std::string>());
    });
    golden::for_each(kRoutingModule, "_without_polite_end", [](const golden::Case& c) {
        CHECK(rd::without_polite_end(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("routing requests replay OpenWhisper byte for byte", "[golden]") {
    golden::for_each(kRoutingModule, "plan", [](const golden::Case& c) {
        const auto request = plan(c.arg<std::string>(0), actions_arg(c, 2), c.arg<std::string>(1));
        const Json& expected = c.result<Json>();
        if (expected.is_null()) {
            CHECK_FALSE(request);
            return;
        }
        REQUIRE(request);
        CHECK(request->text == expected.at("text").get<std::string>());
        CHECK(request->candidates == expected.at("candidates").get<std::vector<std::string>>());
        CHECK(request->matched == expected.at("matched").get<std::vector<std::string>>());
        CHECK(golden::equal(expected.at("state"), request->state));
        // The questions as json.dumps writes them: key order and every byte.
        CHECK(py::dumps(request->questions) == expected.at("questions").get<std::string>());
        Json spans = Json::array();
        for (const auto& [option, span] : request->spans) {
            spans.push_back(Json::array({option, span}));
        }
        CHECK(golden::equal(expected.at("spans"), spans));
    });
}
