// The reading window (ported from tests/test_transcript_viewer_dialog.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalSpy>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QWheelEvent>

#include "app_theme.h"
#include "ui_record/transcript_viewer_dialog.h"
#include "ui_theme/markdown_render.h"
#include "ui_widgets/eliding_label.h"
#include "upload_widgets_support.h"

namespace {

using fv::dictation::BatchItem;
using fv::dictation::BatchItemResult;
using fv::dictation::BatchRelation;
using fv::dictation::BatchResult;
using fv::dictation::BatchUploadRequest;

int firstBlockHeadingLevel(const fv::TranscriptViewerDialog& dialog) {
    return dialog.view()->document()->begin().blockFormat().headingLevel();
}

double defaultPointSize(const fv::TranscriptViewerDialog& dialog) {
    return dialog.view()->document()->defaultFont().pointSizeF();
}

QStringList tabLabels(const fv::TranscriptViewerDialog& dialog) {
    QStringList labels;
    for (int index = 0; index < dialog.pageTabs()->count(); ++index) {
        labels.append(dialog.pageTabs()->tabText(index));
    }
    return labels;
}

// _batch_result.
BatchResult batchResult(bool cleanup = false, bool secondFailed = false) {
    BatchResult result;
    result.request.items = {BatchItem{"a.wav", std::nullopt}, BatchItem{"b.wav", std::nullopt}};
    result.request.relation = BatchRelation::Separate;
    BatchItemResult first;
    first.item = result.request.items[0];
    first.text = cleanup ? "Clean a" : "Raw a";
    if (cleanup) {
        first.raw_text = "Raw a";
        first.cleanup_provider = "openai";
        first.cleanup_model = "gpt-test";
    }
    BatchItemResult second;
    second.item = result.request.items[1];
    second.text = cleanup ? "Clean b" : "Raw b";
    if (cleanup) {
        second.raw_text = "Raw b";
        second.cleanup_provider = "openai";
        second.cleanup_model = "gpt-test";
    }
    if (secondFailed) {
        second.error = "failed";
    }
    result.items = {first, second};
    return result;
}

} // namespace

TEST_CASE("the viewer renders Markdown and names the source", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_renders_markdown_and_names_the_source
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    dialog.setTranscript(QStringLiteral("## a.mp3\n\nHello **there**"), {},
                         QStringLiteral("a.mp3"));
    CHECK(firstBlockHeadingLevel(dialog) == 2);
    CHECK(dialog.view()->toPlainText() == QStringLiteral("a.mp3\nHello there"));
    CHECK(dialog.titleLabel()->text() == QStringLiteral("a.mp3"));
    CHECK(dialog.windowTitle().contains(QStringLiteral("a.mp3")));
    CHECK(dialog.windowTitle() == QStringLiteral(u"Transcript — a.mp3"));
    CHECK(dialog.versionToggle()->isHidden());
}

TEST_CASE("the Raw switch appears only when raw differs", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_raw_switch_appears_only_when_raw_differs
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    dialog.setTranscript(QStringLiteral("clean"), QStringLiteral("clean"));
    CHECK(dialog.versionToggle()->isHidden());

    dialog.setTranscript(QStringLiteral("clean"), QStringLiteral("raw um"));
    CHECK_FALSE(dialog.versionToggle()->isHidden());
    dialog.rawButton()->click();
    CHECK(dialog.shownText() == QStringLiteral("raw um"));
    CHECK(dialog.view()->toPlainText() == QStringLiteral("raw um"));

    // A new transcript lands on Fixed again, whatever was showing before.
    dialog.setTranscript(QStringLiteral("next"), QStringLiteral("next raw"));
    CHECK(dialog.fixedButton()->isChecked());
    CHECK(dialog.shownText() == QStringLiteral("next"));
}

