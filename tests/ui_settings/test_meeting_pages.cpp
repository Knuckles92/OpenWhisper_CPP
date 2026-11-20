// Settings → Meeting Mode → After the meeting and Dashboard (fv_ui_meeting's after_end_page and
// dashboard_page), the re-transcription guard of Developer options, and Home's Meetings card
// (meeting_home_rows), inside the Settings window over an in-memory settings file
// (tests/test_meeting_settings_dialog.py, test_settings_general_layout.py, the meeting halves of
// test_settings_unified.py, test_settings_binder.py and test_settings_retention_commit.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>

#include "app_pages_support.h"
#include "home_support.h"
#include "llm/judge.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "ui_downloads/meeting_voice_page.h"
#include "ui_meeting/after_end_page.h"
#include "ui_meeting/dashboard_page.h"
#include "ui_meeting/meeting_home_rows.h"
#include "ui_settings/developer_page.h"
#include "ui_settings/home_page.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fvtest::AppPagesRig;
using fvtest::HomeRig;
using fvtest::qs;
namespace dest = fv::destinations;
namespace keys = fv::keys;

namespace {

void meetingPages(AppPagesRig& rig) {
    fv::registerMeetingSettingsPages(rig.context());
}

AppPagesRig rigWith(const Json& settings = Json::object()) {
    return AppPagesRig(settings, meetingPages);
}

template <class Tile> std::vector<Tile*> tilesOf(QWidget& page) {
    std::vector<Tile*> found;
    for (Tile* tile : page.findChildren<Tile*>()) {
        found.push_back(tile);
    }
    return found;
}

QStringList groupTitles(QWidget& page) {
    QStringList titles;
    for (QLabel* label : page.findChildren<QLabel*>(QStringLiteral("settingsTileGroupTitle"))) {
        titles.append(label->text());
    }
    return titles;
}

} // namespace

// --- Developer options: the re-transcription guard ---

TEST_CASE("test_guard_defaults_off_and_persists", "[test_meeting_settings_dialog][widget]") {
    AppPagesRig rig;
    auto& page = rig.page<fv::DeveloperPage>(dest::advanced);
    QAbstractButton* check = page.coverageGuardTile()->checkbox();
    CHECK_FALSE(check->isChecked());
    check->setChecked(true);
    rig.dialog().close();
    rig.dropDialog();

    auto& reopened = rig.page<fv::DeveloperPage>(dest::advanced);
    CHECK(reopened.coverageGuardTile()->checkbox()->isChecked());
    reopened.coverageGuardTile()->checkbox()->setChecked(false);
    rig.dialog().close();
    CHECK(rig.saved("meeting_redecode_coverage_guard") == false);
}

TEST_CASE("Developer options holds the guard above Developer mode", "[widget]") {
    AppPagesRig rig;
    auto& page = rig.page<fv::DeveloperPage>(dest::advanced);
    std::vector<fv::SettingTile*> tiles = tilesOf<fv::SettingTile>(page);
    std::ranges::sort(tiles);
    std::vector<fv::SettingTile*> expected{page.coverageGuardTile(), page.developerModeTile()};
    std::ranges::sort(expected);
    CHECK(tiles == expected);
    CHECK(tilesOf<fv::FieldTile>(page).empty());
    CHECK(groupTitles(page) ==
          QStringList{QStringLiteral("Meeting re-transcription"), QStringLiteral("Developer")});
    CHECK(page.namedControl("meeting_redecode_coverage_guard_check") ==
          page.coverageGuardTile()->checkbox());
    CHECK(page.coverageGuardTile()->titleLabel()->text() ==
          QStringLiteral("Keep the live transcript if re-transcription is much shorter"));
}

