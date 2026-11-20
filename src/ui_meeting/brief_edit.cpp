#include "ui_meeting/brief_edit.h"

#include <QFontMetrics>
#include <QMargins>
#include <QTextDocument>

#include "pyport/number.h"

namespace fv {

BriefEdit::BriefEdit(QWidget* parent) : QPlainTextEdit(parent) {}

QSize BriefEdit::sizeHint() const {
    const QMargins margins = contentsMargins();
    // round(documentMargin()): half to even, as Python rounds.
    const int documentMargin = static_cast<int>(py::round_half_even(document()->documentMargin()));
    const int height = kVisibleLines * fontMetrics().lineSpacing() + 2 * documentMargin +
                       margins.top() + margins.bottom();
    return {QPlainTextEdit::sizeHint().width(), height};
}

QSize BriefEdit::minimumSizeHint() const {
    return {QPlainTextEdit::minimumSizeHint().width(), sizeHint().height()};
}

} // namespace fv
