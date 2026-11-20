// WrappedLabel (with the golden vectors of its break points), DecodeLabel, DownloadsLabel and
// EmptyState.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "app_theme.h"
#include "golden/golden.h"
#include "ui_theme/palette.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/wrapped_label.h"

namespace {

const QString kZwsp = QString(QChar(0x200B));

QString shown(const QLabel& label) {
    return label.QLabel::text();
}

} // namespace

TEST_CASE("withBreakPoints replays the golden vectors", "[golden][wrapped_label]") {
    fv::golden::for_each(
        "ui_qt.widgets.wrapped_label", "_with_break_points", [](const fv::golden::Case& c) {
            const QString input = QString::fromStdString(c.arg<std::string>(0));
            CHECK(fv::withBreakPoints(input).toStdString() == c.result<std::string>());
        });
}

TEST_CASE("break points go after backslashes and every 8 characters of a long run", "[widget]") {
    CHECK(fv::withBreakPoints(QStringLiteral("C:\\a\\b")) ==
          QStringLiteral("C:\\") + kZwsp + QStringLiteral("a\\") + kZwsp + QStringLiteral("b"));
    CHECK(fv::withBreakPoints(QString(15, QLatin1Char('x'))) == QString(15, QLatin1Char('x')));
    CHECK(fv::withBreakPoints(QString(16, QLatin1Char('x'))) ==
          QString(8, QLatin1Char('x')) + kZwsp + QString(8, QLatin1Char('x')));
    CHECK(fv::withBreakPoints(QString(17, QLatin1Char('x'))) ==
          QString(8, QLatin1Char('x')) + kZwsp + QString(8, QLatin1Char('x')) + kZwsp +
              QStringLiteral("x"));
    // An astral character is one code point: 16 of them are a run, 15 are not.
    const QString smiley = QString::fromUcs4(U"\U0001F600");
    QString fifteen;
    for (int i = 0; i < 15; ++i) {
        fifteen += smiley;
    }
    CHECK(fv::withBreakPoints(fifteen) == fifteen);
    CHECK(fv::withBreakPoints(fifteen + smiley).contains(kZwsp));
}

TEST_CASE("WrappedLabel wraps words and reports the text as set", "[widget]") {
    fv::WrappedLabel plain(QStringLiteral("some words"));
    CHECK(plain.wordWrap());
    CHECK(plain.text() == QStringLiteral("some words"));
    CHECK(shown(plain) == QStringLiteral("some words"));

    const QString path = QStringLiteral(R"(C:\Users\someone\Documents\Recordings\meeting.wav)");
    fv::WrappedLabel breaking(path, nullptr, true);
    CHECK(breaking.text() == path);
    CHECK(shown(breaking) == fv::withBreakPoints(path));
    CHECK(shown(breaking).contains(kZwsp));
    breaking.setText(QStringLiteral("short"));
    CHECK(breaking.text() == QStringLiteral("short"));
}

TEST_CASE("WrappedLabel's selection and copy lose the break points", "[widget]") {
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        SKIP("the clipboard check runs only on the offscreen platform, whose clipboard is the "
             "process's own");
    }
    const QString path = QStringLiteral(R"(C:\Users\someone\Documents\Recordings\meeting.wav)");
    fv::WrappedLabel label(path, nullptr, true);
    label.setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    label.resize(200, 80);
    label.show();
    label.setFocus();
    QApplication::processEvents();

    QTest::keyClick(&label, Qt::Key_A, Qt::ControlModifier);
    CHECK(label.hasSelectedText());
    CHECK(label.selectedText() == path);

    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(shown(label));
    QTest::keyClick(&label, Qt::Key_C, Qt::ControlModifier);
    CHECK(clipboard->text() == path);
}

TEST_CASE("WrappedLabel asks for the height its text needs at its own width", "[widget]") {
    fv::WrappedLabel label(QStringLiteral("a few words that need to wrap onto several lines when "
                                          "the label is narrow enough to force it"));
    CHECK_FALSE(label.hasHeightForWidth());
    label.resize(120, 20);
    label.show();
    QApplication::processEvents();
    CHECK(label.sizeHint().height() == label.heightForWidth(120));
    CHECK(label.minimumSizeHint().height() == label.heightForWidth(120));
    CHECK(label.sizeHint().height() > label.fontMetrics().height());

    label.resize(480, 20);
    QApplication::processEvents();
    CHECK(label.sizeHint().height() == label.heightForWidth(480));
    CHECK(label.sizeHint().height() < label.heightForWidth(120));
}

