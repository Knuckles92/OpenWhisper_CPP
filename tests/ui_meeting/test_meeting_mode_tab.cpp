// The Meeting Mode tab (ui_qt/widgets/meeting_mode_tab.py), from tests/test_meeting_mode_tab.py.
// A FakeClock stands for the patched time.monotonic(); payloads are the Python dicts as JSON.
// The window tests host the tab in a plain top-level widget: the main window that registers it
// is the presenter package's.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "meeting/finalization_view.h"
#include "meeting_state/patches.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "tab_support.h"
#include "testkit/testkit.h"
#include "ui_meeting/brief_edit.h"
#include "ui_meeting/finalization_card.h"
#include "ui_meeting/live_dot.h"
#include "ui_meeting/recent_meeting_row.h"
#include "ui_shell/page_fit.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
using fvtest::TabFixture;

QString q(const char* text) {
    return QString::fromUtf8(text);
}

// _show_result: an inactive meeting's result card, the layout settled.
void showResult(TabFixture& tab, const Json& finalization, const char* status = "ended",
                Json extra = Json::object()) {
    Json payload = {{"active", false}, {"status", status}, {"finalization", finalization}};
    payload.update(extra);
    tab->setMeetingState(payload);
    QApplication::processEvents();
}

std::vector<QWidget*> stepRows(const fv::MeetingModeTab& tab) {
    std::vector<QWidget*> rows;
    QVBoxLayout* layout = tab.finalizationCard()->stepsLayout();
    for (int index = 0; index < layout->count(); ++index) {
        rows.push_back(layout->itemAt(index)->widget());
    }
    return rows;
}

std::vector<QPushButton*> stepActions(const fv::MeetingModeTab& tab) {
    std::vector<QPushButton*> actions;
    for (QPushButton* button :
         tab.finalizationCard()->stepsWidget()->findChildren<QPushButton*>()) {
        actions.push_back(button);
    }
    return actions;
}

int top(const fv::MeetingModeTab& tab, const QWidget* widget) {
    return widget->mapTo(&tab, QPoint()).y();
}

} // namespace

// ---- TestMeetingModeTabRegistration ----

TEST_CASE("test_launch_controls_are_named_and_keyboard_reachable", "[widget]") {
    TabFixture tab;
    CHECK(tab->accessibleName() == q("Meeting Mode"));
    CHECK_FALSE(tab->accessibleDescription().isEmpty());
    CHECK(tab->cloudCheckbox()->focusPolicy() != Qt::NoFocus);
    CHECK_FALSE(tab->cloudCheckbox()->accessibleDescription().isEmpty());
    CHECK_FALSE(tab->dashboardButton()->accessibleDescription().isEmpty());

    tab->setStatusText(q("Paused"));
    CHECK(tab->statusPill()->accessibleName() == q("Meeting status: Paused"));
}

// ---- TestMeetingModeWindowHeight ----

namespace {

// The window around the tab: the tab fills it, as a page fills the main window.
struct Window {
    explicit Window(fv::MeetingModeTab* tab) {
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(tab);
    }
    void settle() {
        for (int pass = 0; pass < 3; ++pass) {
            QApplication::processEvents();
        }
    }
    QWidget host;
};

Json runningFinalization() {
    return {
        {"status", "running"},
        {"message", "Preparing final report..."},
        {"current_step", 3},
        {"total_steps", 4},
        {"step_details", "Synthesizing executive summary, key decisions, and action items over 6 "
                         "segments..."},
        {"steps",
         Json::array(
             {{{"id", "redecode"}, {"name", "Audio Re-transcription"}, {"status", "completed"}},
              {{"id", "polish"}, {"name", "Transcript Cleanup"}, {"status", "completed"}},
              {{"id", "consolidation"}, {"name", "Summary & Action Items"}, {"status", "running"}},
              {{"id", "finalize"}, {"name", "State Finalization"}, {"status", "pending"}}})}};
}

} // namespace

TEST_CASE("test_stage_changes_never_resize_the_window", "[widget]") {
    fvtest::AppTheme theme(q("dark"));
    TabFixture fixture;
    fv::MeetingModeTab* tab = fixture.tab.release();
    Window window(tab);
    window.host.resize(tab->preferredWidth() + 48, 640);
    window.host.show();
    window.settle();
    CHECK_FALSE(tab->split()->isStacked());
    const QSize size = window.host.size();
    // What a fitting window measures: the page's content.
    const QSize hint = tab->contentWidget()->sizeHint();
    const QSize minimum = tab->contentWidget()->minimumSizeHint();

    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    window.settle();
    CHECK(tab->stageState() == q("live"));
    CHECK(window.host.size() == size);
    CHECK(tab->contentWidget()->sizeHint().height() == hint.height());
    CHECK(tab->contentWidget()->minimumSizeHint() == minimum);

    for (const Json& dashboard : {Json::object(), Json{{"dashboard_available", true}}}) {
        Json payload = {
            {"active", false}, {"status", "ended"}, {"finalization", runningFinalization()}};
        payload.update(dashboard);
        tab->setMeetingState(payload);
        window.settle();
        CHECK(tab->stageState() == q("result"));
        CHECK(window.host.size() == size);
        CHECK(tab->contentWidget()->sizeHint().height() == hint.height());
        CHECK(tab->contentWidget()->minimumSizeHint() == minimum);
        QWidget* steps = tab->finalizationCard()->stepsWidget();
        CHECK(steps->height() >= steps->minimumSizeHint().height());
        for (QWidget* row : stepRows(*tab)) {
            CHECK(row->height() >= row->minimumSizeHint().height());
            CHECK(row->x() > 0);
            CHECK(row->x() + row->width() < steps->width());
        }

        tab->setMeetingState(
            Json{{"status", "idle"}, {"active", false}, {"finalization", nullptr}});
        window.settle();
        CHECK(tab->stageState() == q("empty"));
        CHECK(window.host.size() == size);
        CHECK(tab->scrollArea()->verticalScrollBar()->maximum() == 0);
    }
    window.host.hide();
}

TEST_CASE("test_narrow_window_stacks_the_column_above_the_stage", "[widget]") {
    fvtest::AppTheme theme(q("dark"));
    TabFixture fixture;
    fv::MeetingModeTab* tab = fixture.tab.release();
    Window window(tab);
    window.host.resize(tab->preferredWidth() + 48, 640);
    window.host.show();
    window.settle();
    window.host.resize(640, window.host.height());
    window.settle();
    CHECK(tab->split()->isStacked());
    CHECK(tab->startButton()->mapTo(tab, QPoint()).y() < tab->stage()->mapTo(tab, QPoint()).y());
    window.host.hide();
}

// ---- TestMeetingModeTabState ----

TEST_CASE("test_unchanged_finalization_steps_keep_their_rows", "[widget]") {
    TabFixture tab;
    const auto finalization = [](const char* status) {
        return Json{
            {"finalization",
             {{"status", "failed"},
              {"steps",
               Json::array({{{"id", "polish"}, {"name", "Polish"}, {"status", status}},
                            {{"id", "finalize"}, {"name", "Save"}, {"status", "completed"}}})}}}};
    };
    tab->setMeetingState(finalization("failed"));
    const std::vector<QWidget*> first = stepRows(*tab);
    REQUIRE(first.size() == 2);
    tab->setMeetingState(Json{{"dashboard_available", true}});
    tab->setMeetingState(finalization("failed"));
    CHECK(stepRows(*tab) == first);
    tab->setMeetingState(finalization("completed"));
    CHECK(stepRows(*tab) != first);
}

TEST_CASE("test_an_elapsed_update_does_not_restart_a_paused_clock", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(Json{{"active", true}, {"paused", false}, {"status", "active"}});
    tab->setMeetingState(Json{{"paused", true}});
    tab->setMeetingState(Json{{"paused", true}, {"elapsed_s", 65}});
    CHECK_FALSE(tab->runningSince().has_value());
    CHECK(tab->elapsedLabel()->text() == q("01:05"));
    tab->setMeetingState(Json{{"paused", false}});
    CHECK(tab->runningSince().has_value());
}