TEST_CASE("search and the guard's control names reach Developer options before it is built",
          "[widget]") {
    AppPagesRig rig;
    fv::SettingsPageRegistry& registry = rig.context().settingsPages();
    // PAGE_SEARCH_FIELDS['advanced'] and CONTROL_DESTINATIONS name the guard first.
    CHECK(registry.controlDestination("meeting_redecode_coverage_guard_tile") == dest::advanced);
    CHECK(registry.controlDestination("meeting_redecode_coverage_guard_check") == dest::advanced);
    const fv::SettingsPageSpec* spec = registry.page(dest::advanced);
    REQUIRE(spec != nullptr);
    REQUIRE(spec->searchFields.size() == 2);
    CHECK(spec->searchFields[0].target == "meeting_redecode_coverage_guard_tile");
    CHECK(spec->searchFields[0].label ==
          QStringLiteral("Keep the live transcript if re-transcription is much shorter"));
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK_FALSE(dialog.isBuilt(dest::advanced));
    auto* check =
        qobject_cast<QAbstractButton*>(dialog.control("meeting_redecode_coverage_guard_check"));
    REQUIRE(check != nullptr);
    CHECK(dialog.isBuilt(dest::advanced));
    auto& page = rig.page<fv::DeveloperPage>(dest::advanced);
    CHECK(check == page.coverageGuardTile()->checkbox());
    CHECK(spec->searchFields[0].description ==
          page.coverageGuardTile()->descriptionLabel()->text());
}

TEST_CASE("test_bound_controls_load_without_saving (coverage guard)", "[widget]") {
    AppPagesRig rig({{"meeting_redecode_coverage_guard", true}});
    const std::size_t writes = rig.settings().file().writes();
    auto& page = rig.page<fv::DeveloperPage>(dest::advanced);
    CHECK(page.coverageGuardTile()->checkbox()->isChecked());
    CHECK(rig.settings().file().writes() == writes);
}

// --- After the meeting ---

TEST_CASE("test_report_view_tiles_follow_the_final_report_toggle",
          "[test_settings_general_layout][widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    page.reportTile()->checkbox()->setChecked(true);
    CHECK(page.ribbonTile()->isEnabled());
    page.reportTile()->checkbox()->setChecked(false);
    for (fv::SettingTile* tile : {page.ribbonTile(), page.briefTile(), page.signalTile()}) {
        CHECK_FALSE(tile->isEnabled());
        CHECK_FALSE(tile->checkbox()->isEnabled());
    }
    CHECK_FALSE(page.viewsTitle()->isEnabled());
    CHECK_FALSE(page.viewsInfo()->isEnabled());
    CHECK(rig.saved("meeting_end_report") == false);
}

TEST_CASE("the After the meeting page has its groups and seven switches", "[widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    CHECK(tilesOf<fv::SettingTile>(page).size() == 7);
    CHECK(groupTitles(page) ==
          QStringList{QStringLiteral("After End"), QStringLiteral("Report views")});
    CHECK_THAT(page.redecodeTile()->descriptionLabel()->text().toStdString(),
               ContainsSubstring("run Parakeet again"));
    CHECK(page.reviewTile()->findChild<QLabel*>(QStringLiteral("meetingBetaBadge"))->text() ==
          QStringLiteral("Experimental"));
    CHECK(page.reviewSensitivity()->parentWidget()->parentWidget() == page.reviewTile()->body());
    // The defaults: the report and every view on, the review off, Normal.
    CHECK(page.reportTile()->checkbox()->isChecked());
    CHECK(page.ribbonTile()->checkbox()->isChecked());
    CHECK(page.briefTile()->checkbox()->isChecked());
    CHECK(page.signalTile()->checkbox()->isChecked());
    CHECK_FALSE(page.reviewTile()->checkbox()->isChecked());
    CHECK(page.reviewSensitivity()->currentData().toString() == QStringLiteral("normal"));
    CHECK(page.viewsHint()->isHidden());
    CHECK(rig.dialog().rail()->value(qs(dest::meeting_after)) == QStringLiteral("Report"));
}

