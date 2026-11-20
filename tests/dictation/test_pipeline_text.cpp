// tests/test_personalize_foundation_pipeline.py: the dictation pipeline's stages are inert by
// default, ordered and never raise; tests/test_personalize_fix_f5_stats_spoken.py,
// test_personalize_fix_f5_rich_join.py and test_stats_features.py: what the text stages give
// Stats and the paste. Where OpenWhisper monkeypatches a module, the C++ test passes the stand-in
// through the seam the stage takes (a TextPreparer, CleanupPromptBlocks, an injected join).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include "cleanup_support.h"
#include "dictation/batch.h"
#include "dictation/cleanup_prompt.h"
#include "dictation/pipeline.h"
#include "dictation/recognition.h"
#include "dictation/text_preparer.h"
#include "focus/testing/fake_platform.h"
#include "history/dictation_stats.h"
#include "history/stats_summary.h"
#include "pyport/text.h"
#include "runtime_support.h"
#include "text/cleanup_profiles.h"
#include "text/cleanup_prompts.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;

namespace {

const std::vector<std::string> kRules{"Spell Acme correctly."};

AppIdentity outlook() {
    return app("outlook.exe", "Outlook", 10, "0x1");
}

TextContext dear_sam() {
    TextContext context;
    context.before = "Dear Sam,";
    context.caret_known = true;
    context.source = "uia";
    return context;
}

// _today: the prompt _maybe_cleanup_transcript built before the pipeline.
std::string today(const Json& settings, const std::vector<std::string>& rules,
                  const fv::text::CleanupProfile* profile = nullptr,
                  std::optional<std::string> batch_context = std::nullopt) {
    std::string prompt = profile != nullptr
                             ? fv::text::compose_profile_prompt(*profile, rules)
                             : fv::text::compose_transcript_cleanup_prompt(
                                   fv::text::resolve_transcript_cleanup_prompt(settings), rules);
    if (batch_context) {
        prompt = compose_batch_cleanup_prompt(prompt, *batch_context);
    }
    return prompt;
}

fv::text::CleanupProfile notes_profile() {
    return fv::text::CleanupProfile{"notes", "Notes", "Make bullet notes.", "", true};
}

// The `blocks` fixture: every prompt block returns its name, so the order shows in the prompt.
struct NamedBlocks {
    std::vector<std::optional<FocusSnapshot>> styles;
    CleanupPromptBlocks blocks() {
        CleanupPromptBlocks named;
        named.style_for = [this](const std::optional<FocusSnapshot>& snapshot, const Json&) {
            styles.push_back(snapshot);
            return std::optional<fv::text::app_styles::AppStyle>(
                fv::text::app_styles::AppStyle{"email", "casual", "Outlook", "text"});
        };
        named.style_block = [](const std::optional<fv::text::app_styles::AppStyle>& style) {
            return "STYLE " + style->tone;
        };
        named.dictionary_block = [](const Json&) {
            return std::string("DICTIONARY");
        };
        named.snippet_guard = [](const SnippetPlan&) {
            return std::string("GUARD");
        };
        named.context_block = [](const std::optional<FocusSnapshot>& snapshot) {
            return "CONTEXT " + snapshot->text->before;
        };
        return named;
    }
};

std::string join(const std::vector<std::string>& parts) {
    std::string joined;
    for (const auto& part : parts) {
        joined += joined.empty() ? part : "\n\n" + part;
    }
    return joined;
}

// The dictionary and snippet stand-ins of
// test_dictionary_applies_to_every_job_and_snippets_only_to_dictation.
class StandInPreparer final : public TextPreparer {
public:
    std::string apply_replacements(std::string_view text, const Json&) const override {
        std::string out(text);
        if (const auto at = out.find("acme"); at != std::string::npos) {
            out.replace(at, 4, "Acme");
        }
        return out;
    }
    SnippetPlan plan_expansion(std::string_view text, const Json&) const override {
        planned.emplace_back(text);
        return SnippetPlan{
            "[[S1]]", std::nullopt, {{"[[S1]]", Snippet{"s", "my address", "1 Main St"}}}};
    }
    mutable std::vector<std::string> planned;
};

// A capture service whose every call raises.
class BrokenCaptureService final : public fv::focus::CaptureService {
public:
    std::shared_ptr<fv::focus::FocusRequest> request(bool, bool) override {
        throw std::runtime_error("capture broke");
    }
    std::optional<AppIdentity> current_identity() override {
        throw std::runtime_error("capture broke");
    }
    void reread(const std::optional<AppIdentity>&,
                std::function<void(std::optional<TextContext>)> callback) override {
        callback(std::nullopt);
    }
    void shutdown() override {}
};

} // namespace

