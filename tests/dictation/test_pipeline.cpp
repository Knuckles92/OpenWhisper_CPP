#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/catalog.h"
#include "focus/testing/fake_platform.h"
#include "golden/golden.h"

using namespace fv::dictation;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FakeCaptureService;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;
using std::chrono::milliseconds;

namespace {

std::shared_ptr<const DictationJob> job_with(JobMode mode, std::optional<FocusSnapshot> focus,
                                             std::optional<FocusSnapshot> target = {}) {
    DictationJob job;
    job.mode = mode;
    if (focus) {
        job.focus = fv::focus::FocusRequest::resolved(*focus);
    }
    if (target) {
        job.target = fv::focus::FocusRequest::resolved(*target);
    }
    return std::make_shared<const DictationJob>(std::move(job));
}

TextContext caret(std::string before, std::string after = {}) {
    TextContext context;
    context.before = std::move(before);
    context.after = std::move(after);
    context.caret_known = true;
    context.selection_known = true;
    return context;
}

// tests/test_personalize_foundation_runtime.py OUTLOOK.
AppIdentity outlook() {
    return AppIdentity{"OUTLOOK.EXE", "Outlook", std::nullopt, "", "", false, "windows"};
}

// tests/test_personalize_fix2_g1_paste_target_tab.py _chrome(title).
AppIdentity chrome(std::string_view title) {
    return AppIdentity{"chrome.exe", fv::focus::catalog::app_name("chrome.exe"),          4242,
                       "0x1a2b",     fv::focus::catalog::title_hint("chrome.exe", title), false,
                       "windows"};
}

// Installs a capture service for one test and puts the previous one back.
struct ScopedService {
    explicit ScopedService(std::shared_ptr<fv::focus::CaptureService> service)
        : previous(fv::focus::get_service()) {
        fv::focus::set_service(std::move(service));
    }
    ~ScopedService() { fv::focus::set_service(previous); }
    std::shared_ptr<fv::focus::CaptureService> previous;
};

} // namespace

// ---- tests/test_dictation_runtime_exception_handling.py ----

// from
// tests/test_dictation_runtime_exception_handling.py::test_pipeline_resolved_is_none_when_not_ready_cancelled_or_failed
TEST_CASE("test_pipeline_resolved_is_none_when_not_ready_cancelled_or_failed",
          "[test_dictation_runtime_exception_handling]") {
    std::promise<SelectionValue> pending;
    const SelectionFuture not_ready = pending.get_future().share();
    CHECK_FALSE(resolved(not_ready, milliseconds(10)).has_value());
    CHECK_FALSE(resolved(not_ready, milliseconds(0)).has_value());

    SelectionFuture cancelled;
    {
        std::promise<SelectionValue> abandoned;
        cancelled = abandoned.get_future().share();
    }
    CHECK_FALSE(resolved(cancelled, milliseconds(10)).has_value());

    std::promise<SelectionValue> failing;
    failing.set_exception(std::make_exception_ptr(std::runtime_error("capture broke")));
    CHECK_FALSE(resolved(failing.get_future().share(), milliseconds(10)).has_value());

    const auto done = resolved(as_future("value"), milliseconds(0));
    REQUIRE(done.has_value());
    CHECK(std::get<std::string>(*done) == "value");

    CHECK_FALSE(resolved(SelectionFuture(), milliseconds(0)).has_value());
}

TEST_CASE("selection_text answers only a ready string", "[dictation][job]") {
    DictationJob job;
    CHECK(job.selection_text().empty());
    job.selection = as_future("secret");
    CHECK(job.selection_text() == "secret");
    std::promise<SelectionValue> unread;
    unread.set_value(UnreadSelection{"Release the shortcut keys, then try again"});
    job.selection = unread.get_future().share();
    CHECK(job.selection_text().empty());
}

