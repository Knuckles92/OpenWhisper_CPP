#include "documents/pages.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextLine>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>

namespace fv::documents {

std::vector<QRectF> paginate(QTextDocument& doc, const std::vector<Group>& groups) {
    Pages pages(doc);
    pages.paginate(groups);
    return pages.frames(groups);
}

Pages::Pages(QTextDocument& doc) : doc_(doc), height_(doc.pageSize().height()) {
    doc.documentLayout()->documentSize();
}

void Pages::paginate(const std::vector<Group>& groups) {
    // A later group with the same first block wins, as a dict built from the list does.
    std::map<int, const Group*> starts;
    for (const Group& group : groups) {
        starts[group.first] = &group;
    }
    for (int blockNumber = 0; blockNumber < doc_.blockCount(); ++blockNumber) {
        flushTop(blockNumber);
        if (const auto found = starts.find(blockNumber); found != starts.end()) {
            keepTogether(*found->second);
        }
    }
}

std::vector<QRectF> Pages::frames(const std::vector<Group>& groups) const {
    std::vector<QRectF> frames;
    for (const Group& group : groups) {
        if (group.image) {
            const QTextLayout* shot = layout(*group.image);
            frames.push_back(shot->lineAt(0).naturalTextRect().translated(shot->position()));
        }
    }
    return frames;
}

// Keeps only the part of a block's top margin before a page break.
void Pages::flushTop(int blockNumber) {
    const double margin = this->margin(blockNumber);
    const double top = extent(blockNumber).first;
    const double pageTop = pageOf(top + kSlack) * height_;
    if (margin != 0 && top - margin < pageTop + kSlack && pageTop + kSlack < top) {
        const double kept = std::max(0.0, pageTop - (top - margin));
        setBlockFormat(blockNumber, kept);
    }
}

// Puts `group` on one page, the first way that works. It stays where it is if it fits. Else it
// starts on a new page, with a smaller screenshot if that is what fits there. Else its
// screenshot shrinks to fit below the words on their last page. The section under the header
// starts at the title and never moves.
void Pages::keepTogether(const Group& group) {
    if (onOnePage(group)) {
        return;
    }
    if (!atPageTop(group.first) && fitsOnNewPage(group)) {
        return;
    }
    shrinkToFit(group);
}

// Starts `group` on a new page if it fits there; undone if not.
bool Pages::fitsOnNewPage(const Group& group) {
    const QTextBlockFormat blockFormat = block(group.first).blockFormat();
    const std::optional<QTextImageFormat> saved = imageFormat(group);
    // Flush with the top of the page, like the title.
    setBlockFormat(group.first, 0.0, true);
    shrinkToFit(group);
    if (onOnePage(group)) {
        return true;
    }
    QTextCursor(block(group.first)).setBlockFormat(blockFormat);
    if (saved) {
        imageCursor(*group.image).setCharFormat(*saved);
    }
    return false;
}

// Shrinks the screenshot to fit below the words on their last page, never below half its size:
// a screenshot too small to read is worse than one on the next page. One that starts its
// section has no words to stay with, and fits on any page as it is. The caption, when there is
// one, goes below it on the same page.
void Pages::shrinkToFit(const Group& group) {
    std::optional<QTextImageFormat> format = imageFormat(group);
    if (!format || *group.image == group.first) {
        return;
    }
    const int image = *group.image;
    const double above = end(image - 1) + margin(image);
    const double pageEnd = (pageOf(above - kSlack) + 1) * height_;
    double below = 0.0;
    if (group.last != image) { // the caption
        const auto [captionTop, captionBottom] = extent(group.last);
        below = margin(group.last) + captionBottom - captionTop;
    }
    // A unit to spare: the layout rounds an image's size to whole units.
    const double scale = (pageEnd - above - below - 1) / format->height();
    if (!(kMinImageScale <= scale && scale < 1)) {
        return;
    }
    format->setWidth(format->width() * scale);
    format->setHeight(format->height() * scale);
    imageCursor(image).setCharFormat(*format);
}

bool Pages::onOnePage(const Group& group) const {
    const double top = extent(group.first).first;
    const double bottom = extent(group.last).second;
    return pageOf(top + kSlack) == pageOf(bottom - kSlack);
}

bool Pages::atPageTop(int blockNumber) const {
    const double top = extent(blockNumber).first;
    // Python's float %, which takes the sign of the divisor.
    return top - std::floor(top / height_) * height_ < kSlack;
}

int Pages::pageOf(double y) const {
    return static_cast<int>(std::floor(y / height_));
}

std::pair<double, double> Pages::extent(int blockNumber) const {
    const QTextLayout* blockLayout = layout(blockNumber);
    const double origin = blockLayout->position().y();
    const QTextLine first = blockLayout->lineAt(0);
    const QTextLine last = blockLayout->lineAt(blockLayout->lineCount() - 1);
    return {origin + first.y(), origin + last.y() + last.height()};
}

double Pages::end(int blockNumber) const {
    const QTextLayout* blockLayout = layout(blockNumber);
    const QTextLine last = blockLayout->lineAt(blockLayout->lineCount() - 1);
    const double spacing = block(blockNumber).blockFormat().lineHeight() / 100;
    return blockLayout->position().y() + last.y() + last.height() * spacing;
}

QTextLayout* Pages::layout(int blockNumber) const {
    QTextLayout* blockLayout = block(blockNumber).layout();
    if (blockLayout == nullptr || blockLayout->lineCount() == 0) {
        throw std::runtime_error("Walkthrough PDF block " + std::to_string(blockNumber) +
                                 " was not laid out");
    }
    return blockLayout;
}

double Pages::margin(int blockNumber) const {
    return block(blockNumber).blockFormat().topMargin();
}

void Pages::setBlockFormat(int blockNumber, double topMargin, bool newPage) {
    const QTextBlock target = block(blockNumber);
    QTextBlockFormat blockFormat = target.blockFormat();
    blockFormat.setTopMargin(topMargin);
    if (newPage) {
        blockFormat.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
    }
    QTextCursor(target).setBlockFormat(blockFormat);
}

QTextBlock Pages::block(int blockNumber) const {
    return doc_.findBlockByNumber(blockNumber);
}

std::optional<QTextImageFormat> Pages::imageFormat(const Group& group) const {
    if (!group.image) {
        return std::nullopt;
    }
    return imageCursor(*group.image).charFormat().toImageFormat();
}

QTextCursor Pages::imageCursor(int blockNumber) const {
    QTextCursor cursor(block(blockNumber));
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    return cursor;
}

} // namespace fv::documents
