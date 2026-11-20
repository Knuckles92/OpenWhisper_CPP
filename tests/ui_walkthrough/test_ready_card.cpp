// The ready card (ui_qt/widgets/walkthrough_ready.py): its words against OpenWhisper's golden
// vectors, its timers, menu, placement and dismissal rules, and the two OpenWhisper tests that
// drive it. The card is shown on the offscreen platform only.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSignalSpy>
#include <QTimer>
#include <QToolButton>

#include "appkit/qt_bridge.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "ready_support.h"
#include "ui_walkthrough/walkthrough_copy_tips.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_widgets/file_reveal.h"

namespace fvtest {
namespace {

using fv::WalkthroughReadyCard;
using fv::walkthrough::WalkthroughResult;

const QString kReason = QStringLiteral(
    u"Claude Code is not installed on this computer. Choose another agent in Settings → "
    u"Dictation → Walkthroughs.");

// The card hides itself when the test ends, as OpenWhisper's tests hide and delete theirs.
struct Card {
    Card() : card(std::make_unique<WalkthroughReadyCard>()) {}
    ~Card() { card->hide(); }
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
    WalkthroughReadyCard* operator->() const { return card.get(); }
    WalkthroughReadyCard& operator*() const { return *card; }

    std::unique_ptr<WalkthroughReadyCard> card;
};

QTimer* hideTimer(WalkthroughReadyCard& card) {
    const auto timers = card.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
    REQUIRE(timers.size() == 1);
    return timers.front();
}

// The corner the card belongs in: the pointer's screen's available area, less 18 px.
QPoint corner(const WalkthroughReadyCard& card) {
    QScreen* screen = QApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QApplication::primaryScreen();
    }
    const QRect area = screen->availableGeometry();
    return {area.right() - card.sizeHint().width() - 18 + 1,
            area.bottom() - card.sizeHint().height() - 18 + 1};
}

// Restores the file manager's hooks when the test ends.
struct RevealHooks {
    explicit RevealHooks(fv::FileRevealHooks hooks)
        : previous(fv::setFileRevealHooks(std::move(hooks))) {}
    ~RevealHooks() { fv::setFileRevealHooks(std::move(previous)); }
    RevealHooks(const RevealHooks&) = delete;
    RevealHooks& operator=(const RevealHooks&) = delete;

    fv::FileRevealHooks previous;
};

} // namespace

// from
// tests/test_walkthrough_write_up_notice.py::test_the_ready_card_shows_the_whole_failure_reason
TEST_CASE("the ready card shows the whole failure reason",
          "[test_walkthrough_write_up_notice][widget]") {
    ReadyFixture files;
    Card card;
    WalkthroughResult result = files.result();
    result.write_up_error = kReason.toStdString();
    card->present(result, true, true, "pdf");
    CHECK(card->reasonLabel()->isVisibleTo(&*card));
    CHECK(card->reasonLabel()->text() == QStringLiteral("Write-up failed: ") + kReason);
    CHECK(card->reasonLabel()->wordWrap());

    card->present(files.result(), true, true, "pdf");
    CHECK_FALSE(card->reasonLabel()->isVisibleTo(&*card));
    CHECK(card->reasonLabel()->text().isEmpty());
}

// from tests/test_walkthrough_library_ui.py::test_ready_card_exposes_review_and_every_copy_format
TEST_CASE("ready card exposes review and every copy format",
          "[test_walkthrough_library_ui][widget]") {
    ReadyFixture files;
    Card card;
    QSignalSpy review(&*card, &WalkthroughReadyCard::reviewRequested);
    QSignalSpy copied(&*card, &WalkthroughReadyCard::copyRequested);
    const WalkthroughResult result = files.result(true);
    card->present(result, true, true, "pdf");
    CHECK(card->copyWordAction()->isEnabled());
    card->copyPdfAction()->trigger();
    card->copyWordAction()->trigger();
    card->copyMarkdownAction()->trigger();
    card->copyAgentAction()->trigger();
    const QString folder = QString::fromStdU16String(files.folder().u16string());
    REQUIRE(copied.size() == 4);
    const std::vector<std::pair<QString, QString>> expected{
        {folder, QStringLiteral("pdf")},
        {folder, QStringLiteral("word")},
        {folder, QStringLiteral("agent")},
        {folder, QStringLiteral("agent_task")},
    };
    for (qsizetype index = 0; index < copied.size(); ++index) {
        CHECK(copied.at(index).at(0).toString() == expected[static_cast<std::size_t>(index)].first);
        CHECK(copied.at(index).at(1).toString() ==
              expected[static_cast<std::size_t>(index)].second);
    }
    card->reviewButton()->click();
    REQUIRE(review.size() == 1);
    CHECK(review.at(0).at(0).toString() == folder);
}

