#pragma once

#include <QFont>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTextBlock>
#include <QTextLine>

#include <filesystem>
#include <vector>

class QPdfWriter;
class QTextDocument;

// Painting a paginated QTextDocument into a PDF line by line, with the marks and frames Qt would
// otherwise leave out (services/walkthrough/document.py:561-733). Affinity: as composer.h.
namespace fv::documents {

// What went on one page of a written PDF: the text of every line painted there, in document
// order (an image is U+FFFC, a list marker's tab and a line break inside a paragraph are kept),
// and the screenshot frames drawn there, in document coordinates.
struct PageRecord {
    std::vector<QString> lines;
    std::vector<QRectF> frames;
};

// One line of a document and where its block's lines are placed.
struct PlacedLine {
    QTextBlock block;
    QTextLine line;
    QPointF origin;
};

// Paints every page of `doc` and the frames around its screenshots into `writer`, one page per
// document page, scaled from `dpi` layout units to the writer's resolution. Lossless: Qt's
// print() would encode the screenshots as JPEG. Throws walkthrough::DocumentError("Could not
// write <path>") when the painter cannot begin on the writer.
std::vector<PageRecord> paintPages(QTextDocument& doc, const std::vector<QRectF>& frames,
                                   QPdfWriter& writer, int dpi, const std::filesystem::path& path);

// Every line of `doc`, in order, with where its block is.
std::vector<PlacedLine> lines(const QTextDocument& doc);
// Where `block`'s lines are placed: its layout's position, or for a table cell's block, which is
// placed within its cell, the cell's place from the document layout.
QPointF origin(const QTextDocument& doc, const QTextBlock& block);
// The rectangle `block`'s lines take up, to the page's right edge; null without lines.
QRectF lineExtent(const QTextDocument& doc, const QTextBlock& block);
// The page something starting at `top` is on; pagination puts every line, and so every
// screenshot, wholly on one.
int pageIndex(double top, double height);
// A pixel to 1/96 inch, scaled down to `page`'s width and kMaxImageHeight of its height. `unit`
// is layout units per point.
QSizeF shownSize(const QImage& image, QSizeF page, double unit);
// `family` if this computer has it, else Segoe UI, Noto Sans or DejaVu Sans, else the system's
// sans-serif, at the words' 11 pt.
QFont documentFont(const QString& family = {});

} // namespace fv::documents