// --- inert by default -----------------------------------------------------------------------

TEST_CASE("inert prompt is todays prompt", "[test_personalize_foundation_pipeline]") {
    // from tests/test_personalize_foundation_pipeline.py::test_inert_prompt_is_todays_prompt
    const Json settings = GENERATE(Json::object(), Json{{"transcript_cleanup_enabled", true}},
                                   Json{{"transcript_cleanup_prompt", "  Fix my words.  "}},
                                   Json{{"transcript_cleanup_prompt", "   "}},
                                   Json{{"transcript_cleanup_level", "high"}});
    const auto rules = GENERATE(std::vector<std::string>{}, kRules);
    const auto batch_context = GENERATE(std::optional<std::string>(),
                                        std::optional<std::string>("Two halves of one call."));
    const auto bare = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    const PreparedText prepared =
        prepare_text("hello there", bare.get(), settings, dictionary_preparer());
    const auto job = shared(job_in(outlook(), dear_sam()));

    const std::string prompt = compose_cleanup_prompt(
        settings, job.get(), nullptr, rules, prepared,
        batch_context ? std::optional<std::string_view>(*batch_context) : std::nullopt);

    CHECK(prompt == today(settings, rules, nullptr, batch_context));
}

TEST_CASE("inert profile prompt is todays prompt", "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_inert_profile_prompt_is_todays_prompt
    const auto profile = notes_profile();
    const PreparedText prepared =
        prepare_text("hello", nullptr, Json::object(), dictionary_preparer());

    const std::string prompt = compose_cleanup_prompt(Json::object(), nullptr, &profile, kRules,
                                                      prepared, std::string_view("ctx"));

    CHECK(prompt == today(Json::object(), kRules, &profile, "ctx"));
}

TEST_CASE("inert text is unchanged", "[test_personalize_foundation_pipeline]") {
    // from tests/test_personalize_foundation_pipeline.py::test_inert_text_is_unchanged
    const auto mode = GENERATE(0, 1, 2);
    std::shared_ptr<const DictationJob> job;
    if (mode == 1) {
        job = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    } else if (mode == 2) {
        job = shared(job_in(std::nullopt, std::nullopt, JobMode::Command, false));
    }
    const PreparedText prepared =
        prepare_text("hello there", job.get(), Json::object(), dictionary_preparer());

    CHECK(prepared.original == "hello there");
    CHECK(prepared.text == "hello there");
    CHECK(prepared.cleanup_input == "hello there");
    CHECK_FALSE(prepared.skip_cleanup);
    CHECK(finish_text("Hello there.", prepared, dictionary_preparer()) ==
          FinishedText{"Hello there.", "", true, "hello there"});
    CHECK(finish_text("hello there", prepared, dictionary_preparer()) ==
          FinishedText{"hello there", "", true, "hello there"});
    CHECK(text_for_paste("Hello there.", job.get()) == "Hello there.");
}

// --- block order ----------------------------------------------------------------------------

TEST_CASE("blocks follow the base prompt in order", "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_blocks_follow_the_base_prompt_in_order
    NamedBlocks named;
    const Json settings{{"app_context_read_text", true}};
    const auto job = shared(job_in(outlook(), dear_sam()));
    const PreparedText prepared = prepare_text("hi", job.get(), settings, dictionary_preparer());
    CleanupPromptBlocks blocks = named.blocks();
    // The style fixture's Outlook is no terminal, and its list note is the real one: Outlook is
    // a known app, so it adds nothing.

    const std::string prompt = compose_cleanup_prompt(settings, job.get(), nullptr, kRules,
                                                      prepared, std::string_view("BATCH"), blocks);

    const std::string base = fv::text::compose_transcript_cleanup_prompt(
        fv::text::resolve_transcript_cleanup_prompt(settings), kRules);
    const std::string expected =
        join({base, "STYLE casual", "DICTIONARY", "GUARD", "CONTEXT Dear Sam,"});
    CHECK(prompt == compose_batch_cleanup_prompt(expected, "BATCH"));
    REQUIRE(named.styles.size() == 1);
    CHECK(named.styles[0] == job->snapshot());
}

