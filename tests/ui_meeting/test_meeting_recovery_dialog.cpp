// The startup dialog listing meetings an exit cut short (ui_qt/dialogs/meeting_recovery_dialog.py;
// OpenWhisper has no test of it). Rows, the two actions, and what a settled row reads.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QFrame>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPushButton>

#include "base/identity.h"
#include "ui_meeting/meeting_recovery_dialog.h"
#include "ui_theme/style_props.h"

using fv::MeetingRecoveryDialog;

namespace {

QString q(const char* text) {
    return QString::fromUtf8(text);
}

QJsonObject interrupted(const char* id, const char* title, const char* startedAt) {
    return QJsonObject{{"id", q(id)}, {"title", q(title)}, {"started_at", q(startedAt)}};
}

QList<QLabel*> rowLabels(const MeetingRecoveryDialog& dialog) {
    return dialog.findChildren<QLabel*>(q("meetingRecoveryRowLabel"));
}

QPushButton* rowButton(const MeetingRecoveryDialog& dialog, const char* id, int row) {
    QList<QPushButton*> found;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->property(fv::ui::prop::buttonId).toString() == q(id)) {
            found << button;
        }
    }
    return row < found.size() ? found[row] : nullptr;
}

} // namespace

TEST_CASE("the recovery dialog names what happened and lists each meeting",
          "[meeting_recovery_dialog][widget]") {
    MeetingRecoveryDialog dialog({interrupted("m_a", "Weekly design review", "2026-03-14T15:30:12"),
                                  interrupted("m_b", "", "2026-03-13T09:00:00"),
                                  interrupted("m_c", "No time", "")});

    CHECK(dialog.windowTitle() == q("Recover Interrupted Meetings"));
    CHECK(dialog.isModal());
    CHECK(dialog.minimumWidth() == 520);
    CHECK(dialog.accessibleName() == q("Recover interrupted meetings"));
    const QLabel* body = dialog.findChild<QLabel*>(q("infoLabel"));
    REQUIRE(body != nullptr);
    CHECK(body->text().startsWith(QString::fromUtf8(std::string(fv::identity().product)) +
                                  q(" closed while these meetings were running.")));
    CHECK_FALSE(body->text().contains(q("OpenWhisper")));
    const QList<QLabel*> labels = rowLabels(dialog);
    REQUIRE(labels.size() == 3);
    // The date is the first sixteen characters with the T read as a space, with no zone shift.
    CHECK(labels[0]->text() == q("Weekly design review — 2026-03-14 15:30"));
    CHECK(labels[1]->text() == q("Untitled meeting — 2026-03-13 09:00"));
    CHECK(labels[2]->text() == q("No time"));
    CHECK(dialog.findChildren<QFrame*>(q("meetingRecoveryRow")).size() == 3);
    const QPushButton* close = dialog.findChild<QPushButton*>(q("meetingRecoveryCloseButton"));
    REQUIRE(close != nullptr);
    CHECK(close->isDefault());
}

TEST_CASE("Finalize reports the meeting and settles its row", "[meeting_recovery_dialog][widget]") {
    std::vector<QString> finalized;
    std::vector<QString> discarded;
    MeetingRecoveryDialog dialog(
        {interrupted("m_a", "First", "2026-03-14T15:30:12"),
         interrupted("m_b", "Second", "2026-03-13T09:00:00")},
        [&finalized](const QString& id) { finalized.push_back(id); },
        [&discarded](const QString& id) { discarded.push_back(id); });

    rowButton(dialog, "meetingRecoveryFinalizeButton", 0)->click();

    CHECK(finalized == std::vector<QString>{q("m_a")});
    CHECK(discarded.empty());
    CHECK(rowLabels(dialog)[0]->text() == q("First — 2026-03-14 15:30  (finalizing)"));
    CHECK_FALSE(rowButton(dialog, "meetingRecoveryFinalizeButton", 0)->isEnabled());
    CHECK_FALSE(rowButton(dialog, "meetingRecoveryDiscardButton", 0)->isEnabled());
    // The other row is untouched.
    CHECK(rowLabels(dialog)[1]->text() == q("Second — 2026-03-13 09:00"));
    CHECK(rowButton(dialog, "meetingRecoveryFinalizeButton", 1)->isEnabled());
}

TEST_CASE("Discard reports the meeting and settles its row", "[meeting_recovery_dialog][widget]") {
    std::vector<QString> discarded;
    MeetingRecoveryDialog dialog({interrupted("m_a", "First", "2026-03-14T15:30:12")}, {},
                                 [&discarded](const QString& id) { discarded.push_back(id); });

    rowButton(dialog, "meetingRecoveryDiscardButton", 0)->click();

    CHECK(discarded == std::vector<QString>{q("m_a")});
    CHECK(rowLabels(dialog)[0]->text() == q("First — 2026-03-14 15:30  (discarded)"));
    CHECK_FALSE(rowButton(dialog, "meetingRecoveryFinalizeButton", 0)->isEnabled());
}

TEST_CASE("an action assigned after construction is the one called",
          "[meeting_recovery_dialog][widget]") {
    MeetingRecoveryDialog dialog({interrupted("m_a", "First", "2026-03-14T15:30:12")});
    QString finalized;
    dialog.onFinalize = [&finalized](const QString& id) {
        finalized = id;
    };

    rowButton(dialog, "meetingRecoveryFinalizeButton", 0)->click();

    CHECK(finalized == q("m_a"));
}

TEST_CASE("a row with no action callback still settles", "[meeting_recovery_dialog][widget]") {
    MeetingRecoveryDialog dialog({interrupted("m_a", "First", "2026-03-14T15:30:12")});

    rowButton(dialog, "meetingRecoveryDiscardButton", 0)->click();

    CHECK(rowLabels(dialog)[0]->text().endsWith(q("(discarded)")));
}

TEST_CASE("each row's buttons name the meeting for a screen reader",
          "[meeting_recovery_dialog][widget]") {
    MeetingRecoveryDialog dialog({interrupted("m_a", "Weekly review", "2026-03-14T15:30:12")});

    CHECK(rowButton(dialog, "meetingRecoveryFinalizeButton", 0)->accessibleName() ==
          q("Finalize Weekly review"));
    CHECK(rowButton(dialog, "meetingRecoveryDiscardButton", 0)->accessibleName() ==
          q("Discard Weekly review"));
}
