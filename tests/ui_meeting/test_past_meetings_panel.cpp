// The Past Meetings list and its cards (OpenWhisper's tests/test_past_meetings_panel.py,
// tests/test_past_meetings_show_in_folder.py and the meeting test of
// tests/test_desktop_performance.py). Loads run on a ManualExecutor the test drains.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "meeting_state/time_utils.h"
#include "past_meetings_support.h"
#include "testkit/testkit.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_panel.h"

using fv::Json;
using fv::PastMeetingItem;
using fv::PastMeetingsPanel;
using fvtest::meetingRow;
using fvtest::PanelFixture;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

QAction* findAction(QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

// _list_cards of the Python tests: the cards the list layout holds.
std::vector<PastMeetingItem*> listCards(const PastMeetingsPanel& panel) {
    return panel.cards();
}

// The count line and placeholder labels: findChild by name, as the Python tests do.
QLabel* labelNamed(PastMeetingsPanel& panel, const char* name) {
    return panel.findChild<QLabel*>(QString::fromLatin1(name));
}

void setSearchText(PastMeetingsPanel& panel, const QString& text) {
    panel.searchInput()->blockSignals(true);
    panel.searchInput()->setText(text);
    panel.searchInput()->blockSignals(false);
}

} // namespace

// from
// tests/test_past_meetings_panel.py::test_panel_filters_live_sessions_and_emits_selected_meeting
TEST_CASE("test_panel_filters_live_sessions_and_emits_selected_meeting",
          "[test_past_meetings_panel][widget]") {
    PanelFixture fixture(
        {meetingRow("m_live", "active"), meetingRow("m_done", "ended", "Planning review")});
    std::vector<QString> selected;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::meetingSelected,
                     [&selected](const QString& id) { selected.push_back(id); });

    fixture.refresh();
    const auto cards = listCards(*fixture.panel);

    REQUIRE(cards.size() == 1);
    CHECK(cards[0]->meetingId() == q("m_done"));
    CHECK(cards[0]->titleLabel()->text() == q("Planning review"));
    CHECK(cards[0]->detailLabel()->text().contains(q("40 min")));
    CHECK(cards[0]->findChild<QPushButton*>(q("pastMeetingOpenButton")) == nullptr);
    QTest::mouseClick(cards[0], Qt::LeftButton);
    CHECK(selected == std::vector<QString>{q("m_done")});
    CHECK(cards[0]->property("selected").toBool());
}

// from tests/test_past_meetings_panel.py::test_panel_shows_empty_state
TEST_CASE("test_panel_shows_empty_state", "[test_past_meetings_panel][widget]") {
    PanelFixture fixture;

    fixture.refresh();

    QLabel* empty = labelNamed(*fixture.panel, "pastMeetingsEmpty");
    REQUIRE(empty != nullptr);
    CHECK(empty->text() == q("No past meetings yet"));
}

// from tests/test_past_meetings_panel.py::test_panel_shows_insights_pills
TEST_CASE("test_panel_shows_insights_pills", "[test_past_meetings_panel][widget]") {
    PanelFixture fixture(
        {meetingRow(
             "m_saved", "ended", "Deferred review",
             Json{{"status", "failed"}, {"message", "interrupted"}, {"card_deferred", true}}),
         meetingRow("m_ready", "ended", "Ready review",
                    Json{{"status", "completed"}, {"message", "done"}})});

    fixture.refresh();
    const auto cards = listCards(*fixture.panel);

    REQUIRE(cards.size() == 2);
    CHECK(cards[0]->meetingId() == q("m_saved"));
    CHECK(cards[1]->meetingId() == q("m_ready"));
    CHECK(cards[0]->insightsPill()->text() == q("Saved for later"));
    CHECK(cards[1]->insightsPill()->text() == q("Ready"));
    CHECK(cards[0]->detailLabel()->text().contains(q("40 min")));
    CHECK_FALSE(cards[0]->detailLabel()->text().contains(q("Ended")));
}