TEST_CASE("switching off the last view puts Ribbon back and says so", "[widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    page.briefTile()->checkbox()->setChecked(false);
    page.signalTile()->checkbox()->setChecked(false);
    CHECK(page.viewsHint()->isHidden());
    page.ribbonTile()->checkbox()->setChecked(false);

    CHECK(page.ribbonTile()->checkbox()->isChecked());
    CHECK_FALSE(page.viewsHint()->isHidden());
    CHECK(page.viewsHint()->text() ==
          QStringLiteral("At least one view is required. Ribbon stays on."));
    // The forced Ribbon saves as on: the switch was put back before the save read it.
    CHECK(rig.saved("meeting_report_ribbon") == true);
    CHECK(rig.saved("meeting_report_brief") == false);
    CHECK(rig.saved("meeting_report_signal") == false);

    page.briefTile()->checkbox()->setChecked(true);
    CHECK(page.viewsHint()->isHidden());
    // Off with the report, the hint goes too.
    page.briefTile()->checkbox()->setChecked(false);
    page.ribbonTile()->checkbox()->setChecked(false);
    CHECK_FALSE(page.viewsHint()->isHidden());
    page.reportTile()->checkbox()->setChecked(false);
    CHECK(page.viewsHint()->isHidden());
}

TEST_CASE("saved views that are all off load with Ribbon on and the hint", "[widget]") {
    AppPagesRig rig = rigWith({{"meeting_report_ribbon", false},
                               {"meeting_report_brief", false},
                               {"meeting_report_signal", false}});
    const std::size_t writes = rig.settings().file().writes();
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    CHECK(page.ribbonTile()->checkbox()->isChecked());
    CHECK_FALSE(page.briefTile()->checkbox()->isChecked());
    CHECK_FALSE(page.viewsHint()->isHidden());
    // Loading never writes back.
    CHECK(rig.settings().file().writes() == writes);
}

TEST_CASE("the review switch asks first and saves the consent with it", "[widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
    std::vector<std::pair<QString, QString>> asked;
    bool answer = false;
    page.setConfirm([&](QWidget* parent, const QString& title, const QString& text) {
        CHECK(parent == &rig.dialog());
        asked.emplace_back(title, text);
        return answer;
    });
    QAbstractButton* check = page.reviewTile()->checkbox();
    check->setChecked(true);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].first == QStringLiteral("Enable experimental insight review?"));
    CHECK(asked[0].second ==
          QStringLiteral("For future meetings with AI insights on, send relevant transcript "
                         "excerpts, speaker names, and generated insights to TypeSafe after the "
                         "meeting? This is a separate service from your meeting LLM. No audio is "
                         "sent. Review is optional and does not block saving your recording."));
    CHECK_FALSE(check->isChecked());
    CHECK_FALSE(rig.has("meeting_insight_review_consent"));
    CHECK_FALSE(rig.has("meeting_insight_review"));

    answer = true;
    const std::size_t writes = rig.settings().file().writes();
    check->setChecked(true);
    CHECK(check->isChecked());
    CHECK(rig.saved("meeting_insight_review") == true);
    CHECK(rig.saved("meeting_insight_review_consent") ==
          std::string(fv::enums::kInsightReviewConsent));
    // One write, so the consent and the switch never disagree on disk.
    CHECK(rig.settings().file().writes() == writes + 1);

    page.reviewSensitivity()->setCurrentIndex(1);
    CHECK(rig.saved("meeting_insight_review_sensitivity") == "thorough");

    check->setChecked(false);
    CHECK(asked.size() == 2);
    CHECK(rig.saved("meeting_insight_review") == false);
    CHECK(rig.saved("meeting_insight_review_consent") == "");
}

