// tests/test_personalize_s5_snippets.py: snippet storage, trigger matching, placeholders and
// expansion (the light Markdown tests are in test_snippet_markdown.cpp); plus the
// services.snippets golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/json.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "text/snippets.h"
#include "text_test_support.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::text::Expansion;
using fv::text::Snippet;
using fv::text::SnippetPlan;
using fv::text_test::kSnippets;
using fv::text_test::plain;
using fv::text_test::snippet;
using fv::text_test::stored;
namespace text = fv::text;
namespace golden = fv::golden;

namespace {

const Snippet kCal =
    snippet("cal", "my calendar link", "https://cal.example.com/dana?ref=email#slots");
const Snippet kEmail = snippet("email", "my email", "dana@example.com");
const Snippet kEmailAddress = snippet("address", "my email address", "dana.lee@example.com");
const Snippet kSignOff =
    snippet("sig", "my sign-off",
            "**Dana Lee**\nProduct designer\n[dana.design](https://dana.design)", true);
const std::vector<Snippet> kAll = {kCal, kEmail, kEmailAddress, kSignOff};

std::vector<Snippet> many(std::size_t count) {
    std::vector<Snippet> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(
            snippet("id" + std::to_string(i), "phrase number " + std::to_string(i), "text"));
    }
    return out;
}

SnippetPlan plan_of(std::string text,
                    std::vector<std::pair<std::string, Snippet>> placeholders = {},
                    std::optional<Snippet> whole = std::nullopt) {
    return {std::move(text), std::move(whole), std::move(placeholders)};
}

Json plan_json(const SnippetPlan& plan) {
    Json placeholders = Json::array();
    for (const auto& [token, item] : plan.placeholders) {
        placeholders.push_back(Json::array({token, plain(item)}));
    }
    Json out = Json::object();
    out["text"] = plan.text;
    out["whole"] = plain(plan.whole);
    out["placeholders"] = placeholders;
    return out;
}

// The case files' {"error": message} or the value, for comparing a refused change.
template <class Body> Json error_or(Body body) {
    try {
        return body();
    } catch (const fv::py::ValueError& error) {
        return {{"error", error.what()}};
    }
}

bool same_rebranded(const Json& expected, const Json& actual) {
    if (expected.is_object() && expected.contains("error") && actual.is_object() &&
        actual.contains("error")) {
        return golden::equal_rebranded(expected.at("error").get<std::string>(),
                                       actual.at("error").get<std::string>());
    }
    return golden::equal(expected, actual);
}

} // namespace

// ---- storage ----------------------------------------------------------------------------------

// from
// tests/test_personalize_s5_snippets.py::test_normalize_ignores_case_punctuation_and_hyphens_but_keeps_words_whole
TEST_CASE("test_normalize_ignores_case_punctuation_and_hyphens_but_keeps_words_whole",
          "[test_personalize_s5_snippets]") {
    CHECK(text::normalize("  My  Sign-Off!  ") == "my sign off");
    CHECK(text::normalize("Don’t, Acme.com") == "don't acme.com");
    CHECK(text::normalize("?!").empty());
}