TEST_CASE("the ready card's words are OpenWhisper's, word for word",
          "[golden][walkthrough_ready][widget]") {
    ReadyFixture files;
    fv::golden::for_each("ui_qt.widgets.walkthrough_ready", "present",
                         [&](const fv::golden::Case& c) {
                             const auto name = c.arg<std::string>(0);
                             WalkthroughResult result = files.result();
                             result.written_up = name == "Write-up";
                             result.shots = c.arg<int>(1);
                             result.duration_s = c.arg<double>(2);
                             const auto format = c.arg<std::string>(3);
                             const bool autoPaste = c.arg<bool>(4);
                             result.write_up_error = c.arg<std::string>(5);
                             const bool copied = c.arg<bool>(6);
                             INFO(c.label());
                             Card card;
                             card->present(result, copied, autoPaste, format);
                             const fv::Json expected = c.result<fv::Json>();
                             CHECK(card->titleLabel()->text().toStdString() ==
                                   expected.at("title").get<std::string>());
                             CHECK(card->subtitleLabel()->text().toStdString() ==
                                   expected.at("subtitle").get<std::string>());
                             CHECK(card->reasonLabel()->text().toStdString() ==
                                   expected.at("reason").get<std::string>());
                             CHECK(card->reasonLabel()->isVisibleTo(&*card) ==
                                   expected.at("reason_visible").get<bool>());
                         });
}

TEST_CASE("the card says what it copied and how, line by line", "[walkthrough_ready][widget]") {
    ReadyFixture files;
    WalkthroughResult result = files.result();
    result.shots = 1;
    result.duration_s = 11;
    CHECK(WalkthroughReadyCard::titleFor(result, true) == QStringLiteral("Walkthrough copied"));
    CHECK(WalkthroughReadyCard::titleFor(result, false) == QStringLiteral("Walkthrough saved"));
    result.written_up = true;
    CHECK(WalkthroughReadyCard::titleFor(result, true) == QStringLiteral("Write-up copied"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, true, "pdf") ==
          QStringLiteral(u"1 screenshot · 0:11 — paste it into your agent"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, false, true, "pdf") ==
          QStringLiteral(u"1 screenshot · copying failed — drag it from here"));
    result.shots = 0;
    CHECK(WalkthroughReadyCard::subtitleFor(result, false, true, "agent_task") ==
          QStringLiteral(u"0 screenshots · copying failed — drag it from here"));
    // Auto-paste and the format change the note only when the agent is to be pasted into.
    result.shots = 2;
    result.duration_s = 75;
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, true, "agent_task") ==
          QStringLiteral(u"2 screenshots · 1:15 — preparing to paste PDF, then instructions"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, false, "agent_task") ==
          QStringLiteral(
              u"2 screenshots · 1:15 — paste instructions, then attach the PDF to your chat"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, false, "word") ==
          QStringLiteral(u"2 screenshots · 1:15 — paste it into your agent"));
    result.write_up_error = "no agent";
    CHECK(
        WalkthroughReadyCard::subtitleFor(result, true, true, "agent_task") ==
        QStringLiteral(u"2 screenshots · 1:15 — write-up failed; preparing to paste as-spoken PDF, "
                       u"then instructions"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, false, "agent_task") ==
          QStringLiteral(u"2 screenshots · 1:15 — write-up failed; paste instructions and attach "
                         u"the as-spoken PDF"));
    CHECK(WalkthroughReadyCard::subtitleFor(result, true, true, "both") ==
          QStringLiteral(u"2 screenshots · 1:15 — write-up failed, copied as spoken"));
}