TEST_CASE("test_retained_state_keeps_the_clock_through_pause_and_resume", "[widget]") {
    // The main window merges each payload into the retained state and passes the merge on, so
    // every call below re-sends older fields.
    TabFixture tab(1000.0);
    Json retained = Json::object();
    const auto deliver = [&](const Json& payload) {
        retained.update(payload);
        tab->setMeetingState(retained);
    };
    deliver({{"active", true},
             {"paused", false},
             {"status", "active"},
             {"elapsed_s", 0.0},
             {"elapsed_at", tab.now()}});
    tab.advance(89);
    deliver({{"active", true},
             {"paused", true},
             {"status", "paused"},
             {"elapsed_s", 89.0},
             {"elapsed_at", tab.now()}});
    CHECK(tab->elapsedLabel()->text() == q("01:29"));
    tab.advance(10);
    tab->refreshElapsed();
    CHECK(tab->elapsedLabel()->text() == q("01:29"));
    deliver({{"active", true},
             {"paused", false},
             {"status", "active"},
             {"elapsed_s", 89.0},
             {"elapsed_at", tab.now()}});
    tab.advance(21);
    deliver({{"cloud_enabled", true}});
    CHECK(tab->elapsedLabel()->text() == q("01:50"));
}

TEST_CASE("test_state_replayed_into_a_new_tab_counts_from_when_it_was_read", "[widget]") {
    TabFixture tab(1000.0);
    tab->setMeetingState(Json{{"active", true},
                              {"paused", false},
                              {"status", "active"},
                              {"elapsed_s", 30.0},
                              {"elapsed_at", tab.now() - 60}});
    CHECK(tab->elapsedLabel()->text() == q("01:30"));
    tab->setMeetingState(Json{{"paused", true},
                              {"status", "paused"},
                              {"elapsed_s", 95.0},
                              {"elapsed_at", tab.now() - 40}});
    CHECK(tab->elapsedLabel()->text() == q("01:35"));
    CHECK_FALSE(tab->runningSince().has_value());
}

TEST_CASE("test_idle_shows_start_controls", "[widget]") {
    TabFixture tab;
    CHECK_FALSE(tab->idleCard()->isHidden());
    CHECK(tab->sessionCard()->isHidden());
    CHECK_FALSE(tab->isMeetingActive());
    CHECK(tab->demoButton()->isHidden());
    CHECK(tab->demoHint()->isHidden());
}

TEST_CASE("test_platform_copy_does_not_promise_unavailable_system_audio", "[widget]") {
    using fv::meeting::meeting_audio_shows_platform_warning;
    using fv::meeting::meeting_audio_support_copy;
    using fv::meeting::PlatformInfo;
    const auto [subtitle, linuxHint] =
        meeting_audio_support_copy(PlatformInfo{"linux", "x86_64", ""});
    // meeting_mode_supported("darwin") is True from macOS 13.
    const std::string macHint =
        meeting_audio_support_copy(PlatformInfo{"darwin", "arm64", "14.2"}).second;
    const std::string winHint =
        meeting_audio_support_copy(PlatformInfo{"win32", "AMD64", ""}).second;

    CHECK(subtitle.find("when supported") != std::string::npos);
    CHECK(linuxHint.find("PulseAudio") != std::string::npos);
    CHECK((linuxHint.find("is a preview") != std::string::npos ||
           linuxHint.find("not a supported") != std::string::npos));
    CHECK(linuxHint.find("publicly promoted") == std::string::npos);
    CHECK(winHint.find("WASAPI") != std::string::npos);
    // macOS captures system audio, but only once the user grants it, so the copy names the
    // permission rather than promising the channel.
    CHECK(macHint.find("ScreenCaptureKit") != std::string::npos);
    CHECK(macHint.find("Screen Recording permission") != std::string::npos);
    CHECK(macHint.find("not a supported") == std::string::npos);

    TabFixture tab;
    const QString expected = QString::fromStdString(winHint);
    CHECK(tab->subtitle()->toolTip() == expected);
    CHECK(tab->platformHint()->text() == expected);
    CHECK(tab->platformHint()->isHidden() ==
          !meeting_audio_shows_platform_warning(PlatformInfo{"win32", "AMD64", ""}));
    CHECK_FALSE(meeting_audio_shows_platform_warning(PlatformInfo{"win32", "AMD64", ""}));
    // Linux stays warning-visible until public promotion is attested.
    CHECK(meeting_audio_shows_platform_warning(PlatformInfo{"linux", "x86_64", ""}));
    CHECK_FALSE(meeting_audio_shows_platform_warning(PlatformInfo{"darwin", "arm64", "14.2"}));
    CHECK(meeting_audio_shows_platform_warning(PlatformInfo{"darwin", "arm64", "12.6"}));
}

TEST_CASE("test_platform_copy_marks_pre_screencapturekit_macos_unsupported", "[widget]") {
    const std::string macHint = fv::meeting::meeting_audio_support_copy(
                                    fv::meeting::PlatformInfo{"darwin", "x86_64", "12.6"})
                                    .second;
    CHECK(macHint.find("macOS is not a supported") != std::string::npos);
    CHECK(macHint.find("microphone audio only") != std::string::npos);

    // The tab shows the platform it is given: an unsupported one keeps the warning visible.
    fv::MeetingModeTabOptions options;
    options.platform = fv::meeting::PlatformInfo{"darwin", "x86_64", "12.6"};
    fv::MeetingModeTab tab(options);
    CHECK(tab.platformHint()->text() == QString::fromStdString(macHint));
    CHECK_FALSE(tab.platformHint()->isHidden());
    CHECK(tab.platformHint()->property("tone").toString() == q("warning"));
    CHECK(tab.setupLink() == nullptr);
}

TEST_CASE("test_developer_mode_shows_demo_meeting_control", "[widget]") {
    TabFixture tab;
    std::vector<bool> received;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::demoRequested,
                     [&](bool cloud) { received.push_back(cloud); });
    tab->setDeveloperMode(true);
    QApplication::processEvents();
    CHECK_FALSE(tab->demoButton()->isHidden());
    CHECK_FALSE(tab->demoHint()->isHidden());

    tab->cloudCheckbox()->setChecked(true);
    tab->demoButton()->click();
    CHECK(received == std::vector<bool>{true});

    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    QApplication::processEvents();
    CHECK(tab->demoButton()->isHidden());
}

TEST_CASE("test_active_payload_switches_layout", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}, {"elapsed_s", 65}});
    QApplication::processEvents();
    CHECK(tab->isMeetingActive());
    CHECK(tab->idleCard()->isHidden());
    CHECK_FALSE(tab->sessionCard()->isHidden());
    CHECK(tab->statusPill()->text() == q("Active"));
    CHECK(tab->elapsedLabel()->text() == q("01:05"));
}

TEST_CASE("test_starting_is_non_active_and_does_not_run_timer", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(Json{{"active", false}, {"status", "starting"}});
    QApplication::processEvents();
    CHECK_FALSE(tab->isMeetingActive());
    CHECK(tab->idleCard()->isHidden());
    CHECK_FALSE(tab->sessionCard()->isHidden());
    CHECK(tab->statusPill()->text() == q("Starting"));
    CHECK_FALSE(tab->elapsedTimer()->isActive());
    CHECK_FALSE(tab->pauseButton()->isEnabled());
    CHECK_FALSE(tab->endButton()->isEnabled());
    CHECK(tab->elapsedLabel()->text() == q("00:00"));
}

TEST_CASE("test_startup_status_does_not_flash_idle_before_active_confirmation", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(Json{{"active", false}, {"status", "starting"}});
    // Engine status can arrive before the runtime confirms startup.
    for (const Json& payload :
         {Json{{"status", "active"}}, Json{{"status", "active"}, {"active", false}}}) {
        tab->setMeetingState(payload);
        QApplication::processEvents();
        CHECK(tab->idleCard()->isHidden());
        CHECK_FALSE(tab->sessionCard()->isHidden());
        CHECK(tab->statusPill()->text() == q("Starting"));
        CHECK_FALSE(tab->isMeetingActive());
        CHECK_FALSE(tab->elapsedTimer()->isActive());
        CHECK_FALSE(tab->pauseButton()->isEnabled());
        CHECK_FALSE(tab->endButton()->isEnabled());
    }
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    QApplication::processEvents();
    CHECK(tab->idleCard()->isHidden());
    CHECK_FALSE(tab->sessionCard()->isHidden());
    CHECK(tab->statusPill()->text() == q("Active"));
    CHECK(tab->elapsedTimer()->isActive());
    CHECK(tab->pauseButton()->isEnabled());
    CHECK(tab->endButton()->isEnabled());
}