TEST_CASE("a focus capture resolves without waiting or within the timeout", "[dictation][job]") {
    auto request = std::make_shared<fv::focus::FocusRequest>();
    CHECK_FALSE(resolved(request, milliseconds(0)).has_value());
    CHECK_FALSE(resolved(request, milliseconds(10)).has_value());
    request->settle(FocusSnapshot{outlook(), std::nullopt});
    CHECK(resolved(request, milliseconds(0)) == FocusSnapshot{outlook(), std::nullopt});
    CHECK_FALSE(resolved(std::shared_ptr<fv::focus::FocusRequest>(), milliseconds(10)));
}

// ---- tests/test_personalize_fix2_g1_paste_target_tab.py ----

// from tests/test_personalize_fix2_g1_paste_target_tab.py::test_paste_check_compares_the_site_too
TEST_CASE("test_paste_check_compares_the_site_too", "[test_personalize_fix2_g1_paste_target_tab]") {
    const AppIdentity gmail = chrome("Inbox (12) - someone@gmail.com - Gmail - Google Chrome");
    const AppIdentity compose = chrome("Compose Mail - someone@gmail.com - Gmail - Google Chrome");
    const AppIdentity whatsapp = chrome("(3) WhatsApp - Google Chrome");
    const AppIdentity unknown_site = chrome("Some recipe blog - Google Chrome");

    const bool aware = GENERATE(false, true);
    const auto [now, expected] = GENERATE_COPY(table<AppIdentity, PasteTarget>({
        {whatsapp, PasteTarget::Changed},
        {unknown_site, PasteTarget::Changed},
        {compose, PasteTarget::Same},
        {gmail, PasteTarget::Same},
    }));
    // The test's FakePlatform answers with Gmail at the start; the capture service's fake does
    // the same here.
    auto service = std::make_shared<FakeCaptureService>(FocusSnapshot{gmail, std::nullopt});
    ScopedService installed(service);
    const DictationJob job = begin_job(JobMode::Transform, Json{{"app_context_enabled", aware}}, {},
                                       {.selection = as_future("secret")});
    service->set_current(now);

    CHECK(paste_target(&job) == expected);
    CHECK(paste_target_ok(&job) == (expected == PasteTarget::Same));
}

TEST_CASE("the paste check knows nothing without a start or a current app", "[dictation]") {
    auto service = std::make_shared<FakeCaptureService>(FocusSnapshot{outlook(), std::nullopt});
    ScopedService installed(service);
    CHECK(paste_target(nullptr) == PasteTarget::Unknown);
    const auto bare = job_with(JobMode::Transform, std::nullopt);
    CHECK(paste_target(bare.get()) == PasteTarget::Unknown);
    const auto job = job_with(JobMode::Transform, FocusSnapshot{outlook(), std::nullopt});
    service->set_current(std::nullopt);
    CHECK(paste_target(job.get()) == PasteTarget::Unknown);
    CHECK_FALSE(paste_target_ok(job.get()));
}

// ---- tests/test_personalize_fix_f5_rich_join.py ----
// Snippet expansion is M4's: these start from the plain text and HTML OpenWhisper's snippets
// produced for the same words (the rich_paste vectors) and check the joins the paste uses. The
// Python tests also paste both flavours into a QTextEdit; the core library has no editor, so the
// joined HTML is compared with the HTML OpenWhisper pasted instead.

namespace {

const Json& rich_case(std::size_t index) {
    return fv::golden::load("services.dictation_pipeline")
        .at("functions")
        .at("rich_paste")
        .at(index);
}

struct Pasted {
    std::string plain;
    std::string html;
};

Pasted deliver(const Json& vector, const std::string& before, const std::string& after) {
    const auto job = job_with(JobMode::Dictation, FocusSnapshot{outlook(), caret(before, after)});
    const auto& result = vector.at("result");
    const std::string text = result.at("finished_text").get<std::string>();
    const std::string html = result.at("finished_html").get<std::string>();
    return {text_for_paste(text, job.get()), html_for_paste(html, text, job.get())};
}

} // namespace

