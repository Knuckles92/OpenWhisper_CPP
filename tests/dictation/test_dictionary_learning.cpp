// Learning dictionary words from corrections after a paste: tests/test_personalize_s4_learning.py
// (TestLearning), tests/test_dictionary_cleanup_revert.py (TestLearning) and
// tests/test_personalize_fix2_g1_paste_target_tab.py. The word diff itself is fv_text's and is
// tested there. OpenWhisper's `learning` fixture runs the delayed steps at once and records their
// delays; the Steps executor here does the same.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cleanup_support.h"
#include "dictation/dictionary_learning.h"
#include "dictation/pipeline.h"
#include "dictation/text_preparer.h"
#include "dictation_support.h"
#include "focus/capture_service.h"
#include "settings/testing/memory_store.h"
#include "text/dictionary.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using namespace std::chrono_literals;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::TextContext;

namespace {

const std::string kPasted = "Let's meet with Sonia about the launch.";
const std::string kKsenia = "Let's meet with Ksenia about the launch.";
// tests/test_dictionary_cleanup_revert.py: what was said, before AI cleanup changed it.
const std::string kSpoken = "lets meet with Ksenia about the launch";

AppIdentity notes() {
    return app("notes.exe", "Notes", 42);
}

// _run_later monkeypatched: each step runs at once, and its delay is recorded (0 for a post).
// Affinity: thread-safe.
class Steps final : public fv::Executor {
public:
    void post(fv::Task task) override {
        record(0ms);
        task();
    }
    fv::TimerHandle post_after(std::chrono::milliseconds delay, fv::Task task) override {
        record(delay);
        task();
        return fv::TimerHandle();
    }
    bool is_current() const override { return true; }
    std::vector<std::chrono::milliseconds> later() const {
        std::lock_guard lock(mutex_);
        return later_;
    }

private:
    void record(std::chrono::milliseconds delay) {
        std::lock_guard lock(mutex_);
        later_.push_back(delay);
    }
    mutable std::mutex mutex_;
    std::vector<std::chrono::milliseconds> later_;
};

// FakeService: the field read again holds `field` (nullopt: it could not be read).
// Affinity: thread-safe.
class FieldService final : public fv::focus::CaptureService {
public:
    explicit FieldService(std::optional<std::string> field) : field_(std::move(field)) {}

    std::shared_ptr<fv::focus::FocusRequest> request(bool, bool) override {
        return fv::focus::FocusRequest::resolved(fv::focus::FocusSnapshot{});
    }
    std::optional<AppIdentity> current_identity() override { return std::nullopt; }
    void reread(const std::optional<AppIdentity>& identity,
                std::function<void(std::optional<TextContext>)> callback) override {
        {
            std::lock_guard lock(mutex_);
            rereads_.push_back(identity);
        }
        if (!field_) {
            callback(std::nullopt);
            return;
        }
        TextContext context;
        context.before = *field_;
        context.caret_known = true;
        context.source = "fake";
        callback(context);
    }
    void shutdown() override {}
    std::vector<std::optional<AppIdentity>> rereads() const {
        std::lock_guard lock(mutex_);
        return rereads_;
    }

private:
    std::optional<std::string> field_;
    mutable std::mutex mutex_;
    std::vector<std::optional<AppIdentity>> rereads_;
};

// The `learning` fixture: text reading on, a learner over the service, and what it learned.
struct Learning {
    explicit Learning(std::optional<std::string> field, fv::Executor* ui_executor = nullptr)
        : service(std::move(field)),
          learner(settings, ui_executor != nullptr ? *ui_executor : steps, steps, &service),
          link(learner.term_learned.connect(steps, [this](std::string term, bool) {
              std::lock_guard lock(mutex);
              learned.push_back(std::move(term));
          })) {
        settings.update(Json{{"app_context_read_text", true}});
    }
    std::vector<fv::text::DictionaryTerm> saved_terms() const {
        return fv::text::load_dictionary(*settings.snapshot());
    }
    std::vector<std::string> learned_terms() {
        std::lock_guard lock(mutex);
        return learned;
    }

    fv::MemorySettingsStore settings;
    Steps steps;
    FieldService service;
    DictionaryLearner learner;
    std::mutex mutex;
    std::vector<std::string> learned;
    fv::Subscription link;
};

// s4_learning's job(): a dictation into Notes whose caret text was read.
DictationJob job(std::optional<AppIdentity> identity = notes(), bool caret_known = true,
                 JobMode mode = JobMode::Dictation, bool text = true) {
    std::optional<TextContext> context;
    if (text) {
        context = TextContext{};
        context->before = "Dear team, ";
        context->caret_known = caret_known;
    }
    return job_in(std::move(identity), context, mode);
}

} // namespace

// ---- tests/test_personalize_s4_learning.py ----