TEST_CASE("test_startup_failure_returns_to_idle_after_early_active_status", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(Json{{"active", false}, {"status", "starting"}});
    tab->setMeetingState(Json{{"status", "active"}});
    tab->setMeetingState(Json{{"active", false}, {"status", "failed"}});
    QApplication::processEvents();
    CHECK_FALSE(tab->idleCard()->isHidden());
    CHECK(tab->sessionCard()->isHidden());
    CHECK_FALSE(tab->elapsedTimer()->isActive());
}

namespace {

struct Starts {
    explicit Starts(fv::MeetingModeTab* tab) {
        QObject::connect(
            tab, &fv::MeetingModeTab::startRequested,
            [this](bool cloud, const QString& brief) { received.emplace_back(cloud, brief); });
    }
    std::vector<std::pair<bool, QString>> received;
};

} // namespace

TEST_CASE("test_start_emits_cloud_choice", "[widget]") {
    TabFixture tab;
    Starts starts(tab.tab.get());
    tab->cloudCheckbox()->setChecked(true);
    tab->startButton()->click();
    CHECK(starts.received == std::vector<std::pair<bool, QString>>{{true, QString()}});
}

TEST_CASE("test_start_emits_typed_meeting_brief", "[widget]") {
    TabFixture tab;
    Starts starts(tab.tab.get());
    tab->cloudCheckbox()->setChecked(true);
    tab->briefInput()->setPlainText(q("  Capture every objection to the vendor choice.  "));
    tab->startButton()->click();
    CHECK(starts.received == std::vector<std::pair<bool, QString>>{
                                 {true, q("Capture every objection to the vendor choice.")}});
}

TEST_CASE("test_brief_is_hidden_while_ai_insights_are_off", "[widget]") {
    TabFixture tab;
    Starts starts(tab.tab.get());
    tab->cloudCheckbox()->setChecked(true);
    tab->briefInput()->setPlainText(q("Capture every objection."));
    CHECK_FALSE(tab->briefPanel()->isHidden());

    tab->cloudCheckbox()->setChecked(false);
    CHECK(tab->briefPanel()->isHidden());
    // Hidden text is kept for later but never sent.
    CHECK(tab->briefInput()->toPlainText() == q("Capture every objection."));
    tab->startButton()->click();
    CHECK(starts.received == std::vector<std::pair<bool, QString>>{{false, QString()}});

    tab->cloudCheckbox()->setChecked(true);
    CHECK_FALSE(tab->briefPanel()->isHidden());
    tab->startButton()->click();
    CHECK(starts.received.back() == std::pair<bool, QString>{true, q("Capture every objection.")});
}

TEST_CASE("test_brief_follows_switch_changes_made_without_signals", "[widget]") {
    // A declined consent dialog unticks the switch with signals blocked.
    TabFixture tab;
    tab->cloudCheckbox()->setChecked(true);
    tab->setMeetingState(Json{{"cloud_enabled", false}});
    CHECK_FALSE(tab->cloudCheckbox()->isChecked());
    CHECK(tab->briefPanel()->isHidden());
    tab->setMeetingState(Json{{"cloud_enabled", true}});
    CHECK_FALSE(tab->briefPanel()->isHidden());
}

TEST_CASE("test_meeting_brief_is_capped_at_the_validation_limit", "[widget]") {
    // A pasted essay is trimmed here rather than rejected by the store.
    TabFixture tab;
    tab->cloudCheckbox()->setChecked(true);
    const auto limit = static_cast<qsizetype>(fv::meeting_state::MAX_INTENT_LEN);
    tab->briefInput()->setPlainText(QString(limit + 500, QChar(u'b')));
    CHECK(tab->meetingBrief().size() == limit);
    // The cap counts code points: a character outside the BMP is one.
    tab->briefInput()->setPlainText(QString::fromUcs4(U"\U0001F600").repeated(limit + 5));
    CHECK(tab->meetingBrief().toUcs4().size() == limit);
}

TEST_CASE("test_meeting_brief_field_is_hidden_during_a_meeting", "[widget]") {
    TabFixture tab;
    tab->cloudCheckbox()->setChecked(true);
    CHECK_FALSE(tab->briefPanel()->isHidden());
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    QApplication::processEvents();
    CHECK(tab->idleCard()->isHidden());
    CHECK(tab->briefPanel()->isHidden());
    tab->setMeetingState(Json{{"active", false}, {"status", "idle"}});
    CHECK_FALSE(tab->briefPanel()->isHidden());
}

TEST_CASE("test_stage_follows_the_meeting", "[widget]") {
    TabFixture tab;
    CHECK(tab->stageState() == q("empty"));
    CHECK(tab->liveActions()->isHidden());
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    CHECK(tab->stageState() == q("live"));
    CHECK_FALSE(tab->liveActions()->isHidden());
    CHECK(tab->sessionCard()->property("tone").toString() == q("live"));
    tab->setMeetingState(Json{{"paused", true}, {"status", "paused"}});
    CHECK(tab->sessionCard()->property("tone").toString() == q("paused"));
    CHECK(tab->pauseButton()->text() == q("Resume"));
    tab->setMeetingState(
        Json{{"active", false}, {"status", "ended"}, {"finalization", {{"status", "running"}}}});
    CHECK(tab->stageState() == q("result"));
    CHECK(tab->liveActions()->isHidden());
    // Start stays in the column and says why it is waiting.
    CHECK_FALSE(tab->startButton()->isHidden());
    CHECK_FALSE(tab->startButton()->isEnabled());
    CHECK_FALSE(tab->slotNote()->isHidden());
    CHECK(tab->slotNote()->text().contains(q("wrapping up")));
    tab->setMeetingState(Json{{"finalization", nullptr}});
    CHECK(tab->stageState() == q("empty"));
    CHECK(tab->slotNote()->isHidden());
}

TEST_CASE("test_recent_meetings_open_from_the_empty_stage", "[widget]") {
    TabFixture tab;
    std::vector<QString> opened;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::pastMeetingRequested,
                     [&](const QString& id) { opened.push_back(id); });
    CHECK(tab->recentBox()->isHidden());
    QList<QJsonObject> meetings;
    for (int n = 0; n < 5; ++n) {
        meetings.push_back(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("m%1").arg(n)},
                        {QStringLiteral("title"), QStringLiteral("Meeting %1").arg(n)},
                        {QStringLiteral("status"), QStringLiteral("ended")}});
    }
    tab->setRecentMeetings(meetings);
    CHECK_FALSE(tab->recentBox()->isHidden());
    std::vector<QString> ids;
    for (const fv::RecentMeetingRow* row : tab->recentRows()) {
        ids.push_back(row->meetingId());
    }
    CHECK(ids == std::vector<QString>{q("m0"), q("m1"), q("m2")});
    QTest::keyClick(tab->recentRows()[1], Qt::Key_Return);
    CHECK(opened == std::vector<QString>{q("m1")});
    tab->setRecentMeetings({});
    CHECK(tab->recentBox()->isHidden());
}

TEST_CASE("test_cloud_checkbox_presents_compact_tooltip", "[widget]") {
    // Hover help belongs to the checkbox, without a separate icon.
    TabFixture tab;
    CHECK(tab->findChild<QToolButton*>(q("meetingCloudHelpIcon")) == nullptr);
    const QString tooltip = tab->cloudCheckbox()->toolTip();
    CHECK(tooltip.contains(q("On:")));
    CHECK(tooltip.contains(q("Off:")));
    CHECK(tooltip.contains(q("Audio routing")));
    const QStringList lines = tooltip.split(QChar(u'\n'));
    CHECK(lines.size() > 1);
    for (const QString& line : lines) {
        CHECK(line.size() <= 52);
    }
}

