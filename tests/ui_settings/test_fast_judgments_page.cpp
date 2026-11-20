// Settings must say when TypeSafe features cannot run for want of a key
// (tests/test_typesafe_settings_messaging.py): the Fast judgments notice, the rail badge, the
// saved feature switches, the API keys page's TypeSafe row and Test, the Jev routes and the text
// model route. Over an in-memory credential store, an environment of the test's own and a key
// check that records what it was asked.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLabel>
#include <QLineEdit>

#include "fast_judgments_support.h"
#include "ui_settings/app_pages.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

const Json kLocal{{"typesafe_enabled", true},
                  {"typesafe_provider", "text_model"},
                  {"meeting_llm_provider", "ollama"},
                  {"meeting_llm_model", "qwen3:4b"}};

const Json kEnabled{{"typesafe_enabled", true}};

QString description(fv::InfoTile* tile) {
    return tile->descriptionLabel()->text();
}

QString detail(fv::SegmentedBar* bar, std::size_t index) {
    return bar->buttons().at(index)->detailLabel()->text();
}

std::string saved(FastJudgmentsRig& scene, const char* key) {
    const Json value = scene.rig().saved(key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

// The page's rail value once `expected` shows, or what it showed when the wait ran out.
QString railSettles(FastJudgmentsRig& scene, const QString& expected) {
    pumpUntil([&] { return scene.rail() == expected; });
    return scene.rail();
}

// Waits for the notice to be hidden or shown (a key read finishing redraws it).
bool noticeHidden(fv::FastJudgmentsPage& page) {
    return pumpUntil([&page] { return page.keyNotice()->isHidden(); });
}
bool noticeShown(fv::FastJudgmentsPage& page) {
    return pumpUntil([&page] { return !page.keyNotice()->isHidden(); });
}

} // namespace

// --- TestFastJudgmentsNotice ---

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsNotice::test_notice_names_the_missing_key_when_features_are_on
TEST_CASE("test_notice_names_the_missing_key_when_features_are_on",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    const QString text = description(page.keyNotice());
    CHECK(text.contains(QStringLiteral("TYPESAFE_API_KEY")));
    CHECK(text.contains(QStringLiteral("API keys")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsNotice::test_notice_explains_that_turning_switches_on_will_not_help
TEST_CASE("test_notice_explains_that_turning_switches_on_will_not_help",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", false}});
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    CHECK(description(page.keyNotice()).contains(QStringLiteral("no effect")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsNotice::test_notice_disappears_once_a_key_exists
TEST_CASE("test_notice_disappears_once_a_key_exists",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    CHECK(page.keyNotice()->isHidden());
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsNotice::test_saving_a_key_clears_the_notice_without_reopening
TEST_CASE("test_saving_a_key_clears_the_notice_without_reopening",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    auto& keys = scene.keysPage();
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    keys.edit()->setText(QString::fromStdString(kTypeSafeKey));
    keys.saveKey();
    scene.settleKeys(keys);
    CHECK(noticeHidden(page));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsNotice::test_the_notice_button_opens_the_typesafe_credential
TEST_CASE("test_the_notice_button_opens_the_typesafe_credential",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    page.openKeyButton()->click();
    CHECK(scene.dialog().currentDestination() == qs(dest::api_keys));
    CHECK(scene.keysPage().selectedName() == QString::fromStdString(kTypeSafeEnv));
}

// --- TestFastJudgmentsRailBadge ---

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_off_reads_off
TEST_CASE("test_off_reads_off", "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", false}});
    CHECK(scene.page().railValue() == QStringLiteral("Off"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_enabled_without_a_key_reads_no_key
TEST_CASE("test_enabled_without_a_key_reads_no_key", "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    CHECK(scene.page().railValue() == QStringLiteral("No key"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_enabled_with_a_key_counts_active_features
TEST_CASE("test_enabled_with_a_key_counts_active_features",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", true},
                                {"typesafe_topic_shift_enabled", true},
                                {"typesafe_voice_commands_enabled", false},
                                {"typesafe_citations_enabled", false},
                                {"typesafe_semantic_search_enabled", false},
                                {"typesafe_question_radar_enabled", false},
                                {"typesafe_highlights_enabled", false}});
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    CHECK(scene.page().railValue() == QStringLiteral("On · 1"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_the_rail_shows_the_badge
TEST_CASE("test_the_rail_shows_the_badge", "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    scene.dialog().refreshRailValues();
    // The page is not built: the rail reads the keys on the worker and redraws when they are in.
    CHECK(railSettles(scene, QStringLiteral("No key")) == QStringLiteral("No key"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_toggling_a_feature_updates_the_badge_live
TEST_CASE("test_toggling_a_feature_updates_the_badge_live",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", true},
                                {"typesafe_topic_shift_enabled", false},
                                {"typesafe_voice_commands_enabled", false},
                                {"typesafe_citations_enabled", false},
                                {"typesafe_semantic_search_enabled", false},
                                {"typesafe_question_radar_enabled", false},
                                {"typesafe_highlights_enabled", false}});
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    CHECK(scene.rail() == QStringLiteral("On · no features"));
    // Persisting a setting redraws the rail, so the tiles need no handler of their own.
    page.featureTiles().back().second->checkbox()->setChecked(true);
    CHECK(scene.dialog().rail()->value(qs(dest::meeting_fast)) == QStringLiteral("On · 1"));
    page.enabledTile()->checkbox()->setChecked(false);
    CHECK(scene.dialog().rail()->value(qs(dest::meeting_fast)) == QStringLiteral("Off"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestFastJudgmentsRailBadge::test_removing_a_key_flips_the_badge_back
TEST_CASE("test_removing_a_key_flips_the_badge_back",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    CHECK(page.keyNotice()->isHidden());
    auto& keys = scene.keysPage();
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    scene.rig().answer = true;
    keys.removeKey();
    scene.settleKeys(keys);
    CHECK(railSettles(scene, QStringLiteral("No key")) == QStringLiteral("No key"));
    CHECK(noticeShown(page));
}

// --- TestSavedFeatureFlags ---

// from
// tests/test_typesafe_settings_messaging.py::TestSavedFeatureFlags::test_master_off_keeps_saved_choices_and_restores_them_live
TEST_CASE("test_master_off_keeps_saved_choices_and_restores_them_live",
          "[test_typesafe_settings_messaging][widget]") {
    const std::vector<std::pair<std::string, bool>> flags{{"citations", true},
                                                          {"semantic_search", false},
                                                          {"question_radar", true},
                                                          {"highlights", true}};
    Json initial{{"typesafe_enabled", false}};
    for (const auto& [feature, value] : flags) {
        initial["typesafe_" + feature + "_enabled"] = value;
    }
    FastJudgmentsRig scene(initial);
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    for (const bool enabled : {false, true, false, true}) {
        page.enabledTile()->checkbox()->setChecked(enabled);
        REQUIRE(page.featureTiles().size() == flags.size());
        for (std::size_t i = 0; i < flags.size(); ++i) {
            const auto& [feature, tile] = page.featureTiles()[i];
            const std::string key = "typesafe_" + feature + "_enabled";
            CHECK(feature == flags[i].first);
            CHECK(tile->checkbox()->isChecked() == flags[i].second);
            CHECK(scene.rig().saved(key) == Json(flags[i].second));
            CHECK(tile->isEnabled() == enabled);
        }
    }
    // The semantic topic changes default is on, so with citations, the questions radar and
    // highlights four features count.
    CHECK(scene.rail() == QStringLiteral("On · 4"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestSavedFeatureFlags::test_unset_features_stay_unchecked_when_master_is_enabled
TEST_CASE("test_unset_features_stay_unchecked_when_master_is_enabled",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", false}});
    auto& page = scene.page();
    page.enabledTile()->checkbox()->setChecked(true);
    for (const auto& [feature, tile] : page.featureTiles()) {
        CHECK_FALSE(tile->checkbox()->isChecked());
    }
}

// --- TestTypeSafeKeyTesting ---

// from
// tests/test_typesafe_settings_messaging.py::TestTypeSafeKeyTesting::test_test_button_verifies_through_the_typesafe_probe
TEST_CASE("test_test_button_verifies_through_the_typesafe_probe",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    auto& keys = scene.keysPage();
    std::vector<std::string> asked;
    keys.setTypeSafeVerify([&asked](const std::string& key) {
        asked.push_back(key);
        return std::pair<bool, std::string>(true, "api.typesafe.ai accepted the key.");
    });
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    keys.edit()->setText(QString::fromStdString(kTypeSafeKey));
    keys.testKey();
    scene.settleKeys(keys);
    CHECK(asked == std::vector<std::string>{kTypeSafeKey});
    CHECK(scene.dialog().messageText().contains(QStringLiteral("works")));
}

// A test of mine: without a replacement the check is the context's, over the TypeSafe route.
TEST_CASE("the TypeSafe key test goes to the judge over TypeSafe's route",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    scene.answer(true, "api.typesafe.ai accepted the key.");
    auto& keys = scene.keysPage();
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    keys.edit()->setText(QString::fromStdString(kTypeSafeKey));
    keys.testKey();
    scene.settleKeys(keys);
    const std::vector<Verified> asked = scene.verifiedNow();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].route == "typesafe");
    CHECK(asked[0].model == "jev-1.13.0");
    CHECK(asked[0].key == kTypeSafeKey);
    CHECK(scene.dialog().messageText() ==
          QStringLiteral("TypeSafe API key works: api.typesafe.ai accepted the key."));
}

// from
// tests/test_typesafe_settings_messaging.py::TestTypeSafeKeyTesting::test_a_rejected_key_is_reported
TEST_CASE("test_a_rejected_key_is_reported", "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    auto& keys = scene.keysPage();
    keys.setTypeSafeVerify([](const std::string&) {
        return std::pair<bool, std::string>(false, "api.typesafe.ai rejected the key (HTTP 401).");
    });
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    keys.edit()->setText(QString::fromStdString(kTypeSafeKey));
    keys.testKey();
    scene.settleKeys(keys);
    const QString message = scene.dialog().messageText();
    CHECK(message.contains(QStringLiteral("TypeSafe API key failed")));
    CHECK(message.contains(QStringLiteral("401")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestTypeSafeKeyTesting::test_text_model_keys_still_use_the_openai_probe
TEST_CASE("test_text_model_keys_still_use_the_openai_probe",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    auto& keys = scene.keysPage();
    std::vector<std::string> text;
    bool typesafeAsked = false;
    keys.setTextVerify([&text](const fv::llm::TextLLMProfile&, const std::string& key) {
        text.push_back(key);
        return std::pair<bool, std::string>(true, "ok");
    });
    keys.setTypeSafeVerify([&typesafeAsked](const std::string&) {
        typesafeAsked = true;
        return std::pair<bool, std::string>(true, "ok");
    });
    scene.dialog().focusApiKeys(QStringLiteral("OPENAI_API_KEY"));
    keys.edit()->setText(QStringLiteral("sk-proj-abcdefghijklmnopqrstuvwxyz"));
    keys.testKey();
    scene.settleKeys(keys);
    CHECK(text == std::vector<std::string>{"sk-proj-abcdefghijklmnopqrstuvwxyz"});
    CHECK_FALSE(typesafeAsked);
}

// --- TestJevConnection ---

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_typesafe_is_selected_until_openrouter_is_chosen
TEST_CASE("test_typesafe_is_selected_until_openrouter_is_chosen",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK(page.routeBar()->currentIndex() == 0);
    CHECK(page.destinationLabel()->text().contains(QStringLiteral("api.typesafe.ai")));
    CHECK_FALSE(scene.rig().has("typesafe_provider"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_an_openrouter_key_alone_offers_a_one_click_switch
TEST_CASE("test_an_openrouter_key_alone_offers_a_one_click_switch",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    scene.setKey(kOpenRouterEnv, kOpenRouterKey);
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    CHECK_FALSE(page.switchRouteButton()->isHidden());
    CHECK(page.switchRouteButton()->text() == QStringLiteral("Use OpenRouter"));
    CHECK(description(page.keyNotice())
              .contains(QStringLiteral("OpenRouter API key can run the same model")));

    page.switchRouteButton()->click();

    CHECK(saved(scene, "typesafe_provider") == "openrouter");
    CHECK(page.routeBar()->currentIndex() == 1);
    CHECK(page.keyNotice()->isHidden());
    CHECK(page.destinationLabel()->text().contains(QStringLiteral("openrouter.ai")));
    CHECK(page.destinationLabel()->text().contains(QStringLiteral("text model on this computer")));
    // Semantic topic changes are on by default once the master switch is.
    CHECK(scene.dialog().rail()->value(qs(dest::meeting_fast)) ==
          QStringLiteral("On · 1 · OpenRouter"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_no_switch_is_offered_when_neither_route_has_a_key
TEST_CASE("test_no_switch_is_offered_when_neither_route_has_a_key",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK(page.switchRouteButton()->isHidden());
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_choosing_openrouter_without_its_key_names_that_key
TEST_CASE("test_choosing_openrouter_without_its_key_names_that_key",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    CHECK(page.keyNotice()->isHidden());
    page.routeBar()->buttons()[1]->click();
    CHECK(saved(scene, "typesafe_provider") == "openrouter");
    CHECK_FALSE(page.keyNotice()->isHidden());
    CHECK(page.keyNotice()->titleLabel()->text() == QStringLiteral("No OpenRouter API key"));
    // The TypeSafe key still works, so switching back is offered.
    CHECK(page.switchRouteButton()->text() == QStringLiteral("Use TypeSafe"));
    CHECK(scene.dialog().rail()->value(qs(dest::meeting_fast)) == QStringLiteral("No key"));
    page.openKeyButton()->click();
    CHECK(scene.keysPage().selectedName() == QString::fromStdString(kOpenRouterEnv));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_segments_show_which_routes_have_a_key
TEST_CASE("test_segments_show_which_routes_have_a_key",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    scene.setKey(kOpenRouterEnv, kOpenRouterKey);
    auto& page = scene.page();
    // The text-model route names the AI insights model's provider.
    CHECK(detail(page.routeBar(), 0) == QStringLiteral("Direct · no key"));
    CHECK(detail(page.routeBar(), 1) == QStringLiteral("Same model · key set"));
    CHECK(detail(page.routeBar(), 2) == QStringLiteral("OpenRouter · key set"));
    CHECK(description(page.routeTile()).contains(QStringLiteral("No TypeSafe account needed")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_saving_an_openrouter_key_updates_the_connection_live
TEST_CASE("test_saving_an_openrouter_key_updates_the_connection_live",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_enabled", true}, {"typesafe_provider", "openrouter"}});
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    auto& keys = scene.keysPage();
    scene.dialog().focusApiKeys(QString::fromStdString(kOpenRouterEnv));
    keys.edit()->setText(QString::fromStdString(kOpenRouterKey));
    keys.saveKey();
    scene.settleKeys(keys);
    CHECK(noticeHidden(page));
    CHECK(pumpUntil(
        [&page] { return detail(page.routeBar(), 1) == QStringLiteral("Same model · key set"); }));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_test_button_verifies_the_chosen_route
TEST_CASE("test_test_button_verifies_the_chosen_route",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    scene.setKey(kOpenRouterEnv, kOpenRouterKey);
    scene.answer(true, "openrouter.ai accepted the key and typesafe/jev-1.13 answered in 0.15 s.");
    auto& page = scene.page();
    page.routeTestButton()->click();
    CHECK_FALSE(page.routeTestButton()->isEnabled());
    CHECK(page.testing());
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    const std::vector<Verified> asked = scene.verifiedNow();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].route == "openrouter");
    CHECK(asked[0].key == kOpenRouterKey);
    CHECK(page.routeTestButton()->isEnabled());
    CHECK(page.routeStatus()->text().contains(QStringLiteral("typesafe/jev-1.13 answered")));
    CHECK(page.routeStatus()->property("tone").toString() == QStringLiteral("success"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_testing_without_a_key_says_so_without_a_request
TEST_CASE("test_testing_without_a_key_says_so_without_a_request",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    auto& page = scene.page();
    page.routeTestButton()->click();
    CHECK(scene.verifiedNow().empty());
    CHECK(page.routeStatus()->text().contains(QStringLiteral("No OpenRouter API key to test")));
    CHECK(page.routeStatus()->property("tone").toString() == QStringLiteral("warning"));
    CHECK(page.routeStatus()->text() ==
          QStringLiteral("No OpenRouter API key to test. Add one under API keys → OpenRouter."));
}

// A test of mine: the key is read when the click comes before the page's own read has answered.
TEST_CASE("a Test before the keys are read still asks the store, not the network",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    auto& page = scene.rig().page<fv::FastJudgmentsPage>(dest::meeting_fast);
    page.routeTestButton()->click();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    CHECK(scene.verifiedNow().empty());
    CHECK(page.routeStatus()->text().contains(QStringLiteral("No OpenRouter API key to test")));
    CHECK(page.routeTestButton()->isEnabled());
}

// A test of mine: a key saved a moment ago is tested, not reported missing from the read before.
TEST_CASE("a Test while the keys are being read again asks the store, not the old answer",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    scene.answer(true, "openrouter.ai accepted the key and typesafe/jev-1.13 answered in 0.15 s.");
    auto& page = scene.page();
    scene.setKey(kOpenRouterEnv, kOpenRouterKey);
    // What the API keys page announces after a save: the keys are read again on the worker, and
    // the click comes before that read has answered.
    Q_EMIT scene.rig().pages().sharedSignals()->apiKeysChanged();
    page.routeTestButton()->click();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    const std::vector<Verified> asked = scene.verifiedNow();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].route == "openrouter");
    CHECK(asked[0].key == kOpenRouterKey);
    CHECK(page.routeStatus()->text().contains(QStringLiteral("typesafe/jev-1.13 answered")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_a_result_for_the_route_left_behind_is_dropped
TEST_CASE("test_a_result_for_the_route_left_behind_is_dropped",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    scene.answer(false, "api.typesafe.ai rejected the key (HTTP 401).");
    auto& page = scene.page();
    page.routeTestButton()->click();
    page.routeBar()->buttons()[1]->click();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    CHECK(page.routeStatus()->isHidden());
    CHECK(page.routeTestButton()->isEnabled());
}

// from
// tests/test_typesafe_settings_messaging.py::TestJevConnection::test_api_keys_page_says_openrouter_can_run_fast_judgments
TEST_CASE("test_api_keys_page_says_openrouter_can_run_fast_judgments",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    auto& keys = scene.keysPage();
    scene.dialog().focusApiKeys(QString::fromStdString(kOpenRouterEnv));
    CHECK(keys.usesCaption()->text().contains(QStringLiteral("Fast judgments")));
    scene.dialog().focusApiKeys(QString::fromStdString(kTypeSafeEnv));
    const QString uses = keys.usesCaption()->text();
    CHECK(uses.contains(QStringLiteral("OpenRouter key")));
    CHECK_FALSE(uses.contains(QStringLiteral("sensitive-dictation")));
}

// --- TestTextModelJudge ---

// from
// tests/test_typesafe_settings_messaging.py::TestTextModelJudge::test_a_local_model_needs_no_key_and_keeps_excerpts_here
TEST_CASE("test_a_local_model_needs_no_key_and_keeps_excerpts_here",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kLocal);
    auto& page = scene.page();
    CHECK(page.routeBar()->currentIndex() == 2);
    CHECK(page.keyNotice()->isHidden());
    CHECK(detail(page.routeBar(), 2) == QStringLiteral("Ollama · on this PC"));
    CHECK_FALSE(page.textModelBar()->isHidden());
    CHECK(page.textModelBar()->currentIndex() == 0);
    CHECK(detail(page.textModelBar(), 0) == QStringLiteral("Ollama · qwen3:4b"));
    CHECK(page.destinationLabel()->text().contains(QStringLiteral("stay on this computer")));
    CHECK(description(page.routeTile()).contains(QStringLiteral("answers on this computer")));
    CHECK(scene.dialog().rail()->value(qs(dest::meeting_fast)) ==
          QStringLiteral("On · 1 · Ollama"));
}

// from
// tests/test_typesafe_settings_messaging.py::TestTextModelJudge::test_jev_routes_hide_the_text_model_choice
TEST_CASE("test_jev_routes_hide_the_text_model_choice",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK(page.textModelBar()->isHidden());
    page.routeBar()->buttons()[2]->click();
    CHECK(saved(scene, "typesafe_provider") == "text_model");
    CHECK_FALSE(page.textModelBar()->isHidden());
}

// from
// tests/test_typesafe_settings_messaging.py::TestTextModelJudge::test_choosing_the_hosted_cleanup_model_asks_for_its_key
TEST_CASE("test_choosing_the_hosted_cleanup_model_asks_for_its_key",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kLocal);
    auto& page = scene.page();
    page.textModelBar()->buttons()[1]->click();
    CHECK(saved(scene, "typesafe_text_model") == "cleanup");
    CHECK(noticeShown(page));
    CHECK(page.keyNotice()->titleLabel()->text() == QStringLiteral("No OpenRouter API key"));
    // There is no other Jev route to offer for a text model.
    CHECK(page.switchRouteButton()->isHidden());
    CHECK(page.destinationLabel()->text().contains(QStringLiteral("text model on this computer")));
}

// from
// tests/test_typesafe_settings_messaging.py::TestTextModelJudge::test_test_button_asks_the_local_model_without_a_key
TEST_CASE("test_test_button_asks_the_local_model_without_a_key",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kLocal);
    scene.answer(true, "Ollama · qwen3:4b answered in 0.80 s.");
    auto& page = scene.page();
    page.routeTestButton()->click();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    const std::vector<Verified> asked = scene.verifiedNow();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].key == "dummy");
    CHECK(asked[0].route == "text_model");
    CHECK(asked[0].model == "qwen3:4b");
    CHECK(page.routeStatus()->text().contains(QStringLiteral("answered in 0.80 s")));
}

TEST_CASE("the test result follows the precise text model endpoint", "[widget]") {
    FastJudgmentsRig scene(kLocal);
    scene.answer(true, "The previous Ollama route answered.");
    auto& page = scene.page();
    page.routeTestButton()->click();
    page.textModelBar()->buttons()[1]->click();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    CHECK(page.routeStatus()->isHidden());
    CHECK(page.routeTestButton()->isEnabled());
}

// --- Beyond the OpenWhisper tests ---

TEST_CASE("the page registers its destination, controls and search fields",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    const fv::SettingsPageSpec* spec =
        scene.rig().context().settingsPages().page(dest::meeting_fast);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Fast judgments"));
    CHECK(spec->subtitle == QStringLiteral("Optional checks by Jev or your own text model. Each "
                                           "feature describes the text it shares."));
    CHECK(spec->searchFields.size() == 5);
    CHECK(scene.rig().context().settingsPages().controlDestination("typesafe_route_bar") ==
          dest::meeting_fast);
    auto& page = scene.page();
    for (const std::string& name : fv::FastJudgmentsPage::controlNames()) {
        INFO(name);
        CHECK(scene.dialog().control(name) != nullptr);
    }
    CHECK(scene.dialog().control("typesafe_route_bar") == page.routeBar());
    CHECK(scene.dialog().control("typesafe_enabled_check") == page.enabledTile()->checkbox());
}

TEST_CASE("the controls carry OpenWhisper's object names",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    auto& page = scene.page();
    CHECK(page.openKeyButton()->objectName() == QStringLiteral("typesafeKeyNoticeLink"));
    CHECK(page.routeStatus()->objectName() == QStringLiteral("apiKeyStatus"));
    CHECK(page.enabledTile()->checkbox()->objectName() == QStringLiteral("typesafeEnabledCheck"));
    CHECK(page.topicShiftTile()->checkbox()->objectName() ==
          QStringLiteral("typesafeTopicShiftCheck"));
    CHECK(page.voiceCommandsTile()->checkbox()->objectName() ==
          QStringLiteral("typesafeVoiceCommandsCheck"));
    CHECK(page.routeBar()->accessibleName() == QStringLiteral("Run fast judgments on"));
    CHECK(page.textModelBar()->accessibleName() ==
          QStringLiteral("Text model that answers fast judgments"));
    CHECK(page.routeTestButton()->toolTip() ==
          QStringLiteral("Send one tiny judgment (the word “ok”) over this connection"));
}

TEST_CASE("choosing the route that is already chosen saves nothing",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    page.routeBar()->buttons()[0]->click();
    CHECK_FALSE(scene.rig().has("typesafe_provider"));
    page.routeBar()->buttons()[2]->click();
    page.textModelBar()->buttons()[0]->click();
    CHECK_FALSE(scene.rig().has("typesafe_text_model"));
}

TEST_CASE("the Test button is disabled while a test runs and a second click does nothing",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    page.routeTestButton()->click();
    CHECK_FALSE(page.routeTestButton()->isEnabled());
    page.testRoute();
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    CHECK(scene.verifiedNow().size() == 1);
    CHECK(page.routeStatus()->text() == QStringLiteral("api.typesafe.ai accepted the key."));
}

TEST_CASE("Testing shows its own line first, in the warning tone",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene;
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    auto& page = scene.page();
    page.routeTestButton()->click();
    CHECK(page.routeStatus()->text() == QStringLiteral("Testing TypeSafe…"));
    CHECK(page.routeStatus()->property("tone").toString() == QStringLiteral("warning"));
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
}

TEST_CASE("a key set while the window was closed shows when it opens again",
          "[test_typesafe_settings_messaging][widget]") {
    FastJudgmentsRig scene(kEnabled);
    auto& page = scene.page();
    CHECK_FALSE(page.keyNotice()->isHidden());
    scene.setKey(kTypeSafeEnv, kTypeSafeKey);
    Q_EMIT scene.dialog().windowShown();
    CHECK(noticeHidden(page));
    CHECK(railSettles(scene, QStringLiteral("On · 1")) == QStringLiteral("On · 1"));
}