TEST_CASE("profile and switches leave their blocks out", "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_profile_and_switches_leave_their_blocks_out
    NamedBlocks named;
    const auto profile = notes_profile();
    const auto job = shared(job_in(outlook(), dear_sam()));
    const PreparedText prepared =
        prepare_text("hi", job.get(), Json::object(), dictionary_preparer());
    const CleanupPromptBlocks blocks = named.blocks();

    const std::string with_profile =
        compose_cleanup_prompt(Json{{"app_context_read_text", true}}, job.get(), &profile, kRules,
                               prepared, std::nullopt, blocks);
    const std::string styles_off =
        compose_cleanup_prompt(Json{{"app_styles_enabled", false}}, job.get(), nullptr, {},
                               prepared, std::nullopt, blocks);

    CHECK(with_profile == join({fv::text::compose_profile_prompt(profile, kRules), "DICTIONARY",
                                "GUARD", "CONTEXT Dear Sam,"}));
    CHECK(styles_off == join({fv::text::resolve_transcript_cleanup_prompt(Json::object()),
                              "DICTIONARY", "GUARD"}));
    CHECK(named.styles.empty());
}

TEST_CASE("a failing block is left out", "[test_personalize_foundation_pipeline]") {
    // from tests/test_personalize_foundation_pipeline.py::test_a_failing_block_is_left_out
    NamedBlocks named;
    CleanupPromptBlocks blocks = named.blocks();
    blocks.dictionary_block = [](const Json&) -> std::string {
        throw std::runtime_error("division by zero");
    };
    const PreparedText prepared =
        prepare_text("hi", nullptr, Json::object(), dictionary_preparer());

    const std::string prompt = compose_cleanup_prompt(Json::object(), nullptr, nullptr, {},
                                                      prepared, std::nullopt, blocks);

    CHECK(prompt == join({fv::text::resolve_transcript_cleanup_prompt(Json::object()),
                          "STYLE casual", "GUARD"}));
}

// --- dictionary and snippets ----------------------------------------------------------------

TEST_CASE("dictionary applies to every job and snippets only to dictation",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_dictionary_applies_to_every_job_and_snippets_only_to_dictation
    StandInPreparer preparer;
    const auto command = shared(job_in(std::nullopt, std::nullopt, JobMode::Command, false));
    const auto transform = shared(job_in(std::nullopt, std::nullopt, JobMode::Transform, false));
    for (const DictationJob* job :
         {static_cast<const DictationJob*>(nullptr), command.get(), transform.get()}) {
        const PreparedText prepared = prepare_text("acme rocks", job, Json::object(), preparer);
        CHECK(prepared.text == "Acme rocks");
        CHECK(prepared.cleanup_input == "Acme rocks");
    }
    CHECK(preparer.planned.empty());

    const auto dictation = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    const PreparedText off =
        prepare_text("acme rocks", dictation.get(), Json{{"snippets_enabled", false}}, preparer);
    CHECK(off.cleanup_input == "Acme rocks");
    const PreparedText on = prepare_text("acme rocks", dictation.get(), Json::object(), preparer);
    CHECK(on.text == "Acme rocks");
    CHECK(on.cleanup_input == "[[S1]]");
    CHECK(preparer.planned == std::vector<std::string>{"Acme rocks"});
}

TEST_CASE("a lost placeholder falls back to the uncleaned expansion",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_a_lost_placeholder_falls_back_to_the_uncleaned_expansion
    // The stand-in expansion of the Python test, as a preparer.
    class Expanding final : public TextPreparer {
    public:
        Expansion expand(std::string_view text, const SnippetPlan&) const override {
            std::string out(text);
            const auto at = out.find("[[S1]]");
            if (at == std::string::npos) {
                return Expansion{out, "", false};
            }
            out.replace(at, 6, "Best, Dana");
            return Expansion{out, "<p>Best, Dana</p>", true};
        }
    } preparer;
    const Snippet snippet{"s", "sig", "Best, Dana", true};
    PreparedText prepared;
    prepared.original = "thanks sig";
    prepared.text = "thanks sig";
    prepared.cleanup_input = "thanks [[S1]]";
    prepared.plan = SnippetPlan{"thanks [[S1]]", std::nullopt, {{"[[S1]]", snippet}}};

    CHECK(finish_text("Thanks, [[S1]]", prepared, preparer) ==
          FinishedText{"Thanks, Best, Dana", "<p>Best, Dana</p>", true, "thanks Best, Dana"});
    CHECK(finish_text("Thanks.", prepared, preparer) ==
          FinishedText{"thanks Best, Dana", "<p>Best, Dana</p>", false, "thanks Best, Dana"});
}