TEST_CASE("test_ai_insights_switch_names_the_feature_not_the_location", "[widget]") {
    // The switch says what it produces; the model line, which the presenter fills when the tab
    // asks, says where.
    TabFixture tab;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::aiDestinationWanted, [&tab] {
        tab->setAiDestination(q("OpenRouter · deepseek-v4.1-flash — sends transcript text, never "
                                "audio."),
                              q("OpenRouter · deepseek-v4.1-flash"));
    });
    CHECK(tab->cloudCheckbox()->text() == q("AI insights"));
    tab->show();
    QApplication::processEvents();
    CHECK_FALSE(tab->aiDestination()->text().isEmpty());
    tab->hide();
}

TEST_CASE("test_remote_speech_status_names_actual_host_and_connection", "[widget]") {
    TabFixture tab;
    const Json speech = {{"source", "remote"},
                         {"host", "devbox"},
                         {"model", "base"},
                         {"connected", false},
                         {"message", "Reconnecting; audio saved locally."}};
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}, {"speech", speech}});
    CHECK(tab->speechDestination()->text().contains(q("devbox")));
    CHECK(tab->speechDestination()->text().contains(q("Reconnecting")));
    Json connected = speech;
    connected["connected"] = true;
    tab->setMeetingState(Json{{"speech", connected}});
    CHECK_FALSE(tab->speechDestination()->text().contains(q("Reconnecting")));
    CHECK(tab->speechDestination()->text() ==
          q("Remote speech · devbox · base. Audio sent to this host; recordings saved here."));
}

TEST_CASE("test_beta_tag_marks_the_title_not_the_speech_note", "[widget]") {
    // Beta labels the feature, so it shares a row with the title only.
    TabFixture tab;
    auto* title = tab->column()->findChild<QLabel*>(q("meetingColumnTitle"));
    REQUIRE(title != nullptr);
    CHECK(title->text() == q("Meeting Mode"));
    QLayout* body = tab->column()->layout();
    QLayout* heading = body->itemAt(0)->layout();
    REQUIRE(heading != nullptr);
    CHECK(heading->indexOf(title) >= 0);
    CHECK(heading->indexOf(tab->betaTag()) >= 0);
    CHECK(tab->betaTag()->toolTip().contains(q("inaccurate")));
    CHECK(body->indexOf(tab->speechDestination()) >= 0);
    CHECK(tab->column()->findChild<QLabel*>(q("meetingBetaBadge")) == nullptr);
}

TEST_CASE("test_ai_details_show_only_while_setting_up", "[widget]") {
    // A live meeting keeps only the switch; every other state explains it.
    TabFixture tab;
    CHECK_FALSE(tab->aiDetails()->isHidden());
    CHECK_FALSE(tab->aiSettingsButton()->isHidden());
    CHECK(tab->aiCompactDestination()->isHidden());

    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    QApplication::processEvents();
    // The switch acts on the live meeting, so it stays in view.
    CHECK_FALSE(tab->aiPanel()->isHidden());
    CHECK(tab->aiDetails()->isHidden());
    CHECK(tab->aiSettingsButton()->isHidden());
    CHECK_FALSE(tab->aiCompactDestination()->isHidden());

    showResult(tab, {{"status", "failed"}, {"message", "Final insights failed."}});
    // A failed result is on the stage; the column still sets up the next.
    CHECK_FALSE(tab->aiPanel()->isHidden());
    CHECK_FALSE(tab->aiDetails()->isHidden());
    CHECK(tab->aiCompactDestination()->isHidden());

    tab->setMeetingState(Json{
        {"finalization", {{"status", "completed"}, {"message", "Final insights are ready."}}}});
    QApplication::processEvents();
    CHECK_FALSE(tab->aiDetails()->isHidden());
}

TEST_CASE("test_change_link_requests_intelligence_settings", "[widget]") {
    TabFixture tab;
    int requested = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::aiSettingsRequested, [&] { ++requested; });
    tab->aiSettingsButton()->click();
    CHECK(requested == 1);
}

TEST_CASE("test_ai_destination_holds_still_during_a_meeting", "[widget]") {
    // A running meeting keeps the endpoint it started with.
    TabFixture tab;
    tab->setAiDestination(q("Ollama on this PC · llama3.1 — transcript text stays on this "
                            "computer."),
                          q("Ollama on this PC · llama3.1"));
    CHECK(tab->aiDestination()->text() ==
          q("Ollama on this PC · llama3.1 — transcript text stays on this computer."));
    CHECK(tab->aiCompactDestination()->text() == q("Ollama on this PC · llama3.1"));

    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    tab->setAiDestination(q("OpenAI · gpt-5-mini — sends transcript text, never audio."),
                          q("OpenAI · gpt-5-mini"));
    CHECK(tab->aiDestination()->text().contains(q("Ollama")));
}

TEST_CASE("test_brief_field_behaves_like_a_form_control", "[widget]") {
    // Tab leaves the field, and its height is counted in text lines.
    TabFixture tab;
    fv::BriefEdit* field = tab->briefInput();
    CHECK(field->tabChangesFocus());
    CHECK(tab->briefLabel()->buddy() == field);
    const int before = field->sizeHint().height();
    QFont font = field->font();
    font.setPointSize(font.pointSize() * 2);
    field->setFont(font);
    CHECK(field->sizeHint().height() > before);
}

TEST_CASE("test_running_finalization_holds_start_and_shows_indeterminate_bar", "[widget]") {
    TabFixture tab;
    showResult(tab, {{"status", "running"}, {"message", "Preparing final insights…"}}, "ended",
               {{"dashboard_available", true}});
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK_FALSE(tab->idleCard()->isHidden());
    CHECK_FALSE(tab->startButton()->isHidden());
    CHECK_FALSE(tab->startButton()->isEnabled());
    CHECK(tab->sessionCard()->isHidden());
    CHECK_FALSE(card->isHidden());
    CHECK_FALSE(card->progress()->isHidden());
    CHECK(card->progress()->minimum() == 0);
    CHECK(card->progress()->maximum() == 0);
    CHECK_FALSE(card->reportButton()->isEnabled());
    CHECK(card->doneButton()->isHidden());
}

TEST_CASE("test_completed_and_disabled_restore_start", "[widget]") {
    // Terminal info outcomes keep the card and restore Start Meeting.
    for (const auto& [status, message] :
         {std::pair<const char*, const char*>{"completed", "Final insights are ready."},
          std::pair<const char*, const char*>{"disabled",
                                              "AI insights are off for this meeting."}}) {
        DYNAMIC_SECTION(status) {
            TabFixture tab;
            showResult(tab, {{"status", status}, {"message", message}}, "ended",
                       {{"dashboard_available", true}});
            const fv::FinalizationCard* card = tab->finalizationCard();
            CHECK_FALSE(tab->idleCard()->isHidden());
            CHECK_FALSE(tab->startButton()->isHidden());
            CHECK_FALSE(card->isHidden());
            CHECK(card->progress()->isHidden());
            CHECK(card->message()->text().contains(q(message)));
            CHECK(card->activeBox()->isHidden() == (std::string(status) == "completed"));
            CHECK_FALSE(card->doneButton()->isHidden());
            CHECK(card->keepLaterButton()->isHidden());
        }
    }
}

TEST_CASE("test_empty_meeting_stays_visible_and_disables_speaker_rerun", "[widget]") {
    // A zero-content result cannot look successful or rerun speakers.
    TabFixture tab;
    showResult(tab,
               {{"status", "completed"},
                {"message", "Final insights are ready."},
                {"content_summary",
                 {{"meeting_status", "failed"},
                  {"is_empty", true},
                  {"has_audio", false},
                  {"has_transcript", false},
                  {"can_rerun_speakers", false}}}},
               "failed");
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK(card->title()->text() == q("Meeting Failed"));
    CHECK_FALSE(card->activeBox()->isHidden());
    CHECK(card->message()->text().contains(q("No audio or transcript")));
    CHECK_FALSE(card->retrySpeakersButton()->isEnabled());
    CHECK(card->retrySpeakersButton()->toolTip().contains(q("No system-audio")));
}

