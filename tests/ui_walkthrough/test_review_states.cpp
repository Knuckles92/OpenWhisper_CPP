// What the storyboard shows around its rows (ui_qt/widgets/walkthrough_review.py): the work in
// progress and its Cancel, the copy notices, a missing screenshot, a draft without its audio, an
// empty or unreadable folder, the save bar's two offers and the row drag's MIME type. Not in
// OpenWhisper's tests; each check names the Python lines it holds to.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QTest>
#include <QToolButton>
#include <QUrl>

#include "base/identity.h"
#include "pyport/text.h"
#include "review_support.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "walkthrough/document.h"
#include "walkthrough/library.h"

namespace fvtest {
namespace {

QString labelText(QWidget* parent, const char* name) {
    const auto labels = parent->findChildren<QLabel*>(QString::fromLatin1(name));
    return labels.isEmpty() ? QString() : labels.front()->text();
}

} // namespace

// run (walkthrough_review.py:1292-1333), _done and cancel (1335-1368).
TEST_CASE("work off the UI thread holds every action until it ends",
          "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->show();
    review->showFolder(capture.folder);
    std::vector<bool> busy;
    QObject::connect(&*review, &fv::WalkthroughReview::busyChanged,
                     [&busy](bool on) { busy.push_back(on); });
    review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    review->saveButton()->click();
    REQUIRE(review->isBusy());
    CHECK(review->notice()->text() ==
          QStringLiteral(u"Working… Your saved documents stay as they are if this fails."));
    CHECK(review->cancelButton()->isVisible());
    CHECK_FALSE(review->scroll()->isEnabled());
    for (QWidget* control : std::initializer_list<QWidget*>{
             review->saveButton(), review->discardButton(), review->pdfButton(),
             review->markdownButton(), review->openButton(), review->closeButton()}) {
        CHECK_FALSE(control->isEnabled());
    }
    for (QAction* action : {review->retryAction(), review->deleteAction(), review->folderAction(),
                            review->copyAgentAction()}) {
        CHECK_FALSE(action->isEnabled());
    }

    // A second piece of work is refused while one runs, without asking the gate.
    int ran = 0;
    review->run([&ran](const fv::CancelToken&) {
        ++ran;
        return fv::ReviewOutcome{};
    });
    CHECK(review.host.acquires == 1);

    review->cancel();
    CHECK(review->cancelRequested());
    CHECK(review->notice()->text() == QStringLiteral(u"Stopping safely…"));
    review.wait();
    CHECK(ran == 0);
    CHECK_FALSE(review->isBusy());
    CHECK(busy == std::vector<bool>{true, false});
    CHECK(review.host.releases == 1);
    CHECK_FALSE(review->cancelButton()->isVisible());
    CHECK(review->scroll()->isEnabled());
    CHECK(review->notice()->text() == QStringLiteral("Couldn't finish: canceled"));
    // Canceled before it started: nothing saved, the edits stay to save again.
    CHECK(review->isDirty());
    CHECK(review->saveButton()->isEnabled());
    CHECK(wt::load_document(capture.folder).sections[0].text == "first");
}

TEST_CASE("work that fails with no standard exception still gives the gate back",
          "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->run([](const fv::CancelToken&) -> fv::ReviewOutcome { throw 42; });
    review.wait();
    CHECK_FALSE(review->isBusy());
    CHECK(review.host.releases == 1);
    CHECK(review->notice()->text() == QStringLiteral("Couldn't finish: unknown error"));
}

// _copy (walkthrough_review.py:1400-1416) and _COPIED (88-93).
TEST_CASE("copying says what it copied, or that it could not", "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->pdfButton()->click();
    REQUIRE(review.host.copies.size() == 1);
    CHECK(review.host.copies.back().image.isNull());
    CHECK(review->notice()->text() == QStringLiteral("PDF copied."));
    review->markdownButton()->click();
    CHECK(review->notice()->text() == QStringLiteral("Markdown copied."));
    REQUIRE(review->copyWordButton()->isEnabled());
    review->copyWordButton()->click();
    REQUIRE(review.host.copies.size() == 3);
    CHECK(review.host.copies.back().files ==
          std::vector<std::filesystem::path>{capture.folder / "walkthrough.docx"});
    CHECK(review->notice()->text() == QStringLiteral("Word document copied."));
    review.host.copyWorks = false;
    review->pdfButton()->click();
    CHECK(review->notice()->text() ==
          QStringLiteral("Copying failed. Drag the file instead, or show it in its folder."));
}

// _Row and _Row._picture (walkthrough_review.py:556-635), _spoken_rows (1052-1063).
TEST_CASE("a screenshot that can't be read, and a section without one, keep their places",
          "[walkthrough_review][widget]") {
    Capture capture;
    const auto sections = wt::load_document(capture.folder).sections;
    REQUIRE(sections[0].shot.has_value());
    const auto name = QString::fromStdString(fv::path_to_utf8(sections[0].shot->filename()));
    // Still there (load_document requires it), but no picture any more.
    writeFile(*sections[0].shot, "not a picture");
    Review review;
    CHECK(review->showFolder(capture.folder));
    const auto rows = review->rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]->preview() == nullptr);
    REQUIRE(rows[0]->placeholder() != nullptr);
    CHECK(rows[0]->placeholder()->text() == name + QStringLiteral(" is missing"));
    CHECK(rows[0]->check()->accessibleName() == QStringLiteral("Include ") + name);
    REQUIRE(sections[0].kept_at_s.has_value());
    CHECK(labelText(rows[0], "wlStamp") ==
          QStringLiteral("Screenshot at ") +
              QString::fromStdString(wt::clock(*sections[0].kept_at_s)));
    CHECK(rows[1]->preview() != nullptr);
    REQUIRE(rows[2]->placeholder() != nullptr);
    CHECK(rows[2]->placeholder()->text() == QStringLiteral("No screenshot"));
    CHECK(rows[2]->check()->accessibleName() == QStringLiteral("No screenshot"));
}