// from
// tests/test_personalize_fix_f5_rich_join.py::test_a_rich_paste_continues_the_sentence_like_the_plain_one
TEST_CASE("test_a_rich_paste_continues_the_sentence_like_the_plain_one",
          "[test_personalize_fix_f5_rich_join]") {
    const Json& vector = rich_case(0);
    REQUIRE(vector.at("args") == Json::array({"Here is my calendar link.", "Hi Sam,", "", true}));
    const Pasted pasted = deliver(vector, "Hi Sam,", "");

    CHECK(pasted.plain == " here is my calendar (https://cal.example/sam).");
    CHECK(pasted.html == vector.at("result").at("html").get<std::string>());
    CHECK(pasted.html.find("<a href=\"https://cal.example/sam\">") != std::string::npos);
    CHECK(pasted.html.starts_with("&nbsp;here is "));
}

// from
// tests/test_personalize_fix_f5_rich_join.py::test_a_rich_paste_drops_the_period_before_more_of_the_sentence
TEST_CASE("test_a_rich_paste_drops_the_period_before_more_of_the_sentence",
          "[test_personalize_fix_f5_rich_join]") {
    const Json& vector = rich_case(1);
    REQUIRE(vector.at("args") ==
            Json::array({"Here is my calendar link.", "Thanks", " and talk soon", true}));
    const Pasted pasted = deliver(vector, "Thanks", " and talk soon");

    CHECK(pasted.plain == " here is my calendar (https://cal.example/sam)");
    CHECK(pasted.html == vector.at("result").at("html").get<std::string>());
    CHECK_FALSE(pasted.html.ends_with("."));
}

// from tests/test_personalize_fix_f5_rich_join.py::test_html_is_unchanged_without_a_known_caret
TEST_CASE("test_html_is_unchanged_without_a_known_caret", "[test_personalize_fix_f5_rich_join]") {
    // DictationJob(): no focus capture, so no caret.
    const DictationJob job;
    CHECK(text_for_paste("Dana Lee", &job) == "Dana Lee");
    CHECK(html_for_paste("<b>Dana Lee</b>", "Dana Lee", &job) == "<b>Dana Lee</b>");
}

// from tests/test_personalize_fix_f5_rich_join.py::test_html_for_paste_only_joins_live_dictation
TEST_CASE("test_html_for_paste_only_joins_live_dictation", "[test_personalize_fix_f5_rich_join]") {
    auto service =
        std::make_shared<FakeCaptureService>(FocusSnapshot{outlook(), caret("Dear Sam,")});
    ScopedService installed(service);
    const DictationJob dictation = begin_job(JobMode::Dictation, Json::object(), {});
    const DictationJob command = begin_job(JobMode::Command, Json::object(), {});

    CHECK(html_for_paste("<b>Thanks</b>", "Thanks", &dictation) == "&nbsp;<b>thanks</b>");
    CHECK(html_for_paste("<b>Thanks</b>", "Thanks", &command) == "<b>Thanks</b>");
    CHECK(html_for_paste("<b>Thanks</b>", "Thanks", nullptr) == "<b>Thanks</b>");
    CHECK(html_for_paste("", "Thanks", &dictation).empty());

    const HtmlJoin failing = [](std::string_view, std::string_view,
                                const std::optional<TextContext>&) -> std::string {
        throw std::runtime_error("join failed");
    };
    CHECK(html_for_paste("<b>Thanks</b>", "Thanks", &dictation, failing) == "<b>Thanks</b>");
}

TEST_CASE("a failing plain join pastes the text as it is", "[dictation]") {
    const auto job = job_with(JobMode::Dictation, FocusSnapshot{outlook(), caret("Dear Sam,")});
    const TextJoin failing = [](std::string_view,
                                const std::optional<TextContext>&) -> std::string {
        throw std::runtime_error("join failed");
    };
    CHECK(text_for_paste("Thanks.", job.get()) == " thanks.");
    CHECK(text_for_paste("Thanks.", job.get(), failing) == "Thanks.");
}

// ---- begin_job and the text stages ----

