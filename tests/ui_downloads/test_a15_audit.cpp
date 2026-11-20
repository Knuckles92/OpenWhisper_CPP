// Checks the M10 sweep (auditor AU7) added for rows of docs/port/map/a15-models-downloads-ui.md
// (A15-01, A15-02, A15-07, A15-14) whose words no ported test pinned.

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QLineEdit>

#include "downloads_page_support.h"
#include "ui_downloads/downloads_page.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"

using namespace fvtest;

namespace {

QStringList itemTexts(const QComboBox& combo) {
    QStringList texts;
    for (int i = 0; i < combo.count(); ++i) {
        texts << combo.itemText(i);
    }
    return texts;
}

} // namespace

TEST_CASE("a15 audit: the Downloads header and toolbar carry OpenWhisper's words", "[widget]") {
    DownloadsRig rig;
    std::unique_ptr<fv::DownloadsPage> page = rig.page();
    CHECK(page->tabs()->accessibleName() == QStringLiteral("Downloads section"));
    CHECK(page->downloadAllButton()->text() == QStringLiteral(u"Download all…"));
    CHECK(page->downloadAllButton()->toolTip() ==
          QStringLiteral("Queue every model that is not downloaded yet"));
    // Nothing is downloaded: there is something to queue.
    CHECK_FALSE(page->downloadAllButton()->isHidden());
    CHECK(page->openFolderButton()->text() == QStringLiteral("Open folder"));
    CHECK(page->openFolderButton()->toolTip() ==
          QStringLiteral("Open the folder where downloaded models are stored"));
    CHECK(page->openFolderButton()->minimumWidth() >= 110);
    CHECK(page->filterEdit()->placeholderText() == QStringLiteral("Search models"));
    CHECK(page->filterEdit()->isClearButtonEnabled());
    CHECK(page->backendFilterCombo()->itemText(0) == QStringLiteral("All backends"));
    CHECK(page->backendFilterCombo()->toolTip() ==
          QStringLiteral("Show one speech backend's models"));
    CHECK(itemTexts(*page->statusFilterCombo()) == QStringList{QStringLiteral("All"),
                                                               QStringLiteral("Downloaded"),
                                                               QStringLiteral("Not downloaded")});
    CHECK(page->statusFilterCombo()->toolTip() == QStringLiteral("Filter by download status"));
    CHECK(itemTexts(*page->sortCombo()) ==
          QStringList{QStringLiteral("Recommended"), QStringLiteral("Downloaded first"),
                      QStringLiteral("Smallest first"), QStringLiteral("Name A-Z"),
                      QStringLiteral("By backend")});
    CHECK(page->sortCombo()->toolTip() == QStringLiteral("Sort model list"));
    CHECK(page->stopBatchButton()->text() == QStringLiteral("Stop after current"));
    CHECK(page->stopBatchButton()->toolTip() ==
          QStringLiteral("Finish the model now downloading, then stop the queue"));
    CHECK(page->stopBatchButton()->isHidden());
}
