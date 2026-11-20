#include "documents/markdown_composer.h"

#include <QImage>
#include <QList>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextOption>
#include <QTextTable>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "documents/detail/text.h"
#include "documents/pages.h"
#include "documents/painting.h"
#include "documents/pdf.h"
#include "pyport/text.h"

namespace fv::documents {

namespace {

constexpr char16_t kObjectReplacement = u'￼';

QString bullet(QTextListFormat::Style style) {
    switch (style) {
    case QTextListFormat::ListDisc:
        return QString(QChar(u'•'));
    case QTextListFormat::ListCircle:
        return QString(QChar(u'◦'));
    case QTextListFormat::ListSquare:
        return QString(QChar(u'▪'));
    default:
        return {};
    }
}

double headingSpace(int level) {
    if (level >= 1 && level <= static_cast<int>(kHeadingSpace.size())) {
        return kHeadingSpace[static_cast<std::size_t>(level - 1)];
    }
    return kOtherHeadingSpace;
}

// One layout of one Markdown text.
class Build {
public:
    Build(QSizeF page, double unit, const std::filesystem::path& folder,
          const std::optional<QColor>& textColor, const std::optional<QColor>& headingColor,
          const QString& font)
        : page_(page), unit_(unit), folder_(folder), textColor_(textColor),
          headingColor_(headingColor), doc_(std::make_unique<QTextDocument>()) {
        doc_->setDocumentMargin(0);
        doc_->setUseDesignMetrics(true);
        doc_->setDefaultFont(documentFont(font));
        doc_->setPageSize(page);
    }

    ComposedDocument layout(std::string_view text) {
        doc_->setMarkdown(detail::qtText(text),
                          QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                          QTextDocument::MarkdownNoHTML));
        dropBlankCellLines();
        writeListMarkers();
        std::vector<Group> groups;
        for (QTextBlock block = doc_->begin(); block.isValid(); block = block.next()) {
            if (styleBlock(block)) {
                const int number = block.blockNumber();
                groups.push_back({leadIn(number), number, number});
            }
        }
        return {std::move(doc_), std::move(groups), {}};
    }

private:
    // Removes the empty line Qt puts atop a table's first cell when a code block or quote comes
    // right before the table, pushing that cell's words a line below the rest of its row.
    void dropBlankCellLines() {
        for (QTextFrame* frame : doc_->rootFrame()->childFrames()) {
            auto* table = qobject_cast<QTextTable*>(frame);
            if (table == nullptr) {
                continue;
            }
            for (int row = 0; row < table->rows(); ++row) {
                for (int column = 0; column < table->columns(); ++column) {
                    const QTextTableCell cell = table->cellAt(row, column);
                    const QTextBlock first = cell.firstCursorPosition().block();
                    if (first != cell.lastCursorPosition().block() && first.length() - 1 == 0) {
                        QTextCursor cursor(first);
                        cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
                        cursor.removeSelectedText();
                    }
                }
            }
        }
    }

    // Writes each list item's number or bullet into its text. The pages are painted line by
    // line, which leaves out what Qt draws beside a line, list markers included; as text they
    // are painted, and copied and searched with the words. The marker hangs in the item's
    // indent, so wrapped lines start under the words. Items stay in their lists, which no longer
    // indent them: the list's indent becomes the block's own, as the Word writer reads it.
    void writeListMarkers() {
        struct Item {
            QTextBlock block;
            int listIndent;
            QString marker;
        };
        std::vector<Item> items;
        std::vector<QTextList*> lists;
        for (QTextBlock block = doc_->begin(); block.isValid(); block = block.next()) {
            if (QTextList* list = block.textList(); list != nullptr) {
                if (std::find(lists.begin(), lists.end(), list) == lists.end()) {
                    lists.push_back(list);
                }
                items.push_back({block, list->format().indent(), listMarker(*list, block)});
            }
        }
        for (QTextList* list : lists) {
            QTextListFormat format = list->format();
            format.setIndent(0);
            list->setFormat(format);
        }
        const double hang = kListHangPt * unit_;
        for (const Item& item : items) {
            QTextBlockFormat format = item.block.blockFormat();
            format.setIndent(format.indent() + item.listIndent);
            format.setTextIndent(-hang);
            // From where the first line starts: the words line up with the wrapped lines.
            format.setTabPositions({QTextOption::Tab(hang, QTextOption::LeftTab)});
            QTextCursor cursor(item.block);
            cursor.setBlockFormat(format);
            cursor.insertText(item.marker + QChar(u'\t'), item.block.charFormat());
        }
    }

    // How far the block's text starts in from the page's left edge.
    double left(const QTextBlock& block) const {
        const QTextBlockFormat format = block.blockFormat();
        return format.leftMargin() + format.indent() * doc_->indentWidth();
    }

    // Spaces and sizes one block; true when it is a screenshot on its own.
    bool styleBlock(const QTextBlock& block) {
        QTextBlockFormat format = block.blockFormat();
        const int level = format.headingLevel();
        const int images = sizeImages(block);
        const bool alone = images == 1 && words(block).isEmpty();
        if (level) {
            const double space = block.blockNumber() == 0 ? 0.0 : headingSpace(level);
            format.setTopMargin(space * unit_);
            format.setBottomMargin(kHeadingGap * unit_);
        } else if (alone) {
            // Not the body's line height: on a line as tall as a screenshot it would open a gap
            // under it.
            format.setAlignment(Qt::AlignHCenter);
            format.setTopMargin(kImageStyle.spaceBefore * unit_ * 2);
            // The same below, or a list item after it touches its frame.
            format.setBottomMargin(kImageStyle.spaceBefore * unit_ * 2);
        } else {
            format.setLineHeight(kBodyLineHeight, QTextBlockFormat::ProportionalHeight);
        }
        QTextCursor(block).setBlockFormat(format);
        const std::optional<QColor>& wanted = level ? headingColor_ : textColor_;
        if (wanted) {
            colorWords(block, *wanted);
        }
        return alone;
    }