// _draft_banner (walkthrough_review.py:1188-1210), _show_header (1085-1111), _show_actions
// (1150-1178).
TEST_CASE("an interrupted walkthrough without its audio says restarting finishes it",
          "[walkthrough_review][widget]") {
    Capture capture;
    writeFile(capture.folder / "draft.json", "{}");
    std::filesystem::remove(capture.folder / "walkthrough.json");
    Review review(true, true);
    CHECK_FALSE(review->showFolder(capture.folder));
    CHECK(review->rows().empty());
    CHECK(labelText(review->board(), "wlBannerTitle") ==
          QStringLiteral("This walkthrough was interrupted"));
    CHECK(labelText(review->board(), "wlBannerBody") ==
          QStringLiteral("Its screenshots are kept. The audio hasn't been recovered yet; "
                         "restarting ") +
              QString::fromUtf8(fv::identity().product.data(),
                                static_cast<qsizetype>(fv::identity().product.size())) +
              QStringLiteral(" finishes that."));
    CHECK(review->recoverButton()->isVisibleTo(&*review));
    CHECK_FALSE(review->recoverButton()->isEnabled());
    // No draft.json start: the folder's name says when.
    CHECK(review->titleLabel()->text() == QStringLiteral(u"Wednesday, October 21 · 2:32 PM"));
    CHECK(review->metaLabel()->text() == QStringLiteral("Not finished yet"));
    CHECK(review->kindPill()->text() == QStringLiteral("Interrupted"));
    CHECK(review->kindPill()->property("tone").toString() == QStringLiteral("warning"));
    CHECK(review->kindPill()->isVisibleTo(&*review));
    CHECK_FALSE(review->pdfButton()->isVisibleTo(&*review));
    CHECK_FALSE(review->openButton()->isVisibleTo(&*review));
    CHECK(review->moreButton()->isVisibleTo(&*review));
    CHECK(review->deleteAction()->isEnabled());
    CHECK_FALSE(review->retryAction()->isEnabled());
    CHECK(review->notice()->text().isEmpty());
}

// show_folder's empty and error bodies (walkthrough_review.py:1019-1042).
TEST_CASE("with nothing to show the storyboard says how to start one",
          "[walkthrough_review][widget]") {
    Review review;
    CHECK_FALSE(review->showFolder(std::nullopt));
    CHECK(labelText(review->board(), "wlEmptyTitle") == QStringLiteral("No walkthroughs yet"));
    CHECK(labelText(review->board(), "wlEmptyBody") ==
          QStringLiteral("Start one from the Walkthrough tab or the tray. Each one you finish is "
                         "kept here, ready to review and copy again."));
    CHECK_FALSE(review->titleLabel()->isVisibleTo(&*review));
    CHECK_FALSE(review->kindPill()->isVisibleTo(&*review));
    CHECK_FALSE(review->moreButton()->isVisibleTo(&*review));
    CHECK_FALSE(review->versionBar()->isVisibleTo(&*review));
    CHECK_FALSE(review->folder().has_value());
}

