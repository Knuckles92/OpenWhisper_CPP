#include "documents/painting.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QPainter>
#include <QPdfWriter>
#include <QPen>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/utf8.h"
#include "documents/composer.h"
#include "documents/detail/text.h"
#include "walkthrough/renderer.h"

namespace fv::documents {

namespace {

QColor color(std::string_view name) {
    return QColor(detail::qtText(name));
}

// A rule above the table, under each row, and a firmer one under the header.
void paintTableRules(QPainter& painter, const QTextDocument& doc, QTextTable& table, int index) {
    const double height = doc.pageSize().height();
    QAbstractTextDocumentLayout* layout = doc.documentLayout();
    const QRectF box = layout->frameBoundingRect(&table);
    const double padding = table.format().cellPadding();
    std::vector<double> edges;
    std::vector<QTextTableCell> cells;
    for (int row = 0; row < table.rows(); ++row) {
        cells.clear();
        double top = std::numeric_limits<double>::infinity();
        for (int column = 0; column < table.columns(); ++column) {
            cells.push_back(table.cellAt(row, column));
            top = std::min(
                top, layout->blockBoundingRect(cells.back().firstCursorPosition().block()).top());
        }
        edges.push_back(top - padding);
    }
    if (cells.empty()) {
        return;
    }
    // Under the last row's tallest cell, not the frame's own margin below it.
    double bottom = -std::numeric_limits<double>::infinity();
    for (const QTextTableCell& cell : cells) {
        bottom =
            std::max(bottom, layout->blockBoundingRect(cell.lastCursorPosition().block()).bottom());
    }
    edges.push_back(bottom + padding);
    for (std::size_t number = 0; number < edges.size(); ++number) {
        const double y = edges[number];
        if (pageIndex(y, height) != index) {
            continue;
        }
        QPen pen = painter.pen();
        if (number == 1) {
            pen.setWidthF(pen.widthF() * 2);
        }
        painter.save();
        painter.setPen(pen);
        painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        painter.restore();
    }
}

// What Qt would draw beside the lines on page `index`: painting line by line leaves out
// horizontal rules, quote bars, code shading and table rules, so they are drawn here in the
// screenshot frames' hairline. Only a write-up's Markdown has them.
void paintMarks(QPainter& painter, const QTextDocument& doc, int index, const QPen& pen) {
    const double height = doc.pageSize().height();
    const double unit = pen.widthF() / kFrameWidthPt;
    painter.save();
    painter.setPen(pen);
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat blockFormat = block.blockFormat();
        const QRectF rect = lineExtent(doc, block);
        if (rect.isNull() || pageIndex(rect.top(), height) != index) {
            continue;
        }
        if (blockFormat.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            const double middle = rect.center().y();
            painter.drawLine(QPointF(0, middle), QPointF(doc.pageSize().width(), middle));
        } else if (blockFormat.hasProperty(QTextFormat::BlockCodeFence)) {
            const double pad = kCodePadPt * unit;
            painter.fillRect(rect.adjusted(-pad, -pad / 2, 0, pad / 2), color(kCodeFill));
        } else if (blockFormat.hasProperty(QTextFormat::BlockQuoteLevel)) {
            const double x = rect.left() - kQuoteGapPt * unit;
            painter.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        }
    }
    for (QTextFrame* frame : doc.rootFrame()->childFrames()) {
        if (auto* table = qobject_cast<QTextTable*>(frame)) {
            paintTableRules(painter, doc, *table, index);
        }
    }
    painter.restore();
}

// The lines, screenshots included, and frames on page `index`. Line by line rather than with
// drawContents: QTextLayout also paints the line before a clip rectangle, which the clip hides
// but the PDF would keep as text, so text copied or searched across a page break would come up
// twice.
PageRecord paintPage(QPainter& painter, const QTextDocument& doc, int index,
                     const std::vector<QRectF>& frames, const QPen& pen) {
    PageRecord record;
    const double height = doc.pageSize().height();
    painter.save();
    painter.translate(0, -index * height);
    paintMarks(painter, doc, index, pen);
    for (const PlacedLine& placed : lines(doc)) {
        if (pageIndex(placed.origin.y() + placed.line.y(), height) == index) {
            placed.line.draw(&painter, placed.origin);
            record.lines.push_back(
                placed.block.text().mid(placed.line.textStart(), placed.line.textLength()));
        }
    }
    painter.setPen(pen);
    for (const QRectF& frame : frames) {
        if (pageIndex(frame.top(), height) == index) {
            painter.drawRect(frame);
            record.frames.push_back(frame);
        }
    }
    painter.restore();
    return record;
}

} // namespace

std::vector<PageRecord> paintPages(QTextDocument& doc, const std::vector<QRectF>& frames,
                                   QPdfWriter& writer, int dpi, const std::filesystem::path& path) {
    QPainter painter;
    if (!painter.begin(&writer)) {
        throw walkthrough::DocumentError("Could not write " + path_to_utf8(path));
    }
    struct End {
        QPainter& painter;
        ~End() { painter.end(); }
    } end{painter};
    painter.setRenderHint(QPainter::LosslessImageRendering);
    const double scale = writer.resolution() / static_cast<double>(dpi);
    painter.scale(scale, scale);
    const QPen pen(color(kFrameColor), kFrameWidthPt * dpi / 72);
    std::vector<PageRecord> pages;
    for (int index = 0; index < doc.pageCount(); ++index) {
        if (index) {
            writer.newPage();
        }
        pages.push_back(paintPage(painter, doc, index, frames, pen));
    }
    return pages;
}

std::vector<PlacedLine> lines(const QTextDocument& doc) {
    std::vector<PlacedLine> placed;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextLayout* layout = block.layout();
        const QPointF where = origin(doc, block);
        for (int number = 0; number < layout->lineCount(); ++number) {
            placed.push_back({block, layout->lineAt(number), where});
        }
    }
    return placed;
}

QPointF origin(const QTextDocument& doc, const QTextBlock& block) {
    if (QTextCursor(block).currentTable() == nullptr) {
        return block.layout()->position();
    }
    return doc.documentLayout()->blockBoundingRect(block).topLeft();
}

QRectF lineExtent(const QTextDocument& doc, const QTextBlock& block) {
    const QTextLayout* layout = block.layout();
    if (layout->lineCount() == 0) {
        return {};
    }
    const QTextLine first = layout->lineAt(0);
    const QTextLine last = layout->lineAt(layout->lineCount() - 1);
    double left = first.x();
    for (int number = 1; number < layout->lineCount(); ++number) {
        left = std::min(left, layout->lineAt(number).x());
    }
    const QRectF rect(left, first.y(), doc.pageSize().width() - left,
                      last.y() + last.height() - first.y());
    return rect.translated(origin(doc, block));
}

int pageIndex(double top, double height) {
    return static_cast<int>(std::floor((top + kSlack) / height));
}

QSizeF shownSize(const QImage& image, QSizeF page, double unit) {
    const QSizeF natural = QSizeF(image.width(), image.height()) * (unit * 72 / 96);
    const double scale = std::min(
        {1.0, page.width() / natural.width(), page.height() * kMaxImageHeight / natural.height()});
    return natural * scale;
}

QFont documentFont(const QString& family) {
    QStringList families;
    if (!family.isEmpty()) {
        families.append(family);
    }
    for (std::string_view name : kFontFamilies) {
        families.append(detail::qtText(name));
    }
    QFont font(families);
    font.setStyleHint(QFont::SansSerif);
    font.setPointSizeF(kWordsStyle.size);
    return font;
}

} // namespace fv::documents
