// Learned rules: the composer's polish and dictation flows, the library and the rail, inside a
// real Settings window over fakes (rule dictation records from audio::FakeBackend through a real
// Recorder and is transcribed by a fake RuleTranscriber).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "cleanup_support.h"
#include "text/dictionary.h"
#include "ui_personalize/rule_activity.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

const Json kCleanupOn{{"transcript_cleanup_enabled", true}};
const Json kSnowball{{"name", "Microphone (Blue Snowball )"}, {"hostapi", "MME"}};
const Json kUsb{{"name", "Microphone (2- USB Audio Device"}, {"hostapi", "MME"}};

using State = fv::CleanupRulesSection::DictationState;

// One block of speech-like samples from the microphone, on a thread of its own, as the device
// thread delivers it.
void speak(CleanupRig& rig, int blocks = 8) {
    std::shared_ptr<fv::audio::testing::FakeStream> stream;
    for (const auto& candidate : rig.microphone->streams()) {
        if (candidate->kind() == fv::audio::testing::FakeStream::Kind::Capture &&
            candidate->started() && !candidate->closed()) {
            stream = candidate;
        }
    }
    REQUIRE(stream != nullptr);
    std::thread device([stream, blocks] {
        std::vector<std::int16_t> samples(1024);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::int16_t>(i % 2 == 0 ? 6000 : -6000);
        }
        for (int block = 0; block < blocks; ++block) {
            stream->deliver(samples);
        }
    });
    device.join();
}

fv::CleanupRulesSection& startDictation(CleanupRig& rig) {
    fv::CleanupRulesSection& rules = rig.rules();
    QTest::mouseClick(rules.micButton(), Qt::LeftButton);
    REQUIRE(rules.dictationState() == State::Recording);
    return rules;
}