TEST_CASE("test_unavailable_and_failed_use_warning_tone", "[widget]") {
    // Unavailable and failed stay persistent warnings without dialogs.
    for (const auto& [status, message] :
         {std::pair<const char*, const char*>{"failed", "Final insights failed: boom"},
          std::pair<const char*, const char*>{"unavailable", "Final insights could not run."}}) {
        DYNAMIC_SECTION(status) {
            TabFixture tab;
            showResult(tab, {{"status", status}, {"message", message}});
            const fv::FinalizationCard* card = tab->finalizationCard();
            CHECK(card->property("finalizationTone").toString() == q("warning"));
            CHECK_FALSE(card->isHidden());
            CHECK_FALSE(tab->startButton()->isHidden());
            CHECK(tab->startButton()->isEnabled());
            CHECK(tab->slotNote()->text().contains(q("Past Meetings")));
            CHECK_FALSE(card->keepLaterButton()->isHidden());
            CHECK(card->doneButton()->isHidden());
            CHECK(card->keepHint()->text().contains(q("stay in Past Meetings")));
        }
    }
}

TEST_CASE("test_continue_in_background_appears_after_local_pass_and_emits", "[widget]") {
    TabFixture tab;
    std::vector<std::string> order;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::backgroundRequested,
                     [&] { order.push_back("background"); });
    tab->setMeetingState(
        Json{{"active", false}, {"status", "ended"}, {"finalization", {{"status", "running"}}}});
    QPushButton* button = tab->finalizationCard()->backgroundButton();
    CHECK(button->isHidden());
    tab->setMeetingState(Json{{"background_available", true}});
    CHECK_FALSE(button->isHidden());
    CHECK(button->text() == q("Continue in the background"));
    button->click();
    CHECK(order == std::vector<std::string>{"background"});

    // Start is usable once the wrap-up can leave the stage, and sends it to the background
    // before asking for the new meeting.
    order.clear();
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::startRequested,
                     [&](bool, const QString&) { order.push_back("start"); });
    CHECK(tab->startButton()->isEnabled());
    CHECK(tab->slotNote()->text().contains(q("background")));
    tab->startButton()->click();
    CHECK(order == std::vector<std::string>{"background", "start"});

    tab->setMeetingState(Json{{"finalization", nullptr},
                              {"meeting_id", nullptr},
                              {"background_available", false},
                              {"background_message", "1 meeting finishing in the background."}});
    CHECK_FALSE(tab->idleCard()->isHidden());
    CHECK(tab->finalizationCard()->isHidden());
    CHECK_FALSE(tab->backgroundNotice()->isHidden());
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    tab->setMeetingState(Json{{"background_message", "Background meeting finished."}});
    CHECK_FALSE(tab->sessionCard()->isHidden());
    CHECK(tab->finalizationCard()->isHidden());
    CHECK(tab->endButton()->isEnabled());
}

TEST_CASE("test_incomplete_result_groups_actions_by_context", "[widget]") {
    TabFixture tab;
    tab->setMeetingState(
        Json{{"display_title", "Team sync"}, {"finalization", {{"status", "failed"}}}});
    tab->resize(650, 560);
    tab->show();
    QApplication::processEvents();
    const fv::FinalizationCard* card = tab->finalizationCard();

    CHECK(tab->intro()->isHidden());
    CHECK(top(*tab.tab, card->reportButton()) < top(*tab.tab, card->title()));
    CHECK(top(*tab.tab, card->retryButton()) < top(*tab.tab, card->message()));
    CHECK(top(*tab.tab, card->message()) < top(*tab.tab, card->keepLaterButton()));
    // The result is on the stage; the next meeting's switch stays in the column.
    CHECK(tab->stageState() == q("result"));
    CHECK_FALSE(tab->cloudCheckbox()->isHidden());
    CHECK(tab->cloudCheckbox()->text() == q("AI insights for next meeting"));

    tab->setMeetingState(Json{{"finalization", nullptr}});
    CHECK_FALSE(tab->intro()->isHidden());
    CHECK(tab->cloudCheckbox()->text() == q("AI insights"));
    tab->hide();
}

TEST_CASE("test_starting_clears_previous_finalization", "[widget]") {
    // A subsequent start payload clears the previous result card.
    TabFixture tab;
    tab->setMeetingState(Json{{"active", false},
                              {"status", "ended"},
                              {"finalization", {{"status", "completed"}, {"message", "done"}}}});
    tab->setMeetingState(Json{{"status", "starting"}, {"active", false}});
    QApplication::processEvents();
    CHECK_FALSE(tab->finalizationStatus().has_value());
    CHECK(tab->finalizationCard()->isHidden());
}

TEST_CASE("test_finalization_open_report_remains_wired_after_ended", "[widget]") {
    TabFixture tab;
    int clicked = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::openReportRequested, [&] { ++clicked; });
    showResult(tab, {{"status", "completed"}, {"message", "Done"}}, "ended",
               {{"dashboard_available", true}});
    tab->finalizationCard()->reportButton()->click();
    CHECK(clicked == 1);
}

TEST_CASE("test_failed_finalization_shows_retry_button_and_emits_signal", "[widget]") {
    TabFixture tab;
    int clicked = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::retryInsightsRequested,
                     [&] { ++clicked; });
    showResult(tab, {{"status", "failed"}, {"message", "Final insights failed: RPC timeout"}},
               "ended", {{"dashboard_available", true}});
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK_FALSE(card->retryButton()->isHidden());
    CHECK(card->retryButton()->isEnabled());
    CHECK(card->retryButton()->text() == q("Retry failed steps"));
    CHECK_FALSE(card->retrySpeakersButton()->isHidden());
    card->retryButton()->click();
    CHECK(clicked == 1);

    showResult(tab, {{"status", "running"}, {"message", "Re-running final insights…"}});
    CHECK(card->retryButton()->isHidden());
}

namespace {

void completedWithSpeakerGate(TabFixture& tab, bool offered, const char* reason,
                              Json steps = Json::array()) {
    showResult(tab, {{"status", "completed"},
                     {"message", "Final insights are ready."},
                     {"steps", steps},
                     {"content_summary",
                      {{"meeting_status", "ended"},
                       {"has_audio", true},
                       {"has_transcript", true},
                       {"can_rerun_speakers", true},
                       {"speaker_rerun_offered", offered},
                       {"speaker_rerun_reason", reason}}}});
}

} // namespace

TEST_CASE("test_on_device_labels_hide_speaker_rerun", "[widget]") {
    // Nothing offers an upload the settings would refuse.
    TabFixture tab;
    completedWithSpeakerGate(tab, false, "Speaker identification is not set to OpenAI.");
    CHECK(tab->finalizationCard()->retrySpeakersButton()->isHidden());
}

TEST_CASE("test_missing_consent_disables_speaker_rerun_with_reason", "[widget]") {
    TabFixture tab;
    completedWithSpeakerGate(tab, true, "Audio-upload consent has not been given.");
    QPushButton* button = tab->finalizationCard()->retrySpeakersButton();
    CHECK_FALSE(button->isHidden());
    CHECK_FALSE(button->isEnabled());
    CHECK(button->toolTip() == q("Audio-upload consent has not been given."));
}

TEST_CASE("test_eligible_speaker_rerun_is_enabled", "[widget]") {
    TabFixture tab;
    completedWithSpeakerGate(tab, true, "");
    QPushButton* button = tab->finalizationCard()->retrySpeakersButton();
    CHECK_FALSE(button->isHidden());
    CHECK(button->isEnabled());
}

TEST_CASE("test_refused_speaker_step_rerun_is_disabled_with_reason", "[widget]") {
    TabFixture tab;
    completedWithSpeakerGate(tab, false, "Speaker identification is not set to OpenAI.",
                             Json::array({{{"id", "speaker_id"},
                                           {"name", "Speaker Identification"},
                                           {"status", "completed"},
                                           {"detail", "Updated 3 speaker labels"}},
                                          {{"id", "finalize"},
                                           {"name", "State Finalization"},
                                           {"status", "completed"},
                                           {"detail", "Done"}}}));
    std::vector<QPushButton*> actions;
    for (QPushButton* button : stepActions(*tab)) {
        if (button->accessibleName() == q("Run again Speaker Identification")) {
            actions.push_back(button);
        }
    }
    REQUIRE(actions.size() == 1);
    CHECK_FALSE(actions[0]->isEnabled());
    CHECK(actions[0]->toolTip() == q("Speaker identification is not set to OpenAI."));
}

