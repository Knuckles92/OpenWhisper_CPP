// The walkthrough storyboard (ui_qt/widgets/walkthrough_review.py), from
// tests/test_walkthrough_storyboard.py: rows, reordering, editing a write-up, its versions and
// copying. Work off the UI thread runs on a ManualExecutor; write-ups come from a stand-in agent.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDropEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QTest>
#include <QToolButton>

#include "review_support.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_walkthrough/walkthrough_review.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "walkthrough/library.h"

namespace fvtest {
namespace {

constexpr const char* kWriteUp = "# Two slow spots\n\n"
                                 "1. Billing stalls.\n\n"
                                 "   ![Billing](shot-1.png)\n\n"
                                 "2. Invoices hang.\n\n"
                                 "![Invoices](shot-2.png)\n\n"
                                 "Both happen after sign-in.";

// OpenWhisper's _written: the capture as if an agent had written it up.
void written(const Capture& capture) {
    writeFile(capture.folder / "write-up.md", kWriteUp);
    writeFile(capture.folder / "write-up.pdf", "%PDF-1.4 old");
}

std::vector<std::string> savedTexts(const std::filesystem::path& folder) {
    std::vector<std::string> out;
    for (const auto& section : wt::load_document(folder).sections) {
        out.push_back(section.text);
    }
    return out;
}

std::vector<QString> qtexts(std::initializer_list<const char*> items) {
    std::vector<QString> out;
    for (const char* item : items) {
        out.push_back(QString::fromUtf8(item));
    }
    return out;
}

std::vector<bool> checks(const Review& review) {
    std::vector<bool> out;
    for (auto* row : review->rows()) {
        out.push_back(row->check()->isChecked());
    }
    return out;
}

// OpenWhisper's _large_capture: two screenshots as large as a real screen's, so previews can
// take any size.
struct LargeCapture {
    fv::testkit::TempDataRoot tmp;
    std::shared_ptr<wt::FakeRenderer> renderer = std::make_shared<wt::FakeRenderer>();
    ScopedRenderer installed{renderer};
    std::filesystem::path folder;

