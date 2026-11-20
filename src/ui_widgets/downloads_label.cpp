#include "ui_widgets/downloads_label.h"

#include <algorithm>

#include <QFontMetrics>

#include "pyport/text.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

const QString kLinkSuffix = QStringLiteral(" in Downloads.");
const QString kLinkText = QStringLiteral(" Downloads.");

// html.escape(text) on a QString.
QString htmlEscape(const QString& text) {
    return QString::fromStdString(py::html_escape(text.toStdString()));
}

} // namespace

DownloadsLabel::DownloadsLabel(const QString& text, QWidget* parent)
    : ElidingLabel(text, Qt::ElideRight, parent) {
    setOpenExternalLinks(false);
    connect(this, &QLabel::linkActivated, this, &DownloadsLabel::onLinkActivated);
    // ElidingLabel's constructor elided text with its own applyElide, as a base constructor
    // cannot reach this override.
    applyElide();
}

void DownloadsLabel::onLinkActivated(const QString& link) {
    if (link == QLatin1String("downloads")) {
        Q_EMIT downloadsRequested();
    }
}

QSize DownloadsLabel::minimumSizeHint() const {
    QSize hint = ElidingLabel::minimumSizeHint();
    if (fullText_.endsWith(kLinkSuffix)) {
        const int linkWidth = fontMetrics().horizontalAdvance(QStringLiteral(u"… Downloads."));
        hint.setWidth(std::max(hint.width(), linkWidth));
    }
    return hint;
}

void DownloadsLabel::applyElide() {
    const bool hasLink = fullText_.endsWith(kLinkSuffix);
    setTextFormat(hasLink ? Qt::RichText : Qt::PlainText);
    setTextInteractionFlags(hasLink ? (Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard)
                                    : Qt::NoTextInteraction);
    if (!hasLink) {
        ElidingLabel::applyElide();
        return;
    }

    const QFontMetrics metrics = fontMetrics();
    QString prefix = fullText_.chopped(kLinkText.size());
    const int contentsWidth = contentsRect().width();
    const int available = contentsWidth != 0 ? contentsWidth : metrics.horizontalAdvance(fullText_);
    // Reserve the action's width before shortening the explanation.
    prefix = metrics.elidedText(prefix, Qt::ElideRight,
                                std::max(0, available - metrics.horizontalAdvance(kLinkText)));
    QLabel::setText(QStringLiteral("%1 <a href=\"downloads\" style=\"color: %2; "
                                   "text-decoration: underline;\">Downloads</a>.")
                        .arg(htmlEscape(prefix), ui::currentPalette().qCss(u"accent")));
}

} // namespace fv