// --- job start and recognition --------------------------------------------------------------

TEST_CASE("begin job asks for focus only when app context is on",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_begin_job_asks_for_focus_only_when_app_context_is_on
    fv::focus::FakeCaptureService service(FocusSnapshot{outlook(), std::nullopt});
    const Json reads{{"app_context_read_text", true}};
    JobOptions hotkey;
    hotkey.source = "hotkey";

    const DictationJob dictation =
        begin_job(JobMode::Dictation, reads, recognition_for(nullptr, reads), hotkey, &service);
    JobOptions command_options = hotkey;
    command_options.selection = as_future("the selected words");
    const DictationJob command =
        begin_job(JobMode::Command, Json::object(), recognition_for(nullptr, Json::object()),
                  command_options, &service);
    const Json off{{"app_context_enabled", false}};
    const DictationJob unaware =
        begin_job(JobMode::Dictation, off, recognition_for(nullptr, off), {}, &service);

    // A dictation replaces the selection, so it reads it with the text near the cursor.
    CHECK(service.requests == std::vector<std::pair<bool, bool>>{{true, true}, {false, true}});
    CHECK(dictation.snapshot() == FocusSnapshot{outlook(), std::nullopt});
    CHECK(command.selection_text() == "the selected words");
    CHECK(unaware.focus == nullptr);
    CHECK_FALSE(unaware.snapshot());
    CHECK(dictation.recognition == RecognitionContext{});
}

TEST_CASE("begin job survives a broken capture service", "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_begin_job_survives_a_broken_capture_service
    BrokenCaptureService service;

    const DictationJob job =
        begin_job(JobMode::Dictation, Json::object(), RecognitionContext{}, {}, &service);

    CHECK(job.focus == nullptr);
    CHECK(job.mode == JobMode::Dictation);
}

TEST_CASE("recognition carries language and phrases when steering is on",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_recognition_carries_language_and_phrases_when_steering_is_on
    // Python patches job_language to "de" and the phrases; here the settings give them: French
    // (a Parakeet language) and a dictionary whose starred term comes first. The failing half
    // has no input in C++: the settings resolvers behind recognition_for never throw.
    Json settings{{"selected_model", "parakeet"},
                  {"dictation_languages", Json::array({"en", "fr"})},
                  {"dictation_active_language", "fr"},
                  {"dictation_dictionary",
                   Json::array({Json{{"id", "b"}, {"term", "Kubernetes"}},
                                Json{{"id", "a"}, {"term", "Acme"}, {"starred", true}}})}};

    CHECK(recognition_for(nullptr, settings) == RecognitionContext{"fr", {"Acme", "Kubernetes"}});
    settings["dictionary_steer_recognition"] = false;
    const auto job = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    CHECK(recognition_for(job.get(), settings) == RecognitionContext{"fr", {}});
    CHECK(RecognitionContext{} == RecognitionContext{"", {}});
}

// --- futures never raise --------------------------------------------------------------------

TEST_CASE("job futures never raise or wait at timeout zero",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_job_futures_never_raise_or_wait_at_timeout_zero
    // A wrongly typed future has no C++ counterpart: both futures are typed.
    std::promise<SelectionValue> never;
    DictationJob pending;
    pending.focus = std::make_shared<fv::focus::FocusRequest>();
    pending.selection = never.get_future().share();
    CHECK_FALSE(pending.snapshot());
    CHECK(pending.selection_text().empty());
    CHECK_FALSE(pending.snapshot(std::chrono::milliseconds(10)));

    std::promise<SelectionValue> failing;
    failing.set_exception(std::make_exception_ptr(std::runtime_error("copy")));
    DictationJob broken;
    broken.selection = failing.get_future().share();
    CHECK(broken.selection_text(std::chrono::seconds(1)).empty());
    never.set_value(std::string("late"));
}