TEST_CASE("a walkthrough that can't be read says so", "[walkthrough_review][widget]") {
    Capture capture;
    writeFile(capture.folder / "walkthrough.json", "not json");
    Review review;
    CHECK_FALSE(review->showFolder(capture.folder));
    CHECK(labelText(review->board(), "wlEmptyTitle") ==
          QStringLiteral("This walkthrough can't be opened"));
    CHECK_FALSE(labelText(review->board(), "wlEmptyBody").isEmpty());
    CHECK(review->rows().empty());
    CHECK_FALSE(review->pdfButton()->isVisibleTo(&*review));
    // Still there to delete.
    CHECK(review->deleteAction()->isEnabled());
}

// _set_dirty and _discard (walkthrough_review.py:1234-1250).
TEST_CASE("the save bar offers a redo only for the words of a written-up walkthrough",
          "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    CHECK_FALSE(review->saveBar()->isVisibleTo(&*review));
    review->rows()[0]->editor()->setPlainText(QStringLiteral("corrected"));
    CHECK(review->saveBar()->isVisibleTo(&*review));
    CHECK(review->saveText()->text() ==
          QStringLiteral("You have unsaved edits. Saving rebuilds the PDF and Word document."));
    CHECK(review->saveButton()->objectName() == QStringLiteral("wlPrimary"));
    CHECK_FALSE(review->saveRedoButton()->isVisibleTo(&*review));

    writeFile(capture.folder / "write-up.md", "# Written\n\nWords.");
    writeFile(capture.folder / "write-up.pdf", "%PDF-1.4 old");
    review->showFolder(capture.folder, true);
    review->rows()[1]->check()->setChecked(false);
    CHECK(review->saveText()->text() ==
          QStringLiteral("Saving rebuilds the PDF and Word document. Your AI write-ups stay as "
                         "they are; redo it to write up the new words as well."));
    CHECK(review->saveButton()->objectName() == QStringLiteral("wlSecondary"));
    CHECK(review->saveRedoButton()->isVisibleTo(&*review));

    // Discard reloads the same version, as it was saved.
    review->discardButton()->click();
    CHECK_FALSE(review->isDirty());
    CHECK(review->isSpoken());
    CHECK_FALSE(review->saveBar()->isVisibleTo(&*review));
    CHECK(review->saveButton()->objectName() == QStringLiteral("wlPrimary"));
    CHECK(review->rows()[1]->check()->isChecked());
}

// run without a host submit (walkthrough_review.py:1326-1330): a thread of its own, whose result
// comes back to the GUI thread.
TEST_CASE("with no executor the work runs on a thread of its own and comes back",
          "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    auto host = review.host.callbacks();
    host.submit = nullptr;
    review->bind(host);
    review->showFolder(capture.folder);
    const auto gui = std::this_thread::get_id();
    std::atomic<bool> elsewhere{false};
    review->run(
        [gui, &elsewhere](const fv::CancelToken&) {
            elsewhere = std::this_thread::get_id() != gui;
            return fv::ReviewOutcome{};
        },
        std::nullopt, QStringLiteral("Done here."));
    // Its result waits for the GUI thread's event loop.
    CHECK(review->isBusy());
    for (int i = 0; i < 500 && review->isBusy(); ++i) {
        QTest::qWait(10);
    }
    REQUIRE_FALSE(review->isBusy());
    CHECK(elsewhere);
    CHECK(review.host.releases == 1);
    CHECK(review->notice()->text() == QStringLiteral("Done here."));
}

// _copy_screenshot without a host copy_files (walkthrough_review.py:1427-1435) writes the real
// clipboard: written for the owner, never run by the default presets.
TEST_CASE("with no host copy a screenshot goes on the clipboard",
          "[walkthrough_review][widget][interactive]") {
    Capture capture;
    Review review(false);
    review->showFolder(capture.folder);
    auto* preview = review->rows()[0]->preview();
    REQUIRE(preview != nullptr);
    preview->copyButton()->click();
    CHECK(review->notice()->text() == QStringLiteral("Screenshot copied."));
    const QMimeData* data = QApplication::clipboard()->mimeData();
    REQUIRE(data != nullptr);
    const QString path = QString::fromStdString(fv::path_to_utf8(preferred(preview->image())));
    CHECK(data->hasImage());
    CHECK(data->text() == path);
    REQUIRE(data->urls().size() == 1);
    CHECK(data->urls()[0] == QUrl::fromLocalFile(path));
}

TEST_CASE("a dragged row is named after the product, not spelled", "[walkthrough_review]") {
    CHECK(fv::WalkthroughReview::rowMime() ==
          QStringLiteral("application/x-") +
              QString::fromStdString(fv::py::lower(fv::identity().product)) +
              QStringLiteral("-walkthrough-row"));
}

} // namespace fvtest