    LargeCapture() {
        using namespace std::chrono_literals;
        folder = tmp.path() / "captures" / "2026-10-30_14-15-02";
        std::filesystem::create_directories(folder);
        wt::WalkthroughSession::Options options;
        options.started_at =
            std::chrono::local_days{std::chrono::year{2026} / 10 / 30} + 14h + 15min + 2s;
        options.folder = folder;
        wt::WalkthroughSession session(std::move(options));
        session.add_shot(std::make_shared<wt::FakeShotImage>(1600, 1000, 0xe8845c), 14.0);
        session.add_shot(std::make_shared<wt::FakeShotImage>(1600, 1000, 0xe8845c), 36.0);
        const std::vector<std::string> words{"On billing, Change plan stalls for three seconds.",
                                             "Then the invoices table hangs on its last rows.",
                                             "That's it."};
        wt::build_documents(session, session.saved_shots(), words, words, 50.0, {},
                            fv::Json::object());
        session.end();
    }
};

} // namespace

// from tests/test_walkthrough_storyboard.py::test_rows_show_each_screenshot_beside_its_words
TEST_CASE("rows show each screenshot beside its words", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    CHECK(review.texts() == qtexts({"first", "second", "last"}));
    CHECK(checks(review) == std::vector<bool>{true, true, false});
    CHECK_FALSE(review->versionBar()->isVisibleTo(&*review));
}

// from tests/test_walkthrough_storyboard.py::
//     test_held_notes_show_on_their_rows_and_stay_through_a_save
TEST_CASE("held notes show on their rows and stay through a save",
          "[test_walkthrough_storyboard][widget]") {
    const auto capture = notedCapture();
    Review review;
    review->showFolder(capture->folder);
    std::vector<std::vector<QString>> shown;
    for (auto* row : review->rows()) {
        std::vector<QString> notes;
        for (auto* label : row->findChildren<QLabel*>(QStringLiteral("wlNote"))) {
            notes.push_back(label->text().split(u'\n')[1]);
        }
        shown.push_back(notes);
    }
    CHECK(shown == std::vector<std::vector<QString>>{{},
                                                     {QStringLiteral("make it purple")},
                                                     {QStringLiteral("sign off warmly")}});
    review->rows()[0]->editor()->setPlainText(QStringLiteral("first, fixed"));
    review->saveButton()->click();
    review.waitOk();
    std::vector<std::vector<std::string>> notes;
    for (const auto& section : wt::load_document(capture->folder).sections) {
        notes.push_back(section.notes);
    }
    CHECK(notes ==
          std::vector<std::vector<std::string>>{{}, {"make it purple"}, {"sign off warmly"}});
}

// from tests/test_walkthrough_storyboard.py::
//     test_moving_a_row_renumbers_it_and_saving_keeps_the_new_order
TEST_CASE("moving a row renumbers it and saving keeps the new order",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->moveRow(1, 0);
    CHECK(review->isDirty());
    std::vector<QString> steps;
    for (auto* row : review->rows()) {
        steps.push_back(row->step()->text());
    }
    CHECK(steps == qtexts({"1", "2", "3"}));
    CHECK(review->rows()[0]->editor()->accessibleName() == QStringLiteral("Section 1 transcript"));
    review->saveButton()->click();
    review.waitOk();
    CHECK(savedTexts(capture.folder) == std::vector<std::string>{"second", "first", "last"});
}

// from tests/test_walkthrough_storyboard.py::test_alt_arrows_in_the_words_move_their_row
TEST_CASE("alt arrows in the words move their row", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    auto* last = review->rows()[2];
    QTest::keyClick(last->editor(), Qt::Key_Up, Qt::AltModifier);
    auto at = [&] {
        const auto rows = review->rows();
        return std::find(rows.begin(), rows.end(), last) - rows.begin();
    };
    CHECK(at() == 1);
    QTest::keyClick(review->rows()[0]->editor(), Qt::Key_Up, Qt::AltModifier);
    CHECK(at() == 1);
}

// from tests/test_walkthrough_storyboard.py::
//     test_a_write_up_opens_on_itself_and_can_switch_to_the_words_as_spoken
TEST_CASE("a write-up opens on itself and can switch to the words as spoken",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    written(capture);
    review->showFolder(capture.folder);
    CHECK(review->versionBar()->isVisibleTo(&*review));
    CHECK(review.texts()[1] == QStringLiteral("2. Invoices hang."));
    CHECK(review->rows()[0]->line() == "   ![Billing](shot-1.png)");
    CHECK_FALSE(review->rows()[0]->check()->isVisibleTo(&*review));
    CHECK(review->chip()->nameLabel()->text() == QStringLiteral("write-up.pdf"));
    review->chooseVersion(1);
    CHECK(review.texts() == qtexts({"first", "second", "last"}));
    CHECK(review->chip()->nameLabel()->text() == QStringLiteral("walkthrough.pdf"));
}

// from tests/test_walkthrough_storyboard.py::
//     test_the_version_switch_sits_inside_the_header_beside_the_title
TEST_CASE("the version switch sits inside the header beside the title",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    written(capture);
    review->resize(900, 600);
    review->show();
    review->showFolder(capture.folder);
    QTest::qWait(20);
    const QRect bar = review->versionBar()->geometry();
    CHECK(bar.width() > 0);
    CHECK(bar.right() < review->width());
    CHECK(review->titleLabel()->geometry().right() < bar.left());
}

// from tests/test_walkthrough_storyboard.py::
//     test_saving_edited_write_up_rows_rebuilds_the_write_up_in_their_order
TEST_CASE("saving edited write-up rows rebuilds the write-up in their order",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    written(capture);
    review->showFolder(capture.folder);
    review->moveRow(1, 0);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("2. Invoices hang for four seconds."));
    review->saveButton()->click();
    review.waitOk();
    const std::string saved = readFile(capture.folder / "write-up.md");
    CHECK(saved.find("Invoices hang for four seconds") < saved.find("Billing stalls"));
    CHECK(saved.find("shot-2.png") < saved.find("shot-1.png"));
}

// from tests/test_walkthrough_storyboard.py::
//     test_editing_the_words_as_spoken_offers_to_redo_the_write_up
TEST_CASE("editing the words as spoken offers to redo the write-up",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    written(capture);
    review->showFolder(capture.folder, true);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("first, fixed"));
    CHECK(review->saveRedoButton()->isVisibleTo(&*review));
    CHECK(review->saveButton()->text() == QStringLiteral("Save changes"));
    review.agent.reply = "# Redone\n\nWords.";
    review->saveRedoButton()->click();
    review.waitOk();
    CHECK(review.agent.calls.size() == 1);
    CHECK(wt::load_document(capture.folder).sections[0].text == "first, fixed");
}