TEST_CASE("the review switch is on only when its consent matches", "[widget]") {
    for (const auto& [consent, on] : std::vector<std::pair<std::string, bool>>{
             {"typesafe-text-v1", true}, {"typesafe-text-v0", false}, {"", false}}) {
        DYNAMIC_SECTION("consent '" << consent << "'") {
            AppPagesRig rig = rigWith(
                {{"meeting_insight_review", true}, {"meeting_insight_review_consent", consent}});
            auto& page = rig.page<fv::AfterEndPage>(dest::meeting_after);
            int asked = 0;
            page.setConfirm([&](QWidget*, const QString&, const QString&) {
                ++asked;
                return false;
            });
            CHECK(page.reviewTile()->checkbox()->isChecked() == on);
            // Loading never asks.
            CHECK(asked == 0);
        }
    }
}

TEST_CASE("the review question names where excerpts go for each route", "[widget]") {
    const fv::llm::JudgeRoute& typesafe = fv::llm::typesafe_route();
    CHECK(fv::after_end::review_destination(typesafe) == "TypeSafe");
    CHECK(fv::after_end::review_destination(fv::llm::openrouter_route()) ==
          "TypeSafe through OpenRouter");
    const Json local{{"typesafe_provider", "text_model"},
                     {"meeting_llm_provider", "lm_studio"},
                     {"meeting_llm_model", "qwen3-4b"}};
    const fv::llm::JudgeRoute lmStudio = fv::llm::route_from_settings(local);
    CHECK(fv::after_end::review_destination(lmStudio) == lmStudio.name + " on this computer");
    CHECK_THAT(fv::after_end::review_confirm_text(lmStudio),
               !ContainsSubstring("separate service"));
}

TEST_CASE("test_saving_preserves_unrelated_meeting_keys",
          "[test_meeting_settings_dialog][widget]") {
    // OpenWhisper's test edits the knowledge folder of the Intelligence page (M8); here the
    // After the meeting and Dashboard pages' controls write, beside the same unrelated keys.
    AppPagesRig rig = rigWith({{"meeting_asr_model", "orukeet-v0.1"},
                               {"meeting_llm_provider", "openai"},
                               {"meeting_llm_model", "gpt-4o-mini"}});
    auto& after = rig.page<fv::AfterEndPage>(dest::meeting_after);
    after.polishTile()->checkbox()->setChecked(false);
    after.redecodeTile()->checkbox()->setChecked(true);
    auto& dashboard = rig.page<fv::DashboardPage>(dest::meeting_dashboard);
    dashboard.bindCombo()->setCurrentIndex(1);
    rig.dialog().close();

    const Json saved = rig.settings().contents();
    CHECK(saved["meeting_asr_model"] == "orukeet-v0.1");
    CHECK(saved["meeting_llm_provider"] == "openai");
    CHECK(saved["meeting_llm_model"] == "gpt-4o-mini");
    CHECK(saved["meeting_end_polish"] == false);
    CHECK(saved["meeting_end_redecode"] == true);
    CHECK(saved["meeting_server_bind"] == "lan");
}

TEST_CASE("the After the meeting rail names the first step on", "[widget]") {
    CHECK(fv::after_end::rail_value(Json::object()) == "Report");
    CHECK(fv::after_end::rail_value({{"meeting_end_report", false}}) == "Polish");
    CHECK(fv::after_end::rail_value({{"meeting_end_report", false},
                                     {"meeting_end_polish", false},
                                     {"meeting_end_redecode", true}}) == "Re-decode");
    CHECK(fv::after_end::rail_value(
              {{"meeting_end_report", false}, {"meeting_end_polish", false}}) == "Off");
}

// --- Dashboard ---