// from
// tests/test_personalize_s5_snippets.py::test_load_keeps_valid_snippets_in_order_and_skips_the_rest
TEST_CASE("test_load_keeps_valid_snippets_in_order_and_skips_the_rest",
          "[test_personalize_s5_snippets]") {
    Json email = text::snippet_to_json(kEmail);
    email["formatted"] = "yes";
    const Json settings = fv::text_test::settings_with(
        kSnippets,
        Json::array({
            text::snippet_to_json(kCal),
            "not a dict",
            {{"id", "x"}, {"trigger", "no text"}},
            {{"id", "y"}, {"trigger", "blank"}, {"text", "   "}},
            {{"id", "z"}, {"trigger", "a"}, {"text", "too short a trigger"}},
            {{"id", "w"}, {"trigger", std::string(61, 't')}, {"text", "too long a trigger"}},
            {{"id", "v"}, {"trigger", "too long a text"}, {"text", std::string(10'001, 'x')}},
            {{"id", "cal"}, {"trigger", "same id"}, {"text", "duplicate id"}},
            {{"id", "u"}, {"trigger", "My Calendar, Link!"}, {"text", "duplicate trigger"}},
            {{"id", 7}, {"trigger", "bad id"}, {"text", "text"}},
            email,
            text::snippet_to_json(kSignOff),
        }));
    CHECK(text::load_snippets(settings) == std::vector<Snippet>{kCal, kEmail, kSignOff});
    CHECK(text::load_snippets(fv::text_test::settings_with(kSnippets, "nope")).empty());
    CHECK(text::load_snippets(Json::object()).empty());
}

// from tests/test_personalize_s5_snippets.py::test_load_caps_the_library
TEST_CASE("test_load_caps_the_library", "[test_personalize_s5_snippets]") {
    CHECK(text::load_snippets(stored(many(text::MAX_SNIPPETS + 5))).size() == text::MAX_SNIPPETS);
}

// from tests/test_personalize_s5_snippets.py::test_save_adds_replaces_and_delete_removes
TEST_CASE("test_save_adds_replaces_and_delete_removes", "[test_personalize_s5_snippets]") {
    Json settings = Json::object();
    text::save_snippet(settings, kCal);
    text::save_snippet(settings, kEmail);
    text::save_snippet(settings,
                       snippet("cal", "  my booking link ", "https://cal.example.com/new"));

    CHECK(text::load_snippets(settings) ==
          std::vector<Snippet>{snippet("cal", "my booking link", "https://cal.example.com/new"),
                               kEmail});
    const Json expected = {{"id", "cal"},
                           {"trigger", "my booking link"},
                           {"text", "https://cal.example.com/new"},
                           {"formatted", false}};
    CHECK(settings.at(kSnippets).at(0) == expected);

    text::delete_snippet(settings, "cal");
    CHECK(text::load_snippets(settings) == std::vector<Snippet>{kEmail});
}

// from tests/test_personalize_s5_snippets.py::test_validation_explains_what_to_fix
TEST_CASE("test_validation_explains_what_to_fix", "[test_personalize_s5_snippets]") {
    const auto [item, message] = GENERATE(values<std::pair<Snippet, std::string>>({
        {snippet("n", "", "text"), "Add a trigger"},
        {snippet("n", "!!", "text"), "words you can say"},
        {snippet("n", "x", "text"), "2 to 60"},
        {snippet("n", std::string(61, 'x'), "text"), "2 to 60"},
        {snippet("n", "say [[S1]]", "text"), "words you can say"},
        {snippet("n", "my phrase", "  \n"), "Add the text"},
        {snippet("n", "my phrase", std::string(10'001, 'x')), "10,000"},
        {snippet("n", "MY CALENDAR-LINK.", "text"), "Another snippet already uses this trigger"},
    }));
    CHECK_THROWS_AS(text::validate_snippet(item, stored({kCal})), fv::py::ValueError);
    CHECK_THROWS_WITH(text::validate_snippet(item, stored({kCal})), ContainsSubstring(message));
}

// from tests/test_personalize_s5_snippets.py::test_saving_the_same_snippet_again_is_not_a_duplicate
TEST_CASE("test_saving_the_same_snippet_again_is_not_a_duplicate",
          "[test_personalize_s5_snippets]") {
    CHECK_NOTHROW(
        text::validate_snippet(snippet("cal", "My calendar link", "new text"), stored({kCal})));
}

// from
// tests/test_personalize_s5_snippets.py::test_a_full_library_refuses_new_snippets_but_still_saves_edits
TEST_CASE("test_a_full_library_refuses_new_snippets_but_still_saves_edits",
          "[test_personalize_s5_snippets]") {
    const Json settings = stored(many(text::MAX_SNIPPETS));
    CHECK_THROWS_WITH(text::validate_snippet(snippet("new", "one more phrase", "text"), settings),
                      ContainsSubstring("Delete one"));
    CHECK_THROWS_WITH(text::validate_snippet(snippet("new", "one more phrase", "text"), settings),
                      "You have 200 snippets, the most " + std::string(fv::identity().product) +
                          " keeps. Delete one to add another.");
    CHECK_NOTHROW(text::validate_snippet(snippet("id3", "phrase number 3", "edited"), settings));
}

// from tests/test_personalize_s5_snippets.py::test_trigger_note
TEST_CASE("test_trigger_note", "[test_personalize_s5_snippets]") {
    struct Row {
        std::string trigger;
        std::string exclude;
        std::string note;
    };
    const auto row = GENERATE(values<Row>({
        {"My Email!", "", "Another snippet already uses this trigger."},
        {"my email", "email", "Also part of “my email address”"},
        {"send my calendar link", "", "Includes “my calendar link”"},
        {"my calender link", "", "Sounds close to “my calendar link”"},
        {"signature", "", "A single word"},
        {"my home address", "", ""},
        {"", "", ""},
    }));
    CHECK_THAT(text::trigger_note(row.trigger, kAll, row.exclude), StartsWith(row.note));
}

// ---- matching ---------------------------------------------------------------------------------

// from
// tests/test_personalize_s5_snippets.py::test_a_whole_utterance_trigger_ignores_case_and_punctuation
TEST_CASE("test_a_whole_utterance_trigger_ignores_case_and_punctuation",
          "[test_personalize_s5_snippets]") {
    const std::string utterance = GENERATE(
        values<std::string>({"My calendar link.", "my CALENDAR link", " my calendar, link! "}));
    const SnippetPlan plan = text::plan_expansion(utterance, kAll);
    CHECK(plan.whole == kCal);
    CHECK(plan.placeholders.empty());
    CHECK(plan.text == utterance);
}

// from
// tests/test_personalize_s5_snippets.py::test_a_trigger_inside_a_sentence_becomes_a_placeholder
TEST_CASE("test_a_trigger_inside_a_sentence_becomes_a_placeholder",
          "[test_personalize_s5_snippets]") {
    const SnippetPlan plan = text::plan_expansion("Here's my Calendar link, thanks!", kAll);
    CHECK(plan.text == "Here's [[S1]], thanks!");
    CHECK(plan.placeholders == std::vector<std::pair<std::string, Snippet>>{{"[[S1]]", kCal}});
    CHECK_FALSE(plan.whole);
}

// from
// tests/test_personalize_s5_snippets.py::test_every_occurrence_gets_its_own_placeholder_and_the_longest_trigger_wins
TEST_CASE("test_every_occurrence_gets_its_own_placeholder_and_the_longest_trigger_wins",
          "[test_personalize_s5_snippets]") {
    const SnippetPlan plan =
        text::plan_expansion("Use my email address, or my email, or my email address", kAll);
    CHECK(plan.text == "Use [[S1]], or [[S2]], or [[S3]]");
    std::vector<std::string> ids;
    for (const auto& [token, item] : plan.placeholders) {
        ids.push_back(item.id);
    }
    CHECK(ids == std::vector<std::string>{"address", "email", "address"});
}

// from
// tests/test_personalize_s5_snippets.py::test_triggers_match_whole_words_only_and_never_across_a_sentence_end
TEST_CASE("test_triggers_match_whole_words_only_and_never_across_a_sentence_end",
          "[test_personalize_s5_snippets]") {
    CHECK(text::plan_expansion("Check my emails today", kAll).placeholders.empty());
    CHECK(text::plan_expansion("That's my calendar. Link it later", kAll).placeholders.empty());
    CHECK(text::plan_expansion("that was my sign off for today", kAll).text ==
          "that was [[S1]] for today");
}

// from tests/test_personalize_s5_snippets.py::test_nothing_to_plan_returns_the_text_unchanged
TEST_CASE("test_nothing_to_plan_returns_the_text_unchanged", "[test_personalize_s5_snippets]") {
    CHECK(text::plan_expansion("hello there", kAll) == plan_of("hello there"));
    CHECK(text::plan_expansion("my calendar link", {}) == plan_of("my calendar link"));
    CHECK(text::plan_expansion("", kAll) == plan_of(""));
}

// from
// tests/test_personalize_s5_snippets.py::test_text_that_already_reads_as_a_placeholder_is_left_alone
TEST_CASE("test_text_that_already_reads_as_a_placeholder_is_left_alone",
          "[test_personalize_s5_snippets]") {
    CHECK(text::plan_expansion("The [[S1]] marker and my email", kAll) ==
          plan_of("The [[S1]] marker and my email"));
}

// ---- placeholders and expansion ---------------------------------------------------------------

// from tests/test_personalize_s5_snippets.py::test_placeholders_round_trip_through_a_cleanup
TEST_CASE("test_placeholders_round_trip_through_a_cleanup", "[test_personalize_s5_snippets]") {
    const SnippetPlan plan =
        text::plan_expansion("send it to my email address and my calendar link", kAll);
    const Expansion out = text::expand("Send it to [[S1]] and [[S2]].", plan);
    CHECK(out.ok);
    CHECK(out.plain ==
          "Send it to dana.lee@example.com and https://cal.example.com/dana?ref=email#slots.");
    CHECK(out.html.empty());
}

// from
// tests/test_personalize_s5_snippets.py::test_placeholders_a_model_respaced_or_lowercased_still_expand
TEST_CASE("test_placeholders_a_model_respaced_or_lowercased_still_expand",
          "[test_personalize_s5_snippets]") {
    const SnippetPlan plan = text::plan_expansion("mail my email please", kAll);
    CHECK(text::expand("Mail [[ s1 ]], please.", plan) ==
          Expansion{"Mail dana@example.com, please.", "", true});
}

// from
// tests/test_personalize_s5_snippets.py::test_a_lost_duplicated_or_invented_placeholder_is_not_ok
TEST_CASE("test_a_lost_duplicated_or_invented_placeholder_is_not_ok",
          "[test_personalize_s5_snippets]") {
    const std::string cleaned = GENERATE(values<std::string>({
        "Mail it, please.",
        "Mail [[S1]] and [[S1]], please.",
        "Mail [[S1]] to [[S2]].",
    }));
    const SnippetPlan plan = text::plan_expansion("mail my email please", kAll);
    CHECK(text::expand(cleaned, plan) == Expansion{cleaned, "", false});
}

// from tests/test_personalize_s5_snippets.py::test_a_whole_snippet_expands_to_its_text
TEST_CASE("test_a_whole_snippet_expands_to_its_text", "[test_personalize_s5_snippets]") {
    CHECK(text::expand("My calendar link.", text::plan_expansion("My calendar link.", kAll)) ==
          Expansion{kCal.text, "", true});
}

// from
// tests/test_personalize_s5_snippets.py::test_a_formatted_whole_snippet_has_rich_text_and_a_readable_plain_text
TEST_CASE("test_a_formatted_whole_snippet_has_rich_text_and_a_readable_plain_text",
          "[test_personalize_s5_snippets]") {
    const Expansion out = text::expand("my sign off", text::plan_expansion("my sign off", kAll));
    CHECK(out.ok);
    CHECK(out.plain == "Dana Lee\nProduct designer\ndana.design (https://dana.design)");
    CHECK(
        out.html ==
        R"(<b>Dana Lee</b><br>Product designer<br><a href="https://dana.design">dana.design</a>)");
}

// from
// tests/test_personalize_s5_snippets.py::test_text_around_a_formatted_snippet_is_escaped_in_the_html
TEST_CASE("test_text_around_a_formatted_snippet_is_escaped_in_the_html",
          "[test_personalize_s5_snippets]") {
    const SnippetPlan plan = text::plan_expansion("1 < 2 & my email\nthen my sign off", kAll);
    const Expansion out = text::expand(plan.text, plan);
    CHECK(out.ok);
    CHECK_THAT(out.plain, StartsWith("1 < 2 & dana@example.com\nthen Dana Lee\n"));
    CHECK_THAT(out.html, StartsWith("1 &lt; 2 &amp; dana@example.com<br>then <b>Dana Lee</b><br>"));
}

// from tests/test_personalize_s5_snippets.py::test_snippet_text_is_never_scanned_for_placeholders
TEST_CASE("test_snippet_text_is_never_scanned_for_placeholders", "[test_personalize_s5_snippets]") {
    const Snippet tricky = snippet("t", "my tricky one", "literal [[S2]] stays");
    const SnippetPlan plan = text::plan_expansion("my tricky one and my email", {tricky, kEmail});
    CHECK(text::expand(plan.text, plan) ==
          Expansion{"literal [[S2]] stays and dana@example.com", "", true});
}

// from tests/test_personalize_s5_snippets.py::test_no_placeholders_means_no_guard
TEST_CASE("test_no_placeholders_means_no_guard", "[test_personalize_s5_snippets]") {
    CHECK(text::prompt_guard(plan_of("hello")).empty());
    CHECK(text::prompt_guard(text::plan_expansion("my calendar link", kAll)).empty());
}

// from
// tests/test_personalize_s5_snippets.py::test_the_guard_is_one_sentence_naming_every_placeholder
TEST_CASE("test_the_guard_is_one_sentence_naming_every_placeholder",
          "[test_personalize_s5_snippets]") {
    const std::string one = text::prompt_guard(text::plan_expansion("mail my email", kAll));
    const std::string three = text::prompt_guard(
        text::plan_expansion("my email, my email address and my calendar link", kAll));
    CHECK_THAT(one, ContainsSubstring("[[S1]]") && ContainsSubstring("exactly once") &&
                        ContainsSubstring("unchanged"));
    CHECK_THAT(three, ContainsSubstring("[[S1]], [[S2]] and [[S3]]"));
    for (const std::string& guard : {one, three}) {
        CHECK(guard.ends_with("."));
        CHECK(guard.find(". ") == std::string::npos);
    }
}

TEST_CASE("demo step 3: a sign-off inserts its signature alone or in a sentence", "[snippets]") {
    Json settings = Json::object();
    text::save_snippet(settings, kSignOff);
    const auto saved = text::load_snippets(settings);
    const Expansion alone = text::expand("x", text::plan_expansion("My sign-off.", saved));
    CHECK(alone.html.starts_with("<b>Dana Lee</b>"));
    const SnippetPlan inner = text::plan_expansion("thanks, my sign off", saved);
    CHECK(inner.text == "thanks, [[S1]]");
    const Expansion rich = text::expand("Thanks, [[S1]]", inner);
    CHECK(rich.ok);
    CHECK(rich.html == "Thanks, " + text::snippet_html(kSignOff));
}

// ---- golden vectors ---------------------------------------------------------------------------

TEST_CASE("golden normalize", "[golden]") {
    golden::for_each("services.snippets", "normalize", [](const golden::Case& c) {
        CHECK(text::normalize(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden load_snippets", "[golden]") {
    golden::for_each("services.snippets", "load_snippets", [](const golden::Case& c) {
        Json got = Json::array();
        for (const Snippet& item : text::load_snippets(c.args().at(0))) {
            got.push_back(plain(item));
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden validate_snippet", "[golden]") {
    golden::for_each("services.snippets", "validate_snippet", [](const golden::Case& c) {
        const Json got = error_or([&]() -> Json {
            text::validate_snippet(fv::text_test::snippet_from(c.args().at(0)), c.args().at(1));
            return nullptr;
        });
        CHECK(same_rebranded(c.result<Json>(), got));
    });
}

TEST_CASE("golden save_snippet", "[golden]") {
    golden::for_each("services.snippets", "save_snippet", [](const golden::Case& c) {
        const Json got = error_or([&]() -> Json {
            Json settings = c.args().at(0);
            text::save_snippet(settings, fv::text_test::snippet_from(c.args().at(1)));
            return settings;
        });
        CHECK(same_rebranded(c.result<Json>(), got));
    });
}

TEST_CASE("golden delete_snippet", "[golden]") {
    golden::for_each("services.snippets", "delete_snippet", [](const golden::Case& c) {
        Json settings = c.args().at(0);
        text::delete_snippet(settings, c.arg<std::string>(1));
        CHECK(golden::equal(c.result<Json>(), settings));
    });
}

TEST_CASE("golden find_duplicate", "[golden]") {
    golden::for_each("services.snippets", "find_duplicate", [](const golden::Case& c) {
        const auto found = text::find_duplicate(c.arg<std::string>(0),
                                                fv::text_test::snippets_from(c.args().at(1)),
                                                c.arg<std::string>(2));
        CHECK(golden::equal(c.result<Json>(), plain(found)));
    });
}

TEST_CASE("golden trigger_note", "[golden]") {
    golden::for_each("services.snippets", "trigger_note", [](const golden::Case& c) {
        CHECK(text::trigger_note(c.arg<std::string>(0),
                                 fv::text_test::snippets_from(c.args().at(1)),
                                 c.arg<std::string>(2)) == c.result<std::string>());
    });
}

TEST_CASE("golden plan_expansion", "[golden]") {
    golden::for_each("services.snippets", "plan_expansion", [](const golden::Case& c) {
        const SnippetPlan plan = text::plan_expansion(c.arg<std::string>(0),
                                                      fv::text_test::snippets_from(c.args().at(1)));
        CHECK(golden::equal(c.result<Json>(), plan_json(plan)));
    });
}

TEST_CASE("golden prompt_guard", "[golden]") {
    golden::for_each("services.snippets", "prompt_guard", [](const golden::Case& c) {
        CHECK(text::prompt_guard(text::plan_expansion(
                  c.arg<std::string>(0), fv::text_test::snippets_from(c.args().at(1)))) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden expand", "[golden]") {
    golden::for_each("services.snippets", "expand", [](const golden::Case& c) {
        const SnippetPlan plan = text::plan_expansion(c.arg<std::string>(1),
                                                      fv::text_test::snippets_from(c.args().at(2)));
        const Expansion out = text::expand(c.arg<std::string>(0), plan);
        CHECK(golden::equal(c.result<Json>(), Json::array({out.plain, out.html, out.ok})));
    });
}

TEST_CASE("golden plain_text and snippet_html", "[golden]") {
    golden::for_each("services.snippets", "plain_text", [](const golden::Case& c) {
        CHECK(text::plain_text(fv::text_test::snippet_from(c.args().at(0))) ==
              c.result<std::string>());
    });
    golden::for_each("services.snippets", "snippet_html", [](const golden::Case& c) {
        CHECK(text::snippet_html(fv::text_test::snippet_from(c.args().at(0))) ==
              c.result<std::string>());
    });
}
