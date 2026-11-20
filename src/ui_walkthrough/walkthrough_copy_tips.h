#pragma once

#include <string_view>

#include <QString>
#include <QStringList>

class QAction;
class QWidget;

// Shared, wrapped hints for walkthrough copy choices and copy actions
// (ui_qt/widgets/walkthrough_copy_tips.py). Affinity: GUI thread.
namespace fv {

// What copying in format (a WalkthroughCopyFormat value: pdf, agent_task, both, agent, word)
// does; on finish, the agent_task choice explains auto-paste on and off instead. Empty for an
// unknown format.
QStringList copyTipParagraphs(std::string_view format, bool onFinish = false);

// The paragraphs, HTML-escaped, in a 300 px rich-text table: Qt then wraps the tooltip, menus
// included.
QString copyTipHtml(std::string_view format, bool onFinish = false);

// Explains the copy action (or delivery when a walkthrough finishes) in target's tooltip. A
// widget also gets the paragraphs as plain text in its accessible description, joined by a
// blank line, so readers do not announce markup.
void setCopyTip(QWidget* target, std::string_view format, bool onFinish = false);
void setCopyTip(QAction* target, std::string_view format, bool onFinish = false);

} // namespace fv