TEST_CASE("test_multi_step_progress_rendering", "[widget]") {
    // Multi-step finalization renders the step badge, determinate progress, details and rows.
    TabFixture tab;
    const Json steps = Json::array({{{"id", "redecode"},
                                     {"name", "Audio Re-transcription"},
                                     {"status", "completed"},
                                     {"detail", "Re-decoded 8 windows"}},
                                    {{"id", "polish"},
                                     {"name", "Transcript Cleanup"},
                                     {"status", "running"},
                                     {"detail", "Polishing block 1 of 2"}},
                                    {{"id", "consolidation"},
                                     {"name", "Summary & Action Items"},
                                     {"status", "pending"},
                                     {"detail", "Queued"}},
                                    {{"id", "finalize"},
                                     {"name", "State Finalization"},
                                     {"status", "pending"},
                                     {"detail", "Queued"}}});
    showResult(tab,
               {{"status", "running"},
                {"message", "Cleaning transcript…"},
                {"stage", "polish"},
                {"current_step", 2},
                {"total_steps", 4},
                {"step_details", "Polishing block 1 of 2 (40 segments)..."},
                {"steps", steps}},
               "ended", {{"dashboard_available", true}});
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK_FALSE(card->isHidden());
    CHECK_FALSE(card->progress()->isHidden());
    CHECK(card->progress()->minimum() == 0);
    CHECK(card->progress()->maximum() == 100);
    CHECK(card->progress()->value() > 0);
    CHECK(card->stepBadge()->text() == q("Step 2 of 4"));
    CHECK_FALSE(card->stepBadge()->isHidden());
    CHECK_FALSE(card->detail()->isHidden());
    CHECK(card->detail()->text() == q("Polishing block 1 of 2 (40 segments)..."));
    CHECK_FALSE(card->stepsWidget()->isHidden());
    CHECK(card->stepsLayout()->count() == 4);
}

TEST_CASE("test_running_step_without_detail_reads_in_progress", "[widget]") {
    // The blue running dot and the row's label agree even with no detail.
    TabFixture tab;
    showResult(
        tab,
        {{"status", "running"},
         {"steps",
          Json::array(
              {{{"id", "redecode"}, {"name", "Audio Re-transcription"}, {"status", "completed"}},
               {{"id", "consolidation"}, {"name", "Summary & Action Items"}, {"status", "running"}},
               {{"id", "finalize"}, {"name", "State Finalization"}, {"status", "pending"}}})}},
        "ended", {{"dashboard_available", true}});
    QStringList details;
    for (const QLabel* label : tab->finalizationCard()->stepsWidget()->findChildren<QLabel*>()) {
        if (label->objectName() == q("meetingFinalizationStepDetail")) {
            details.push_back(label->text());
        }
    }
    CHECK(details == QStringList{q("Done"), q("In progress"), q("Queued")});
}

TEST_CASE("test_completed_hides_recap_and_keeps_checklist", "[widget]") {
    // Completed finalization keeps the checklist and hides the recap boxes.
    TabFixture tab;
    showResult(tab,
               {{"status", "completed"},
                {"message", "Final insights ready — 32 segments, 4 key points, 2 action items, 1 "
                            "decisions."},
                {"stage", "complete"},
                {"current_step", 3},
                {"total_steps", 3},
                {"step_details", "All finalization passes completed successfully."},
                {"steps", Json::array({{{"id", "redecode"},
                                        {"name", "Audio Re-transcription"},
                                        {"status", "completed"},
                                        {"detail", "Done"}},
                                       {{"id", "consolidation"},
                                        {"name", "Summary & Action Items"},
                                        {"status", "completed"},
                                        {"detail", "Done"}},
                                       {{"id", "finalize"},
                                        {"name", "State Finalization"},
                                        {"status", "completed"},
                                        {"detail", "Done"}}})},
                {"summary_stats",
                 {{"duration_s", 150.0},
                  {"segments", 32},
                  {"words", 520},
                  {"key_points", 4},
                  {"action_items", 2},
                  {"decisions", 1}}}},
               "ended", {{"dashboard_available", true}});
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK_FALSE(card->isHidden());
    CHECK(card->progress()->isHidden());
    CHECK(card->stepBadge()->isHidden());
    CHECK(card->title()->text() == q("Final Insights Ready"));
    CHECK(card->activeBox()->isHidden());
    CHECK(card->findChild<QWidget*>(q("meetingFinalizationStatsWidget")) == nullptr);
    CHECK_FALSE(card->stepsWidget()->isHidden());
    CHECK(card->stepsLayout()->count() == 3);
}

TEST_CASE("test_failed_redecode_under_completed_shows_retry_controls", "[widget]") {
    // A failed checklist row stays retryable even when the overall status is completed.
    TabFixture tab;
    std::vector<QString> clicked;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::retryStepRequested,
                     [&](const QString& id) { clicked.push_back(id); });
    const Json steps = Json::array(
        {{{"id", "redecode"},
          {"name", "Audio Re-transcription"},
          {"status", "failed"},
          {"detail", "Re-decoding failed; kept live transcript"}},
         {{"id", "polish"}, {"name", "Transcript Cleanup"}, {"status", "completed"}},
         {{"id", "consolidation"}, {"name", "Summary & Action Items"}, {"status", "completed"}},
         {{"id", "finalize"}, {"name", "State Finalization"}, {"status", "completed"}}});
    showResult(tab,
               {{"status", "completed"},
                {"message", "Final insights ready — 12 segments."},
                {"steps", steps}},
               "ended", {{"dashboard_available", true}});
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK(card->stepBadge()->isHidden());
    CHECK(card->title()->text() == q("Meeting Finished With Issues"));
    CHECK_FALSE(card->retryButton()->isHidden());
    QStringList labels;
    QPushButton* retry = nullptr;
    for (QPushButton* button : stepActions(*tab)) {
        labels.push_back(button->text());
        if (button->text() == q("Retry")) {
            retry = button;
        }
    }
    CHECK(labels.contains(q("Retry")));
    CHECK(labels.contains(q("Run again")));
    REQUIRE(retry != nullptr);
    retry->click();
    CHECK(clicked == std::vector<QString>{q("redecode")});

    showResult(tab,
               {{"status", "running"}, {"message", "Re-transcribing meeting…"}, {"steps", steps}});
    CHECK(card->retryButton()->isHidden());
    std::vector<QPushButton*> runningActions;
    for (QWidget* row : stepRows(*tab)) {
        if (row != nullptr) {
            for (QPushButton* button : row->findChildren<QPushButton*>()) {
                runningActions.push_back(button);
            }
        }
    }
    CHECK(runningActions.empty());
}

TEST_CASE("test_incomplete_card_keeps_start_and_emits_keep_later", "[widget]") {
    // An incomplete card never takes Start away from the column.
    TabFixture tab;
    int deferred = 0;
    int backgrounded = 0;
    std::vector<bool> started;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::deferInsightsRequested,
                     [&] { ++deferred; });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::startRequested,
                     [&](bool cloud, const QString&) { started.push_back(cloud); });
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::backgroundRequested,
                     [&] { ++backgrounded; });
    tab->cloudCheckbox()->setChecked(true);
    showResult(tab, {{"status", "failed"}, {"message", "Final insights were interrupted."}});

    CHECK_FALSE(tab->startButton()->isHidden());
    CHECK(tab->startButton()->isEnabled());
    CHECK_FALSE(tab->briefPanel()->isHidden());
    CHECK_FALSE(tab->finalizationCard()->keepLaterButton()->isHidden());
    tab->finalizationCard()->keepLaterButton()->click();
    tab->startButton()->click();
    CHECK(deferred == 1);
    CHECK(started == std::vector<bool>{true});
    CHECK(backgrounded == 0);
}

TEST_CASE("test_recording_elsewhere_holds_start_in_every_idle_state", "[widget]") {
    TabFixture tab;
    tab->setRecordingActive(true);
    CHECK_FALSE(tab->startButton()->isEnabled());
    tab->setMeetingState(Json{{"finalization", {{"status", "failed"}}}});
    CHECK_FALSE(tab->startButton()->isEnabled());
    tab->setRecordingActive(false);
    CHECK(tab->startButton()->isEnabled());
}

