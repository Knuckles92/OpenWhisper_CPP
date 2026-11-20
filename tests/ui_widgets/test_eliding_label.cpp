// ElidingLabel sizing when a stylesheet pads or borders the text. A padding or border rule lands
// in the label's contents margins. The label must reserve that chrome in its size hint and elide
// against the area the text is actually painted into, otherwise a layout that grants exactly the
// hint clips the last characters without ever showing an ellipsis.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QWidget>

#include <memory>

#include "ui_widgets/eliding_label.h"

namespace {

constexpr int kPadding = 8;
constexpr int kBorder = 1;
constexpr int kChrome = 2 * (kPadding + kBorder);

const QString kStyle = QStringLiteral("QLabel { padding: 3px %1px; border: %2px solid red; }")
                           .arg(kPadding)
                           .arg(kBorder);

std::unique_ptr<fv::ElidingLabel> padded(const QString& text) {
    auto label = std::make_unique<fv::ElidingLabel>(text);
    label->setStyleSheet(kStyle);
    return label;
}

// What the label shows, as opposed to text(), which is the full text.
QString shown(const QLabel& label) {
    return label.QLabel::text();
}

} // namespace

// from tests/test_eliding_label.py::test_size_hint_reserves_the_stylesheet_chrome
TEST_CASE("size hint reserves the stylesheet chrome", "[test_eliding_label][widget]") {
    const QString text = QStringLiteral("25 European languages");
    const auto label = padded(text);
    const int advance = label->fontMetrics().horizontalAdvance(text);
    CHECK(label->sizeHint().width() >= advance + kChrome);
}

// from tests/test_eliding_label.py::test_full_text_is_shown_at_its_own_size_hint
TEST_CASE("full text is shown at its own size hint", "[test_eliding_label][widget]") {
    const int fontSize = GENERATE(10, 11, 12, 13, 14, 15);
    const QString text = QStringLiteral("25 European languages");
    const auto label = padded(text);
    label->setStyleSheet(
        label->styleSheet() +
        QStringLiteral("QLabel { font-size: %1px; font-weight: 600; }").arg(fontSize));
    label->ensurePolished();
    label->resize(label->sizeHint());
    label->show();
    QApplication::processEvents();
    CHECK(shown(*label) == text);
}

// from tests/test_eliding_label.py::test_elision_fits_inside_the_padded_area
TEST_CASE("elision fits inside the padded area", "[test_eliding_label][widget]") {
    const QString text =
        QStringLiteral("A secondary line that is far too long for the room it is given");
    const auto label = padded(text);
    label->resize(120, label->sizeHint().height());
    label->show();
    QApplication::processEvents();
    const QString visible = shown(*label);
    CHECK(visible != text);
    CHECK(visible.endsWith(QStringLiteral(u"…")));
    CHECK(label->fontMetrics().horizontalAdvance(visible) <= label->contentsRect().width());
}

// from tests/test_eliding_label.py::test_minimum_width_still_ignores_text_length
TEST_CASE("minimum width still ignores text length", "[test_eliding_label][widget]") {
    const auto shortLabel = padded(QStringLiteral("Multilingual"));
    const auto longLabel = padded(QString(200, QLatin1Char('x')));
    CHECK(longLabel->minimumSizeHint().width() == shortLabel->minimumSizeHint().width());
    CHECK(longLabel->minimumSizeHint().width() <
          longLabel->fontMetrics().horizontalAdvance(longLabel->text()));
}

// from tests/test_eliding_label.py::test_new_text_grows_a_label_laid_out_while_empty
TEST_CASE("new text grows a label laid out while empty", "[test_eliding_label][widget]") {
    // Laid out empty, the label is narrower than an ellipsis, so the new text elides to "" just
    // as the old one did; the layout must still hear that the hint changed.
    QWidget host;
    auto* row = new QHBoxLayout(&host);
    auto* label = new fv::ElidingLabel(QString());
    // A sliver of width, as a hidden panel laid out before its text is set leaves it; a width
    // of zero takes a different path in the elide.
    label->setMinimumWidth(6);
    row->addWidget(label);
    auto* filler = new QWidget;
    filler->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->addWidget(filler, 1);
    host.resize(600, 40);
    host.show();
    QApplication::processEvents();
    REQUIRE(label->width() < label->fontMetrics().horizontalAdvance(QStringLiteral(u"…")));

    label->setText(QStringLiteral("Microphone (USB Audio)"));
    QApplication::processEvents();
    CHECK(label->width() >= label->sizeHint().width());
    CHECK(shown(*label) == QStringLiteral("Microphone (USB Audio)"));
    host.close();
}

// from tests/test_eliding_label.py::test_size_hint_is_the_same_whether_or_not_currently_elided
TEST_CASE("size hint is the same whether or not currently elided", "[test_eliding_label][widget]") {
    const QString text =
        QStringLiteral("A secondary line that is far too long for the room it is given");
    const auto elided = padded(text);
    elided->resize(120, elided->sizeHint().height());
    elided->show();
    QApplication::processEvents();
    REQUIRE(shown(*elided) != text);

    const auto fresh = padded(text);
    CHECK(elided->sizeHint().width() == fresh->sizeHint().width());

    // A plain QLabel with the same text and style is the reference for how much room the text
    // plus its chrome needs.
    QLabel plain(text);
    plain.setStyleSheet(fresh->styleSheet());
    plain.ensurePolished();
    // ElidingLabel rounds fractional advances up to prevent clipping; QLabel can round down by
    // one pixel.
    const int difference = elided->sizeHint().width() - plain.sizeHint().width();
    CHECK(difference >= 0);
    CHECK(difference <= 1);
}

TEST_CASE("text returns the full text and the tooltip carries it", "[widget]") {
    fv::ElidingLabel label(QStringLiteral("A long value that will not fit"));
    label.resize(60, 20);
    label.show();
    QApplication::processEvents();
    CHECK(label.text() == QStringLiteral("A long value that will not fit"));
    CHECK(label.toolTip() == label.text());
    CHECK(shown(label) != label.text());
    label.setText(QString());
    CHECK(label.text().isEmpty());
    CHECK(label.toolTip().isEmpty());
}

TEST_CASE("the label elides on the side its mode names", "[widget]") {
    const QString text = QStringLiteral("C:/Users/someone/Documents/Recordings/meeting.wav");
    fv::ElidingLabel left(text, Qt::ElideLeft);
    left.resize(100, 20);
    left.show();
    QApplication::processEvents();
    CHECK(shown(left).startsWith(QStringLiteral(u"…")));
    fv::ElidingLabel middle(text, Qt::ElideMiddle);
    middle.resize(100, 20);
    middle.show();
    QApplication::processEvents();
    CHECK(shown(middle).contains(QStringLiteral(u"…")));
    CHECK_FALSE(shown(middle).startsWith(QStringLiteral(u"…")));
    CHECK_FALSE(shown(middle).endsWith(QStringLiteral(u"…")));
}