TEST_CASE("the Dashboard page shows the port as Automatic and warns only for the LAN",
          "[test_settings_general_layout][widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::DashboardPage>(dest::meeting_dashboard);
    CHECK(tilesOf<fv::FieldTile>(page).size() == 2);
    CHECK(tilesOf<fv::TileBase>(page).size() == 2);
    CHECK(page.bindWarning()->parentWidget() == page.bindTile()->body());
    CHECK(groupTitles(page) == QStringList{QStringLiteral("Access"), QStringLiteral("Port")});
    QSpinBox* port = page.portSpin();
    CHECK(port->specialValueText() == QStringLiteral("Automatic"));
    CHECK(port->text() == QStringLiteral("Automatic"));
    CHECK_FALSE(port->keyboardTracking());
    CHECK(port->minimumWidth() >= 120);
    CHECK(page.bindWarning()->isHidden());
    CHECK(rig.dialog().rail()->value(qs(dest::meeting_dashboard)) == QStringLiteral("Localhost"));

    page.bindCombo()->setCurrentIndex(page.bindCombo()->findData(QStringLiteral("lan")));
    CHECK_FALSE(page.bindWarning()->isHidden());
    CHECK(rig.saved("meeting_server_bind") == "lan");
    CHECK(rig.dialog().rail()->value(qs(dest::meeting_dashboard)) == QStringLiteral("LAN · auto"));
    page.bindCombo()->setCurrentIndex(0);
    CHECK(page.bindWarning()->isHidden());
    CHECK(rig.saved("meeting_server_bind") == "localhost");
}

TEST_CASE("a saved LAN dashboard loads with its warning and port", "[widget]") {
    AppPagesRig rig = rigWith({{"meeting_server_bind", "lan"}, {"meeting_server_port", 8080}});
    auto& page = rig.page<fv::DashboardPage>(dest::meeting_dashboard);
    CHECK(page.bindCombo()->currentData().toString() == QStringLiteral("lan"));
    CHECK_FALSE(page.bindWarning()->isHidden());
    CHECK(page.portSpin()->value() == 8080);
    CHECK(rig.dialog().rail()->value(qs(dest::meeting_dashboard)) == QStringLiteral("LAN · 8080"));
}

TEST_CASE("test_live_applied_spinboxes_commit_typed_values_once (dashboard port)", "[widget]") {
    AppPagesRig rig = rigWith();
    auto& page = rig.page<fv::DashboardPage>(dest::meeting_dashboard);
    QSpinBox* port = page.portSpin();
    std::vector<int> committed;
    QObject::connect(port, &QSpinBox::valueChanged, port,
                     [&committed](int value) { committed.push_back(value); });
    port->selectAll();
    QTest::keyClicks(port, QStringLiteral("8080"));
    CHECK(committed.empty());
    CHECK_FALSE(rig.has("meeting_server_port"));
    QTest::keyClick(port, Qt::Key_Return);
    CHECK(committed == std::vector<int>{8080});
    CHECK(rig.saved("meeting_server_port") == 8080);
}

TEST_CASE("the meeting pages' controls build their page", "[widget]") {
    AppPagesRig rig = rigWith();
    fv::SettingsPageRegistry& registry = rig.context().settingsPages();
    CHECK(registry.controlDestination("meeting_report_views_hint") == dest::meeting_after);
    CHECK(registry.controlDestination("meeting_port_spinbox") == dest::meeting_dashboard);
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK_FALSE(dialog.isBuilt(dest::meeting_dashboard));
    CHECK(qobject_cast<QSpinBox*>(dialog.control("meeting_port_spinbox")) != nullptr);
    CHECK(dialog.isBuilt(dest::meeting_dashboard));
    CHECK(dialog.pageTitle(dest::meeting_after) == QStringLiteral("After the meeting"));
    CHECK(dialog.pageSubtitle(dest::meeting_dashboard) ==
          QStringLiteral("Who can open the live meeting dashboard, and on which port."));
    const fv::SettingsPageSpec* after = registry.page(dest::meeting_after);
    REQUIRE(after != nullptr);
    CHECK(after->searchFields.size() == 7);
    CHECK(after->searchFields[3].description.startsWith(QStringLiteral("Optional, for new")));
}

// --- Home's Meetings card ---