// --- delivery -------------------------------------------------------------------------------

TEST_CASE("text joins the caret only for dictation with a known caret",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_text_joins_the_caret_only_for_dictation_with_a_known_caret
    const TextJoin lower = [](std::string_view text, const std::optional<TextContext>&) {
        std::string out = " ";
        for (const char c : text) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const auto dictation = shared(job_in(outlook(), dear_sam()));
    const auto command = shared(job_in(outlook(), dear_sam(), JobMode::Command));
    TextContext unknown;
    unknown.before = "Dear Sam,";
    const auto unknown_caret = shared(job_in(outlook(), unknown));

    CHECK(text_for_paste("And more.", dictation.get(), lower) == " and more.");
    CHECK(text_for_paste("And more.", command.get(), lower) == "And more.");
    CHECK(text_for_paste("And more.", unknown_caret.get(), lower) == "And more.");
    const TextJoin failing = [](std::string_view,
                                const std::optional<TextContext>&) -> std::string {
        throw std::runtime_error("division by zero");
    };
    CHECK(text_for_paste("And more.", dictation.get(), failing) == "And more.");
}

TEST_CASE("a rewrite pastes only into the app it provably started in",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_a_rewrite_pastes_only_into_the_app_it_provably_started_in
    const auto job = shared(job_in(outlook(), std::nullopt, JobMode::Command));

    fv::focus::FakeCaptureService same(FocusSnapshot{}, outlook());
    CHECK(paste_target(job.get(), &same) == PasteTarget::Same);
    CHECK(paste_target_ok(job.get(), &same));
    fv::focus::FakeCaptureService slack(FocusSnapshot{}, app("slack.exe", "Slack", 11));
    CHECK(paste_target(job.get(), &slack) == PasteTarget::Changed);
    CHECK_FALSE(paste_target_ok(job.get(), &slack));
    // Not knowing either side never pastes blind.
    fv::focus::FakeCaptureService nobody(FocusSnapshot{}, std::optional<AppIdentity>());
    CHECK(paste_target(job.get(), &nobody) == PasteTarget::Unknown);
    CHECK_FALSE(paste_target_ok(job.get(), &nobody));
    BrokenCaptureService broken;
    CHECK(paste_target(job.get(), &broken) == PasteTarget::Unknown);
    CHECK(paste_target(nullptr, &same) == PasteTarget::Unknown);
    const auto uncaptured = shared(job_in(std::nullopt, std::nullopt, JobMode::Command, false));
    CHECK(paste_target(uncaptured.get(), &same) == PasteTarget::Unknown);
    // With app awareness off the job keeps only the target.
    DictationJob transform = job_in(std::nullopt, std::nullopt, JobMode::Transform, false);
    transform.target = fv::focus::FocusRequest::resolved(FocusSnapshot{outlook(), std::nullopt});
    CHECK(paste_target(&transform, &same) == PasteTarget::Same);
}

TEST_CASE("after paste schedules learning for dictation only",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_after_paste_schedules_learning_for_dictation_only
    RecordingPreparer preparer;
    const auto dictation = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    const auto command = shared(job_in(std::nullopt, std::nullopt, JobMode::Command, false));

    after_paste(dictation.get(), "Hello", std::nullopt, preparer);
    after_paste(dictation.get(), "Hello.", std::string_view("hello"), preparer);
    after_paste(command.get(), "Rewritten", std::nullopt, preparer);
    after_paste(nullptr, "Upload", std::nullopt, preparer);
    CHECK(preparer.learned() ==
          std::vector<RecordingPreparer::Learning>{{JobMode::Dictation, "Hello", std::nullopt},
                                                   {JobMode::Dictation, "Hello.", "hello"}});

    preparer.fail = true;
    after_paste(dictation.get(), "Hello", std::nullopt, preparer);
}

// --- history and stats ----------------------------------------------------------------------

TEST_CASE("history fields name the kind and leave unknowns out",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_history_fields_name_the_kind_and_leave_unknowns_out
    CHECK(history_fields(nullptr, std::nullopt, true, Json::object()) ==
          Json{{"entry_kind", "dictation"}});
    CHECK(history_fields(nullptr, std::nullopt, false, Json::object()) ==
          Json{{"entry_kind", "file"}});
    const auto transform = shared(job_in(std::nullopt, std::nullopt, JobMode::Transform, false));
    CHECK(history_fields(transform.get(), std::nullopt, false, Json::object()) ==
          Json{{"entry_kind", "transform"}});

    DictationJob command = job_in(outlook(), dear_sam(), JobMode::Command);
    command.recognition = RecognitionContext{"fr", {}};
    CHECK(history_fields(&command, std::string_view("medium"), true, Json::object()) ==
          Json{{"entry_kind", "command"},
               {"app_id", "outlook.exe"},
               {"app_name", "Outlook"},
               {"app_category", "email"},
               {"cleanup_level", "medium"},
               {"language", "fr"}});
}

TEST_CASE("history fields never hold window titles or text",
          "[test_personalize_foundation_pipeline]") {
    // from
    // tests/test_personalize_foundation_pipeline.py::test_history_fields_never_hold_window_titles_or_text
    const auto job = shared(job_in(app("chrome.exe", "Chrome", 0, {}, "Gmail"), dear_sam()));

    const Json fields = history_fields(job.get(), std::nullopt, true, Json::object());

    for (const auto& [key, value] : fields.items()) {
        CHECK(value != "Gmail");
        CHECK(value != "Dear Sam,");
    }
}

TEST_CASE("record stats swallows failures", "[test_personalize_foundation_pipeline]") {
    // from tests/test_personalize_foundation_pipeline.py::test_record_stats_swallows_failures
    std::vector<Json> calls;

    record_stats(Json{{"entry_kind", "dictation"}}, 1, [&](const Json& fields, int) {
        calls.push_back(fields);
        throw std::runtime_error("division by zero");
    });

    CHECK(calls == std::vector<Json>{Json{{"entry_kind", "dictation"}}});
}

// --- what Stats count ------------------------------------------------------------------------

TEST_CASE("a prepared transcript says what snippets and the dictionary did",
          "[test_stats_features]") {
    // from
    // tests/test_stats_features.py::test_a_prepared_transcript_says_what_snippets_and_the_dictionary_did
    const WordCounter words{
        [](std::string_view text) { return fv::dictation_stats::count_words(text); },
        [](std::string_view before, std::string_view after) {
            const auto edited = fv::dictation_stats::words_edited(before, after);
            return edited > 0 ? static_cast<std::size_t>(edited) : std::size_t{0};
        }};
    const Snippet sign_off{"1", "sign off", "Kind regards, Dana Fiori, Acme"};
    PreparedText inline_snippet;
    inline_snippet.original = "tell acme sign off";
    inline_snippet.text = "tell Acme sign off";
    inline_snippet.cleanup_input = "tell Acme [[S1]]";
    inline_snippet.plan = SnippetPlan{"tell Acme [[S1]]", std::nullopt, {{"[[S1]]", sign_off}}};
    CHECK(stats_counts(inline_snippet, words) ==
          Json{{"snippets", 1}, {"snippet_words", 3}, {"corrections", 1}});

    PreparedText whole;
    whole.original = "sign off";
    whole.text = "sign off";
    whole.cleanup_input = "sign off";
    whole.plan = SnippetPlan{"sign off", sign_off, {}};
    whole.skip_cleanup = true;
    CHECK(stats_counts(whole, words) == Json{{"snippets", 1}, {"snippet_words", 3}});

    PreparedText plain;
    plain.original = plain.text = plain.cleanup_input = "just words";
    plain.plan = SnippetPlan{"just words", std::nullopt, {}};
    CHECK(stats_counts(plain, words) == Json::object());
}

namespace {

const std::string kIntroText = [] {
    std::string text;
    for (int i = 0; i < 120; ++i) {
        text += (i == 0 ? "word" : " word") + std::to_string(i);
    }
    return text;
}();

// The `dictation` fixture: a recording's transcript `raw` of `seconds`, cleaned by a stand-in
// that capitalizes and ends it with a period.
struct SpokenRig {
    explicit SpokenRig(bool cleanup) : rig(options(cleanup)) { rig.runtime->set_cleaner(&cleaner); }
    ~SpokenRig() { rig.runtime->set_cleaner(nullptr); }

    static RigOptions options(bool cleanup) {
        RigOptions options;
        options.settings["dictation_snippets"] =
            Json::array({snippet_json("intro", "my intro", kIntroText)});
        options.settings["transcript_cleanup_enabled"] = cleanup;
        return options;
    }

    fv::HistoryEntry dictate(const std::string& raw, double seconds) {
        say(*rig.engine, raw);
        REQUIRE(rig.runtime->claim_job(
            shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false))));
        PendingAudio pending;
        pending.audio_path = rig.wav("recording.wav");
        pending.audio_duration = seconds;
        rig.runtime->set_pending(pending);
        rig.runtime->transcribe_audio_file(*pending.audio_path);
        rig.run_workers();
        const auto entries = rig.entries();
        REQUIRE(!entries.empty());
        return entries.front();
    }
    std::vector<std::int64_t> spoken_words() {
        std::vector<std::int64_t> words;
        for (const auto& row : rig.stats->rows()) {
            if (row.entry_kind != "marker") {
                words.push_back(row.spoken_words);
            }
        }
        return words;
    }
    double average_wpm() {
        return fv::dictation_stats::load_summary(*rig.stats).period("all").average_wpm;
    }

    FakeCleaner cleaner{[](const std::string& text) {
        return fv::py::upper(fv::py::slice(text, 0, 1)) + fv::py::slice(text, 1, std::nullopt) +
               ".";
    }};
    RuntimeRig rig;
};

} // namespace