TEST_CASE("copy emits the shown source text, not the rendering", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_copy_emits_the_shown_source_text_not_the_rendering
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    QSignalSpy copied(&dialog, &fv::TranscriptViewerDialog::copyRequested);
    dialog.setTranscript(QStringLiteral("## head\n\n**bold** body"));
    dialog.copyButton()->click();
    REQUIRE(copied.count() == 1);
    CHECK(copied.at(0).at(0).toString() == QStringLiteral("## head\n\n**bold** body"));
}

TEST_CASE("copy is disabled while the viewer is empty", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_copy_is_disabled_while_empty
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    dialog.clear();
    CHECK_FALSE(dialog.copyButton()->isEnabled());
    QSignalSpy copied(&dialog, &fv::TranscriptViewerDialog::copyRequested);
    // A disabled button is a no-op; the Ctrl+C shortcut stays live, and it is the handler itself
    // (Python's test calls _copy_shown_text) that refuses empty or blank text.
    dialog.copyButton()->click();
    CHECK(copied.count() == 0);
    QShortcut* copyShortcut = nullptr;
    for (QShortcut* shortcut : dialog.findChildren<QShortcut*>()) {
        if (shortcut->key() == QKeySequence(QKeySequence::Copy)) {
            copyShortcut = shortcut;
        }
    }
    REQUIRE(copyShortcut != nullptr);
    Q_EMIT copyShortcut->activated();
    CHECK(copied.count() == 0);

    dialog.setTranscript(QStringLiteral(" \n\t "));
    CHECK_FALSE(dialog.copyButton()->isEnabled());
    Q_EMIT copyShortcut->activated();
    CHECK(copied.count() == 0);

    // Text is copied stripped, as Python's str.strip() leaves it.
    dialog.setTranscript(QStringLiteral("\n  body  \n"));
    CHECK(dialog.copyButton()->isEnabled());
    Q_EMIT copyShortcut->activated();
    REQUIRE(copied.count() == 1);
    CHECK(copied.at(0).at(0).toString() == QStringLiteral("body"));
}

TEST_CASE("zoom steps scale the document and stop at the ends", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_zoom_steps_scale_the_document_and_stop_at_the_ends
    const fvtest::AppTheme theme(QStringLiteral("dark"), 100);
    fv::TranscriptViewerDialog dialog;
    dialog.setTranscript(QStringLiteral("body"));
    const double base = defaultPointSize(dialog);
    CHECK(base == fv::ui::MarkdownStyle::READER.bodyPt);

    dialog.zoom(1);
    CHECK(defaultPointSize(dialog) > base);
    CHECK(dialog.zoomOutButton()->isEnabled());

    for (int i = 0; i < fv::TranscriptViewerDialog::kZoomStepCount; ++i) {
        dialog.zoom(1);
    }
    CHECK(dialog.zoomFactor() == fv::TranscriptViewerDialog::kZoomSteps[5]);
    CHECK_FALSE(dialog.zoomInButton()->isEnabled());

    dialog.resetZoom();
    CHECK(dialog.zoomFactor() == 1.0);
    CHECK(defaultPointSize(dialog) == base);
}

TEST_CASE("the zoom survives the next transcript", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_zoom_survives_the_next_transcript
    const fvtest::AppTheme theme(QStringLiteral("dark"), 100);
    fv::TranscriptViewerDialog dialog;
    dialog.setTranscript(QStringLiteral("one"));
    dialog.zoom(1);
    const double zoomed = defaultPointSize(dialog);
    dialog.setTranscript(QStringLiteral("two"));
    CHECK(defaultPointSize(dialog) == zoomed);
}

