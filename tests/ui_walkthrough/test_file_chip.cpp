// The ready card's file chip (ui_qt/widgets/walkthrough_ready.py:106-159).
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include "ui_walkthrough/walkthrough_ready.h"

namespace fvtest {

TEST_CASE("the file chip names the document and drops it as a file",
          "[walkthrough_ready][widget]") {
    fv::FileChip chip;
    CHECK(chip.objectName() == QStringLiteral("walkthroughFileChip"));
    CHECK(chip.toolTip() == QStringLiteral(u"Click to open · drag into any app to attach it"));
    CHECK(chip.cursor().shape() == Qt::OpenHandCursor);
    auto* row = qobject_cast<QHBoxLayout*>(chip.layout());
    REQUIRE(row != nullptr);
    CHECK(row->contentsMargins() == QMargins(9, 5, 11, 5));
    CHECK(row->spacing() == 6);
    auto* icon = chip.findChild<QLabel*>(QStringLiteral("walkthroughFileIcon"));
    REQUIRE(icon != nullptr);
    CHECK(icon->pixmap().deviceIndependentSize().toSize() == QSize(14, 14));
    CHECK(chip.nameLabel()->objectName() == QStringLiteral("walkthroughFileName"));
    CHECK(chip.nameLabel()->text().isEmpty());

    // Nothing to drop until there is a document.
    std::unique_ptr<QMimeData> empty(chip.mimeData());
    CHECK_FALSE(empty->hasUrls());

    const QString path = QStringLiteral("C:/captures/2026-10-21 14-32-05/walkthrough.pdf");
    chip.setPath(path);
    CHECK(chip.nameLabel()->text() == QStringLiteral("walkthrough.pdf"));
    std::unique_ptr<QMimeData> data(chip.mimeData());
    REQUIRE(data->urls().size() == 1);
    CHECK(data->urls().front() == QUrl::fromLocalFile(path));
    CHECK(data->hasFormat(QStringLiteral("text/uri-list")));

    const fv::FileChip named(path, nullptr);
    CHECK(named.nameLabel()->text() == QStringLiteral("walkthrough.pdf"));
}

TEST_CASE("a click on the file chip opens it", "[walkthrough_ready][widget]") {
    fv::FileChip chip;
    chip.setPath(QStringLiteral("C:/captures/walkthrough.pdf"));
    chip.resize(160, 28);
    chip.show();
    QSignalSpy clicked(&chip, &fv::FileChip::clicked);
    QTest::mouseClick(&chip, Qt::LeftButton, Qt::NoModifier, QPoint(20, 10));
    CHECK(clicked.size() == 1);
    // Only the left button clicks.
    QTest::mouseClick(&chip, Qt::RightButton, Qt::NoModifier, QPoint(20, 10));
    CHECK(clicked.size() == 1);
}

} // namespace fvtest