QStringList listed(QListWidget* list) {
    QStringList texts;
    for (int i = 0; i < list->count(); ++i) {
        texts << list->item(i)->text();
    }
    return texts;
}

} // namespace

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_dictating_shows_a_live_strip_and_a_red_stop
TEST_CASE("test_dictating_shows_a_live_strip_and_a_red_stop", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& rules = startDictation(rig);
    fv::Button* button = rules.micButton();
    CHECK(button->text() == QStringLiteral("Stop"));
    CHECK(button->property("recording").toBool());
    CHECK(rules.activity()->state() == fv::RuleActivityState::Listening);
    // Levels arrive on the audio thread and reach the trace queued.
    speak(rig);
    fv::LevelTrace* trace = rules.activity()->trace();
    CHECK(pumpUntil([trace] { return trace->peak() > 0.0; }));
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_escape_discards_the_take_and_keeps_settings_open
TEST_CASE("test_escape_discards_the_take_and_keeps_settings_open", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& rules = startDictation(rig);
    std::shared_ptr<fv::Recorder> recorder = rules.recorder();

    QTest::keyClick(&rig.window(), Qt::Key_Escape);

    CHECK(recorder->capture_canceled());
    CHECK(rules.dictationState() == State::Idle);
    CHECK(rig.window().isVisible());
    CHECK(rules.micButton()->text() == QStringLiteral("Dictate"));
    CHECK_FALSE(rules.micButton()->property("recording").toBool());
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_closing_settings_mid_take_leaves_no_stale_stop_button
TEST_CASE("test_closing_settings_mid_take_leaves_no_stale_stop_button",
          "[test_rule_composer][widget]") {
    // Settings is a reused window, so this state would greet the next open.
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& rules = startDictation(rig);

    rig.window().close();

    CHECK(rules.recorder() == nullptr);
    CHECK(rules.dictationState() == State::Idle);
    CHECK(rules.micButton()->text() == QStringLiteral("Dictate"));
    CHECK(rules.activity()->isHidden());
    CHECK(pumpUntil([&rig] { return rig.microphone->open_streams() == 0; }));
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_dictated_words_wait_in_the_input_when_the_review_is_canceled
TEST_CASE("test_dictated_words_wait_in_the_input_when_the_review_is_canceled",
          "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& rules = startDictation(rig);
    speak(rig);
    QTest::mouseClick(rules.micButton(), Qt::LeftButton);

    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }, 10000));
    const fv::RuleReview& review = rig.reviews.back();
    CHECK(review.dictated);
    CHECK(review.original == QStringLiteral("capitalize kubernetes"));
    CHECK(review.rule == QStringLiteral("Always write Kubernetes with a capital K."));
    CHECK(review.notice.isEmpty());
    CHECK(rules.ruleInput()->text() == QStringLiteral("capitalize kubernetes"));
    CHECK(rules.rulesList()->count() == 0);
    CHECK_FALSE(rules.polishing());
    // The take was transcribed from its saved audio, which is gone again.
    CHECK(rig.transcriber->sawAudio);
    CHECK_FALSE(std::filesystem::exists(rules.dictationPath()));
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_dictation_adds_to_words_already_typed
TEST_CASE("test_dictation_adds_to_words_already_typed", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    rig.rules().ruleInput()->setText(QStringLiteral("Always"));
    fv::CleanupRulesSection& rules = startDictation(rig);
    speak(rig);
    QTest::mouseClick(rules.micButton(), Qt::LeftButton);

    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }, 10000));
    CHECK(rig.reviews.back().original == QStringLiteral("Always capitalize kubernetes"));
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_saved_rule_joins_the_library_and_glows
TEST_CASE("test_saved_rule_joins_the_library_and_glows", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    rig.reviewAnswer = QStringLiteral("Always write Kubernetes with a capital K.");
    fv::CleanupRulesSection& rules = rig.rules();
    rules.ruleInput()->setText(QStringLiteral("capitalize kubernetes"));

    QTest::mouseClick(rules.addButton(), Qt::LeftButton);

    REQUIRE(pumpUntil([&rules] { return rules.rulesList()->count() == 1; }));
    CHECK_FALSE(rig.reviews.back().dictated);
    CHECK(rules.ruleInput()->text().isEmpty());
    CHECK(rules.glow()->isVisible());
    CHECK(rig.railValue(dest::cleanup_rules) == QStringLiteral("1 rule"));
    CHECK(rig.saved("transcript_cleanup_rules") ==
          Json::array({"Always write Kubernetes with a capital K."}));
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_typed_rule_notice_says_written_when_polish_fails
TEST_CASE("test_typed_rule_notice_says_written_when_polish_fails", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    rig.polish->error = "cleanup unavailable";
    fv::CleanupRulesSection& rules = rig.rules();
    rules.ruleInput()->setText(QStringLiteral("keep acronyms uppercase"));

    QTest::mouseClick(rules.addButton(), Qt::LeftButton);

    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }));
    CHECK(rig.reviews.back().notice.contains(QStringLiteral("saved as written")));
    CHECK(rig.reviews.back().rule == QStringLiteral("keep acronyms uppercase"));
    rig.window().close();
}

// from
// tests/test_rule_composer.py::TestRuleComposerFlow::test_duplicate_rule_is_a_notice_and_starts_no_work
TEST_CASE("test_duplicate_rule_is_a_notice_and_starts_no_work", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& rules = rig.rules();
    rules.rulesList()->addItem(QStringLiteral("Keep acronyms uppercase."));
    rules.ruleInput()->setText(QStringLiteral("keep acronyms uppercase."));

    QTest::mouseClick(rules.addButton(), Qt::LeftButton);

    QLabel* status = rules.status();
    CHECK_FALSE(status->isHidden());
    CHECK(status->text() == QStringLiteral("That rule already exists."));
    CHECK_FALSE(rules.polishing());
    CHECK(rules.activity()->isHidden());
    // Editing the words clears the notice.
    QTest::keyClick(rules.ruleInput(), Qt::Key_X);
    CHECK(status->isHidden());
    CleanupRig::settle();
    CHECK(rig.polish->asked.empty());
    rig.window().close();
}

