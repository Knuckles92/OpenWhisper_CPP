#include "ui_widgets/wrapped_label.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QKeySequence>
#include <QResizeEvent>

#include <cstdint>
#include <functional>
#include <string>

#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv {
namespace {

// A line break opportunity that draws nothing: ZERO WIDTH SPACE.
constexpr char16_t kBreak = u'​';
constexpr std::int64_t kChunk = 8;

// A run with no space, hyphen, slash or backslash to wrap at, as Python's re reads it: \s is
// Unicode whitespace and the length counts code points.
const py::Regex& unbreakableRun() {
    static const py::Regex pattern(std::string("[^\\s\xE2\x80\x8B\\-/\\\\]{16,}"));
    return pattern;
}

} // namespace

QString withBreakPoints(const QString& text) {
    QString spaced = text;
    spaced.replace(QLatin1Char('\\'), QString(QLatin1Char('\\')) + QChar(kBreak));
    const std::string result =
        unbreakableRun().sub(std::function<std::string(const py::Match&)>([](const py::Match& run) {
                                 const std::string whole = run.group();
                                 const auto length = static_cast<std::int64_t>(py::len(whole));
                                 std::string joined;
                                 for (std::int64_t start = 0; start < length; start += kChunk) {
                                     if (start != 0) {
                                         joined += "\xE2\x80\x8B";
                                     }
                                     joined += py::slice(whole, start, start + kChunk);
                                 }
                                 return joined;
                             }),
                             spaced.toStdString());
    return QString::fromStdString(result);
}

WrappedLabel::WrappedLabel(const QString& text, QWidget* parent, bool breakLongWords)
    : QLabel(parent), breakLongWords_(breakLongWords) {
    setWordWrap(true);
    setText(text);
}

void WrappedLabel::setText(const QString& text) {
    plain_ = text;
    QLabel::setText(breakLongWords_ ? withBreakPoints(text) : text);
}

QString WrappedLabel::text() const {
    return breakLongWords_ ? plain_ : QLabel::text();
}

QString WrappedLabel::selectedText() const {
    QString selected = QLabel::selectedText();
    selected.remove(QChar(kBreak));
    return selected;
}

void WrappedLabel::keyPressEvent(QKeyEvent* event) {
    QLabel::keyPressEvent(event);
    if (breakLongWords_ && event->matches(QKeySequence::Copy)) {
        cleanClipboard();
    }
}

void WrappedLabel::contextMenuEvent(QContextMenuEvent* event) {
    QLabel::contextMenuEvent(event);
    if (breakLongWords_) {
        cleanClipboard();
    }
}

void WrappedLabel::cleanClipboard() {
    QClipboard* clipboard = QApplication::clipboard();
    const QString copied = clipboard->text();
    QString clean = copied;
    clean.remove(QChar(kBreak));
    // Only ever repair a copy of this label's own text, never another app's clipboard.
    if (clean != copied && plain_.contains(clean)) {
        clipboard->setText(clean);
    }
}

QSize WrappedLabel::sizeHint() const {
    const QSize hint = QLabel::sizeHint();
    const int width = this->width();
    if (width <= 0) {
        return hint;
    }
    return QSize(hint.width(), heightForWidth(width));
}

QSize WrappedLabel::minimumSizeHint() const {
    const QSize hint = QLabel::minimumSizeHint();
    const int width = this->width();
    if (width <= 0) {
        return hint;
    }
    return QSize(hint.width(), heightForWidth(width));
}

void WrappedLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    if (event->oldSize().width() != event->size().width()) {
        updateGeometry();
    }
}

} // namespace fv
