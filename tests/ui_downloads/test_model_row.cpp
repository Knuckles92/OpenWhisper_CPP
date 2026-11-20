// The model row of Settings > Downloads: what each ModelRowState shows, the in-row progress
// overlay, and the row's clicks and keys (download_row.py and model_row_widget.py).
#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QSignalSpy>
#include <QTest>

#include "models/downloads_model.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_downloads_support.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"

using fv::ModelRowWidget;
using fv::models::CachedModelInfo;
using fv::models::ModelRowInput;

namespace {

constexpr char kModel[] = "parakeet-v3";

CachedModelInfo cachedInfo(std::int64_t bytes = 713'975'456) {
    CachedModelInfo info;
    info.repo_id = "nvidia/parakeet-tdt-0.6b-v3";
    info.size_on_disk = bytes;
    return info;
}

QString badgeTone(const ModelRowWidget& row) {
    return row.badge()->property("tone").toString();
}

// Whether the widget is shown by its own flag: the row itself is never shown in these tests.
bool shown(const QWidget* widget) {
    return !widget->isHidden();
}

} // namespace

TEST_CASE("a model row that is not downloaded offers the estimate, a checkbox and Download",
          "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    row.updateState(input);

    CHECK(row.sizeLabel()->text() == "~714 MB");
    CHECK(row.sizeLabel()->property("muted").toBool());
    CHECK_FALSE(shown(row.badge()));
    CHECK(badgeTone(row) == "idle");
    CHECK(row.badge()->text() == "Not downloaded");
    CHECK(shown(row.selectCheckbox()));
    CHECK(row.selectCheckbox()->isEnabled());
    CHECK(shown(row.downloadButton()));
    CHECK(row.downloadButton()->isEnabled());
    CHECK(row.downloadButton()->toolTip().isEmpty());
    CHECK_FALSE(shown(row.setActiveButton()));
    CHECK_FALSE(shown(row.deleteButton()));
    CHECK_FALSE(shown(row.progressBar()));
    CHECK_FALSE(row.isCached());
    CHECK(row.sortSizeBytes() == 714'000'000.0);
}

TEST_CASE("a model row names the model, its languages and its repository", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    auto* name = row.findChild<fv::ElidingLabel*>("modelRowName");
    auto* summary = row.findChild<fv::ElidingLabel*>("modelRowSummary");
    REQUIRE(name != nullptr);
    REQUIRE(summary != nullptr);
    CHECK(name->text() == "Parakeet TDT 0.6B v3");
    CHECK(name->toolTip() == "parakeet-v3");
    CHECK(summary->toolTip() == "nvidia/parakeet-tdt-0.6b-v3");
    CHECK(row.repoId() == "nvidia/parakeet-tdt-0.6b-v3");
    CHECK(row.toolTip() == "Click to view technical details");
    CHECK(row.accessibleName() == "parakeet-v3 model");
    CHECK(row.accessibleDescription() ==
          "Open technical details. Model management actions are separate.");
    CHECK(row.selectCheckbox()->accessibleName() == "Select parakeet-v3 for download");
    CHECK(row.selectCheckbox()->toolTip() == "Select for batch download");
}

TEST_CASE("a downloaded model row shows the size on disk and Delete", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    input.info = cachedInfo();
    row.updateState(input);

    CHECK(row.sizeLabel()->text() == "714 MB");
    CHECK_FALSE(row.sizeLabel()->property("muted").toBool());
    CHECK(shown(row.badge()));
    CHECK(row.badge()->text() == "Downloaded");
    CHECK(badgeTone(row) == "downloaded");
    CHECK_FALSE(shown(row.selectCheckbox()));
    CHECK_FALSE(shown(row.downloadButton()));
    CHECK(shown(row.setActiveButton()));
    CHECK(shown(row.deleteButton()));
    CHECK(row.deleteButton()->isEnabled());
    CHECK(row.isCached());
    CHECK(row.sortSizeBytes() == 713'975'456.0);
    CHECK_FALSE(row.property("active").toBool());
}