TEST_CASE("begin_job never reads for a walkthrough and keeps what it was told", "[dictation]") {
    auto service = std::make_shared<FakeCaptureService>(FocusSnapshot{outlook(), caret("x")});
    ScopedService installed(service);
    const RecognitionContext recognition{"fr", {"Siobhan"}};
    const DictationJob walkthrough =
        begin_job(JobMode::Walkthrough, Json{{"app_context_read_text", true}}, recognition,
                  {.action_id = "a1", .source = "ui", .route = "auto"});
    CHECK(service->requests.empty());
    CHECK_FALSE(walkthrough.focus);
    CHECK_FALSE(walkthrough.target);
    CHECK(walkthrough.recognition == recognition);
    CHECK(walkthrough.action_id == "a1");
    CHECK(walkthrough.source == "ui");
    CHECK(walkthrough.route == "auto");
    CHECK(walkthrough.id.size() == 16);
    CHECK(walkthrough.id.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_FALSE(walkthrough.program_words());

    const DictationJob from_api =
        begin_job(JobMode::Dictation, Json::object(), {}, {.source = "api"});
    CHECK(from_api.program_words());
    CHECK(from_api.id != walkthrough.id);
    DictationJob changed = walkthrough;
    changed.changed_by_program = true;
    CHECK(changed.program_words());
}

TEST_CASE("begin_job goes on without a capture when the service throws", "[dictation]") {
    class Throwing final : public fv::focus::CaptureService {
    public:
        std::shared_ptr<fv::focus::FocusRequest> request(bool, bool) override {
            throw std::runtime_error("no UI Automation");
        }
        std::optional<AppIdentity> current_identity() override { return std::nullopt; }
        void reread(const std::optional<AppIdentity>&,
                    std::function<void(std::optional<TextContext>)> callback) override {
            callback(std::nullopt);
        }
        void shutdown() override {}
    } service;
    const DictationJob job =
        begin_job(JobMode::Command, Json::object(), {}, {.source = "hotkey"}, &service);
    CHECK_FALSE(job.focus);
    CHECK_FALSE(job.target);
    CHECK(job.mode == JobMode::Command);
}

TEST_CASE("the pass-through stages keep the words", "[dictation]") {
    const auto job = job_with(JobMode::Dictation, std::nullopt);
    const PreparedText prepared = prepare_text("Hello there.", job.get(), Json::object());
    CHECK(prepared.original == "Hello there.");
    CHECK(prepared.text == "Hello there.");
    CHECK(prepared.cleanup_input == "Hello there.");
    CHECK(prepared.plan == SnippetPlan{"Hello there.", std::nullopt, {}});
    CHECK_FALSE(prepared.skip_cleanup);

    CHECK(finish_text("Hello there.", prepared) ==
          FinishedText{"Hello there.", "", true, "Hello there."});
    CHECK(finish_text("Hello, there.", prepared) ==
          FinishedText{"Hello, there.", "", true, "Hello there."});
}

TEST_CASE("prepare_text runs the dictionary on every job and snippets on live ones",
          "[dictation]") {
    struct Preparer final : TextPreparer {
        std::string apply_replacements(std::string_view text, const Json&) const override {
            return std::string(text) == "teh cat" ? "the cat" : std::string(text);
        }
        SnippetPlan plan_expansion(std::string_view text, const Json&) const override {
            ++plans;
            return SnippetPlan{
                "[[S1]]", Snippet{"s", std::string(text), "Kind regards", false}, {}};
        }
        Expansion expand(std::string_view, const SnippetPlan& plan) const override {
            return Expansion{plan.whole->text, "", true};
        }
        mutable int plans = 0;
    } preparer;

    const auto dictation = job_with(JobMode::Dictation, std::nullopt);
    const PreparedText live = prepare_text("teh cat", dictation.get(), Json::object(), preparer);
    CHECK(live.original == "teh cat");
    CHECK(live.text == "the cat");
    CHECK(live.cleanup_input == "[[S1]]");
    CHECK(live.skip_cleanup);
    CHECK(finish_text(live.cleanup_input, live, preparer).text == "Kind regards");

    const auto command = job_with(JobMode::Command, std::nullopt);
    const PreparedText rewrite = prepare_text("teh cat", command.get(), Json::object(), preparer);
    CHECK(rewrite.text == "the cat");
    CHECK(rewrite.cleanup_input == "the cat");
    CHECK(preparer.plans == 1);
    const PreparedText off =
        prepare_text("teh cat", dictation.get(), Json{{"snippets_enabled", false}}, preparer);
    CHECK(off.cleanup_input == "the cat");
    CHECK(prepare_text("teh cat", nullptr, Json::object(), preparer).cleanup_input == "the cat");
    CHECK(preparer.plans == 1);
}

TEST_CASE("a failing preparer costs only its own step", "[dictation]") {
    struct Broken final : TextPreparer {
        std::string apply_replacements(std::string_view, const Json&) const override {
            throw std::runtime_error("dictionary unreadable");
        }
        SnippetPlan plan_expansion(std::string_view, const Json&) const override {
            throw std::runtime_error("snippets unreadable");
        }
        void schedule_learning(const DictationJob&, std::string_view,
                               std::optional<std::string_view>) const override {
            throw std::runtime_error("learning unavailable");
        }
    } broken;
    const auto job = job_with(JobMode::Dictation, std::nullopt);
    const PreparedText prepared = prepare_text("as heard", job.get(), Json::object(), broken);
    CHECK(prepared.text == "as heard");
    CHECK(prepared.cleanup_input == "as heard");
    CHECK_NOTHROW(after_paste(job.get(), "as heard", std::nullopt, broken));

    // A plan only the snippets library could expand falls back to the words as heard.
    PreparedText planned = prepared;
    planned.plan.placeholders.push_back({"[[S1]]", Snippet{"s", "sig", "Dana", false}});
    planned.cleanup_input = "[[S1]] here";
    CHECK(finish_text("[[S1]] here", planned) == FinishedText{"as heard", "", true, "as heard"});
    // Cleanup's text cannot be expanded either: the uncleaned words, marked not ok.
    CHECK(finish_text("cleaned", planned) == FinishedText{"as heard", "", false, "as heard"});
}

TEST_CASE("after_paste learns only from live dictation", "[dictation]") {
    struct Learner final : TextPreparer {
        void schedule_learning(const DictationJob&, std::string_view pasted,
                               std::optional<std::string_view> before) const override {
            seen.emplace_back(std::string(pasted), before ? std::string(*before) : "<none>");
        }
        mutable std::vector<std::pair<std::string, std::string>> seen;
    } learner;
    const auto dictation = job_with(JobMode::Dictation, std::nullopt);
    const auto command = job_with(JobMode::Command, std::nullopt);
    after_paste(dictation.get(), "Hello.", std::string_view("hello"), learner);
    after_paste(command.get(), "Hi.", std::nullopt, learner);
    after_paste(nullptr, "Hi.", std::nullopt, learner);
    CHECK(learner.seen == std::vector<std::pair<std::string, std::string>>{{"Hello.", "hello"}});
}

TEST_CASE("stats_counts counts snippets beyond their triggers", "[dictation]") {
    // count_words as whitespace words (the replay of the vectors uses Python's own answers).
    const WordCounter words{[](std::string_view text) {
                                std::size_t count = 0;
                                bool in_word = false;
                                for (char c : text) {
                                    const bool space = c == ' ';
                                    count += (!space && !in_word) ? 1 : 0;
                                    in_word = !space;
                                }
                                return count;
                            },
                            [](std::string_view, std::string_view) -> std::size_t {
                                return 0;
                            }};
    PreparedText prepared;
    prepared.plan.placeholders = {
        {"[[S1]]", Snippet{"a", "my sign off", "Kind regards, Dana Lee", false}},
        {"[[S2]]", Snippet{"b", "my long trigger phrase", "Hi", false}}};
    CHECK(stats_counts(prepared, words) == Json{{"snippets", 2}, {"snippet_words", 1}});

    PreparedText whole;
    whole.plan.whole = Snippet{"c", "address", "1 Main Street Springfield", false};
    CHECK(stats_counts(whole, words) == Json{{"snippets", 1}, {"snippet_words", 3}});

    const WordCounter broken{
        [](std::string_view) -> std::size_t { throw std::runtime_error("no words"); },
        [](std::string_view, std::string_view) -> std::size_t {
            return 0;
        }};
    CHECK(stats_counts(prepared, broken) == Json::object());
}
