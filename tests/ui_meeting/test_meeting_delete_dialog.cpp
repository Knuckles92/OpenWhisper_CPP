// The rendered deletion options and safe dismissal behavior (OpenWhisper's
// tests/test_meeting_delete_dialog.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QTest>

#include "ui_meeting/meeting_delete_dialog.h"
#include "ui_widgets/buttons.h"

using fv::MeetingDeleteDialog;

namespace {

void settle() {
    QApplication::processEvents();
    QTest::qWait(20);
}

} // namespace

// from tests/test_meeting_delete_dialog.py::test_options_stay_in_layout_after_show_and_resize
TEST_CASE("test_options_stay_in_layout_after_show_and_resize",
          "[test_meeting_delete_dialog][widget]") {
    const bool hasAudio = GENERATE(false, true);
    CAPTURE(hasAudio);
    MeetingDeleteDialog dialog(nullptr, hasAudio);
    dialog.show();
    settle();
    for (const int width : {dialog.width(), dialog.width() + 100}) {
        dialog.resize(width, dialog.height() + 30);
        settle();
        QList<QWidget*> controls;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            controls << label;
        }
        controls << dialog.deleteRecordings() << dialog.dontAskAgain() << dialog.buttons();
        QList<QWidget*> visible;
        for (QWidget* control : controls) {
            if (control->isVisible()) {
                visible << control;
            }
        }
        CHECK(dialog.deleteRecordings()->isVisible() == hasAudio);
        CHECK_FALSE(dialog.deleteRecordings()->isChecked());
        for (qsizetype index = 0; index < visible.size(); ++index) {
            QWidget* control = visible[index];
            CHECK(dialog.layout()->indexOf(control) >= 0);
            CHECK(dialog.rect().contains(control->geometry()));
            CHECK(control->width() >= control->minimumSizeHint().width());
            for (qsizetype other = index + 1; other < visible.size(); ++other) {
                CHECK_FALSE(control->geometry().intersects(visible[other]->geometry()));
            }
        }
        CHECK(dialog.buttons()->y() > dialog.dontAskAgain()->geometry().bottom());
    }
    QStringList texts;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        texts << button->text();
    }
    texts.sort();
    CHECK(texts == QStringList{QStringLiteral("Cancel"), QStringLiteral("Delete")});
    dialog.close();
}

// from tests/test_meeting_delete_dialog.py::test_cancel_comes_first_and_delete_is_the_danger_action
TEST_CASE("test_cancel_comes_first_and_delete_is_the_danger_action",
          "[test_meeting_delete_dialog][widget]") {
    MeetingDeleteDialog dialog;
    dialog.show();
    settle();
    CHECK(dialog.cancelButton()->x() < dialog.deleteButton()->x());
    CHECK(dialog.deleteButton()->property("tone").toString() == QStringLiteral("danger"));
    CHECK(dialog.cancelButton()->height() == dialog.deleteButton()->height());
    CHECK(dialog.cancelButton()->isDefault());
    CHECK_FALSE(dialog.deleteButton()->isDefault());
    CHECK_FALSE(dialog.deleteButton()->autoDefault());
    dialog.close();
}

// from tests/test_meeting_delete_dialog.py::test_confirmation_actions
TEST_CASE("test_confirmation_actions", "[test_meeting_delete_dialog][widget]") {
    const QString action = QString::fromLatin1(
        GENERATE(as<const char*>{}, "delete", "cancel", "enter", "escape", "close"));
    CAPTURE(action);
    MeetingDeleteDialog dialog(nullptr, true);
    dialog.show();
    settle();
    if (action == QStringLiteral("delete") || action == QStringLiteral("cancel")) {
        QPushButton* button = action == QStringLiteral("delete")
                                  ? static_cast<QPushButton*>(dialog.deleteButton())
                                  : static_cast<QPushButton*>(dialog.cancelButton());
        QTest::mouseClick(button, Qt::LeftButton);
    } else if (action == QStringLiteral("enter") || action == QStringLiteral("escape")) {
        QTest::keyClick(&dialog,
                        action == QStringLiteral("enter") ? Qt::Key_Return : Qt::Key_Escape);
    } else {
        dialog.close();
    }
    CHECK_FALSE(dialog.isVisible());
    CHECK(dialog.result() ==
          (action == QStringLiteral("delete") ? int(QDialog::Accepted) : int(QDialog::Rejected)));
}