TEST_CASE("the active model row is tinted and has no Set Active button", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    input.info = cachedInfo();
    input.is_active = true;
    row.updateState(input);

    CHECK(row.badge()->text() == "Active");
    CHECK(badgeTone(row) == "active");
    CHECK(row.property("active").toBool());
    CHECK_FALSE(shown(row.setActiveButton()));
    CHECK(row.isActive());

    // An active model that is not downloaded is not tinted, but stays the persisted selection.
    input.info.reset();
    row.updateState(input);
    CHECK_FALSE(row.property("active").toBool());
    CHECK(row.isActive());
    CHECK_FALSE(row.isCached());

    input.is_active = false;
    row.updateState(input);
    CHECK_FALSE(row.isActive());
}

TEST_CASE("a loaded model cannot be deleted", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    input.info = cachedInfo();
    input.is_loaded = true;
    row.updateState(input);

    CHECK(shown(row.deleteButton()));
    CHECK_FALSE(row.deleteButton()->isEnabled());
    CHECK(row.deleteButton()->toolTip() == QString::fromUtf8("In use — switch models first"));
}

TEST_CASE("a downloading model row shows the chip and an indeterminate overlay", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    row.resize(640, 56);
    ModelRowInput input;
    input.downloading = true;
    input.download_slot_busy = true;
    row.updateState(input);

    CHECK(row.badge()->text() == QString::fromUtf8("Downloading…"));
    CHECK(badgeTone(row) == "downloading");
    CHECK(shown(row.badge()));
    CHECK_FALSE(row.downloadButton()->isEnabled());
    CHECK_FALSE(row.selectCheckbox()->isEnabled());
    // Its own download in flight is not "another download".
    CHECK(row.downloadButton()->toolTip().isEmpty());
    CHECK(row.progressBar()->parent() == &row);
    CHECK(row.progressBar()->minimum() == 0);
    CHECK(row.progressBar()->maximum() == 0);

    row.setProgress(0, 0);
    CHECK(row.progressBar()->maximum() == 0);

    row.setProgress(357'000'000, 714'000'000);
    CHECK(row.progressBar()->maximum() == 100);
    CHECK(row.progressBar()->value() == 50);
    CHECK(row.sizeLabel()->text() == "357 MB of 714 MB");

    // Done: the bar is hidden and reset.
    input.downloading = false;
    row.updateState(input);
    CHECK_FALSE(shown(row.progressBar()));
    CHECK(row.progressBar()->value() == 0);
}

TEST_CASE("the progress overlay sits inside the row's margins at its bottom edge", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    row.resize(640, 60);
    row.show();
    ModelRowInput input;
    input.downloading = true;
    row.updateState(input);
    row.resize(700, 64);

    const QRect bar = row.progressBar()->geometry();
    CHECK(bar.left() == 14);
    CHECK(bar.width() == 700 - 14 - 12);
    CHECK(bar.height() == 6);
    CHECK(bar.bottom() + 1 == 64 - 10);
}

TEST_CASE("a queued model row says Queued, and a blocked or busy slot disables Download",
          "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    input.queued = true;
    input.download_slot_busy = true;
    row.updateState(input);

    CHECK(row.badge()->text() == "Queued");
    CHECK(badgeTone(row) == "queued");
    CHECK(shown(row.badge()));
    CHECK_FALSE(row.downloadButton()->isEnabled());
    CHECK(row.downloadButton()->toolTip() == "Another download is in progress");

    input = {};
    input.downloads_blocked = true;
    row.updateState(input);
    CHECK_FALSE(row.downloadButton()->isEnabled());
    CHECK(row.downloadButton()->toolTip() == "Downloads are disabled by HF_HUB_OFFLINE");

    input = {};
    input.selection_enabled = false;
    row.updateState(input);
    CHECK_FALSE(row.selectCheckbox()->isEnabled());
}