TEST_CASE("EmptyState shows a title, an optional hint and a centred action", "[widget]") {
    fv::EmptyState empty(QStringLiteral("No snippets yet"));
    CHECK(empty.objectName() == QStringLiteral("emptyState"));
    CHECK(empty.titleLabel()->objectName() == QStringLiteral("emptyStateTitle"));
    CHECK(empty.hintLabel()->objectName() == QStringLiteral("emptyStateHint"));
    CHECK(empty.text() == QStringLiteral("No snippets yet"));
    empty.show();
    CHECK_FALSE(empty.hintLabel()->isVisible());

    empty.setText(QStringLiteral("No snippets yet"), QStringLiteral("Add one."));
    CHECK(empty.hintLabel()->isVisible());
    CHECK(empty.text() == QStringLiteral("No snippets yet Add one."));
    CHECK(empty.titleLabel()->alignment() == Qt::AlignCenter);

    empty.setText(QString(), QString());
    CHECK(empty.text().isEmpty());

    auto* button = new QPushButton(QStringLiteral("Add"));
    empty.addAction(button);
    auto* layout = qobject_cast<QVBoxLayout*>(empty.layout());
    REQUIRE(layout != nullptr);
    // Title, hint, an 8 px spacer, then the button.
    CHECK(layout->count() == 4);
    CHECK(layout->itemAt(3)->widget() == button);
    CHECK(layout->itemAt(3)->alignment() == Qt::AlignHCenter);
    CHECK(layout->contentsMargins() == QMargins(20, 18, 20, 18));
    CHECK(layout->spacing() == 4);

    auto* second = new QPushButton(QStringLiteral("Import"));
    empty.addAction(second, 0);
    CHECK(layout->count() == 5);
}

TEST_CASE("DecodeLabel shows finished text at once without animation", "[widget]") {
    fv::DecodeLabel label;
    label.setSegments({{QStringLiteral("Language: "), false}, {QStringLiteral("English"), true}},
                      false);
    CHECK_FALSE(label.isRevealing());
    CHECK(label.text() == shown(label));
    CHECK(label.text().startsWith(QStringLiteral("Language: <span style='color:")));
    CHECK(label.text().contains(QStringLiteral("font-weight:600'>English</span>")));
    CHECK(label.textFormat() == Qt::RichText);
}

TEST_CASE("DecodeLabel escapes text and pins runs of spaces", "[widget]") {
    fv::DecodeLabel label;
    label.setSegments(
        {{QStringLiteral("a <b> & \"c\" 'd'"), false}, {QStringLiteral("x  y   z"), false}}, false);
    CHECK(label.text() ==
          QStringLiteral(
              "a &lt;b&gt; &amp; &quot;c&quot; &#x27;d&#x27;x&nbsp;&nbsp;y&nbsp;&nbsp; z"));
}

TEST_CASE("DecodeLabel waits to be shown before it reveals", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(
        {{QStringLiteral("Transcribed "), false}, {QStringLiteral("42 words"), true}});
    CHECK_FALSE(label.isRevealing());
    CHECK(shown(label).isEmpty());
    // The finished text is available at once, however far the reveal has run.
    CHECK(label.text().contains(QStringLiteral("Transcribed ")));

    label.show();
    CHECK(label.isRevealing());
    QTest::qWait(900);
    CHECK_FALSE(label.isRevealing());
    CHECK(shown(label) == label.text());
}

TEST_CASE("DecodeLabel hiding mid-reveal settles on the finished text", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.show();
    label.setSegments({{QStringLiteral("Transcribed 42 words"), false}});
    REQUIRE(label.isRevealing());
    label.hide();
    CHECK_FALSE(label.isRevealing());
    CHECK(shown(label) == label.text());
}

TEST_CASE("DecodeLabel clear forgets the text and stops", "[widget]") {
    fv::DecodeLabel label;
    label.show();
    label.setSegments({{QStringLiteral("abc"), true}});
    label.clear();
    CHECK_FALSE(label.isRevealing());
    CHECK(label.text().isEmpty());
    CHECK(shown(label).isEmpty());
}

