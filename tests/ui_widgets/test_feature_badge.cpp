// FeatureBadge (the "New" and "?" chip on a button) and the list context menu helper.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QTest>
#include <QWidget>

#include "app_theme.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/feature_badge.h"

namespace {

// A button of the given width in a window, with a badge on it.
struct OnButton {
    explicit OnButton(int width, const QString& label = QStringLiteral("Walkthrough"))
        : button(label), badge(&button, QStringLiteral("Explains the button.")) {
        button.resize(width, 48);
        button.show();
        QApplication::processEvents();
    }
    fv::PrimaryButton button;
    fv::FeatureBadge badge;
};

} // namespace

TEST_CASE("a badge on a button says New, sits at its right edge and is a child of the button",
          "[widget]") {
    OnButton on(260);
    CHECK(on.badge.objectName() == QStringLiteral("featureBadge"));
    CHECK(on.badge.parentWidget() == &on.button);
    CHECK(on.badge.text() == QStringLiteral("New"));
    CHECK_FALSE(on.badge.isCompact());
    CHECK(on.badge.height() == 18);
    // Padding 7, star 10, gap 4, then the word in 9 px bold with letter spacing.
    CHECK(on.badge.width() > 7 * 2 + 10 + 4);
    CHECK(on.badge.x() == on.button.width() - on.badge.width() - 12);
    CHECK(on.badge.y() == (on.button.height() - on.badge.height()) / 2);
    CHECK(on.badge.cursor().shape() == Qt::WhatsThisCursor);
}

TEST_CASE("a narrow button shrinks the badge to its star", "[widget]") {
    OnButton on(150);
    CHECK(on.badge.isCompact());
    CHECK(on.badge.size() == QSize(18, 18));
    CHECK(on.badge.x() == on.button.width() - 18 - 12);

    // Widening the button gives the word back.
    on.button.resize(300, 48);
    QApplication::processEvents();
    CHECK_FALSE(on.badge.isCompact());
    CHECK(on.badge.width() > 18);
}

TEST_CASE("the question mark is a plain 18 px circle", "[widget]") {
    OnButton on(260);
    on.badge.setNew(false);
    CHECK(on.badge.text() == QStringLiteral("?"));
    CHECK(on.badge.size() == QSize(18, 18));
    CHECK(on.badge.accessibleName() == QStringLiteral("what this button does"));
    on.badge.setNew(true);
    CHECK(on.badge.text() == QStringLiteral("New"));
    CHECK(on.badge.accessibleName() == QStringLiteral("New: what this button does"));
    CHECK(on.badge.width() > 18);
}

TEST_CASE("the explanation is a wrapping rich-text tooltip and the accessible description",
          "[widget]") {
    OnButton on(260);
    CHECK(on.badge.toolTip() ==
          QStringLiteral("<p style='white-space:normal'>Explains the button.</p>"));
    CHECK(on.badge.accessibleDescription() == QStringLiteral("Explains the button."));
    on.badge.setExplanation(QStringLiteral("Something else."));
    CHECK(on.badge.accessibleDescription() == QStringLiteral("Something else."));
}

TEST_CASE("a badge follows a new label on its button without a resize", "[widget]") {
    OnButton on(220, QStringLiteral("Go"));
    const bool before = on.badge.isCompact();
    on.button.setText(QStringLiteral("Finish a very long walkthrough label"));
    on.button.resize(on.button.width(), on.button.height());
    QApplication::processEvents();
    CHECK(before != on.badge.isCompact());
}

TEST_CASE("a standalone badge is sized by its word and left to its layout", "[widget]") {
    QWidget host;
    auto* row = new QHBoxLayout(&host);
    auto* badge = new fv::FeatureBadge(nullptr, QStringLiteral("Explains."), &host);
    row->addWidget(badge);
    CHECK(badge->parentWidget() == &host);
    CHECK(badge->size() == badge->sizeHint());
    CHECK(badge->width() > 18);
    badge->setNew(false);
    CHECK(badge->size() == QSize(18, 18));
}

TEST_CASE("an inactive button greys the badge", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    OnButton on(260);
    const QImage live = on.badge.grab().toImage();
    static_cast<fv::Button&>(on.button).setActive(false);
    QApplication::processEvents();
    CHECK(on.badge.grab().toImage() != live);
}

TEST_CASE("the list context menu is owned by its parent and carries the roomy style", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    QWidget parent;
    QMenu* menu = fv::contextMenu(&parent);
    CHECK(menu->parent() == &parent);
    CHECK(menu->styleSheet().contains(QStringLiteral("border-radius: 10px")));
    CHECK(menu->styleSheet().contains(QStringLiteral("padding: 8px 28px 8px 14px")));
    // The tokens are resolved against the palette: none is left behind.
    CHECK_FALSE(menu->styleSheet().contains(QLatin1Char('@')));
    CHECK(menu->styleSheet().contains(fv::ui::currentPalette().qCss(u"accent")));
}