// from tests/test_walkthrough_storyboard.py::
//     test_saving_the_words_as_spoken_keeps_the_write_up_and_stays_on_them
TEST_CASE("saving the words as spoken keeps the write-up and stays on them",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    written(capture);
    review->showFolder(capture.folder, true);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("first, fixed"));
    review->saveButton()->click();
    review.waitOk();
    CHECK(review->isSpoken());
    CHECK(review->versionBar()->isVisibleTo(&*review));
    CHECK(review->versionBar()->isEnabled());
    CHECK(review.texts()[0] == QStringLiteral("first, fixed"));
    CHECK(readFile(capture.folder / "write-up.md") == kWriteUp);
    review->chooseVersion(0);
    CHECK(review.texts()[1] == QStringLiteral("2. Invoices hang."));
}

// from tests/test_walkthrough_storyboard.py::
//     test_writing_it_up_from_the_menu_leaves_the_version_switch_working
TEST_CASE("writing it up from the menu leaves the version switch working",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    // It finished as spoken, then More → Write up with AI.
    review.agent.reply = "# Written\n\nWords.";
    review->retryAction()->trigger();
    review.waitOk();
    CHECK(review->versionBar()->isVisibleTo(&*review));
    CHECK_FALSE(review->isSpoken());
    CHECK(review->versionBar()->isEnabled());
    CHECK(review->closeButton()->isEnabled());
    CHECK(review->retryAction()->isEnabled());
    CHECK(review->pdfButton()->isEnabled());
    CHECK(review->saveRedoButton()->isEnabled());
    review->chooseVersion(1);
    CHECK(review.texts() == qtexts({"first", "second", "last"}));
    review->chooseVersion(0);
    CHECK(review.texts()[0] == QStringLiteral("# Written\n\nWords."));
}

// from tests/test_walkthrough_storyboard.py::
//     test_writing_it_up_again_keeps_the_one_before_under_a_version_picker
TEST_CASE("writing it up again keeps the one before under a version picker",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review.agent.reply = "# First";
    review->retryAction()->trigger();
    review.waitOk();
    CHECK_FALSE(review->versionPicker()->isVisibleTo(&*review));
    review.agent.reply = "# Second";
    review->retryAction()->trigger();
    review.waitOk();
    CHECK(review->versionPicker()->isVisibleTo(&*review));
    CHECK(review->versionPicker()->isEnabled());
    CHECK(review->versionPicker()->text().startsWith(QStringLiteral("Version 2 of 2")));
    const auto actions = review->versionMenu()->actions();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0]->text().split(QStringLiteral(" ·"))[0] == QStringLiteral("Version 2"));
    CHECK(actions[1]->text().split(QStringLiteral(" ·"))[0] == QStringLiteral("Version 1"));
    CHECK(actions[0]->isChecked());
    CHECK(actions[0]->text().endsWith(QStringLiteral("(latest)")));
    CHECK(review.texts()[0] == QStringLiteral("# Second"));
    actions[1]->trigger();
    CHECK(review.texts()[0] == QStringLiteral("# First"));
    CHECK(review->chip()->nameLabel()->text() == QStringLiteral("write-up-1.pdf"));
    CHECK(review->versionPicker()->text().startsWith(QStringLiteral("Version 1 of 2")));
    review->pdfButton()->click();
    REQUIRE_FALSE(review.host.copies.empty());
    CHECK(review.host.copies.back().files ==
          std::vector<std::filesystem::path>{capture.folder / "write-up-1.pdf"});
    // Saving an earlier one keeps it in place, and keeps it showing.
    review->rows()[0]->editor()->setPlainText(QStringLiteral("# First, fixed"));
    review->saveButton()->click();
    review.waitOk();
    CHECK(review->version() == 1);
    CHECK(review.texts()[0] == QStringLiteral("# First, fixed"));
    CHECK(readFile(capture.folder / "write-up.md").starts_with("# Second"));
    // The words as spoken have no versions.
    review->chooseVersion(1);
    CHECK_FALSE(review->versionPicker()->isVisibleTo(&*review));
}

// from tests/test_walkthrough_storyboard.py::
//     test_copying_waits_for_saved_edits_and_copies_the_version_shown
TEST_CASE("copying waits for saved edits and copies the version shown",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("unsaved"));
    review->pdfButton()->click();
    CHECK(review.host.copies.empty());
    review->showFolder(capture.folder);
    review->pdfButton()->click();
    REQUIRE(review.host.copies.size() == 1);
    CHECK(review.host.copies.back().files ==
          std::vector<std::filesystem::path>{capture.folder / "walkthrough.pdf"});
}