TEST_CASE("batch tabs show the overview and each completed transcript", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_batch_tabs_show_overview_and_each_completed_transcript
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    QSignalSpy copied(&dialog, &fv::TranscriptViewerDialog::copyRequested);
    dialog.setBatchResult(batchResult(), QStringLiteral("## a.wav\n\nRaw a\n\n## b.wav\n\nRaw b"),
                          {}, QStringLiteral("2 files"));

    CHECK_FALSE(dialog.pageTabs()->isHidden());
    CHECK(tabLabels(dialog) == QStringList{QStringLiteral("Overview"), QStringLiteral("Trans. 1"),
                                           QStringLiteral("Trans. 2")});
    CHECK(dialog.shownText().startsWith(QStringLiteral("## a.wav")));

    dialog.pageTabs()->setCurrentIndex(1);
    CHECK(dialog.shownText() == QStringLiteral("## a.wav\n\nRaw a"));
    CHECK(dialog.view()->toPlainText() == QStringLiteral("a.wav\nRaw a"));
    CHECK(dialog.pageTabs()->tabToolTip(1) == QStringLiteral("a.wav"));
    dialog.copyButton()->click();
    REQUIRE(copied.count() == 1);
    CHECK(copied.at(0).at(0).toString() == QStringLiteral("## a.wav\n\nRaw a"));

    dialog.pageTabs()->setCurrentIndex(2);
    CHECK(dialog.shownText() == QStringLiteral("## b.wav\n\nRaw b"));
}

TEST_CASE("the AI Output tab appears only when cleanup ran", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_ai_output_tab_appears_only_when_cleanup_ran
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    const QString overview = QStringLiteral("## a.wav\n\nClean a\n\n## b.wav\n\nClean b");
    const QString raw = QStringLiteral("## a.wav\n\nRaw a\n\n## b.wav\n\nRaw b");
    dialog.setBatchResult(batchResult(true), overview, raw, QStringLiteral("2 files"));

    CHECK(tabLabels(dialog) == QStringList{QStringLiteral("Overview"), QStringLiteral("Trans. 1"),
                                           QStringLiteral("Trans. 2"),
                                           QStringLiteral("AI Output")});
    CHECK(dialog.pageTabs()->tabToolTip(0) == QStringLiteral("All completed transcriptions"));
    CHECK(dialog.pageTabs()->tabToolTip(3) == QStringLiteral("Output after AI cleanup"));

    dialog.pageTabs()->setCurrentIndex(1);
    CHECK_FALSE(dialog.versionToggle()->isHidden());
    dialog.rawButton()->click();
    CHECK(dialog.shownText() == QStringLiteral("## a.wav\n\nRaw a"));

    dialog.pageTabs()->setCurrentIndex(3);
    CHECK(dialog.shownText() == overview);
    CHECK(dialog.versionToggle()->isHidden());
    // Changing page resets to Fixed.
    CHECK(dialog.fixedButton()->isChecked());
}

TEST_CASE("a stitched batch keeps the source parts beside the AI output", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_stitched_batch_keeps_source_parts_beside_ai_output
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    BatchResult result;
    result.request.items = {BatchItem{"part-1.wav", std::nullopt},
                            BatchItem{"part-2.wav", std::nullopt}};
    result.request.relation = BatchRelation::Sequential;
    BatchItemResult first;
    first.item = result.request.items[0];
    first.text = "Raw first part";
    BatchItemResult second;
    second.item = result.request.items[1];
    second.text = "Raw second part";
    result.items = {first, second};
    result.combined_text = "One cleaned transcript.";
    result.combined_raw_text = "Raw first part\n\nRaw second part";
    result.combined_cleanup_provider = "openai";
    result.combined_cleanup_model = "gpt-test";

    fv::TranscriptViewerDialog dialog;
    dialog.setBatchResult(result, QString::fromStdString(*result.combined_text),
                          QString::fromStdString(*result.combined_raw_text),
                          QStringLiteral("2 files"));

    dialog.pageTabs()->setCurrentIndex(1);
    CHECK(dialog.shownText() == QStringLiteral("## part-1.wav\n\nRaw first part"));
    dialog.pageTabs()->setCurrentIndex(2);
    CHECK(dialog.shownText() == QStringLiteral("## part-2.wav\n\nRaw second part"));
    dialog.pageTabs()->setCurrentIndex(3);
    CHECK(dialog.shownText() == QStringLiteral("One cleaned transcript."));
}