TEST_CASE("second sighting adds a learned word and notifies", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_second_sighting_adds_a_learned_word_and_notifies
    Learning learning("Dear team, Let's meet with Ksenia about the launch.");

    learning.learner.schedule_learning(job(), kPasted);
    CHECK(learning.saved_terms().empty());
    CHECK(learning.learned_terms().empty());
    learning.learner.schedule_learning(job(), kPasted);

    CHECK(learning.service.rereads() == std::vector<std::optional<AppIdentity>>{notes(), notes()});
    const auto later = learning.steps.later();
    CHECK(std::find(later.begin(), later.end(), LEARN_DELAY) != later.end());
    const auto terms = learning.saved_terms();
    REQUIRE(terms.size() == 1);
    CHECK(terms[0].term == "Ksenia");
    CHECK(terms[0].learned);
    CHECK(terms[0].is_new);
    CHECK(terms[0].heard.empty());
    CHECK(learning.learned_terms() == std::vector<std::string>{"Ksenia"});
}

TEST_CASE("through the pipeline after a paste", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_through_the_pipeline_after_a_paste
    Learning learning(kKsenia);
    const DictionaryTextPreparer preparer(&learning.learner);
    const DictationJob dictation = job();

    after_paste(&dictation, kPasted, std::nullopt, preparer);
    after_paste(&dictation, kPasted, std::nullopt, preparer);
    CHECK(learning.learned_terms() == std::vector<std::string>{"Ksenia"});
    const DictationJob command = job(notes(), true, JobMode::Command);
    after_paste(&command, kPasted, std::nullopt, preparer);
    CHECK(learning.saved_terms().size() == 1);
}

TEST_CASE("a known word is not added again", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_a_known_word_is_not_added_again
    Learning learning(kKsenia);
    learning.settings.mutate([](Json& values) { fv::text::add_term(values, "ksenia"); });

    for (int i = 0; i < 2; ++i) {
        learning.learner.schedule_learning(job(), kPasted);
    }

    CHECK(learning.learned_terms().empty());
    CHECK(learning.saved_terms().size() == 1);
}

TEST_CASE("off unless learning and text reading are on", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_off_unless_learning_and_text_reading_are_on
    const Json off =
        GENERATE(Json{{"dictionary_learn_enabled", false}}, Json{{"app_context_read_text", false}});
    Learning learning(kKsenia);
    learning.settings.update(off);

    for (int i = 0; i < 2; ++i) {
        learning.learner.schedule_learning(job(), kPasted);
    }

    CHECK(learning.service.rereads().empty());
    CHECK(learning.learned_terms().empty());
    CHECK(learning.saved_terms().empty());
}

TEST_CASE("needs the caret text of another app", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_needs_the_caret_text_of_another_app
    const int which = GENERATE(0, 1, 2, 3, 4, 5);
    Learning learning(kKsenia);
    const DictionaryTextPreparer preparer(&learning.learner);
    AppIdentity self = app("fiorivoce", "FioriVoce");
    self.is_self = true;

    switch (which) {
    case 0: // no job: after_paste never reaches the learner
        after_paste(nullptr, kPasted, std::nullopt, preparer);
        break;
    case 1:
        learning.learner.schedule_learning(job(std::nullopt), kPasted);
        break;
    case 2:
        learning.learner.schedule_learning(job(notes(), false), kPasted);
        break;
    case 3:
        learning.learner.schedule_learning(job(notes(), true, JobMode::Dictation, false), kPasted);
        break;
    case 4:
        learning.learner.schedule_learning(job(self), kPasted);
        break;
    default:
        learning.learner.schedule_learning(
            job_in(std::nullopt, std::nullopt, JobMode::Dictation, false), kPasted);
        break;
    }

    CHECK(learning.steps.later().empty());
    CHECK(learning.service.rereads().empty());
}

TEST_CASE("a failed reread learns nothing", "[test_personalize_s4_learning]") {
    // from tests/test_personalize_s4_learning.py::TestLearning::test_a_failed_reread_learns_nothing
    Learning learning(std::nullopt);

    for (int i = 0; i < 3; ++i) {
        learning.learner.schedule_learning(job(), kPasted);
    }

    CHECK(learning.learned_terms().empty());
}

TEST_CASE("an unpending job never waits", "[test_personalize_s4_learning]") {
    // from tests/test_personalize_s4_learning.py::TestLearning::test_an_unpending_job_never_waits
    Learning learning(kKsenia);
    DictationJob pending;
    pending.focus = std::make_shared<fv::focus::FocusRequest>();

    learning.learner.schedule_learning(pending, kPasted);

    CHECK(learning.steps.later().empty());
}