// from tests/test_rule_composer.py::TestRuleComposerFlow::test_rail_counts_in_the_singular
TEST_CASE("test_rail_counts_in_the_singular", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    fv::CleanupRulesSection& rules = rig.rules();
    rules.rulesList()->addItem(QStringLiteral("Keep acronyms uppercase."));
    rig.window().refreshRailValues();
    CHECK(rig.railValue(dest::cleanup_rules) == QStringLiteral("1 rule"));
    rules.rulesList()->addItem(QStringLiteral("Spell out numbers under ten."));
    rig.window().refreshRailValues();
    CHECK(rig.railValue(dest::cleanup_rules) == QStringLiteral("2 rules"));
    CHECK(std::regex_match(rig.railValue(dest::cleanup_profiles).toStdString(),
                           std::regex(R"(^\d+ profiles?$)")));
}

// from tests/test_personalize_s7_settings.py::test_rule_dictation_records_from_the_saved_order
TEST_CASE("test_rule_dictation_records_from_the_saved_order",
          "[test_personalize_s7_settings][widget]") {
    Json values = kCleanupOn;
    values["audio_input_priority"] = Json::array({kSnowball, kUsb});
    CleanupRig rig(values);
    // No microphone opens, as OpenWhisper's fake recorder refuses to start.
    rig.microphone->fail_open(0, "Device unavailable");
    rig.microphone->fail_open(-1, "Device unavailable");
    fv::CleanupRulesSection& rules = rig.rules();

    rules.toggleDictation();
    rules.toggleDictation();

    REQUIRE(rig.recorderPriorities.size() == 1);
    CHECK(rig.recorderPriorities.front() ==
          std::vector<fv::audio::DeviceKey>{{"Microphone (Blue Snowball )", "MME"},
                                            {"Microphone (2- USB Audio Device", "MME"}});
    CHECK(rules.dictationState() == State::Idle);
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_adds_a_learned_rule
TEST_CASE("test_enter_adds_a_learned_rule", "[test_personalize_fix2_g2_enter_key][widget]") {
    CleanupRig rig(kCleanupOn);
    fv::SettingsDialog& dialog = rig.shownWindow();
    dialog.selectDestination(qs(dest::cleanup_rules));
    processEvents();
    const QString title = dialog.pageTitleLabel()->text();
    QLineEdit* input = rig.rules().ruleInput();

    input->setFocus(Qt::TabFocusReason);
    processEvents();
    QTest::keyClicks(input, QStringLiteral("Use British spelling"));
    QTest::keyClick(input, Qt::Key_Return);

    // Adding a rule polishes it with the cleanup provider.
    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }));
    CHECK(rig.polish->asked == std::vector<std::string>{"Use British spelling"});
    CHECK(dialog.pageTitleLabel()->text() == title);
    // Enter never reached a header button: the page menu stayed shut.
    CHECK(dialog.pageMenu() == nullptr);
    dialog.close();
}

// from
// tests/test_personalize_fix2_g2_enter_key.py::test_every_home_button_with_a_label_keeps_it_in_its_name
TEST_CASE("test_every_home_button_with_a_label_keeps_it_in_its_name",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    CleanupRig rig;
    rig.window().showHome();
    processEvents();
    const auto label = [](const QAbstractButton* button) {
        QString text = button->text();
        text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
        text.remove(QLatin1Char('&'));
        text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
        return text.simplified();
    };
    int labelled = 0;
    for (QAbstractButton* button : rig.home().findChildren<QAbstractButton*>()) {
        const QString text = label(button);
        if (text.isEmpty()) {
            continue;
        }
        ++labelled;
        const QString name = button->accessibleName();
        INFO(text.toStdString() << " / " << name.toStdString());
        CHECK((name.isEmpty() || name.toCaseFolded().contains(text.toCaseFolded())));
    }
    CHECK(labelled > 0);
}