TEST_CASE("the tabs stay hidden without several completed transcriptions", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_tabs_stay_hidden_without_multiple_completed_transcriptions
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::TranscriptViewerDialog dialog;
    dialog.setBatchResult(batchResult(false, true),
                          QStringLiteral("## a.wav\n\nRaw a\n\n## b.wav\n\nError: failed"), {},
                          QStringLiteral("2 files"));
    CHECK(dialog.pageTabs()->isHidden());
    CHECK(dialog.pageTabs()->count() == 1);
}

TEST_CASE("the viewer is a resizable non-modal window", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestTranscriptViewerDialog::
    // test_is_a_resizable_non_modal_window
    fv::TranscriptViewerDialog dialog;
    CHECK_FALSE(dialog.isModal());
    CHECK(dialog.minimumWidth() < dialog.width());
    CHECK(dialog.objectName() == QStringLiteral("transcriptViewerDialog"));
    CHECK(dialog.minimumSize() == QSize(560, 440));
    CHECK(dialog.size() == QSize(940, 760));
    CHECK(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    CHECK_FALSE(dialog.windowFlags().testFlag(Qt::MSWindowsFixedSizeDialogHint));
    CHECK(dialog.windowTitle() == QStringLiteral("Transcript"));
}

TEST_CASE("wide reader views center a capped column", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestReaderView::
    // test_wide_views_center_a_capped_column
    // Qt delivers a hidden widget's resize on show, so the views are shown.
    fv::ReaderView view;
    view.resize(1600, 600);
    view.show();
    const QMargins margins = view.readerMargins();
    const int expected = (1600 - fv::ReaderView::kMaxMeasure) / 2;
    CHECK(margins.left() == expected);
    CHECK(margins.right() == expected);
    CHECK(view.viewport()->width() <= fv::ReaderView::kMaxMeasure);
    CHECK(margins.top() == fv::ReaderView::kTopMargin);
    CHECK(margins.bottom() == fv::ReaderView::kBottomMargin);
}

TEST_CASE("narrow reader views keep the minimum gutter", "[widget]") {
    // from tests/test_transcript_viewer_dialog.py::TestReaderView::
    // test_narrow_views_keep_the_minimum_gutter
    fv::ReaderView view;
    view.resize(500, 600);
    view.show();
    CHECK(view.readerMargins().left() == fv::ReaderView::kMinSideMargin);
}

TEST_CASE("Ctrl+wheel zooms through the dialog's steps and a plain wheel does not", "[widget]") {
    // Lead trap: zoom goes through the dialog's own steps, never Qt's font zoom.
    const fvtest::AppTheme theme(QStringLiteral("dark"), 100);
    fv::TranscriptViewerDialog dialog;
    dialog.setTranscript(QStringLiteral("body"));
    dialog.show();
    QApplication::processEvents();
    QSignalSpy zoomRequested(dialog.view(), &fv::ReaderView::zoomRequested);

    QWheelEvent up(QPointF(10, 10), QPointF(10, 10), QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                   Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(dialog.view()->viewport(), &up);
    REQUIRE(zoomRequested.count() == 1);
    CHECK(zoomRequested.at(0).at(0).toInt() == 1);
    CHECK(up.isAccepted());
    CHECK(dialog.zoomFactor() == 1.15);

    QWheelEvent down(QPointF(10, 10), QPointF(10, 10), QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
                     Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(dialog.view()->viewport(), &down);
    REQUIRE(zoomRequested.count() == 2);
    CHECK(zoomRequested.at(1).at(0).toInt() == -1);
    CHECK(dialog.zoomFactor() == 1.0);

    QWheelEvent plain(QPointF(10, 10), QPointF(10, 10), QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(dialog.view()->viewport(), &plain);
    CHECK(zoomRequested.count() == 2);
}

TEST_CASE("the viewer's tabs and buttons carry the Python object names", "[widget]") {
    fv::TranscriptViewerDialog dialog;
    CHECK(dialog.titleLabel()->objectName() == QStringLiteral("transcriptViewerTitle"));
    CHECK(dialog.pageTabs()->objectName() == QStringLiteral("transcriptViewerTabs"));
    CHECK(dialog.pageTabs()->documentMode());
    CHECK(dialog.pageTabs()->elideMode() == Qt::ElideRight);
    CHECK(dialog.view()->objectName() == QStringLiteral("transcriptViewerText"));
    CHECK(dialog.copyButton()->objectName() == QStringLiteral("transcriptViewerCopyButton"));
    CHECK(dialog.copyButton()->toolTip() == QStringLiteral("Copy the transcript as text (Ctrl+C)"));
    CHECK(dialog.zoomInButton()->toolTip() == QStringLiteral("Larger text (Ctrl++)"));
    CHECK(dialog.zoomOutButton()->toolTip() == QStringLiteral("Smaller text (Ctrl+-)"));
    CHECK(dialog.zoomOutButton()->text() == QStringLiteral(u"−"));
    CHECK(dialog.zoomInButton()->size() == QSize(28, 28));
    CHECK(dialog.fixedButton()->height() == 24);
    CHECK(dialog.titleLabel()->text() == QStringLiteral("Transcript"));
}

TEST_CASE("refreshing the typography keeps the scroll fraction", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"), 100);
    fv::TranscriptViewerDialog dialog;
    QString text;
    for (int i = 0; i < 200; ++i) {
        text += QStringLiteral("Paragraph %1 of a long transcript.\n\n").arg(i);
    }
    dialog.setTranscript(text);
    dialog.show();
    QApplication::processEvents();
    QScrollBar* bar = dialog.view()->verticalScrollBar();
    REQUIRE(bar->maximum() > 0);
    bar->setValue(bar->maximum() / 2);
    const double before = static_cast<double>(bar->value()) / bar->maximum();
    dialog.zoom(1);
    QApplication::processEvents();
    const double after = static_cast<double>(bar->value()) / bar->maximum();
    CHECK(after == Catch::Approx(before).margin(0.01));
}

// Map a08 A08-42 and A08-67: the zoom steps, the default step, and the keys (Ctrl+= is a second
// zoom-in key, Ctrl+0 resets).
TEST_CASE("the zoom steps and keys are OpenWhisper's", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    const std::vector<double> steps(std::begin(fv::TranscriptViewerDialog::kZoomSteps),
                                    std::end(fv::TranscriptViewerDialog::kZoomSteps));
    CHECK(steps == std::vector<double>{0.85, 1.0, 1.15, 1.3, 1.5, 1.75});
    CHECK(fv::TranscriptViewerDialog::kDefaultZoomIndex == 1);
    CHECK(fv::ReaderView::kMaxMeasure == 780);
    fv::TranscriptViewerDialog dialog;
    QStringList keys;
    for (const QShortcut* shortcut : dialog.findChildren<QShortcut*>()) {
        keys.append(shortcut->key().toString(QKeySequence::PortableText));
    }
    CHECK(keys.contains(QStringLiteral("Ctrl+=")));
    CHECK(keys.contains(QStringLiteral("Ctrl+0")));
    QShortcut* reset = nullptr;
    QShortcut* second = nullptr;
    for (QShortcut* shortcut : dialog.findChildren<QShortcut*>()) {
        const QString text = shortcut->key().toString(QKeySequence::PortableText);
        if (text == QStringLiteral("Ctrl+0")) {
            reset = shortcut;
        } else if (text == QStringLiteral("Ctrl+=")) {
            second = shortcut;
        }
    }
    REQUIRE(second != nullptr);
    REQUIRE(reset != nullptr);
    Q_EMIT second->activated();
    CHECK(dialog.zoomFactor() == 1.15);
    Q_EMIT reset->activated();
    CHECK(dialog.zoomFactor() == 1.0);
}