TEST_CASE("the reveal locks round(length * progress) characters, half to even", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments({{QStringLiteral("abcde"), false}}, false);
    // 5 * 0.5 = 2.5 rounds to 2 (half to even), 5 * 0.7 = 3.5 rounds to 4.
    CHECK(label.frameAt(0.5).startsWith(QStringLiteral("ab<span style='color:")));
    CHECK(label.frameAt(0.7).startsWith(QStringLiteral("abcd<span style='color:")));
    CHECK(label.frameAt(0.0).startsWith(QStringLiteral("<span style='color:")));
    CHECK(label.frameAt(1.0) == label.text());
    CHECK(label.frameAt(2.0) == label.text());
}

TEST_CASE("the reveal counts code points and keeps spaces unscrambled", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    const QString smiley = QString::fromUcs4(U"\U0001F600");
    label.setSegments({{smiley + smiley + smiley + smiley, false}}, false);
    // 4 code points * 0.5 = 2 locked: two whole emoji, then scrambled glyphs.
    CHECK(label.frameAt(0.5).startsWith(smiley + smiley + QStringLiteral("<span")));

    label.setSegments({{QStringLiteral("a b"), false}}, false);
    const QString frame = label.frameAt(0.34);
    // One character locked, the space stays a space, the next one scrambles.
    CHECK(frame.startsWith(QStringLiteral("a ")));
}

TEST_CASE("DownloadsLabel keeps its link when the explanation is elided", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    const QString text =
        QStringLiteral("The model is missing and cannot be loaded. Get it in Downloads.");
    fv::DownloadsLabel label(text);
    label.resize(200, 24);
    label.show();
    QApplication::processEvents();
    CHECK(label.text() == text);
    CHECK(label.textFormat() == Qt::RichText);
    CHECK(shown(label).contains(QStringLiteral("<a href=\"downloads\"")));
    CHECK(shown(label).endsWith(QStringLiteral(">Downloads</a>.")));
    CHECK(shown(label).contains(QStringLiteral(u"…")));
    CHECK(shown(label).contains(
        QStringLiteral("color: %1;").arg(fv::ui::currentPalette().qCss(u"accent"))));
    CHECK(label.textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
    CHECK(label.textInteractionFlags().testFlag(Qt::LinksAccessibleByKeyboard));
    CHECK_FALSE(label.openExternalLinks());
}

TEST_CASE("DownloadsLabel is a plain eliding label without the link text", "[widget]") {
    fv::DownloadsLabel label(QStringLiteral("The model is missing."));
    label.resize(300, 24);
    label.show();
    QApplication::processEvents();
    CHECK(label.textFormat() == Qt::PlainText);
    CHECK(label.textInteractionFlags() == Qt::NoTextInteraction);
    CHECK(shown(label) == QStringLiteral("The model is missing."));

    label.setText(QStringLiteral("Not there. Get it in Downloads."));
    CHECK(label.textFormat() == Qt::RichText);
    label.setText(QStringLiteral("Plain again"));
    CHECK(label.textFormat() == Qt::PlainText);
}

TEST_CASE("DownloadsLabel announces a click on its link and nothing else", "[widget]") {
    fv::DownloadsLabel label(QStringLiteral("Not there. Get it in Downloads."));
    QSignalSpy requested(&label, &fv::DownloadsLabel::downloadsRequested);
    Q_EMIT label.linkActivated(QStringLiteral("elsewhere"));
    CHECK(requested.count() == 0);
    Q_EMIT label.linkActivated(QStringLiteral("downloads"));
    CHECK(requested.count() == 1);
}

TEST_CASE("DownloadsLabel's minimum width keeps the link readable", "[widget]") {
    fv::DownloadsLabel withLink(QString(80, QLatin1Char('x')) + QStringLiteral(" in Downloads."));
    fv::DownloadsLabel without(QString(80, QLatin1Char('x')) + QStringLiteral(" elsewhere."));
    CHECK(withLink.minimumSizeHint().width() >=
          withLink.fontMetrics().horizontalAdvance(QStringLiteral(u"… Downloads.")));
    CHECK(withLink.minimumSizeHint().width() > 0);
    CHECK(withLink.minimumSizeHint().width() >= without.minimumSizeHint().width());
}
