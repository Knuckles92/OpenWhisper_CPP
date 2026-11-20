#pragma once

#include <QRectF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QTextLayout>

#include <optional>
#include <utility>
#include <vector>

#include "documents/composer.h"

class QTextDocument;

// Fits a paginated document's screenshot sections to its pages (services/walkthrough/
// document.py:864-1038). QTextDocument breaks pages between any two lines, so a section's words
// could end one page and its screenshot start the next, and it keeps a block's top margin across
// a page break, so a page could start lower than the others. Affinity: as composer.h.
namespace fv::documents {

// Fits `doc` to its pages; returns where its screenshots ended up (_paginate).
std::vector<QRectF> paginate(QTextDocument& doc, const std::vector<Group>& groups);

class Pages {
public:
    // Lays `doc` out in full first: QTextDocumentLayout lays out lazily, and a format changed
    // while it is unfinished leaves the blocks after it without lines (see layout()). Laid out
    // in full, it lays out again all that follows each change.
    explicit Pages(QTextDocument& doc);

    // Starts every page flush with its top and keeps each screenshot section on one page,
    // block by block in order: a change only moves what follows it.
    void paginate(const std::vector<Group>& groups);
    // Where each screenshot ended up, in document coordinates.
    std::vector<QRectF> frames(const std::vector<Group>& groups) const;

private:
    void flushTop(int blockNumber);
    void keepTogether(const Group& group);
    bool fitsOnNewPage(const Group& group);
    void shrinkToFit(const Group& group);
    bool onOnePage(const Group& group) const;
    bool atPageTop(int blockNumber) const;
    int pageOf(double y) const;
    // Where a block's first line starts and its last line ends. Not blockBoundingRect: when
    // pagination pushes a block's lines to the next page, that rectangle stays where the block
    // began.
    std::pair<double, double> extent(int blockNumber) const;
    // Where the next block's top margin starts: below the last line, with the spacing a
    // proportional line height adds there too.
    double end(int blockNumber) const;
    // The block's layout. Throws std::runtime_error when it has no lines: asking an empty
    // layout for a line crashes the process, and the full layout in the constructor means it
    // never should be empty.
    QTextLayout* layout(int blockNumber) const;
    double margin(int blockNumber) const;
    void setBlockFormat(int blockNumber, double topMargin, bool newPage = false);
    QTextBlock block(int blockNumber) const;
    std::optional<QTextImageFormat> imageFormat(const Group& group) const;
    // A cursor selecting the screenshot, the only thing in its block.
    QTextCursor imageCursor(int blockNumber) const;

    QTextDocument& doc_;
    double height_;
};

} // namespace fv::documents