TEST_CASE("the usage chip shows its text and hides when empty", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    CHECK_FALSE(shown(row.usageLabel()));
    row.setUsage(QString::fromUtf8("Dictation · Meetings"));
    CHECK(shown(row.usageLabel()));
    CHECK(row.usageLabel()->text() == QString::fromUtf8("Dictation · Meetings"));
    row.setUsage(QString());
    CHECK_FALSE(shown(row.usageLabel()));
}

TEST_CASE("a model row matches a filter on its name or its repository", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    CHECK(row.matchesFilter("parakeet"));
    CHECK(row.matchesFilter("nvidia"));
    CHECK_FALSE(row.matchesFilter("moonshine"));
    CHECK(row.facts().model_name == "parakeet-v3");
}

TEST_CASE("the buttons carry the model name in their signals", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    ModelRowInput input;
    row.updateState(input);
    QSignalSpy download(&row, &ModelRowWidget::downloadClicked);
    QSignalSpy toggled(&row, &ModelRowWidget::selectionToggled);
    QSignalSpy details(&row, &ModelRowWidget::detailsRequested);

    row.downloadButton()->click();
    REQUIRE(download.count() == 1);
    CHECK(download.takeFirst().at(0).toString() == "parakeet-v3");

    row.selectCheckbox()->setChecked(true);
    REQUIRE(toggled.count() == 1);
    const QVariantList args = toggled.takeFirst();
    CHECK(args.at(0).toString() == "parakeet-v3");
    CHECK(args.at(1).toBool());
    CHECK(details.count() == 0);

    input.info = cachedInfo();
    row.updateState(input);
    QSignalSpy remove(&row, &ModelRowWidget::deleteClicked);
    QSignalSpy active(&row, &ModelRowWidget::setActiveClicked);
    row.deleteButton()->click();
    row.setActiveButton()->click();
    CHECK(remove.count() == 1);
    CHECK(active.count() == 1);
    CHECK(details.count() == 0);
}

TEST_CASE("a click on the row body and Return, Enter and Space open the details, a button click "
          "does not",
          "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    row.resize(640, 56);
    ModelRowInput input;
    row.updateState(input);
    row.show();
    QSignalSpy details(&row, &ModelRowWidget::detailsRequested);

    // The checkbox is a button: ticking it is not opening details.
    QTest::mouseClick(row.selectCheckbox(), Qt::LeftButton);
    CHECK(details.count() == 0);
    QTest::mouseClick(row.downloadButton(), Qt::LeftButton);
    CHECK(details.count() == 0);

    // The name label sits on the body.
    QTest::mouseClick(&row, Qt::LeftButton, Qt::NoModifier, QPoint(300, 28));
    REQUIRE(details.count() == 1);
    CHECK(details.takeFirst().at(0).toString() == "parakeet-v3");

    // A right click is not a request.
    QTest::mouseClick(&row, Qt::RightButton, Qt::NoModifier, QPoint(300, 28));
    CHECK(details.count() == 0);

    for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space}) {
        QTest::keyClick(&row, key);
        CHECK(details.count() == 1);
        details.clear();
    }
    QTest::keyClick(&row, Qt::Key_A);
    CHECK(details.count() == 0);
}

TEST_CASE("the row's buttons have the compact sizes", "[widget]") {
    fvtest::AppTheme theme;
    ModelRowWidget row(kModel);
    for (auto [button, width] :
         {std::pair{row.downloadButton(), 90}, std::pair{row.setActiveButton(), 90},
          std::pair{row.deleteButton(), 72}}) {
        CHECK(button->minimumWidth() == width);
        CHECK(button->maximumWidth() == width);
        // The sheet's min-height and max-height (28 px) are the content box; padding and the
        // border make the button 38 px tall.
        CHECK(button->font().family() == "Segoe UI");
    }
    CHECK(row.downloadButton()->objectName() == "modelDownloadButton");
    CHECK(row.deleteButton()->objectName() == "modelDeleteButton");
    CHECK(row.setActiveButton()->objectName() == "modelSetActiveButton");
}