// from
// tests/test_personalize_s4_settings_page.py::TestSpellingRules::test_moving_spelling_rules_updates_the_rules_page
TEST_CASE("test_moving_spelling_rules_updates_the_rules_page",
          "[test_personalize_s4_settings_page][widget]") {
    CleanupRig rig({{"transcript_cleanup_rules",
                     Json::array({"Spell \"jon\" as \"John\".", "Use British spelling."})}});
    rig.window().show();
    rig.builtRulesPage();
    fv::DictionaryPage& page = rig.dictionary();
    CHECK_FALSE(page.rulesTile()->isHidden());
    auto* button =
        rig.window().findChild<QPushButton*>(QStringLiteral("dictionaryMoveRulesButton"));
    REQUIRE(button != nullptr);
    CHECK(button->text() == QStringLiteral("Move it here"));

    button->click();

    CHECK(rig.saved("transcript_cleanup_rules") == Json::array({"Use British spelling."}));
    // The Learned rules page re-reads the rules that are left when it is shown.
    fv::RulesPage& rules = rig.rulesPage();
    CHECK(listed(rules.section()->rulesList()) ==
          QStringList{QStringLiteral("Use British spelling.")});
    const auto words = rig.words();
    REQUIRE(words.size() == 1);
    CHECK(words.front().term == "John");
    CHECK(words.front().heard == std::vector<std::string>{"jon"});
    CHECK(page.rulesTile()->isHidden());
    CHECK(page.message()->text() == QStringLiteral("Moved 1 spelling rule here."));
    rig.window().close();
}

// --- The composer's refusals and the dictation cap (settings_cleanup_rules.py:392-609) ---

TEST_CASE("the 51st rule is a notice and starts no work", "[test_rule_composer][widget]") {
    Json rules = Json::array();
    for (int i = 0; i < 50; ++i) {
        rules.push_back("Rule number " + std::to_string(i) + ".");
    }
    Json values = kCleanupOn;
    values["transcript_cleanup_rules"] = rules;
    CleanupRig rig(values);
    rig.window().show();
    fv::CleanupRulesSection& section = rig.rules();
    REQUIRE(section.rulesList()->count() == 50);
    CHECK(section.count()->text() == QStringLiteral("50 / 50"));
    section.ruleInput()->setText(QStringLiteral("Keep acronyms uppercase."));

    QTest::mouseClick(section.addButton(), Qt::LeftButton);

    CHECK_FALSE(section.status()->isHidden());
    CHECK(section.status()->text() == QStringLiteral("Rule limit reached (50)."));
    CHECK_FALSE(section.polishing());
    CleanupRig::settle();
    CHECK(rig.polish->asked.empty());
    CHECK(rig.reviews.empty());
    // The words wait in the input.
    CHECK(section.ruleInput()->text() == QStringLiteral("Keep acronyms uppercase."));
    rig.window().close();
}

TEST_CASE("a rule is not dictated while a meeting holds the microphone",
          "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.meetingActive = true;
    rig.window().show();
    fv::CleanupRulesSection& section = rig.rules();

    QTest::mouseClick(section.micButton(), Qt::LeftButton);

    CHECK(section.dictationState() == State::Idle);
    CHECK(section.status()->text() ==
          QStringLiteral(u"Meeting Mode is active — end the meeting to dictate a rule."));
    CHECK(rig.recorderPriorities.empty());
    CHECK(rig.microphone->open_streams() == 0);
    rig.window().close();
}

TEST_CASE("without a rule transcriber dictation is unavailable", "[test_rule_composer][widget]") {
    // Until the controller provides one (M4.14), as OpenWhisper's dialog without
    // on_dictation_transcribe.
    CleanupRig rig(kCleanupOn, false);
    rig.window().show();
    fv::CleanupRulesSection& section = rig.rules();

    QTest::mouseClick(section.micButton(), Qt::LeftButton);

    CHECK(section.dictationState() == State::Idle);
    CHECK(section.status()->text() == QStringLiteral("Dictation is unavailable."));
    CHECK(rig.recorderPriorities.empty());
    rig.window().close();
}