namespace {

// The meeting destinations Home's rig leaves to the meeting pages.
const std::vector<std::string_view> kMeetingPages{dest::meeting_voice, dest::meeting_after,
                                                  dest::meeting_dashboard};

// Registers the meeting pages and Home rows in the rig, before its window is built.
void addMeetingPages(HomeRig& rig) {
    fv::registerMeetingSettingsPages(rig.context());
    fv::registerMeetingVoicePage(rig.context(), [] { return fv::MeetingVoicePageOptions{}; });
    fv::registerMeetingHomeRows(rig.homeRows());
}

QPushButton* buttonNamed(QWidget& root, const QString& accessible) {
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->accessibleName() == accessible) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("test_app_theme_and_meeting_report_edits_use_existing_handlers (meeting report half)",
          "[test_settings_unified][widget]") {
    HomeRig rig(Json::object(), kMeetingPages);
    addMeetingPages(rig);
    fv::HomePage& home = rig.home();
    auto* report = qobject_cast<QAbstractButton*>(home.control(keys::meeting_end_report.key()));
    REQUIRE(report != nullptr);
    CHECK(report->isChecked());
    report->click();
    CHECK(rig.saved("meeting_end_report") == report->isChecked());
    rig.dialog().selectDestination(qs(dest::meeting_after));
    auto* page = dynamic_cast<fv::AfterEndPage*>(rig.dialog().pageContent(dest::meeting_after));
    REQUIRE(page != nullptr);
    CHECK(page->reportTile()->checkbox()->isChecked() == report->isChecked());
    // The page's own handler ran: the views follow the report.
    CHECK_FALSE(page->ribbonTile()->isEnabled());
}

TEST_CASE("test_rows_and_storage_repeat_what_each_destination_reports (meeting voice half)",
          "[test_settings_unified][widget]") {
    HomeRig rig({{"meeting_speaker_id_backend", "off"}, {"meeting_language", "fr"}}, kMeetingPages);
    addMeetingPages(rig);
    rig.dialog().selectDestination(qs(dest::overview));
    fv::HomePage& home = rig.home();
    fv::HomeCard* meetings = home.card("meetings");
    REQUIRE(meetings != nullptr);
    CHECK_FALSE(meetings->isHidden());
    QPushButton* change =
        buttonNamed(*meetings, QStringLiteral("Change meeting voice and speakers"));
    REQUIRE(change != nullptr);
    std::vector<QString> lines;
    for (auto* label :
         meetings->findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowDescription"))) {
        lines.push_back(label->text());
    }
    CHECK(std::ranges::find(lines, QStringLiteral("Parakeet TDT 0.6B v3 · French · Me / Others "
                                                  "labels")) != lines.end());
    // settings_home.py: meeting_intelligence_overview's model, then provider and core.
    CHECK(std::ranges::find(lines, QStringLiteral("openrouter/free · OpenRouter · FioriVoce. Turn "
                                                  "it on for each meeting when you start it.")) !=
          lines.end());
    REQUIRE(buttonNamed(*meetings, QStringLiteral("Set up AI insights")) != nullptr);
    // What every row title says, in order.
    QStringList titles;
    for (auto* label : meetings->findChildren<fv::WrappedLabel*>(QStringLiteral("homeRowTitle"))) {
        titles.append(label->text());
    }
    CHECK(titles ==
          QStringList{QStringLiteral("Voice & speakers"), QStringLiteral("Clean up the transcript"),
                      QStringLiteral("Write a final report"), QStringLiteral("AI insights")});
}

TEST_CASE("test_links_and_buttons_open_their_destination (meeting voice half)",
          "[test_settings_unified][widget]") {
    HomeRig rig(Json::object(), kMeetingPages);
    addMeetingPages(rig);
    fv::HomePage& home = rig.home();
    QPushButton* change = buttonNamed(home, QStringLiteral("Change meeting voice and speakers"));
    REQUIRE(change != nullptr);
    QTest::mouseClick(change, Qt::LeftButton);
    CHECK(rig.dialog().currentDestination() == qs(dest::meeting_voice));
    rig.dialog().showHome();
    QPushButton* link = nullptr;
    for (QPushButton* button :
         home.card("meetings")->findChildren<QPushButton*>(QStringLiteral("homeLink"))) {
        link = button;
    }
    REQUIRE(link != nullptr);
    CHECK(link->text() == QStringLiteral("All meeting settings  →"));
    CHECK_FALSE(link->isHidden());
    link->click();
    CHECK(rig.dialog().currentDestination() == qs(dest::meeting_voice));
}

TEST_CASE("the where strip says where meeting audio goes", "[widget]") {
    {
        const auto items = fv::meeting_where_items(Json::object());
        REQUIRE(items.size() == 2);
        CHECK(items[0].text == QStringLiteral("Meeting voice"));
        CHECK_FALSE(items[0].leaves);
        CHECK(items[0].order == fv::HomeRows::kWhereMeetingVoice);
        CHECK(items[1].order == fv::HomeRows::kWhereMeetingIntelligence);
        CHECK(items[1].text ==
              QStringLiteral("Meeting intelligence (transcript text, when enabled)"));
        CHECK(items[1].leaves);
    }
    {
        const auto items = fv::meeting_where_items({{"selected_model", "remote"}});
        REQUIRE(items.size() == 2);
        CHECK(items[0].text == QStringLiteral("Meeting voice"));
        CHECK_FALSE(items[0].leaves);
    }
    {
        const std::string fingerprint(64, 'a');
        Json saved{{"meeting_speech_separate", true},
                   {"meeting_asr_source", "remote"},
                   {"remote_engine_client",
                    {{"host", "127.0.0.1"},
                     {"host_name", "Synthetic computer"},
                     {"fingerprint", fingerprint}}}};
        const auto unbound = fv::meeting_where_items(saved);
        REQUIRE(unbound.size() == 2);
        CHECK(unbound[0].text == QStringLiteral("Meeting voice (confirm paired computer)"));
        CHECK_FALSE(unbound[0].leaves);

        saved["fv_meeting_remote_fingerprint"] = fingerprint;
        const auto bound = fv::meeting_where_items(saved);
        REQUIRE(bound.size() == 2);
        CHECK(bound[0].text ==
              QStringLiteral("Meeting voice (audio sent to paired computer when confirmed)"));
        CHECK(bound[0].leaves);
    }
    {
        // A stored OpenAI speaker pass reads as on-device: nothing leaves for speaker labels.
        const auto items = fv::meeting_where_items({{"meeting_speaker_id_backend", "openai"}});
        CHECK(items.size() == 2);
    }
    {
        const auto items = fv::meeting_where_items(
            {{"meeting_llm_provider", "ollama"}, {"meeting_llm_model", "local-model"}});
        REQUIRE(items.size() == 2);
        CHECK(items[1].text == QStringLiteral("Meeting intelligence"));
        CHECK_FALSE(items[1].leaves);
    }
    {
        // An installed agent signs in to a provider of its own: transcript text leaves, even
        // with a meeting text provider on this computer (meeting_intelligence_is_remote).
        const auto items = fv::meeting_where_items({{"meeting_llm_provider", "ollama"},
                                                    {"meeting_llm_model", "local-model"},
                                                    {"meeting_agent_core", "claude_code"}});
        REQUIRE(items.size() == 2);
        CHECK(items[1].text ==
              QStringLiteral("Meeting intelligence (transcript text, when enabled)"));
        CHECK(items[1].leaves);
    }
    HomeRig rig(Json::object(), kMeetingPages);
    addMeetingPages(rig);
    rig.dialog().selectDestination(qs(dest::overview));
    CHECK_THAT(rig.home().localList()->text().toStdString(), ContainsSubstring("Meeting voice"));
    // In OpenWhisper's order: after the dictation voice, before AI cleanup.
    const QString local = rig.home().localList()->text();
    CHECK(local.indexOf(QStringLiteral("Dictation voice")) <
          local.indexOf(QStringLiteral("Meeting voice")));
}
