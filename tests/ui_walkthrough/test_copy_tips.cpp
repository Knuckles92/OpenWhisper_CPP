// Copy hints (ui_qt/widgets/walkthrough_copy_tips.py), from tests/test_walkthrough_copy_tips.py,
// and the golden tips of tools/parity/cases/ui_qt.widgets.walkthrough_copy_tips.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <tuple>

#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTextDocument>
#include <QWidget>

#include "golden/golden.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "tab_support.h"
#include "ui_walkthrough/walkthrough_copy_tips.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_walkthrough/walkthrough_review.h"

namespace fvtest {
namespace {

QString plain(const QString& tip) {
    QTextDocument document;
    document.setHtml(tip);
    return document.toPlainText();
}

QStringList words(const QString& text) {
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

} // namespace

// from tests/test_walkthrough_copy_tips.py::
//     test_each_copy_choice_and_the_chosen_one_explain_themselves
// The Walkthrough tab's "Copy when it finishes" field: its menu and the field itself. (The
// desktop parameter, OPENWHISPER_UI, changes nothing the tab reads.)
TEST_CASE("each copy choice and the chosen one explain themselves",
          "[test_walkthrough_copy_tips][widget]") {
    const auto [format, expected] = GENERATE(table<std::string, QString>({
        {"pdf", QStringLiteral("PDF file")},
        {"agent_task", QStringLiteral("Auto-paste on:")},
        {"both", QStringLiteral("chooses which format")},
        {"agent", QStringLiteral("local file link")},
        {"word", QStringLiteral(".docx")},
    }));
    CAPTURE(format);
    TabRig tab;
    QString label;
    for (const auto& choice : fv::walkthrough_tab::COPY_CHOICES) {
        if (choice.value == format) {
            label =
                QString::fromUtf8(choice.text.data(), static_cast<qsizetype>(choice.text.size()));
        }
    }
    std::unique_ptr<QMenu> menu(tab->copyField()->buildMenu());
    CHECK(menu->toolTipsVisible());
    QAction* action = nullptr;
    for (QAction* each : menu->actions()) {
        if (each->text() == label) {
            action = each;
        }
    }
    REQUIRE(action != nullptr);
    CHECK(plain(action->toolTip()).contains(expected));

    // Chosen, the field explains it on hover and to screen readers.
    action->trigger();
    fv::WalkthroughField* field = tab->copyField();
    CHECK(plain(field->toolTip()).contains(expected));
    CHECK(words(plain(field->toolTip())) == words(field->accessibleDescription()));
}

TEST_CASE("manual copy hints never mention auto-paste", "[walkthrough_copy_tips][widget]") {
    QAction agent(QStringLiteral("Copy for agent"));
    fv::setCopyTip(&agent, "agent_task");
    const QString hint = plain(agent.toolTip());
    CHECK(hint.contains(QStringLiteral("attach the PDF")));
    CHECK_FALSE(hint.contains(QStringLiteral("Auto-paste")));
    const QString finish = fv::copyTipParagraphs("agent_task", true).join(u' ');
    CHECK(finish.contains(QStringLiteral("Auto-paste off:")));
    CHECK(finish.contains(QStringLiteral("unsent")));
    // An action has no accessible description to set; a widget gets the paragraphs.
    QWidget field;
    fv::setCopyTip(&field, "agent_task", true);
    CHECK(field.accessibleDescription() == fv::copyTipParagraphs("agent_task", true).join(u"\n\n"));
}

// from tests/test_walkthrough_copy_tips.py::test_manual_copy_hints_match_in_review_and_ready_card
// The Python test runs once per Linux desktop (classic, Omarchy); on Windows there is one. The
// storyboard is built alone rather than inside the Walkthrough tab (M6.15's), and the tab's
// "Agent" choice is setCopyTip with onFinish, as above.
TEST_CASE("manual copy hints match in review and ready card",
          "[test_walkthrough_copy_tips][widget]") {
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WalkthroughReview review(settings);
    fv::WalkthroughReadyCard card;
    CHECK(card.testAttribute(Qt::WA_AlwaysShowToolTips));
    CHECK(card.menu()->toolTipsVisible());
    CHECK(review.menu()->toolTipsVisible());
    CHECK(card.copyPdfAction()->toolTip() == review.pdfButton()->toolTip());
    CHECK(card.copyWordAction()->toolTip() == review.copyWordButton()->toolTip());
    CHECK(card.copyMarkdownAction()->toolTip() == review.markdownButton()->toolTip());
    CHECK(card.copyAgentAction()->toolTip() == review.copyAgentAction()->toolTip());
    const QString agentHint = plain(card.copyAgentAction()->toolTip());
    CHECK(agentHint.contains(QStringLiteral("attach the PDF")));
    CHECK_FALSE(agentHint.contains(QStringLiteral("Auto-paste")));
    QAction agentChoice(QStringLiteral("Agent"));
    fv::setCopyTip(&agentChoice, "agent_task", true);
    const QString finishHint = plain(agentChoice.toolTip());
    CHECK(finishHint.contains(QStringLiteral("Auto-paste off:")));
    CHECK(finishHint.contains(QStringLiteral("unsent")));
}
TEST_CASE("copy tips are OpenWhisper's, word for word", "[golden][walkthrough_copy_tips]") {
    fv::golden::for_each(
        "ui_qt.widgets.walkthrough_copy_tips", "set_copy_tip", [](const fv::golden::Case& c) {
            const auto format = c.arg<std::string>(0);
            const bool onFinish = c.arg<bool>(1);
            if (c.raises()) {
                CHECK(c.raises_type() == "KeyError");
                CHECK(fv::copyTipParagraphs(format, onFinish).isEmpty());
                return;
            }
            const fv::Json expected = c.result<fv::Json>();
            CHECK(fv::copyTipHtml(format, onFinish).toStdString() ==
                  expected.at("tooltip").get<std::string>());
            QStringList paragraphs;
            for (const auto& text : expected.at("paragraphs")) {
                paragraphs.append(QString::fromStdString(text.get<std::string>()));
            }
            CHECK(fv::copyTipParagraphs(format, onFinish) == paragraphs);
            QAction action(QStringLiteral("copy"));
            fv::setCopyTip(&action, format, onFinish);
            CHECK(action.toolTip().toStdString() == expected.at("tooltip").get<std::string>());
        });
}

} // namespace fvtest
