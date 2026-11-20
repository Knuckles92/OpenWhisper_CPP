// Past Meetings delete and clear-history actions (OpenWhisper's
// tests/test_past_meetings_delete.py). The delete dialog and the clear questions are answered by
// the test through the panel's seams (the Python tests patch MeetingDeleteDialog.exec and
// QMessageBox.question).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QMenu>

#include "past_meetings_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_meeting/meeting_delete_dialog.h"

using fv::Json;
using fv::MeetingDeleteDialog;
using fv::PastMeetingsPanel;
using fvtest::PanelFixture;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

Json deletableMeeting(const std::string& id, bool hasAudio = true) {
    return Json{{"id", id},
                {"title", "Review"},
                {"status", "ended"},
                {"started_at", "2025-01-02T09:30:00"},
                {"ended_at", "2025-01-02T10:12:00"},
                {"content_summary", {{"has_audio", hasAudio}, {"has_transcript", true}}}};
}

using Deletes = std::vector<std::pair<QString, bool>>;

struct DeleteFixture {
    explicit DeleteFixture(bool confirm = true)
        : settings(Json{{"confirm_meeting_delete", confirm}}),
          fixture({deletableMeeting("m_done")}, false, &settings) {
        fixture.refresh();
        QObject::connect(
            fixture.panel.get(), &PastMeetingsPanel::deleteMeetingRequested,
            [this](const QString& id, bool recordings) { deleted.emplace_back(id, recordings); });
        QObject::connect(fixture.panel.get(), &PastMeetingsPanel::clearMeetingsRequested,
                         [this](bool recordings) { cleared.push_back(recordings); });
    }

    // patch.object(MeetingDeleteDialog, "exec", new=answer)
    void answer(std::function<int(MeetingDeleteDialog&)> answer) {
        fixture.panel->runDeleteDialog = [this, answer](MeetingDeleteDialog& dialog) {
            ++asked;
            return answer(dialog);
        };
    }
    PastMeetingsPanel& panel() { return *fixture.panel; }

    fv::MemorySettingsStore settings;
    PanelFixture fixture;
    Deletes deleted;
    std::vector<bool> cleared;
    int asked = 0;
};