TEST_CASE("the card is a focus-less corner tool window", "[walkthrough_ready][widget]") {
    Card card;
    CHECK(card->objectName() == QStringLiteral("walkthroughReady"));
    CHECK(card->windowTitle() == QStringLiteral("Walkthrough"));
    const Qt::WindowFlags flags = card->windowFlags();
    CHECK(flags.testFlag(Qt::Tool));
    CHECK(flags.testFlag(Qt::FramelessWindowHint));
    CHECK(flags.testFlag(Qt::WindowStaysOnTopHint));
    CHECK(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK_FALSE(card->testAttribute(Qt::WA_QuitOnClose));
    CHECK(card->testAttribute(Qt::WA_ShowWithoutActivating));
    CHECK(card->testAttribute(Qt::WA_AlwaysShowToolTips));
    CHECK(card->testAttribute(Qt::WA_TranslucentBackground));

    CHECK(card->thumbLabel()->objectName() == QStringLiteral("walkthroughThumb"));
    CHECK(card->thumbLabel()->size() == QSize(76, 48));
    CHECK(card->reasonLabel()->maximumWidth() == 300);
    CHECK(card->closeButton()->accessibleName() == QStringLiteral("Close"));
    CHECK(card->moreButton()->accessibleName() == QStringLiteral("More"));
    CHECK(card->moreButton()->popupMode() == QToolButton::InstantPopup);
    CHECK(card->reviewButton()->text() == QStringLiteral("Review"));
    CHECK(card->reviewButton()->toolTip() ==
          QStringLiteral("Edit the words, leave screenshots out, or redo the write-up"));
    CHECK(card->reviewButton()->property("walkthroughAction").toBool());
}

TEST_CASE("the menu lists every way to open and copy, with a hint on each copy",
          "[walkthrough_ready][widget]") {
    Card card;
    QStringList labels;
    int separators = 0;
    for (QAction* action : card->menu()->actions()) {
        if (action->isSeparator()) {
            ++separators;
            labels.append(QString());
        } else {
            labels.append(action->text());
        }
    }
    CHECK(separators == 1);
    CHECK(labels == QStringList{QStringLiteral("Open PDF"), QStringLiteral("Open Word document"),
                                QStringLiteral("Show in folder"), QString(),
                                QStringLiteral("Copy PDF"), QStringLiteral("Copy for agent"),
                                QStringLiteral("Copy Word document"),
                                QStringLiteral("Copy Markdown")});
    CHECK(card->menu()->toolTipsVisible());
    CHECK(card->copyPdfAction()->toolTip() == fv::copyTipHtml("pdf"));
    CHECK(card->copyAgentAction()->toolTip() == fv::copyTipHtml("agent_task"));
    CHECK(card->copyWordAction()->toolTip() == fv::copyTipHtml("word"));
    CHECK(card->copyMarkdownAction()->toolTip() == fv::copyTipHtml("agent"));
}

TEST_CASE("the first screenshot is cropped to the thumbnail's shape with rounded corners",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    card->present(files.result(false, true), true, true, "pdf");
    REQUIRE_FALSE(card->thumbLabel()->pixmap().isNull());
    CHECK(card->thumbLabel()->isVisibleTo(&*card));
    const QImage thumb = card->thumbLabel()->pixmap().toImage();
    REQUIRE(thumb.size() == QSize(76, 48));
    // Expanded to the shape, then cropped from the middle: the sides of the picture are gone.
    CHECK(thumb.pixelColor(38, 24) == QColor(0xc0, 0x20, 0x20));
    CHECK(thumb.pixelColor(2, 24).red() < 0x40);
    CHECK(thumb.pixelColor(2, 24).blue() > 0x90);
    // The corners are cut round and left clear.
    CHECK(thumb.pixelColor(0, 0).alpha() == 0);
    CHECK(thumb.pixelColor(75, 47).alpha() == 0);
    CHECK(thumb.pixelColor(38, 0).alpha() == 255);

    // Without a screenshot, or one that cannot be read, there is no thumbnail.
    card->present(files.result(), true, true, "pdf");
    CHECK_FALSE(card->thumbLabel()->isVisibleTo(&*card));
    WalkthroughResult unreadable = files.result();
    unreadable.first_shot = files.folder() / "missing.png";
    card->present(unreadable, true, true, "pdf");
    CHECK_FALSE(card->thumbLabel()->isVisibleTo(&*card));
    CHECK(WalkthroughReadyCard::thumbnail(std::nullopt).isNull());
}

TEST_CASE("the chip names the PDF and the Word items need a .docx", "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    card->present(files.result(), true, true, "pdf");
    CHECK(card->chip()->nameLabel()->text() == QStringLiteral("walkthrough.pdf"));
    CHECK(card->chip()->path().endsWith(QStringLiteral("walkthrough.pdf")));
    CHECK_FALSE(card->openWordAction()->isEnabled());
    CHECK_FALSE(card->copyWordAction()->isEnabled());
    CHECK(card->openAction()->isEnabled());
    CHECK(card->copyPdfAction()->isEnabled());

    card->present(files.result(true), true, true, "pdf");
    CHECK(card->openWordAction()->isEnabled());
    CHECK(card->copyWordAction()->isEnabled());
}

TEST_CASE("the card stays 14 s, then 4 s after the pointer leaves, and the menu holds it",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    CHECK_FALSE(card->hideTimerActive());
    card->present(files.result(), true, true, "pdf");
    CHECK(card->isVisible());
    CHECK(card->hideTimerActive());
    CHECK(card->hideTimerInterval() == 14'000);

    // The pointer on the card holds it; leaving starts the short linger.
    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QCoreApplication::sendEvent(&*card, &enter);
    CHECK_FALSE(card->hideTimerActive());
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&*card, &leave);
    CHECK(card->hideTimerActive());
    CHECK(card->hideTimerInterval() == 4'000);

    // The menu opening stops it; closing starts the linger again.
    Q_EMIT card->menu()->aboutToShow();
    CHECK_FALSE(card->hideTimerActive());
    Q_EMIT card->menu()->aboutToHide();
    CHECK(card->hideTimerActive());
    CHECK(card->hideTimerInterval() == 4'000);

    // A copy starts the long wait again.
    card->copyPdfAction()->trigger();
    CHECK(card->hideTimerActive());
    CHECK(card->hideTimerInterval() == 14'000);

    // A hidden card has nothing to linger over.
    hideTimer(*card)->stop();
    card->hide();
    Q_EMIT card->menu()->aboutToHide();
    CHECK_FALSE(card->hideTimerActive());
}

TEST_CASE("the timer hides the card", "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    card->present(files.result(), true, true, "pdf");
    REQUIRE(card->isVisible());
    QMetaObject::invokeMethod(hideTimer(*card), "timeout");
    CHECK_FALSE(card->isVisible());
}

TEST_CASE("Review, Open PDF, Open Word and Show in folder hide the card, copying does not",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    const WalkthroughResult result = files.result(true);
    std::vector<QUrl> opened;
    std::vector<QString> commands;
    RevealHooks hooks({[&](const QUrl& url) {
                           opened.push_back(url);
                           return true;
                       },
                       [&](const QString& command) {
                           commands.push_back(command);
                           return true;
                       }});
    Card card;
    card->setOpener([&](const QUrl& url) {
        opened.push_back(url);
        return true;
    });
    const auto present = [&] {
        card->present(result, true, true, "pdf");
    };
    const auto pdf = QUrl::fromLocalFile(QString::fromStdU16String(result.pdf.u16string()));
    const auto docx = QUrl::fromLocalFile(QString::fromStdU16String(result.docx().u16string()));

    present();
    card->copyWordAction()->trigger();
    CHECK(card->isVisible());

    card->openAction()->trigger();
    CHECK_FALSE(card->isVisible());
    REQUIRE(opened.size() == 1);
    CHECK(opened.back() == pdf);

    present();
    card->openWordAction()->trigger();
    CHECK_FALSE(card->isVisible());
    REQUIRE(opened.size() == 2);
    CHECK(opened.back() == docx);

    // A click on the chip opens the PDF too.
    present();
    Q_EMIT card->chip()->clicked();
    CHECK_FALSE(card->isVisible());
    REQUIRE(opened.size() == 3);
    CHECK(opened.back() == pdf);

    // Python's _reveal hides the card as well as the other two (OpenWhisper's map lists it as
    // staying).
    present();
    card->revealAction()->trigger();
    CHECK_FALSE(card->isVisible());
#ifdef Q_OS_WIN
    REQUIRE(commands.size() == 1);
    CHECK(commands.back().contains(QStringLiteral("walkthrough.pdf")));
    CHECK(commands.back().startsWith(QStringLiteral("explorer /select,")));
#else
    // Without Explorer's /select the file manager opens the folder holding the PDF
    // (revealInFileManager).
    CHECK(commands.empty());
    REQUIRE(opened.size() == 4);
    CHECK(opened.back() ==
          QUrl::fromLocalFile(
              QFileInfo(fv::qt::toQString(fv::path_to_utf8(result.pdf))).absolutePath()));
#endif

    present();
    card->closeButton()->click();
    CHECK_FALSE(card->isVisible());
    present();
    card->reviewButton()->click();
    CHECK_FALSE(card->isVisible());
}

TEST_CASE("the card sits at the bottom right of the pointer's screen, 18 px in",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    card->present(files.result(), true, true, "pdf");
    CHECK(card->pos() == corner(*card));
    // Placed again after every adjustSize, including a paste status that changes the size.
    const WalkthroughResult result = files.result();
    card->showPasteStatus(result, QStringLiteral("PDF and instructions pasted; review them before "
                                                 "sending, then press Enter in your chat"));
    CHECK(card->pos() == corner(*card));
}

TEST_CASE("a paste status replaces the card's words only for its own PDF",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    Card card;
    WalkthroughResult result = files.result();
    result.written_up = true;
    // No card yet: nothing to say it on.
    card->showPasteStatus(result, QStringLiteral("too early"));
    CHECK(card->titleLabel()->text().isEmpty());

    card->present(result, true, true, "agent_task");
    const QString title = card->titleLabel()->text();
    const QString subtitle = card->subtitleLabel()->text();
    WalkthroughResult other = result;
    other.pdf = files.folder() / "other.pdf";
    card->showPasteStatus(other, QStringLiteral("not this one"));
    CHECK(card->titleLabel()->text() == title);
    CHECK(card->subtitleLabel()->text() == subtitle);

    card->showPasteStatus(
        result, QStringLiteral("PDF and instructions pasted; review them before sending"));
    CHECK(card->titleLabel()->text() == QStringLiteral("Write-up"));
    CHECK(card->subtitleLabel()->text() ==
          QStringLiteral("PDF and instructions pasted; review them before sending"));
}

TEST_CASE("showReady builds the card once and shows each walkthrough on it",
          "[walkthrough_ready][widget]") {
    ReadyFixture files;
    std::unique_ptr<WalkthroughReadyCard> slot;
    WalkthroughReadyCard* first = fv::showReady(slot, files.result(), true, true, "pdf");
    REQUIRE(first != nullptr);
    CHECK(first == slot.get());
    CHECK(first->isVisible());
    WalkthroughResult next = files.result();
    next.shots = 1;
    WalkthroughReadyCard* second = fv::showReady(slot, next, false, true, "pdf");
    CHECK(second == first);
    CHECK(second->subtitleLabel()->text() ==
          QStringLiteral(u"1 screenshot · copying failed — drag it from here"));
    slot->hide();
}

} // namespace fvtest