TEST_CASE("a whole snippet counts its trigger", "[test_personalize_fix_f5_stats_spoken]") {
    // from tests/test_personalize_fix_f5_stats_spoken.py::test_a_whole_snippet_counts_its_trigger
    const bool cleanup = GENERATE(true, false);
    SpokenRig spoken(cleanup);

    const auto entry = spoken.dictate("My intro.", 1.2);

    CHECK(entry.text == kIntroText);
    CHECK(spoken.spoken_words() == std::vector<std::int64_t>{2});
    CHECK(spoken.average_wpm() == 100.0);
}

TEST_CASE("an inline snippet counts the words around its trigger",
          "[test_personalize_fix_f5_stats_spoken]") {
    // from
    // tests/test_personalize_fix_f5_stats_spoken.py::test_an_inline_snippet_counts_the_words_around_its_trigger
    const bool cleanup = GENERATE(true, false);
    SpokenRig spoken(cleanup);

    const auto entry = spoken.dictate("here is my intro thanks", 2.0);

    CHECK(entry.text.find(kIntroText) != std::string::npos);
    CHECK(spoken.spoken_words() == std::vector<std::int64_t>{5});
    CHECK(spoken.average_wpm() == 150.0);
}

TEST_CASE("raw text for undo still holds the expansion", "[test_personalize_fix_f5_stats_spoken]") {
    // from
    // tests/test_personalize_fix_f5_stats_spoken.py::test_raw_text_for_undo_still_holds_the_expansion
    SpokenRig spoken(true);

    const auto entry = spoken.dictate("here is my intro thanks", 2.0);

    CHECK(entry.raw_text == "here is " + kIntroText + " thanks");
}

TEST_CASE("a whole formatted snippet gets its leading space",
          "[test_personalize_fix_f5_rich_join]") {
    // from
    // tests/test_personalize_fix_f5_rich_join.py::test_a_whole_formatted_snippet_gets_its_leading_space
    // What a rich-text editor shows after the paste (_rich_paste) needs a QTextEdit; this core
    // test checks the two flavours the clipboard is given.
    RuntimeRig rig;
    rig.settings.update(
        Json{{"dictation_snippets",
              Json::array({snippet_json("cal", "my calendar link",
                                        "**[my calendar](https://cal.example/sam)**", true),
                           snippet_json("sig", "my sign off", "**Dana Lee**", true)})}});
    say(*rig.engine, "My sign off.");
    REQUIRE(rig.runtime->claim_job(shared(job_in(outlook(), caret("Regards,", {}, true)))));

    rig.runtime->transcribe_audio_file(rig.wav("clip.wav"));
    rig.run_workers();

    const auto staged = rig.clipboard.inner.stages;
    REQUIRE(staged.size() == 1);
    CHECK(staged[0].text == " Dana Lee");
    REQUIRE(staged[0].html);
    CHECK(staged[0].html->find("<b>Dana Lee</b>") != std::string::npos);
}
