// The "Describe These Files" dialog (ported from tests/test_batch_relation_dialog.py).
#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QLabel>
#include <QTextEdit>

#include "ui_record/batch_relation_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"

namespace {

QStringList fileNameRows(const fv::BatchRelationDialog& dialog) {
    QStringList rows;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->objectName() == QStringLiteral("batchRelationFileName")) {
            // ElidingLabel keeps the full text apart from what it shows.
            const auto* eliding = qobject_cast<const fv::ElidingLabel*>(label);
            rows.append(eliding != nullptr ? eliding->text() : label->text());
        }
    }
    return rows;
}

} // namespace

TEST_CASE("the batch dialog prefills the remembered description and choice", "[widget]") {
    // from tests/test_batch_relation_dialog.py::TestBatchRelationDialog::
    // test_prefills_the_remembered_description_and_choice
    fv::BatchRelationDialog dialog({QStringLiteral("a.wav")}, QStringLiteral("Two halves."), true);

    CHECK(dialog.instructionsText() == QStringLiteral("Two halves."));
    CHECK(dialog.combineChecked() == true);
    CHECK(dialog.objectName() == QStringLiteral("batchRelationDialog"));
    CHECK(dialog.isModal());
    CHECK(dialog.windowTitle() == QStringLiteral("Describe These Files"));
    CHECK(dialog.minimumSize() == QSize(560, 460));
    CHECK(dialog.size() == QSize(620, 520));
}

TEST_CASE("the batch dialog's accessors strip and cap the text", "[widget]") {
    // from tests/test_batch_relation_dialog.py::TestBatchRelationDialog::
    // test_accessors_strip_and_cap_the_text
    fv::BatchRelationDialog dialog({QStringLiteral("a.wav")}, QString(), false);
    dialog.instructionsEdit()->setPlainText(
        QStringLiteral("  ") +
        QString(fv::MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS + 20, QLatin1Char('x')) +
        QStringLiteral("  "));

    const QString text = dialog.instructionsText();
    CHECK(text.size() == fv::MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS);
    CHECK_FALSE(text.startsWith(QLatin1Char(' ')));
    CHECK(dialog.combineChecked() == false);
}

TEST_CASE("the cap counts code points, so an emoji is never cut in two", "[widget]") {
    fv::BatchRelationDialog dialog({QStringLiteral("a.wav")}, QString(), false);
    const QString smiley = QString::fromUcs4(U"\U0001F600");
    QString text;
    for (int i = 0; i < fv::MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS + 5; ++i) {
        text += smiley;
    }
    dialog.instructionsEdit()->setPlainText(text);
    const QString capped = dialog.instructionsText();
    CHECK(capped.size() == 2 * fv::MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS);
    CHECK(capped.endsWith(smiley));
}

TEST_CASE("the batch dialog lists the queued files and collapses the rest", "[widget]") {
    // from tests/test_batch_relation_dialog.py::TestBatchRelationDialog::
    // test_lists_the_queued_files_and_collapses_the_rest
    QStringList names;
    for (int i = 0; i < 8; ++i) {
        names.append(QStringLiteral("part%1.mp3").arg(i));
    }
    fv::BatchRelationDialog dialog(names, QString(), false);

    const QStringList rows = fileNameRows(dialog);
    CHECK(rows.mid(0, 6) == names.mid(0, 6));
    CHECK(rows.last() == QStringLiteral("+2 more"));
    CHECK(rows.size() == 7);
}

TEST_CASE("the batch dialog's primary button accepts", "[widget]") {
    // from tests/test_batch_relation_dialog.py::TestBatchRelationDialog::
    // test_primary_button_accepts
    fv::BatchRelationDialog dialog({QStringLiteral("a.wav")}, QString(), false);
    dialog.combineCheck()->setChecked(true);

    dialog.useButton()->click();

    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.combineChecked() == true);
}

TEST_CASE("Cancel rejects and the dialog's texts are OpenWhisper's", "[widget]") {
    fv::BatchRelationDialog dialog({QStringLiteral("a.wav"), QStringLiteral("b.wav")});
    dialog.cancelButton()->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK_FALSE(dialog.cancelButton()->autoDefault());
    CHECK(dialog.useButton()->isDefault());
    CHECK(dialog.useButton()->text() == QStringLiteral("Use Description"));

    const auto text = [&](const char* name) {
        return dialog.findChild<QLabel*>(QString::fromLatin1(name))->text();
    };
    CHECK(text("batchRelationTitle") == QStringLiteral("Describe these files"));
    CHECK(text("batchRelationFilesLabel") == QStringLiteral(u"FILES IN THIS BATCH  ·  2"));
    CHECK(text("batchRelationEditorLabel") == QStringLiteral("HOW THEY RELATE"));
    CHECK(dialog.combineCheck()->text() == QStringLiteral("Combine into one transcript"));
    CHECK(dialog.combineCheck()->toolTip() ==
          QStringLiteral("On: the files are joined in order and cleaned as one transcript with one "
                         "History entry. Off: each file is cleaned and saved on its own."));
    CHECK(dialog.instructionsEdit()->placeholderText().startsWith(
        QStringLiteral("e.g. Three interviews with the same client")));
    CHECK_FALSE(dialog.instructionsEdit()->acceptRichText());
    CHECK(dialog.instructionsEdit()->minimumHeight() == 110);
}