TEST_CASE("scheduling returns before the delay", "[test_personalize_s4_learning]") {
    // from
    // tests/test_personalize_s4_learning.py::TestLearning::test_scheduling_returns_before_the_delay
    InlineExecutor ui;
    Learning learning(kKsenia, &ui);

    learning.learner.schedule_learning(job(), kPasted);

    CHECK(ui.pending_delays() == std::vector<std::chrono::milliseconds>{LEARN_DELAY});
    CHECK(learning.service.rereads().empty());
    CHECK(ui.fire_timers() == 1);
    CHECK(learning.service.rereads().size() == 1);
}

TEST_CASE("a learner destroyed before its delay ends reads nothing", "[dictation][learning]") {
    InlineExecutor ui;
    FieldService service(kKsenia);
    fv::MemorySettingsStore settings;
    settings.update(Json{{"app_context_read_text", true}});
    Steps persistence;
    auto learner = std::make_unique<DictionaryLearner>(settings, ui, persistence, &service);
    learner->schedule_learning(job(), kPasted);

    learner.reset();

    CHECK(ui.fire_timers() == 1);
    CHECK(service.rereads().empty());
}

// ---- tests/test_dictionary_cleanup_revert.py ----

TEST_CASE("one revert learns a protected word", "[test_dictionary_cleanup_revert]") {
    // from
    // tests/test_dictionary_cleanup_revert.py::TestLearning::test_one_revert_learns_a_protected_word
    Learning learning(kKsenia);

    learning.learner.schedule_learning(job(), kPasted, kSpoken);

    const auto terms = learning.saved_terms();
    REQUIRE(terms.size() == 1);
    CHECK(terms[0].term == "Ksenia");
    CHECK(terms[0].learned);
    CHECK(terms[0].is_new);
    CHECK(terms[0].is_protected);
    CHECK(learning.learned_terms() == std::vector<std::string>{"Ksenia"});
    CHECK(learning.steps.later() == std::vector<std::chrono::milliseconds>{LEARN_DELAY, 0ms});
}

TEST_CASE("an ordinary correction still needs two sightings", "[test_dictionary_cleanup_revert]") {
    // from
    // tests/test_dictionary_cleanup_revert.py::TestLearning::test_an_ordinary_correction_still_needs_two_sightings
    Learning learning(kKsenia);

    learning.learner.schedule_learning(job(), kPasted, "lets meet with Sonia about the launch");
    CHECK(learning.saved_terms().empty());
    learning.learner.schedule_learning(job(), kPasted);

    const auto terms = learning.saved_terms();
    REQUIRE(terms.size() == 1);
    CHECK_FALSE(terms[0].is_protected);
    CHECK(learning.learned_terms() == std::vector<std::string>{"Ksenia"});
}

TEST_CASE("a known word becomes protected once", "[test_dictionary_cleanup_revert]") {
    // from
    // tests/test_dictionary_cleanup_revert.py::TestLearning::test_a_known_word_becomes_protected_once
    Learning learning(kKsenia);
    learning.settings.mutate([](Json& values) { fv::text::add_term(values, "Ksenia"); });
    std::vector<bool> marks;
    const auto link = learning.learner.term_learned.connect(
        learning.steps,
        [&marks](std::string, bool is_protected) { marks.push_back(is_protected); });

    learning.learner.schedule_learning(job(), kPasted, kSpoken);
    learning.learner.schedule_learning(job(), kPasted, kSpoken);

    const auto terms = learning.saved_terms();
    REQUIRE(terms.size() == 1);
    // Still the user's own word.
    CHECK(terms[0].is_protected);
    CHECK_FALSE(terms[0].learned);
    CHECK(learning.learned_terms() == std::vector<std::string>{"Ksenia"});
    CHECK(marks == std::vector<bool>{true});
}

TEST_CASE("off when learning is off", "[test_dictionary_cleanup_revert]") {
    // from tests/test_dictionary_cleanup_revert.py::TestLearning::test_off_when_learning_is_off
    Learning learning(kKsenia);
    learning.settings.update(Json{{"dictionary_learn_enabled", false}});

    learning.learner.schedule_learning(job(), kPasted, kSpoken);

    CHECK(learning.learned_terms().empty());
    CHECK(learning.saved_terms().empty());
}

// ---- tests/test_personalize_fix2_g1_paste_target_tab.py ----

TEST_CASE("the paste check and the learning reread agree",
          "[test_personalize_fix2_g1_paste_target_tab]") {
    // from
    // tests/test_personalize_fix2_g1_paste_target_tab.py::test_the_paste_check_and_the_learning_reread_agree
    const auto chrome = [](std::string hint) {
        AppIdentity identity = app("chrome.exe", "Google Chrome", 4242, "0x1a2b", std::move(hint));
        identity.platform = "windows";
        return identity;
    };
    const AppIdentity gmail = chrome("Gmail");

    CHECK_FALSE(fv::focus::same_target(chrome("WhatsApp"), gmail));
    CHECK(fv::focus::same_target(chrome("Gmail"), gmail));
    CHECK_FALSE(fv::focus::same_target(std::nullopt, gmail));
}
