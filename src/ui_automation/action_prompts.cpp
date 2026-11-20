#include "ui_automation/action_prompts.h"

#include <QClipboard>
#include <QGuiApplication>

namespace fv::action_prompts {

Question& question() {
    static Question current = [](QWidget* parent, const QString& title, const QString& text,
                                 QMessageBox::StandardButtons buttons,
                                 QMessageBox::StandardButton defaultButton) {
        return QMessageBox::question(parent, title, text, buttons, defaultButton);
    };
    return current;
}

CopyText& copyText() {
    static CopyText current = [](const QString& text) {
        if (QClipboard* clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text);
        }
    };
    return current;
}

QMessageBox::StandardButton ask(QWidget* parent, const QString& title, const QString& text,
                                QMessageBox::StandardButtons buttons,
                                QMessageBox::StandardButton defaultButton) {
    const Question& current = question();
    return current ? current(parent, title, text, buttons, defaultButton) : QMessageBox::Cancel;
}

} // namespace fv::action_prompts