TEST_CASE("without a worker to transcribe on no microphone opens for a rule",
          "[test_rule_composer][widget]") {
    // A context without the jobs strand could never transcribe the take: it is not started.
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& section = rig.rules();
    fv::CleanupServices& services = rig.cleanup().services();
    fv::Executor* jobs = std::exchange(services.jobs, nullptr);

    QTest::mouseClick(section.micButton(), Qt::LeftButton);

    CHECK(section.dictationState() == State::Idle);
    CHECK(section.status()->text() == QStringLiteral("Dictation is unavailable."));
    CHECK(rig.recorderPriorities.empty());
    CHECK(rig.microphone->open_streams() == 0);
    services.jobs = jobs;
    rig.window().close();
}

TEST_CASE("a take nothing can transcribe any more is thrown away and closes the microphone",
          "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& section = startDictation(rig);
    std::shared_ptr<fv::Recorder> recorder = section.recorder();
    fv::CleanupServices& services = rig.cleanup().services();
    fv::Executor* jobs = std::exchange(services.jobs, nullptr);

    QTest::mouseClick(section.micButton(), Qt::LeftButton);

    CHECK(recorder->capture_canceled());
    REQUIRE(pumpUntil([&section] { return section.dictationState() == State::Idle; }));
    CHECK(section.status()->text() == QStringLiteral("Dictation is unavailable."));
    CHECK(pumpUntil([&rig] { return rig.microphone->open_streams() == 0; }));
    CHECK(rig.transcriber->calls == 0);
    services.jobs = jobs;
    rig.window().close();
}

TEST_CASE("a take stops on its own at the cap and is transcribed", "[test_rule_composer][widget]") {
    CleanupRig rig(kCleanupOn);
    rig.window().show();
    fv::CleanupRulesSection& section = rig.rules();
    QTimer* cap = nullptr;
    for (QTimer* timer : section.findChildren<QTimer*>()) {
        if (timer->interval() == fv::kRuleDictationCapMs) {
            cap = timer;
        }
    }
    REQUIRE(cap != nullptr);
    CHECK(cap->isSingleShot());
    CHECK(fv::kRuleDictationCapMs == 60'000);
    // The minute is shortened so the test does not wait it out.
    cap->setInterval(200);

    startDictation(rig);
    speak(rig);

    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }, 10000));
    CHECK(rig.reviews.back().dictated);
    CHECK(rig.reviews.back().original == QStringLiteral("capitalize kubernetes"));
    CHECK(section.dictationState() == State::Idle);
    CHECK(rig.transcriber->calls == 1);
    rig.window().close();
}

// Rule dictation on the real default microphone, transcribed by the fake transcriber: the owner
// speaks for a second, then the take is stopped. [interactive]: it opens a real audio device, so
// every default preset leaves it out; it was written and not run.
TEST_CASE("a rule dictated on the real microphone is transcribed and reviewed",
          "[interactive][test_rule_composer]") {
    std::shared_ptr<fv::audio::Backend> device = fv::audio::make_portaudio_backend();
    CleanupRig rig(kCleanupOn);
    rig.cleanup().services().makeRecorder = [&rig, device](
                                                const std::vector<fv::audio::DeviceKey>& priority,
                                                const std::filesystem::path& output) {
        fv::RecorderOptions options;
        options.output_file = output;
        options.device_priority = priority;
        return std::make_shared<fv::Recorder>(*device, rig.context().paths(), std::move(options));
    };
    rig.window().show();
    fv::CleanupRulesSection& rules = startDictation(rig);
    wait(1500);
    QTest::mouseClick(rules.micButton(), Qt::LeftButton);

    REQUIRE(pumpUntil([&rig] { return !rig.reviews.empty(); }, 15000));
    CHECK(rig.transcriber->sawAudio);
    CHECK(rig.reviews.back().dictated);
    CHECK(rules.dictationState() == State::Idle);
    rig.window().close();
}