// from tests/test_past_meetings_panel.py::test_failed_and_empty_meetings_are_labeled_honestly
TEST_CASE("test_failed_and_empty_meetings_are_labeled_honestly",
          "[test_past_meetings_panel][widget]") {
    Json failed = meetingRow("m_failed", "failed", "");
    failed["content_summary"] = {
        {"is_empty", true}, {"has_audio", false}, {"has_transcript", false}};
    PanelFixture fixture({failed});

    fixture.refresh();
    const auto cards = listCards(*fixture.panel);

    REQUIRE(cards.size() == 1);
    CHECK(cards[0]->titleLabel()->text() == q("Failed meeting"));
    CHECK(cards[0]->contentLabel()->text() == q("Meeting failed to start"));
    CHECK(cards[0]->detailLabel()->text().contains(q("Failed")));
    CHECK(cards[0]->insightsPill()->text() == q("Failed start"));
}

// from tests/test_past_meetings_panel.py::test_untitled_row_shows_topic_as_title
TEST_CASE("test_untitled_row_shows_topic_as_title", "[test_past_meetings_panel][widget]") {
    Json row = meetingRow("m_topic", "ended", "");
    row["state_json"] = Json{{"meeting_id", "m_topic"},
                             {"title", ""},
                             {"topic", {{"current", "Quarterly roadmap"}}}}
                            .dump();
    PanelFixture fixture({row});

    fixture.refresh();
    const auto cards = listCards(*fixture.panel);

    REQUIRE(cards.size() == 1);
    CHECK(cards[0]->titleLabel()->text() == q("Quarterly roadmap"));
}

// from tests/test_past_meetings_panel.py::test_panel_search_filters_and_empty_copy
TEST_CASE("test_panel_search_filters_and_empty_copy", "[test_past_meetings_panel][widget]") {
    Json planning = meetingRow("m_plan", "ended", "Planning review");
    planning["content_summary"] = {{"has_transcript", true},
                                   {"preview_text", "We should ship the sidebar polish next."}};
    Json standup = meetingRow("m_stand", "ended", "Standup");
    standup["content_summary"] = {{"has_transcript", false}, {"preview_text", ""}};
    PanelFixture fixture({planning, standup});
    fixture.refresh();

    QLabel* header = labelNamed(*fixture.panel, "sectionHeader");
    REQUIRE(header != nullptr);
    CHECK(header->text() == q("2 meetings"));

    std::vector<QString> copied;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::copyTranscriptRequested,
                     [&copied](const QString& id) { copied.push_back(id); });
    const auto cards = listCards(*fixture.panel);
    REQUIRE(cards.size() == 2);
    CHECK(cards[0]->previewLabel()->text().startsWith(q("We should ship")));
    Q_EMIT cards[0]->copyTranscriptRequested(cards[0]->meetingId());
    CHECK(copied == std::vector<QString>{q("m_plan")});

    setSearchText(*fixture.panel, q("planning"));
    fixture.panel->rebuildList();
    CHECK(fixture.cardIds() == std::vector<std::string>{"m_plan"});

    setSearchText(*fixture.panel, q("no-such-meeting"));
    fixture.panel->rebuildList();
    CHECK(listCards(*fixture.panel).empty());
    QLabel* empty = labelNamed(*fixture.panel, "pastMeetingsEmpty");
    REQUIRE(empty != nullptr);
    CHECK(empty->text() == q("No matching meetings"));
}

// from
// tests/test_past_meetings_panel.py::test_repository_refresh_runs_off_ui_thread_and_requests_one_page
TEST_CASE("test_repository_refresh_runs_off_ui_thread_and_requests_one_page",
          "[test_past_meetings_panel][widget]") {
    Json row = meetingRow("m_async", "ended", "Async review");
    row["content_summary"] = {
        {"has_audio", false}, {"has_transcript", false}, {"is_empty", true}, {"preview_text", ""}};
    PanelFixture fixture({row}, true);

    fixture.refresh();

    REQUIRE(fixture.calls.size() == 1);
    CHECK(fixture.calls[0].limit == PastMeetingsPanel::kMaxMeetings + 1);
    CHECK(fixture.calls[0].query.isEmpty());
    CHECK(fixture.calls[0].thread != std::this_thread::get_id());
    CHECK(fixture.cardIds() == std::vector<std::string>{"m_async"});
}

