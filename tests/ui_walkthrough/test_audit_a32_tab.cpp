// Checklist lines of docs/port/map/a32-user-facing-features.md that the tab's and the
// storyboard's ported tests left unpinned: the order of the Style and On-finish-copy menus and
// the PDF default (WT-04, WT-06); the empty stage's three newest walkthroughs, opened by a click,
// Enter or Space (WT-08); the storyboard's Copy Word, Open PDF and a screenshot opening full
// size (WT-09); Saved Walkthroughs applying a retention after its question (WT-76).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QTest>
#include <QUrl>

#include "base/utf8.h"
#include "review_support.h"
#include "settings/testing/memory_store.h"
#include "tab_support.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "walkthrough/library.h"
#include "walkthrough/styles.h"

namespace fvtest {
namespace {

using fv::Json;
namespace tab = fv::walkthrough_tab;

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

TEST_CASE("the style menu lists every style, then your own, then the prompt and new style",
          "[walkthrough_tab][widget]") {
    TabRig rig;
    rig.settings->mutate(
        [](Json& settings) { wt::save_style(settings, "Onboarding", "A checklist."); });
    rig->loadSettings();
    rig->promptEdit()->setPlainText(QStringLiteral("Words of my own."));

    QStringList expected;
    for (const auto& style : wt::load_styles(*rig.settings->snapshot())) {
        expected << qs(style.name);
    }
    // The built-in style first, then the starters, then the user's own.
    REQUIRE(expected.size() == 6);
    CHECK(expected.mid(0, 5) ==
          QStringList{QStringLiteral("Walkthrough"), QStringLiteral("Step-by-step guide"),
                      QStringLiteral("Bug report"), QStringLiteral("Design feedback"),
                      QStringLiteral("Task for an agent")});
    CHECK(expected[5] == QStringLiteral("Onboarding"));
    expected << qs(tab::OWN_PROMPT) << QString() << QStringLiteral("Edit the prompt…")
             << qs(tab::NEW_STYLE);
    CHECK(menuTexts(rig->styleField()) == expected);
    CHECK(menuTexts(rig->styleField(), true) == QStringList{qs(tab::OWN_PROMPT)});
}

TEST_CASE("the copy menu lists the five choices in order, PDF first", "[walkthrough_tab][widget]") {
    TabRig rig;
    CHECK(rig->copyField()->value() == QStringLiteral("PDF"));
    CHECK(menuTexts(rig->copyField()) == QStringList{QStringLiteral("PDF"), QStringLiteral("Agent"),
                                                     QStringLiteral("PDF + Markdown"),
                                                     QStringLiteral("Markdown"),
                                                     QStringLiteral("Word")});
}

TEST_CASE("the empty stage offers the three newest walkthroughs to a click, Enter or Space",
          "[walkthrough_tab][widget]") {
    Capture capture;
    for (const char* name : {"2026-10-18_09-00-00", "2026-10-19_09-00-00", "2026-10-20_09-00-00"}) {
        std::filesystem::copy(capture.folder, capture.root / name,
                              std::filesystem::copy_options::recursive);
    }
    TabRig::Options options;
    options.root = capture.root;
    TabRig rig(options);
    rig->resize(1100, 760);
    rig->show();

    const auto activations = {0, 1, 2};
    for (const int how : activations) {
        CAPTURE(how);
        rig->closeWalkthrough();
        rig.settle();
        REQUIRE(rig->stageState() == QStringLiteral("empty"));
        const auto& rows = rig->recentRows();
        REQUIRE(rows.size() == 3);
        CHECK(rows[0]->folder() == capture.folder);
        CHECK(rows[1]->folder() == capture.root / "2026-10-20_09-00-00");
        CHECK(rows[2]->folder() == capture.root / "2026-10-19_09-00-00");
        fv::WalkthroughRecentRow* row = rows[1];
        const std::filesystem::path wanted = row->folder();
        QApplication::processEvents();
        if (how == 0) {
            QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, row->rect().center());
        } else {
            QTest::keyClick(row, how == 1 ? Qt::Key_Enter : Qt::Key_Space);
        }
        CHECK(rig->stageState() == QStringLiteral("review"));
        CHECK(rig->review()->folder() == wanted);
    }
}

TEST_CASE("the storyboard copies Word, opens the PDF and opens a screenshot full size",
          "[walkthrough_review][widget]") {
    Capture capture;
    Review review;
    review->showFolder(capture.folder);
    REQUIRE(std::filesystem::is_regular_file(capture.result.docx()));

    CHECK(review->pdfButton()->text() == QStringLiteral("Copy PDF"));
    CHECK(review->markdownButton()->text() == QStringLiteral("Copy Markdown"));
    CHECK(review->copyWordButton()->text() == QStringLiteral("Copy Word"));
    CHECK(review->openButton()->text() == QStringLiteral("Open PDF"));

    REQUIRE(review->copyWordButton()->isEnabled());
    review->copyWordButton()->click();
    REQUIRE(review.host.copies.size() == 1);
    CHECK(review.host.copies.back().files ==
          std::vector<std::filesystem::path>{capture.result.docx()});
    CHECK(review->notice()->text() == QStringLiteral("Word document copied."));

    review->openButton()->click();
    REQUIRE(review.opened.size() == 1);
    CHECK(std::filesystem::equivalent(
        fv::path_from_utf8(review.opened.back().toLocalFile().toStdString()), capture.result.pdf));

    auto* preview = review->rows()[0]->preview();
    QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
    REQUIRE(review.opened.size() == 2);
    const auto shot = fv::path_from_utf8(review.opened.back().toLocalFile().toStdString());
    CHECK(shot.filename() == "shot-1.png");
    CHECK(std::filesystem::equivalent(shot.parent_path(), capture.folder));
}

TEST_CASE("a retention is applied only after its question, deleting expired walkthroughs now",
          "[walkthrough_library][widget]") {
    Capture capture;
    // A finished walkthrough last saved sixty days ago, and an interrupted draft as old.
    const auto old = capture.root / "2026-08-01_09-00-00";
    std::filesystem::copy(capture.folder, old, std::filesystem::copy_options::recursive);
    const auto draft = capture.root / "2026-08-02_09-00-00";
    std::filesystem::copy(capture.folder, draft, std::filesystem::copy_options::recursive);
    writeFile(draft / "draft.json", "{}");
    const auto longAgo =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 60);
    std::filesystem::last_write_time(old / "walkthrough.json", longAgo);
    std::filesystem::last_write_time(draft / "walkthrough.json", longAgo);
    fv::MemorySettingsStore settings;
    Host host;
    fv::WalkthroughLibraryDialog dialog(settings, capture.root, host.callbacks());
    auto* keep = dialog.findChild<QComboBox*>(QStringLiteral("wlKeep"));
    REQUIRE(keep);
    CHECK(keep->currentText() == QStringLiteral("Forever"));
    std::vector<QString> asked;
    QMessageBox::StandardButton answer = QMessageBox::No;
    dialog.review()->setQuestion([&](QWidget*, const QString&, const QString& text,
                                     QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        asked.push_back(text);
        return answer;
    });