    // Where the words leading into the screenshot in block `image` start. The paragraph right
    // before it, and the heading over that paragraph, stay on the screenshot's page; the
    // screenshot itself when neither is there. With only headings above them, they keep the
    // title company, so it never sits alone on the first page.
    int leadIn(int image) const {
        int first = image;
        for (const int number : {image - 1, image - 2}) {
            const QTextBlock block = doc_->findBlockByNumber(number);
            if (!block.isValid() || words(block).isEmpty()) {
                break;
            }
            first = number;
            if (block.blockFormat().headingLevel()) {
                break;
            }
        }
        for (int number = 0; number < first; ++number) {
            if (!doc_->findBlockByNumber(number).blockFormat().headingLevel()) {
                return first;
            }
        }
        return 0;
    }

    // Loads and sizes the block's images; returns how many it shows.
    int sizeImages(const QTextBlock& block) {
        int shown = 0;
        // An indented screenshot (in a list or quote) keeps to its column; one in a table to
        // its share of the width.
        QSizeF room(page_.width() - left(block), page_.height());
        if (const QTextTable* table = QTextCursor(block).currentTable(); table != nullptr) {
            room.setWidth(room.width() / table->columns());
        }
        QTextBlock::iterator iterator = block.begin();
        while (!iterator.atEnd()) {
            const QTextFragment fragment = iterator.fragment();
            ++iterator;
            const QTextCharFormat charFormat = fragment.charFormat();
            if (!charFormat.isImageFormat()) {
                continue;
            }
            QTextImageFormat imageFormat = charFormat.toImageFormat();
            const QString name = imageFormat.name();
            const QImage image(detail::qtPath(folder_ / path_from_utf8(detail::utf8Text(name))));
            QTextCursor cursor(doc_.get());
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            if (image.isNull()) {
                detail::logger.warning("Could not read {} for the walkthrough write-up",
                                       detail::utf8Text(name));
                cursor.removeSelectedText();
                continue;
            }
            doc_->addResource(QTextDocument::ImageResource, QUrl(name), QVariant(image));
            const QSizeF size = shownSize(image, room, unit_);
            imageFormat.setWidth(size.width());
            imageFormat.setHeight(size.height());
            cursor.setCharFormat(imageFormat);
            ++shown;
        }
        return shown;
    }

    QSizeF page_;
    double unit_;
    const std::filesystem::path& folder_;
    const std::optional<QColor>& textColor_;
    const std::optional<QColor>& headingColor_;
    std::unique_ptr<QTextDocument> doc_;
};

} // namespace

QString listMarker(const QTextList& list, const QTextBlock& block) {
    const QString shown = bullet(list.format().style());
    return shown.isEmpty() ? list.itemText(block) : shown;
}

QString words(const QTextBlock& block) {
    QString text = block.text();
    text.remove(QChar(kObjectReplacement));
    return text.trimmed();
}

std::optional<QColor> color(std::string_view name) {
    const QString stripped = detail::qtText(name).trimmed();
    const QColor parsed = QColor::fromString(stripped);
    if (!stripped.isEmpty() && !parsed.isValid()) {
        detail::logger.info("Ignored a write-up colour Qt can't read: {}", py::repr(name));
    }
    if (!parsed.isValid()) {
        return std::nullopt;
    }
    return parsed;
}

void colorWords(const QTextBlock& block, const QColor& color) {
    std::vector<std::pair<int, int>> spans;
    for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
        const QTextFragment fragment = iterator.fragment();
        const QTextCharFormat format = fragment.charFormat();
        if (!(format.isImageFormat() || format.hasProperty(QTextFormat::ForegroundBrush))) {
            spans.emplace_back(fragment.position(), fragment.position() + fragment.length());
        }
    }
    // Merging reshapes the fragments, so only once they have all been read.
    QTextCharFormat colored;
    colored.setForeground(color);
    QTextCursor cursor(block);
    for (const auto& [start, end] : spans) {
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(colored);
    }
}

MarkdownComposer::MarkdownComposer(QSizeF page, int dpi, std::filesystem::path folder,
                                   walkthrough::Look look)
    : page_(page), unit_(dpi / 72.0), folder_(std::move(folder)),
      textColor_(color(look.text_color)), font_(detail::qtText(look.font).trimmed()) {
    headingColor_ = color(look.heading_color);
    if (!headingColor_) {
        headingColor_ = textColor_;
    }
}

ComposedDocument MarkdownComposer::compose(std::string_view text) const {
    ComposedDocument composed = layout(text);
    composed.frames = paginate(*composed.doc, composed.groups);
    return composed;
}

ComposedDocument MarkdownComposer::layout(std::string_view text) const {
    return Build(page_, unit_, folder_, textColor_, headingColor_, font_).layout(text);
}

ComposedDocument composeMarkdown(std::string_view text, const std::filesystem::path& folder,
                                 const walkthrough::Look& look, const QPageLayout& layout) {
    const int dpi = layoutDpi();
    return MarkdownComposer(textArea(layout, dpi), dpi, folder, look).compose(text);
}

} // namespace fv::documents