// from
// tests/test_past_meetings_panel.py::test_subminute_duration_uses_seconds_instead_of_zero_minutes
TEST_CASE("test_subminute_duration_uses_seconds_instead_of_zero_minutes",
          "[test_past_meetings_panel][widget]") {
    const Json meeting = {{"started_at", "2026-08-20T16:55:00Z"},
                          {"ended_at", "2026-08-20T16:55:40Z"},
                          {"paused_total_s", 0}};

    CHECK(fv::meeting_state::format_meeting_duration(meeting) == "40 sec");
    PastMeetingItem card(meeting);
    CHECK(card.detailLabel()->text() == q("40 sec"));
}

// from
// tests/test_desktop_performance.py::test_meeting_refresh_reuses_cards_and_preserves_selected_card
TEST_CASE("test_meeting_refresh_reuses_cards_and_preserves_selected_card",
          "[test_desktop_performance][widget]") {
    std::vector<Json> meetings;
    for (int index = 0; index < 3; ++index) {
        meetings.push_back(Json{{"id", std::to_string(index)},
                                {"title", "Meeting " + std::to_string(index)},
                                {"status", "ended"},
                                {"content_summary", {{"has_transcript", true}}}});
    }
    PanelFixture fixture(meetings);
    fixture.refresh();
    const auto cards = [&fixture] {
        std::map<std::string, QWidget*> made;
        for (const auto& [key, entry] : fixture.panel->meetingCards()) {
            made[key] = entry.card;
        }
        return made;
    };
    const auto before = cards();
    fixture.panel->setSelectedMeetingId(q("1"));
    fixture.refresh();
    CHECK(cards() == before);
    CHECK(fixture.panel->meetingCards().at("1").card->property("selected").toBool());

    fixture.rows[0]["title"] = "Changed title";
    fixture.refresh();
    CHECK(cards().at("0") != before.at("0"));
    CHECK(cards().at("1") == before.at("1"));
    CHECK(fixture.panel->findChildren<PastMeetingItem*>().size() == 3);
}

// ---- tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction ----