TEST_CASE("test_completed_card_keeps_idle_start_and_emits_done", "[widget]") {
    // A clean completed card keeps idle Start and offers Done.
    TabFixture tab;
    int deferred = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::deferInsightsRequested,
                     [&] { ++deferred; });
    showResult(tab, {{"status", "completed"}, {"message", "Final insights are ready."}});
    CHECK_FALSE(tab->startButton()->isHidden());
    CHECK(tab->slotNote()->isHidden());
    CHECK(tab->finalizationCard()->keepLaterButton()->isHidden());
    CHECK_FALSE(tab->finalizationCard()->doneButton()->isHidden());
    tab->finalizationCard()->doneButton()->click();
    CHECK(deferred == 1);
}

TEST_CASE("test_disabled_card_emits_done", "[widget]") {
    // An AI-insights-off leftover card can be dismissed with Done.
    TabFixture tab;
    int deferred = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::deferInsightsRequested,
                     [&] { ++deferred; });
    showResult(tab, {{"status", "disabled"}, {"message", "AI insights are off for this meeting."}});
    CHECK_FALSE(tab->startButton()->isHidden());
    CHECK_FALSE(tab->finalizationCard()->doneButton()->isHidden());
    tab->finalizationCard()->doneButton()->click();
    CHECK(deferred == 1);
}

TEST_CASE("test_leftover_card_shows_identity_and_report_button", "[widget]") {
    // A selected past meeting shows title, time and Open report.
    TabFixture tab;
    int opened = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::openReportRequested, [&] { ++opened; });
    tab->setMeetingState(Json{
        {"active", false},
        {"status", "ended"},
        {"meeting_id", "m_done"},
        {"display_title", "Quarterly roadmap"},
        {"started_at", "2025-01-02T09:30:00"},
        {"ended_at", "2025-01-02T10:12:00"},
        {"paused_total_s", 120},
        {"insights_pill", "Ready"},
        {"insights_tone", "success"},
        {"finalization", {{"status", "completed"}, {"message", "Final insights are ready."}}}});
    QApplication::processEvents();
    const fv::FinalizationCard* card = tab->finalizationCard();
    CHECK_FALSE(card->identityBox()->isHidden());
    CHECK(card->identityTitle()->text() == q("Quarterly roadmap"));
    CHECK(card->identityMeta()->text().contains(q("Jan 02, 2025")));
    CHECK(card->identityMeta()->text().contains(q("40 min")));
    CHECK_FALSE(card->reportButton()->isHidden());
    CHECK(card->reportButton()->isEnabled());
    card->reportButton()->click();
    CHECK(opened == 1);
}

TEST_CASE("test_empty_meeting_disables_open_report", "[widget]") {
    // Empty leftover meetings have no report to open.
    TabFixture tab;
    tab->setMeetingState(Json{{"active", false},
                              {"status", "failed"},
                              {"display_title", "Failed meeting"},
                              {"finalization",
                               {{"status", "completed"},
                                {"message", "Final insights are ready."},
                                {"content_summary",
                                 {{"meeting_status", "failed"},
                                  {"is_empty", true},
                                  {"has_audio", false},
                                  {"has_transcript", false},
                                  {"can_rerun_speakers", false}}}}}});
    QApplication::processEvents();
    CHECK_FALSE(tab->finalizationCard()->reportButton()->isEnabled());
    CHECK(tab->finalizationCard()->reportButton()->toolTip().contains(q("No report")));
}

// ---- FioriVoce's seams (no OpenWhisper test pins them) ----

TEST_CASE("the empty stage reads its meetings off the GUI thread and keeps the newest read",
          "[widget]") {
    TabFixture tab;
    fv::ManualExecutor io;
    int reads = 0;
    int wanted = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::recentMeetingsWanted, [&] { ++wanted; });
    tab->setRecentMeetingsReader(io, [&reads] {
        ++reads;
        return fv::finishedRecentMeetings(
            {Json{{"id", "live"}, {"status", "active"}},
             Json{{"id", "m1"}, {"title", "One"}, {"status", "ended"}},
             Json{{"id", "m2"}, {"title", "Two"}, {"status", "PAUSED"}},
             Json{{"id", "m3"}, {"title", "Three"}, {"status", "ended"}},
             Json{{"id", "m4"}, {"title", "Four"}, {"status", "failed"}},
             Json{{"id", "m5"}, {"title", "Five"}, {"status", "ended"}}});
    });
    tab->show();
    QApplication::processEvents();
    CHECK(wanted >= 1);
    // Nothing is read on the GUI thread.
    CHECK(reads == 0);
    CHECK(tab->recentBox()->isHidden());

    // A window activation asks again before the first read is delivered: that read is dropped.
    io.run_all();
    CHECK(reads >= 1);
    const int asked = wanted;
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(tab.tab.get(), &activate);
    CHECK(wanted == asked + 1);
    QApplication::processEvents();
    CHECK(tab->recentRows().empty());
    io.run_all();
    QApplication::processEvents();
    std::vector<QString> ids;
    for (const fv::RecentMeetingRow* row : tab->recentRows()) {
        ids.push_back(row->meetingId());
    }
    CHECK(ids == std::vector<QString>{q("m1"), q("m3"), q("m4")});
    CHECK_FALSE(tab->recentBox()->isHidden());
    tab->hide();
}

TEST_CASE("a failed recent read leaves the empty stage without rows", "[widget]") {
    TabFixture tab;
    fv::ManualExecutor io;
    tab->setRecentMeetingsReader(
        io, []() -> std::vector<Json> { throw std::runtime_error("database is locked"); });
    tab->showRecentMeetings({Json{{"id", "old"}, {"status", "ended"}}});
    tab->show();
    QApplication::processEvents();
    io.run_all();
    QApplication::processEvents();
    CHECK(tab->recentRows().empty());
    CHECK(tab->recentBox()->isHidden());
    tab->hide();
}

TEST_CASE("a recent row names its meeting and opens on a click", "[widget]") {
    fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 11 / 14},
                        "America/New_York");
    fv::RecentMeetingRow row(Json{{"id", "m_failed"},
                                  {"title", ""},
                                  {"status", "failed"},
                                  {"started_at", "2026-11-10T14:00:00"},
                                  {"ended_at", "2026-11-10T14:00:20"}},
                             clock);
    CHECK(row.titleLabel()->text() == q("Failed meeting"));
    CHECK(row.metaLabel()->text() == q("Nov 10, 2026 · 02:00 PM · 20 sec"));
    REQUIRE(row.pill() != nullptr);
    CHECK(row.pill()->text() == q("Failed start"));
    CHECK(row.pill()->property("tone").toString() == q("warning"));
    CHECK(row.accessibleName() == q("Open Failed meeting. Nov 10, 2026 · 02:00 PM · 20 sec"));
    std::vector<QString> opened;
    QObject::connect(&row, &fv::RecentMeetingRow::clicked,
                     [&](const QString& id) { opened.push_back(id); });
    row.resize(300, 50);
    QTest::mouseClick(&row, Qt::LeftButton, {}, QPoint(10, 10));
    QTest::keyClick(&row, Qt::Key_Space);
    CHECK(opened == std::vector<QString>{q("m_failed"), q("m_failed")});
}

TEST_CASE("without AI insights the section and the brief are hidden and the start is plain",
          "[widget]") {
    TabFixture tab;
    Starts starts(tab.tab.get());
    std::vector<bool> demos;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::demoRequested,
                     [&](bool cloud) { demos.push_back(cloud); });
    tab->cloudCheckbox()->setChecked(true);
    tab->briefInput()->setPlainText(q("Capture every objection."));
    tab->setAiInsightsAvailable(false);
    CHECK(tab->aiPanel()->isHidden());
    CHECK(tab->briefPanel()->isHidden());
    auto* overline = tab->column()->findChild<QLabel*>(q("meetingOverline"));
    REQUIRE(overline != nullptr);
    CHECK(overline->isHidden());
    CHECK(tab->meetingBrief().isEmpty());
    tab->startButton()->click();
    CHECK(starts.received == std::vector<std::pair<bool, QString>>{{false, QString()}});
    tab->setDeveloperMode(true);
    tab->demoButton()->click();
    CHECK(demos == std::vector<bool>{false});
    // A payload's cloud choice keeps the brief hidden while the section is.
    tab->setMeetingState(Json{{"cloud_enabled", true}});
    CHECK(tab->briefPanel()->isHidden());

    tab->setAiInsightsAvailable(true);
    CHECK_FALSE(tab->aiPanel()->isHidden());
    CHECK_FALSE(tab->briefPanel()->isHidden());
}