// from
// tests/test_walkthrough_storyboard.py::test_a_screenshot_grows_with_its_row_and_keeps_its_shape
TEST_CASE("a screenshot grows with its row and keeps its shape",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    Review review(false);
    review->showFolder(capture.folder);
    review->show();
    std::vector<QSize> sizes;
    for (const int width : {560, 900}) {
        review->resize(width, 700);
        for (int i = 0; i < 3; ++i) {
            QApplication::processEvents();
        }
        sizes.push_back(review->rows()[0]->preview()->size());
    }
    const QSize narrow = sizes[0];
    const QSize wide = sizes[1];
    CHECK(fv::WalkthroughReview::kPreviewMinWidth <= narrow.width());
    CHECK(narrow.width() < wide.width());
    CHECK(wide.width() <= fv::WalkthroughReview::kPreviewSize.width());
    for (const QSize size : sizes) {
        CHECK(std::abs(size.height() - size.width() * 1000.0 / 1600.0) <= 1);
    }
}

// from tests/test_walkthrough_storyboard.py::test_a_screenshot_left_out_fades_and_comes_back
TEST_CASE("a screenshot left out fades and comes back", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    auto* row = review->rows()[0];
    row->check()->setChecked(false);
    CHECK_FALSE(row->preview()->included());
    row->check()->setChecked(true);
    CHECK(row->preview()->included());
}

// from tests/test_walkthrough_storyboard.py::
//     test_a_screenshot_corner_button_copies_just_that_picture
TEST_CASE("a screenshot corner button copies just that picture",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    auto* row = review->rows()[1];
    auto* button = row->preview()->copyButton();
    CHECK(button->x() + fv::WalkthroughReview::kShotCopySize +
              fv::WalkthroughReview::kShotCopyInset <=
          row->preview()->width());
    QTest::mouseClick(button, Qt::LeftButton);
    CHECK(review.opened.empty());
    REQUIRE(review.host.copies.size() == 1);
    const auto& copied = review.host.copies.back();
    REQUIRE(copied.files.size() == 1);
    const auto path = copied.files[0];
    CHECK(path == preferred(row->preview()->image()));
    CHECK(std::filesystem::equivalent(path.parent_path(), capture.folder));
    CHECK(copied.text == QString::fromStdString(fv::path_to_utf8(path)));
    CHECK_FALSE(copied.image.isNull());
    CHECK(review->notice()->text() == QStringLiteral("Screenshot copied."));
}

// from tests/test_walkthrough_storyboard.py::test_a_screenshot_copies_even_with_unsaved_edits
TEST_CASE("a screenshot copies even with unsaved edits", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->rows()[0]->editor()->setPlainText(QStringLiteral("unsaved"));
    review->rows()[0]->preview()->copyButton()->click();
    CHECK(review.host.copies.size() == 1);
    CHECK(review->notice()->text() == QStringLiteral("Screenshot copied."));
}

// Not in OpenWhisper's tests: where a dragged row lands (by row centre), and that a drop whose
// source is not one of the board's grips is ignored. A real drag's source cannot be made outside
// QDrag::exec, which takes over the pointer, so the grip's own drop is left to the owner.
TEST_CASE("a row dropped on the board lands by row centre", "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    review->resize(900, 700);
    review->show();
    QApplication::processEvents();
    auto* board = review->board();
    const auto rows = review->rows();
    CHECK(board->slot(rows[0]->geometry().center().y() - 1) == 0);
    CHECK(board->slot(rows[0]->geometry().center().y() + 1) == 1);
    CHECK(board->slot(rows[2]->geometry().bottom() + 50) == 3);
    std::vector<std::pair<int, int>> moves;
    QObject::connect(board, &fv::ReviewBoard::moved,
                     [&moves](int start, int end) { moves.emplace_back(start, end); });
    QMimeData data;
    data.setData(fv::WalkthroughReview::rowMime(), QByteArray());
    QDropEvent event(QPointF(10, rows[2]->geometry().bottom() + 50), Qt::MoveAction, &data,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(board, &event);
    CHECK(moves.empty());
    CHECK_FALSE(board->dropMarker()->isVisible());
}

} // namespace fvtest
