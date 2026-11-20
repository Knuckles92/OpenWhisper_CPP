#pragma once

#include <functional>

#include <QMessageBox>
#include <QString>

class QWidget;

// What the Actions page asks and copies outside its own widgets: the confirmations it puts up
// (QMessageBox.question) and the clipboard a signing secret goes to (action_editor.copy_text).
// Tests replace them, so no test shows a message box or writes the real clipboard; the defaults
// are Qt's.
namespace fv::action_prompts {

// QMessageBox::question(parent, title, text, buttons, defaultButton): the button chosen.
using Question = std::function<QMessageBox::StandardButton(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;
Question& question();

// Puts the text on the clipboard.
using CopyText = std::function<void(const QString& text)>;
CopyText& copyText();

// question() when set; Cancel when it was cleared.
QMessageBox::StandardButton ask(QWidget* parent, const QString& title, const QString& text,
                                QMessageBox::StandardButtons buttons,
                                QMessageBox::StandardButton defaultButton);

} // namespace fv::action_prompts
