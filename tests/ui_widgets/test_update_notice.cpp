// The title-bar update notice button. The check flow behind it (the other seven functions of
// tests/test_update_notice.py) is M3's.
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QTest>
#include <QWidget>

#include "base/identity.h"
#include "ui_widgets/update_notice.h"

namespace {

QString product() {
    return QString::fromUtf8(fv::identity().product.data(),
                             static_cast<qsizetype>(fv::identity().product.size()));
}

} // namespace

// from
// tests/test_update_notice.py::test_compact_window_drops_the_version_but_keeps_it_in_the_tooltip
TEST_CASE("compact window drops the version but keeps it in the tooltip",
          "[test_update_notice][widget]") {
    // A parent that is never shown, so the button's own show/hide state is what isHidden()
    // reports, without opening a top-level window.
    QWidget holder;
    fv::UpdateNoticeButton button(&holder);
    button.setCompact(true);
    button.showVersion(QStringLiteral("2.2.0"));
    CHECK(button.text() == QStringLiteral("Update"));
    CHECK(button.toolTip().contains(QStringLiteral("2.2.0")));
    button.setCompact(false);
    CHECK(button.text() == QStringLiteral("Update to 2.2.0"));
}

TEST_CASE("the notice is hidden until a version is offered and an empty one hides it again",
          "[widget]") {
    QWidget holder;
    fv::UpdateNoticeButton button(&holder);
    CHECK(button.isHidden());
    CHECK(button.objectName() == QStringLiteral("updateNoticeButton"));
    CHECK(button.text().isEmpty());

    button.showVersion(QStringLiteral("2.9.0"));
    CHECK_FALSE(button.isHidden());
    CHECK(button.text() == QStringLiteral("Update to 2.9.0"));

    button.showVersion(QString());
    CHECK(button.isHidden());
}

TEST_CASE("the tooltip and accessible name name the product and the version", "[widget]") {
    QWidget holder;
    fv::UpdateNoticeButton button(&holder);
    button.showVersion(QStringLiteral("2.9.0"));
    CHECK(button.toolTip() ==
          product() + QStringLiteral(" 2.9.0 is available. Click to see what's new."));
    CHECK(button.accessibleName() ==
          QStringLiteral("Update to ") + product() + QStringLiteral(" 2.9.0"));
}

TEST_CASE("a compact notice set before any version stays unlabelled until one arrives",
          "[widget]") {
    QWidget holder;
    fv::UpdateNoticeButton button(&holder);
    button.setCompact(true);
    CHECK(button.text().isEmpty());
    button.showVersion(QStringLiteral("2.9.0"));
    CHECK(button.text() == QStringLiteral("Update"));
}

TEST_CASE("clicking the notice emits clicked", "[widget]") {
    QWidget holder;
    holder.show();
    fv::UpdateNoticeButton button(&holder);
    button.showVersion(QStringLiteral("2.9.0"));
    QSignalSpy clicked(&button, &QPushButton::clicked);
    QTest::mouseClick(&button, Qt::LeftButton);
    CHECK(clicked.count() == 1);
}