namespace {

Json folderMeeting(const std::optional<std::string>& spoolDir) {
    Json row = {{"id", "m_abc1234567"},
                {"title", "Review"},
                {"status", "ended"},
                {"started_at", "2025-01-02T09:30:00"},
                {"content_summary", {{"has_transcript", true}, {"preview_text", "hello"}}}};
    if (spoolDir) {
        row["spool_dir"] = *spoolDir;
    }
    return row;
}

// Moves the working directory, and puts it back.
class ScopedWorkingDirectory {
public:
    explicit ScopedWorkingDirectory(const std::filesystem::path& folder)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(folder);
    }
    ~ScopedWorkingDirectory() { std::filesystem::current_path(previous_); }
    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

private:
    std::filesystem::path previous_;
};

} // namespace

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_action_is_enabled_when_the_spool_folder_exists
TEST_CASE("test_action_is_enabled_when_the_spool_folder_exists",
          "[test_past_meetings_show_in_folder][widget]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path spool = root.path() / "m_abc1234567";
    std::filesystem::create_directories(spool);
    std::ofstream(spool / "mic_session.wav", std::ios::binary) << "RIFF";

    PastMeetingItem card(folderMeeting(spool.generic_string()));
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QAction* action = findAction(*menu, q("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK(action->isEnabled());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_action_is_listed_but_disabled_without_a_spool_dir
TEST_CASE("test_action_is_listed_but_disabled_without_a_spool_dir",
          "[test_past_meetings_show_in_folder][widget]") {
    PastMeetingItem card(folderMeeting(std::nullopt));
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QAction* action = findAction(*menu, q("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK_FALSE(action->isEnabled());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_action_is_disabled_once_the_recordings_are_cleared
TEST_CASE("test_action_is_disabled_once_the_recordings_are_cleared",
          "[test_past_meetings_show_in_folder][widget]") {
    // "Clear history + recordings" removes the folder but the row keeps its spool_dir, so
    // existence, not the string, gates the action.
    fv::testkit::TempDataRoot root;
    PastMeetingItem card(folderMeeting((root.path() / "gone").generic_string()));
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QAction* action = findAction(*menu, q("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK_FALSE(action->isEnabled());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_a_relative_spool_dir_resolves_against_the_working_directory
TEST_CASE("test_a_relative_spool_dir_resolves_against_the_working_directory",
          "[test_past_meetings_show_in_folder][platform]") {
    fv::testkit::TempDataRoot root;
    std::filesystem::create_directories(root.path() / "meetings" / "m_abc1234567");
    const ScopedWorkingDirectory here(root.path());

    PastMeetingItem card(folderMeeting(std::string("meetings/m_abc1234567")));
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QAction* action = findAction(*menu, q("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK(action->isEnabled());
    CHECK(card.spoolDir().is_absolute());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_triggering_opens_the_meeting_folder
TEST_CASE("test_triggering_opens_the_meeting_folder",
          "[test_past_meetings_show_in_folder][widget]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path spool = root.path() / "m_abc1234567";
    std::filesystem::create_directories(spool);
    fvtest::RecordedLaunches launches;

    PastMeetingItem card(folderMeeting(spool.generic_string()));
    std::unique_ptr<QMenu> menu(card.buildContextMenu());
    findAction(*menu, q("Show in Folder"))->trigger();

    // The folder itself is opened, not its parent with the folder selected.
    REQUIRE(launches.urls.size() == 1);
    CHECK(QDir::cleanPath(launches.urls[0].toLocalFile()) ==
          QDir::cleanPath(QString::fromStdWString(spool.wstring())));
    CHECK(launches.commands.empty());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestShowInFolderAction::test_nothing_is_opened_when_the_card_has_no_folder
TEST_CASE("test_nothing_is_opened_when_the_card_has_no_folder",
          "[test_past_meetings_show_in_folder][widget]") {
    fvtest::RecordedLaunches launches;
    PastMeetingItem card(folderMeeting(std::nullopt));

    card.showInFolder();

    CHECK(launches.urls.empty());
    CHECK(launches.commands.empty());
}

// ---- what Python's panel does and the tests above do not read ----

TEST_CASE("the loading placeholder shows until the first result", "[past_meetings_panel][widget]") {
    PanelFixture fixture({meetingRow("m_done")});

    fixture.panel->refresh();

    QLabel* loading = labelNamed(*fixture.panel, "pastMeetingsEmpty");
    REQUIRE(loading != nullptr);
    CHECK(loading->text() == q("Loading past meetings…"));
    fixture.io.drain();
    CHECK(fixture.cardIds() == std::vector<std::string>{"m_done"});
}

TEST_CASE("a provider that throws shows the failure placeholder", "[past_meetings_panel][widget]") {
    fvtest::Io io;
    PastMeetingsPanel panel(
        [](int, const QString&) -> std::vector<Json> {
            throw std::runtime_error("database is locked");
        },
        io.executor());

    panel.refresh();
    io.drain();

    QLabel* message = panel.findChild<QLabel*>(q("pastMeetingsEmpty"));
    REQUIRE(message != nullptr);
    CHECK(message->text() == q("Past meetings could not be loaded"));
    CHECK(panel.sectionHeader()->isHidden());
    CHECK(panel.meetings().empty());
}

TEST_CASE("only the newest request's result is shown", "[past_meetings_panel][widget]") {
    PanelFixture fixture({meetingRow("m_old", "ended", "Old")});
    fixture.panel->refresh();
    fixture.rows = {meetingRow("m_new", "ended", "New")};
    fixture.panel->refresh();

    fixture.io.drain();

    CHECK(fixture.cardIds() == std::vector<std::string>{"m_new"});
}

TEST_CASE("typing drops a result in flight and searches after the pause",
          "[past_meetings_panel][widget]") {
    PanelFixture fixture({meetingRow("m_done")}, true);
    fixture.panel->refresh();
    fixture.panel->searchInput()->setText(q("  Plan  "));
    // The pause is the panel's own timer: held while the result in flight is delivered (a
    // loaded machine can take longer than the pause), then let run.
    QTimer* pause = fixture.panel->findChild<QTimer*>();
    REQUIRE(pause != nullptr);
    CHECK(pause->isActive());
    CHECK(pause->interval() == PastMeetingsPanel::kSearchDelayMs);
    pause->stop();

    fixture.io.drain();
    CHECK(fixture.cardIds().empty());

    pause->start(0);
    QTest::qWait(50);
    fixture.io.drain();
    REQUIRE(fixture.calls.size() == 2);
    CHECK(fixture.calls[1].query == q("plan"));
    CHECK(fixture.cardIds() == std::vector<std::string>{"m_done"});
}

TEST_CASE("a hundred and one rows show a hundred cards and say there are more",
          "[past_meetings_panel][widget]") {
    std::vector<Json> rows;
    for (int index = 0; index < 101; ++index) {
        rows.push_back(meetingRow("m_" + std::to_string(index)));
    }
    PanelFixture fixture(rows, true);

    fixture.refresh();

    CHECK(fixture.panel->cards().size() == 100);
    CHECK(fixture.panel->sectionHeader()->text() == q("100+ meetings"));
    const QList<QLabel*> notes = fixture.panel->findChildren<QLabel*>(q("pastMeetingsEmpty"));
    REQUIRE(notes.size() == 1);
    CHECK(notes[0]->text() ==
          q("Showing the first 100 matches — refine your search to find older meetings"));
}

TEST_CASE("selecting a card highlights it and clearing the id clears it",
          "[past_meetings_panel][widget]") {
    PanelFixture fixture({meetingRow("m_a"), meetingRow("m_b")});
    fixture.refresh();

    fixture.panel->setSelectedMeetingId(q("m_b"));
    CHECK_FALSE(fixture.panel->cards()[0]->property("selected").toBool());
    CHECK(fixture.panel->cards()[1]->property("selected").toBool());
    fixture.panel->setSelectedMeetingId(QString());
    CHECK_FALSE(fixture.panel->cards()[1]->property("selected").toBool());
}

TEST_CASE("the card menu copies, deletes and keeps its shape", "[past_meetings_panel][widget]") {
    Json row = folderMeeting(std::nullopt);
    PastMeetingItem card(row);
    std::vector<QString> copied;
    std::vector<QString> deleted;
    QObject::connect(&card, &PastMeetingItem::copyTranscriptRequested,
                     [&copied](const QString& id) { copied.push_back(id); });
    QObject::connect(&card, &PastMeetingItem::deleteRequested,
                     [&deleted](const QString& id) { deleted.push_back(id); });
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QStringList texts;
    for (QAction* action : menu->actions()) {
        if (!action->isSeparator()) {
            texts << action->text();
        }
    }
    CHECK(texts == QStringList{q("Copy transcript"), q("Show in Folder"), q("Delete")});
    findAction(*menu, q("Copy transcript"))->trigger();
    findAction(*menu, q("Delete"))->trigger();
    CHECK(copied == std::vector<QString>{q("m_abc1234567")});
    CHECK(deleted == std::vector<QString>{q("m_abc1234567")});

    Json silent = folderMeeting(std::nullopt);
    silent["content_summary"] = {{"has_transcript", false}};
    PastMeetingItem quiet(silent);
    std::unique_ptr<QMenu> quietMenu(quiet.buildContextMenu());
    CHECK_FALSE(findAction(*quietMenu, q("Copy transcript"))->isEnabled());
}

TEST_CASE("a host-kept card offers to open and delete there", "[past_meetings_panel][widget]") {
    Json row = folderMeeting(std::nullopt);
    row["stored_on"] = "Studio PC";
    PastMeetingItem card(row);
    std::unique_ptr<QMenu> menu(card.buildContextMenu());

    QStringList texts;
    for (QAction* action : menu->actions()) {
        if (!action->isSeparator()) {
            texts << action->text();
        }
    }
    CHECK(texts == QStringList{q("Open (from Studio PC)"), q("Delete from Studio PC")});
    CHECK(card.locationPill()->text() == q("On Studio PC"));
    CHECK_FALSE(card.locationPill()->isHidden());
}

TEST_CASE("opening a host-kept meeting asks the owner to fetch it, then selects it",
          "[past_meetings_panel][widget]") {
    Json hosted = meetingRow("m_host", "ended", "Kept on the host");
    hosted["stored_on"] = "Studio PC";
    PanelFixture fixture({hosted});
    fixture.refresh();
    std::vector<QString> requests;
    std::vector<QString> selected;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::hostFetchRequested,
                     [&requests](const QString& id, const QString& host, const QString& then) {
                         requests.push_back(id + "|" + host + "|" + then);
                     });
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::meetingSelected,
                     [&selected](const QString& id) { selected.push_back(id); });

    QTest::mouseClick(fixture.panel->cards()[0], Qt::LeftButton);
    // A second click while the first is fetching does nothing.
    QTest::mouseClick(fixture.panel->cards()[0], Qt::LeftButton);

    CHECK(requests == std::vector<QString>{q("m_host|Studio PC|open")});
    CHECK(selected.empty());
    CHECK(fixture.panel->cards()[0]->contentLabel()->text() == q("Getting it from Studio PC…"));

    fixture.panel->onHostFetchDone(q("m_host"), q("open"), q("no route"));
    CHECK(fixture.panel->cards()[0]->contentLabel()->text() == q("Couldn't open it: no route"));

    fixture.panel->onHostFetchDone(q("m_host"), q("open"), QString());
    fixture.io.drain();
    CHECK(selected == std::vector<QString>{q("m_host")});
}

TEST_CASE("the meetings an export offers leave out host-kept ones",
          "[past_meetings_panel][widget]") {
    Json hosted = meetingRow("m_host");
    hosted["stored_on"] = "Studio PC";
    PanelFixture fixture({meetingRow("m_here"), hosted});

    fixture.refresh();

    const std::vector<Json> offered = fixture.panel->exportableMeetings();
    REQUIRE(offered.size() == 1);
    CHECK(offered[0]["id"] == "m_here");
}

TEST_CASE("the header menu asks for the export and the folder", "[past_meetings_panel][widget]") {
    PanelFixture fixture({meetingRow("m_done")});
    fixture.refresh();
    int exports = 0;
    int folders = 0;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::exportRequested, [&] { ++exports; });
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::openMeetingsFolderRequested,
                     [&] { ++folders; });
    std::unique_ptr<QMenu> menu(fixture.panel->buildHeaderMenu());

    findAction(*menu, q("Export past meetings…"))->trigger();
    findAction(*menu, q("Open meetings folder"))->trigger();

    CHECK(exports == 1);
    CHECK(folders == 1);
}

TEST_CASE("opening the meetings folder makes it and shows it", "[past_meetings_panel][platform]") {
    fv::testkit::TempDataRoot root;
    const std::filesystem::path folder = root.path() / "meetings";
    fvtest::RecordedLaunches launches;

    CHECK(fv::openMeetingsFolder(folder));

    CHECK(std::filesystem::is_directory(folder));
    REQUIRE(launches.urls.size() == 1);
    CHECK(QDir::cleanPath(launches.urls[0].toLocalFile()) ==
          QDir::cleanPath(QString::fromStdWString(folder.wstring())));
}