TEST_CASE("the tab asks for its AI line when shown, on activation and at a start, never live",
          "[widget]") {
    TabFixture tab;
    int asked = 0;
    QObject::connect(tab.tab.get(), &fv::MeetingModeTab::aiDestinationWanted, [&] { ++asked; });
    tab->show();
    QApplication::processEvents();
    CHECK(asked >= 1);
    QEvent activate(QEvent::WindowActivate);
    int before = asked;
    QApplication::sendEvent(tab.tab.get(), &activate);
    CHECK(asked == before + 1);
    before = asked;
    tab->setMeetingState(Json{{"status", "starting"}, {"active", false}});
    CHECK(asked == before + 1);
    tab->setMeetingState(Json{{"status", "active"}, {"active", true}});
    QApplication::sendEvent(tab.tab.get(), &activate);
    CHECK(asked == before + 1);
    // The speech note holds still during a meeting, like the AI line.
    tab->setSpeechNote(q("Parakeet can't transcribe meetings."));
    CHECK(tab->speechDestination()->text() == q("Speech recognition runs on this computer."));
    tab->setMeetingState(Json{{"active", false}, {"status", "idle"}});
    tab->setSpeechNote(q("The API engine can't transcribe meetings, so meetings use Parakeet."));
    CHECK(tab->speechDestination()->text() ==
          q("The API engine can't transcribe meetings, so meetings use Parakeet. Speech "
            "recognition runs on this computer."));
    tab->hide();
}

TEST_CASE("remote meeting speech names its host before a meeting", "[widget]") {
    // refresh_ai_destination's remote branch, with and without a pairing.
    TabFixture tab;
    tab->setRemoteSpeechHost(q("devbox"));
    CHECK(tab->speechDestination()->text() ==
          q("Remote speech · microphone and system audio sent to devbox. Recordings saved here."));
    tab->setRemoteSpeechHost(std::nullopt);
    CHECK(tab->speechDestination()->text() ==
          q("Remote speech · microphone and system audio sent to a host (not paired yet). "
            "Recordings saved here."));
    tab->setMeetingState(Json{{"active", true}, {"status", "active"}});
    tab->setRemoteSpeechHost(q("other"));
    CHECK_FALSE(tab->speechDestination()->text().contains(q("other")));
}

TEST_CASE("the live dot breathes only while live and shown", "[widget]") {
    fv::LiveDot dot;
    dot.setLive(true);
    CHECK_FALSE(dot.timer()->isActive());
    dot.show();
    QApplication::processEvents();
    CHECK(dot.timer()->isActive());
    CHECK(dot.timer()->interval() == 50);
    CHECK(QTest::qWaitFor([&dot] { return dot.phase() > 0.0; }, 2000));
    dot.setLive(false);
    CHECK_FALSE(dot.timer()->isActive());
    CHECK(dot.phase() == 0.0);
    dot.hide();
}

TEST_CASE("the tab asks for the width of the column beside the stage", "[widget]") {
    TabFixture tab;
    CHECK(tab->preferredWidth() == fv::studio::kColumnWidth + fv::studio::kGap + 560);
    CHECK(fv::pageScrollArea(tab.tab.get()) == tab->scrollArea());
}

TEST_CASE("a finalization int() refuses is Python's error", "[widget]") {
    TabFixture tab;
    CHECK_THROWS_AS(tab->setMeetingState(
                        Json{{"finalization", {{"status", "running"}, {"current_step", "two"}}}}),
                    fv::py::ValueError);
    CHECK_FALSE(tab->finalizationStatus().has_value());
}

TEST_CASE("a finalization list() or dict() refuses keeps the result on the stage", "[widget]") {
    // _set_finalization builds its dict before assigning it, so a refused value leaves the last
    // result in place and later payloads apply as usual.
    TabFixture tab;
    showResult(tab, {{"status", "completed"}, {"message", "Final insights are ready."}});
    REQUIRE(tab->finalizationStatus() == q("completed"));
    const auto stillShown = [&tab] {
        CHECK(tab->finalizationStatus() == q("completed"));
        CHECK_NOTHROW(tab->setMeetingState(Json{{"dashboard_available", true}}));
        CHECK(tab->stageState() == q("result"));
    };
    // list(5), dict("x") and dict([1]).
    CHECK_THROWS_AS(
        tab->setMeetingState(Json{{"finalization", {{"status", "failed"}, {"steps", 5}}}}),
        fv::py::TypeError);
    stillShown();
    CHECK_THROWS_AS(tab->setMeetingState(
                        Json{{"finalization", {{"status", "failed"}, {"summary_stats", "x"}}}}),
                    fv::py::ValueError);
    stillShown();
    CHECK_THROWS_AS(
        tab->setMeetingState(
            Json{{"finalization", {{"status", "failed"}, {"content_summary", Json::array({1})}}}}),
        fv::py::TypeError);
    stillShown();
    // What dict() accepts: a list of pairs is the dict they make.
    CHECK_NOTHROW(tab->setMeetingState(
        Json{{"finalization",
              {{"status", "completed"},
               {"content_summary", Json::array({Json::array({"meeting_status", "failed"}),
                                                Json::array({"is_empty", true})})}}}}));
    CHECK(tab->finalizationStatus() == q("completed"));
    CHECK(tab->finalizationCard()->title()->text() == q("Meeting Failed"));
}

TEST_CASE("a new type size re-runs the column and on-air sizes", "[widget]") {
    // changeEvent(StyleChange): the sizes follow a font scale chosen after the tab was built.
    TabFixture tab;
    const int columnAt100 = tab->split()->columnWidth();
    {
        fvtest::AppTheme theme(q("dark"), 130);
        QApplication::processEvents();
        CHECK(tab->split()->columnWidth() == fv::ui::scaled(fv::studio::kColumnWidth));
        CHECK(tab->split()->columnWidth() > columnAt100);
        CHECK(tab->topSlot()->minimumHeight() == fv::ui::scaled(fv::meeting_tab::kTopSlotHeight));
        CHECK(tab->sessionCard()->maximumWidth() == fv::ui::scaled(fv::meeting_tab::kOnAirWidth));
        CHECK(tab->pauseButton()->minimumWidth() ==
              fv::ui::scaled(fv::meeting_tab::kOnAirButtonWidth));
        CHECK(tab->preferredWidth() == tab->split()->columnWidth() + fv::studio::kGap +
                                           fv::ui::scaled(fv::meeting_tab::kStageWidth));
    }
    QApplication::processEvents();
    CHECK(tab->split()->columnWidth() == columnAt100);
}

TEST_CASE("read_recent_meetings gives the three newest finished meetings", "[widget]") {
    fv::testkit::TempDataRoot root;
    fv::FakeClock clock;
    auto database = fv::Database::open_or_create(root.path() / "meetings.db");
    fv::meeting_state::SqlMeetingRepository repository(database, clock);
    const auto meeting = [&](const char* id, const char* status, const char* started) {
        repository.create_meeting({{"id", id},
                                   {"title", id},
                                   {"status", status},
                                   {"started_at", started},
                                   {"host_token", std::string("host-") + id},
                                   {"guest_token", std::string("guest-") + id},
                                   {"cloud_enabled", false},
                                   {"spool_dir", ""}});
    };
    meeting("m1", "ended", "2026-11-10T09:00:00Z");
    meeting("m2", "failed", "2026-11-11T09:00:00Z");
    meeting("m3", "ended", "2026-11-12T09:00:00Z");
    meeting("m4", "ended", "2026-11-13T09:00:00Z");
    meeting("m5", "active", "2026-11-14T09:00:00Z");
    std::vector<std::string> ids;
    for (const Json& row : fv::readRecentMeetings(repository)) {
        ids.push_back(row.at("id").get<std::string>());
    }
    CHECK(ids == std::vector<std::string>{"m4", "m3", "m2"});
}
