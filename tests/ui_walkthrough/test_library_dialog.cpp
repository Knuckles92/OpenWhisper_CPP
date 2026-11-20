#include <QComboBox>
#include <QListWidget>
#include <QToolButton>

#include "review_support.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "walkthrough/library.h"

namespace fvtest {

TEST_CASE("Saved Walkthroughs asks once before a refresh and drops discarded edits on close",
          "[walkthrough_library][widget]") {
    Capture capture;
    fv::MemorySettingsStore settings;
    Host host;
    fv::WalkthroughLibraryDialog dialog(settings, capture.root, host.callbacks());
    auto* review = dialog.review();
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("wlList"));
    auto* refresh = dialog.findChild<QToolButton*>(QStringLiteral("wlRefresh"));
    REQUIRE(list);
    REQUIRE(refresh);
    REQUIRE(list->currentRow() == 0);
    REQUIRE(review->folder() == capture.folder);
    int confirmations = 0;
    review->setQuestion([&](auto&&...) {
        ++confirmations;
        return QMessageBox::Yes;
    });
    review->rows().front()->editor()->setPlainText(QStringLiteral("EDITED"));
    refresh->click();
    CHECK(confirmations == 1);
    CHECK(review->rows().front()->editor()->toPlainText() == QStringLiteral("first"));
    dialog.show();
    review->rows().front()->editor()->setPlainText(QStringLiteral("another edit"));
    dialog.reject();
    CHECK(confirmations == 2);
    CHECK_FALSE(dialog.isVisible());
    CHECK_FALSE(review->isDirty());
    CHECK(review->rows().front()->editor()->toPlainText() == QStringLiteral("first"));
}

TEST_CASE("keeping walkthroughs forever can be refused without losing edits",
          "[walkthrough_library][widget]") {
    Capture capture;
    fv::walkthrough::set_retention(capture.root, 30);
    fv::MemorySettingsStore settings;
    Host host;
    fv::WalkthroughLibraryDialog dialog(settings, capture.root, host.callbacks());
    dialog.review()->setQuestion([](auto&&...) { return QMessageBox::No; });
    auto* keep = dialog.findChild<QComboBox*>(QStringLiteral("wlKeep"));
    REQUIRE(keep);
    REQUIRE(keep->currentData().toInt() == 30);
    dialog.review()->rows().front()->editor()->setPlainText(QStringLiteral("EDITED"));
    keep->setCurrentIndex(keep->findData(0));
    CHECK(keep->currentData().toInt() == 30);
    CHECK(fv::walkthrough::retention_days(capture.root) == 30);
    CHECK(dialog.review()->rows().front()->editor()->toPlainText() == QStringLiteral("EDITED"));
    CHECK_FALSE(dialog.review()->isBusy());
}

} // namespace fvtest
