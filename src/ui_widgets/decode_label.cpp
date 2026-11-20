#include "ui_widgets/decode_label.h"

#include <algorithm>
#include <cmath>

#include <QRandomGenerator>
#include <QTimer>

#include "pyport/number.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

const QString kGlyphs = QStringLiteral("0123456789ABCDEF#%&$@<>/\\|");
constexpr int kTickMs = 32;
constexpr int kDurationMs = 720;
// Characters still scrambling ahead of the locked run.
constexpr int kHead = 4;

const QString kEmphasisStyle = QStringLiteral("color:@text; font-weight:600");
const QString kScrambleStyle = QStringLiteral("color:@accent");

// html.escape(text).
QString htmlEscape(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&':
            out += QStringLiteral("&amp;");
            break;
        case '<':
            out += QStringLiteral("&lt;");
            break;
        case '>':
            out += QStringLiteral("&gt;");
            break;
        case '"':
            out += QStringLiteral("&quot;");
            break;
        case '\'':
            out += QStringLiteral("&#x27;");
            break;
        default:
            out += c;
        }
    }
    return out;
}

// The code points of text, since Python counts and slices a str in code points.
QList<char32_t> codePoints(const QString& text) {
    QList<char32_t> out;
    for (const char32_t c : text.toUcs4()) {
        out.push_back(c);
    }
    return out;
}

QString fromCodePoints(const QList<char32_t>& points, qsizetype from, qsizetype to) {
    QString out;
    for (qsizetype i = from; i < to; ++i) {
        out += QString::fromUcs4(&points[i], 1);
    }
    return out;
}

} // namespace

DecodeLabel::DecodeLabel(QWidget* parent) : QLabel(parent) {
    setTextFormat(Qt::RichText);
    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, &DecodeLabel::tick);
}

void DecodeLabel::setSegments(const DecodeSegments& segments, bool animate) {
    segments_ = segments;
    finalHtml_ = render(static_cast<int>(codePoints(plainText()).size()));
    timer_->stop();
    if (!animate) {
        pendingReveal_ = false;
        QLabel::setText(finalHtml_);
        return;
    }
    pendingReveal_ = true;
    QLabel::setText(QString());
    if (isVisible()) {
        reveal();
    }
}

void DecodeLabel::reveal() {
    pendingReveal_ = false;
    startedAt_.start();
    timer_->start();
    tick();
}

void DecodeLabel::clear() {
    timer_->stop();
    segments_.clear();
    finalHtml_.clear();
    pendingReveal_ = false;
    QLabel::clear();
}

bool DecodeLabel::isRevealing() const {
    return timer_->isActive();
}

void DecodeLabel::showEvent(QShowEvent* event) {
    QLabel::showEvent(event);
    if (pendingReveal_) {
        reveal();
    }
}

void DecodeLabel::hideEvent(QHideEvent* event) {
    QLabel::hideEvent(event);
    if (timer_->isActive()) {
        timer_->stop();
        QLabel::setText(finalHtml_);
    }
}

QString DecodeLabel::plainText() const {
    QString out;
    for (const auto& segment : segments_) {
        out += segment.first;
    }
    return out;
}

void DecodeLabel::tick() {
    const double elapsedMs = static_cast<double>(startedAt_.nsecsElapsed()) / 1e6;
    const double progress = std::min(1.0, elapsedMs / kDurationMs);
    if (progress >= 1.0) {
        timer_->stop();
        QLabel::setText(finalHtml_);
        return;
    }
    QLabel::setText(frameAt(progress));
}

QString DecodeLabel::frameAt(double progress) const {
    if (progress >= 1.0) {
        return finalHtml_;
    }
    const auto total = static_cast<double>(codePoints(plainText()).size());
    // Python's round: half to even.
    const int locked = static_cast<int>(py::round_half_even(total * std::max(0.0, progress)));
    return render(locked, kHead);
}

QString DecodeLabel::render(int locked, int scrambleHead) const {
    // Only the style attributes are resolved: the text itself may contain an "@" that is not a
    // token.
    const QString emphasisStyle = ui::resolveTokens(kEmphasisStyle);
    const QString scrambleStyle = ui::resolveTokens(kScrambleStyle);
    QString parts;
    qsizetype start = 0;
    for (const auto& segment : segments_) {
        const QList<char32_t> text = codePoints(segment.first);
        const qsizetype length = text.size();
        const qsizetype end = start + length;
        const qsizetype lockedCount =
            std::max<qsizetype>(0, std::min<qsizetype>(length, locked - start));
        if (lockedCount > 0) {
            const QString escaped = htmlEscape(fromCodePoints(text, 0, lockedCount));
            if (segment.second) {
                parts += QStringLiteral("<span style='%1'>%2</span>").arg(emphasisStyle, escaped);
            } else {
                parts += escaped;
            }
        }
        const qsizetype headFrom = std::max<qsizetype>(start, locked);
        const qsizetype headTo = std::min<qsizetype>(end, locked + scrambleHead);
        const qsizetype from = std::min(headFrom - start, length);
        const qsizetype to = std::min(std::max(headFrom, headTo) - start, length);
        for (qsizetype i = from; i < to; ++i) {
            if (text[i] == U' ') {
                parts += QLatin1Char(' ');
            } else {
                const auto pick = QRandomGenerator::global()->bounded(kGlyphs.size());
                const QString glyph = htmlEscape(kGlyphs.mid(static_cast<qsizetype>(pick), 1));
                parts += QStringLiteral("<span style='%1'>%2</span>").arg(scrambleStyle, glyph);
            }
        }
        start = end;
    }
    // Rich text collapses runs of spaces, which would close up the separators, so pin them.
    return parts.replace(QStringLiteral("  "), QStringLiteral("&nbsp;&nbsp;"));
}

} // namespace fv