    // Refused: nothing changes and the choice goes back to the saved one.
    keep->setCurrentIndex(keep->findData(30));
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == QStringLiteral("Delete completed walkthroughs older than 30 days now and at "
                                     "each startup? Interrupted drafts and standalone screenshots "
                                     "are never deleted."));
    CHECK(keep->currentData().toInt() == 0);
    CHECK(fv::walkthrough::retention_days(capture.root) == 0);
    CHECK(std::filesystem::exists(old));

    // Confirmed: saved, and the expired finished walkthrough goes now; the draft stays.
    answer = QMessageBox::Yes;
    keep->setCurrentIndex(keep->findData(30));
    host.executor.run_all();
    QApplication::processEvents();
    CHECK(asked.size() == 2);
    CHECK(fv::walkthrough::retention_days(capture.root) == 30);
    CHECK_FALSE(std::filesystem::exists(old));
    CHECK(std::filesystem::exists(draft));
    CHECK(std::filesystem::exists(capture.folder));
    CHECK(keep->currentData().toInt() == 30);
    CHECK(dialog.review()->notice()->text() == QStringLiteral("Retention updated."));

    // Back to Forever asks nothing more (only unsaved edits would be asked about).
    keep->setCurrentIndex(keep->findData(0));
    host.executor.run_all();
    QApplication::processEvents();
    CHECK(asked.size() == 2);
    CHECK(fv::walkthrough::retention_days(capture.root) == 0);
}

} // namespace fvtest