std::map<QString, bool> menuState(QMenu& menu) {
    std::map<QString, bool> actions;
    for (QAction* action : menu.actions()) {
        if (!action->isSeparator()) {
            actions[action->text()] = action->isEnabled();
        }
    }
    return actions;
}

} // namespace

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_delete_follows_the_confirmation
TEST_CASE("test_delete_follows_the_confirmation", "[test_past_meetings_delete][widget]") {
    struct Case {
        const char* id;
        std::function<int(MeetingDeleteDialog&)> answer;
        Deletes expected;
    };
    const Case table[] = {
        {"canceled_when_confirmation_is_rejected",
         [](MeetingDeleteDialog&) { return int(QDialog::Rejected); },
         {}},
        {"keeps_recordings_by_default",
         [](MeetingDeleteDialog&) { return int(QDialog::Accepted); },
         {{q("m_done"), false}}},
        {"audio_spool_can_be_deleted_with_meeting",
         [](MeetingDeleteDialog& dialog) {
             dialog.deleteRecordings()->setChecked(true);
             return int(QDialog::Accepted);
         },
         {{q("m_done"), true}}},
    };
    const int index = GENERATE(0, 1, 2);
    const Case& chosen = table[index];
    CAPTURE(chosen.id);
    DeleteFixture fixture;
    fixture.answer(chosen.answer);

    fixture.panel().confirmDelete(q("m_done"));

    CHECK(fixture.deleted == chosen.expected);
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_dont_ask_again_preference_is_saved
TEST_CASE("test_dont_ask_again_preference_is_saved", "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture;
    fixture.answer([](MeetingDeleteDialog& dialog) {
        dialog.dontAskAgain()->setChecked(true);
        return int(QDialog::Accepted);
    });

    fixture.panel().confirmDelete(q("m_done"));

    CHECK_FALSE(fixture.settings.get(fv::keys::confirm_meeting_delete));
    CHECK(fixture.settings.contents().at("confirm_meeting_delete") == false);
    CHECK(fixture.deleted == Deletes{{q("m_done"), false}});
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_saved_opt_out_skips_confirmation_and_keeps_recordings
TEST_CASE("test_saved_opt_out_skips_confirmation_and_keeps_recordings",
          "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture(false);
    fixture.answer([](MeetingDeleteDialog&) { return int(QDialog::Accepted); });

    fixture.panel().confirmDelete(q("m_done"));

    CHECK(fixture.asked == 0);
    CHECK(fixture.deleted == Deletes{{q("m_done"), false}});
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_clear_history_emits_keep_recordings
TEST_CASE("test_clear_history_emits_keep_recordings", "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture;
    QString title;
    QString text;
    fixture.panel().confirm = [&](const QString& heading, const QString& question) {
        title = heading;
        text = question;
        return true;
    };

    fixture.panel().onClearHistory();

    CHECK(fixture.cleared == std::vector<bool>{false});
    CHECK(title == q("Clear History"));
    CHECK(text == q("Delete all past meetings?\n\nSaved recordings will be kept."));
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_clear_history_and_recordings_emits_delete_spools
TEST_CASE("test_clear_history_and_recordings_emits_delete_spools",
          "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture;
    QString title;
    QString text;
    fixture.panel().hostClearNote = [] {
        return q("\n\nThis computer's meetings on Studio PC are deleted too.");
    };
    fixture.panel().confirm = [&](const QString& heading, const QString& question) {
        title = heading;
        text = question;
        return true;
    };

    fixture.panel().onClearHistoryAndRecordings();

    CHECK(fixture.cleared == std::vector<bool>{true});
    CHECK(title == q("Clear History and Recordings"));
    CHECK(text == q("Delete all past meetings AND permanently delete their recordings from "
                    "disk?\n\nThis cannot be undone.\n\nThis computer's meetings on Studio PC "
                    "are deleted too."));
}

TEST_CASE("declining a clear question emits nothing", "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture;
    fixture.panel().confirm = [](const QString&, const QString&) {
        return false;
    };

    fixture.panel().onClearHistory();
    fixture.panel().onClearHistoryAndRecordings();

    CHECK(fixture.cleared.empty());
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_clear_actions_are_disabled_when_empty
TEST_CASE("test_clear_actions_are_disabled_when_empty", "[test_past_meetings_delete][widget]") {
    PanelFixture empty;
    empty.refresh();

    std::unique_ptr<QMenu> menu(empty.panel->buildHeaderMenu());
    const auto actions = menuState(*menu);

    CHECK_FALSE(actions.at(q("Clear history")));
    CHECK_FALSE(actions.at(q("Clear history + recordings")));
    CHECK_FALSE(actions.at(q("Export past meetings…")));
    CHECK(actions.at(q("Refresh")));
    CHECK(actions.at(q("Open meetings folder")));
}

// from
// tests/test_past_meetings_delete.py::TestPastMeetingsDelete::test_clear_actions_are_enabled_when_meetings_exist
TEST_CASE("test_clear_actions_are_enabled_when_meetings_exist",
          "[test_past_meetings_delete][widget]") {
    DeleteFixture fixture;

    std::unique_ptr<QMenu> menu(fixture.panel().buildHeaderMenu());
    const auto actions = menuState(*menu);

    CHECK(actions.at(q("Clear history")));
    CHECK(actions.at(q("Clear history + recordings")));
    CHECK(actions.at(q("Export past meetings…")));
}

TEST_CASE("the delete dialog offers recordings only for a meeting that has some",
          "[test_past_meetings_delete][widget]") {
    const bool hasAudio = GENERATE(false, true);
    CAPTURE(hasAudio);
    fv::MemorySettingsStore settings(Json{{"confirm_meeting_delete", true}});
    PanelFixture fixture({deletableMeeting("m_done", hasAudio)}, false, &settings);
    fixture.refresh();
    bool offered = !hasAudio;
    bool checkedAtAccept = false;
    fixture.panel->runDeleteDialog = [&](MeetingDeleteDialog& dialog) {
        dialog.show();
        offered = dialog.deleteRecordings()->isVisible();
        // Checked with no audio to delete: nothing is asked to be deleted.
        dialog.deleteRecordings()->setChecked(true);
        checkedAtAccept = true;
        return int(QDialog::Accepted);
    };
    Deletes deleted;
    QObject::connect(
        fixture.panel.get(), &PastMeetingsPanel::deleteMeetingRequested,
        [&deleted](const QString& id, bool recordings) { deleted.emplace_back(id, recordings); });

    fixture.panel->confirmDelete(q("m_done"));

    CHECK(offered == hasAudio);
    CHECK(checkedAtAccept);
    CHECK(deleted == Deletes{{q("m_done"), hasAudio}});
}

TEST_CASE("an unreadable setting asks before deleting", "[test_past_meetings_delete][widget]") {
    PanelFixture fixture({deletableMeeting("m_done")}, false, nullptr);
    fixture.refresh();
    int asked = 0;
    fixture.panel->runDeleteDialog = [&asked](MeetingDeleteDialog&) {
        ++asked;
        return int(QDialog::Rejected);
    };

    fixture.panel->confirmDelete(q("m_done"));

    CHECK(asked == 1);
}

TEST_CASE("deleting a host-kept meeting asks about the host and then asks the owner to delete it",
          "[test_past_meetings_delete][widget]") {
    Json hosted = deletableMeeting("m_host");
    hosted["stored_on"] = "Studio PC";
    PanelFixture fixture({hosted});
    fixture.refresh();
    QString question;
    fixture.panel->confirm = [&question](const QString&, const QString& text) {
        question = text;
        return true;
    };
    std::vector<QString> requests;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::hostFetchRequested,
                     [&requests](const QString& id, const QString& host, const QString& then) {
                         requests.push_back(id + "|" + host + "|" + then);
                     });

    fixture.panel->confirmDelete(q("m_host"));

    CHECK(question == q("Delete this meeting from Studio PC?\n\nIts transcript, insights and "
                        "recordings there are deleted. This cannot be undone."));
    CHECK(requests == std::vector<QString>{q("m_host|Studio PC|delete")});
}

TEST_CASE("deleting a second copy names the host and preserves the local meeting",
          "[test_past_meetings_delete][widget]") {
    Json row = deletableMeeting("m_done");
    row["_host_copy_name"] = "Studio PC";
    row["_host_copy_title"] = "Review on host";
    row["_host_copy_fingerprint"] = "exact-peer";
    row["_host_copy_revision"] = "exact-version";
    PanelFixture fixture({row});
    fixture.refresh();
    auto* card = fixture.panel->findChild<fv::PastMeetingItem*>();
    REQUIRE(card);
    std::unique_ptr<QMenu> menu(card->buildContextMenu());
    QAction* deleteCopy = nullptr;
    for (auto* action : menu->actions()) {
        if (action->text() == q("Delete copy on Studio PC…")) {
            deleteCopy = action;
        }
    }
    REQUIRE(deleteCopy);

    bool allow = false;
    QString question;
    fixture.panel->confirm = [&](const QString&, const QString& prompt) {
        question = prompt;
        return allow;
    };
    std::vector<QString> requests;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::hostCopyDeleteRequested,
                     [&](const QString& id, const QString& pin, const QString& version) {
                         requests.push_back(id + "|" + pin + "|" + version);
                     });
    Deletes localDeletes;
    QObject::connect(fixture.panel.get(), &PastMeetingsPanel::deleteMeetingRequested,
                     [&](const QString& id, bool audio) { localDeletes.emplace_back(id, audio); });
    deleteCopy->trigger();
    CHECK(requests.empty());
    CHECK(question.contains(q("Review on host")));
    CHECK(question.contains(q("Studio PC")));

    row["_host_copy_fingerprint"] = "new-peer";
    row["_host_copy_revision"] = "new-version";
    card->setMeeting(row);
    allow = true;
    deleteCopy->trigger();
    CHECK(requests == std::vector<QString>{q("m_done|exact-peer|exact-version")});
    CHECK(localDeletes.empty());
}
