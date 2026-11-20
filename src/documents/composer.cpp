#include "documents/composer.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QUrl>
#include <QVariant>

#include <string>
#include <utility>

#include "base/utf8.h"
#include "documents/detail/text.h"
#include "documents/pages.h"
#include "documents/painting.h"
#include "documents/pdf.h"

namespace fv::documents {

namespace {

constexpr char16_t kLineSeparator = u' ';

// One layout of one walkthrough: the document being filled and where its cursor is.
class Build {
public:
    Build(QSizeF page, double unit, const walkthrough::DocumentFormat& format)
        : page_(page), unit_(unit), format_(format), doc_(std::make_unique<QTextDocument>()) {
        doc_->setDocumentMargin(0);
        // Laid out at screen resolution but painted at the PDF's: hinted advances would space
        // the letters unevenly once scaled.
        doc_->setUseDesignMetrics(true);
        doc_->setDefaultFont(documentFont());
        doc_->setPageSize(page);
        cursor_ = QTextCursor(doc_.get());
    }

    ComposedDocument layout(const walkthrough::Walkthrough& walkthrough) {
        const std::vector<walkthrough::Part> parts = walkthrough::parts(walkthrough);
        const int shots = walkthrough::shot_count(parts);
        if (!format_.title.empty()) {
            headerEnd_ = paragraph(detail::qtText(format_.title), kTitleStyle);
        }
        const std::vector<std::string> header =
            walkthrough::header_lines(walkthrough, shots, format_);
        for (std::size_t index = 0; index < header.size(); ++index) {
            headerEnd_ = paragraph(detail::qtText(header[index]), index ? kAboutStyle : kMetaStyle);
        }
        for (const walkthrough::Part& part : parts) {
            section(part);
        }
        return {std::move(doc_), std::move(groups_), {}};
    }

private:
    void section(const walkthrough::Part& part) {
        // The section right under the header keeps the title company, so the title never sits
        // alone on the first page.
        const bool underHeader = headerEnd_ && cursor_.blockNumber() == *headerEnd_;
        const int first = underHeader || empty_ ? 0 : cursor_.blockNumber() + 1;
        const std::vector<std::string> paragraphs = walkthrough::paragraphs(part.words);
        for (std::size_t index = 0; index < paragraphs.size(); ++index) {
            // A single line break stays inside its paragraph.
            QString text = detail::qtText(paragraphs[index]);
            text.replace(QChar(u'\n'), QChar(kLineSeparator));
            paragraph(text, index ? kMoreWordsStyle : kWordsStyle);
        }
        if (!part.shot) {
            return;
        }
        const Style& style = part.words.empty() ? kLoneImageStyle : kImageStyle;
        const std::optional<int> shot = image(*part.shot, part.number, style);
        std::optional<int> last = shot;
        if (format_.captions) {
            last = paragraph(detail::qtText(walkthrough::shot_label(part, " · ")), kCaptionStyle);
        }
        if (last) {
            groups_.push_back({first, shot, *last});
        }
    }

    // Adds a paragraph; returns its block number.
    int paragraph(const QString& text, const Style& style) {
        const QTextCharFormat format = charFormat(style);
        newBlock(style, format);
        cursor_.insertText(text, format);
        return cursor_.blockNumber();
    }

    // Starts a block in `style`: the document's first, or a new one. A new document holds one
    // empty block, which the first block fills.
    void newBlock(const Style& style, const QTextCharFormat& format) {
        if (empty_) {
            empty_ = false;
            cursor_.setBlockFormat(blockFormat(style));
            cursor_.setBlockCharFormat(format);
            return;
        }
        cursor_.insertBlock(blockFormat(style), format);
    }

    // Adds a screenshot as its own paragraph; returns its block number, or nullopt when the
    // file cannot be read: its words and caption still go in.
    std::optional<int> image(const std::filesystem::path& path, int number, const Style& style) {
        const QImage picture(detail::qtPath(path));
        if (picture.isNull()) {
            detail::logger.warning("Could not read screenshot {} for the walkthrough PDF",
                                   path_to_utf8(path));
            return std::nullopt;
        }
        const QString name = QStringLiteral("shot-%1").arg(number);
        doc_->addResource(QTextDocument::ImageResource, QUrl(name), QVariant(picture));
        const QSizeF size = shownSize(picture, page_, unit_);
        QTextImageFormat format;
        format.setName(name);
        format.setWidth(size.width());
        format.setHeight(size.height());
        newBlock(style, charFormat(style));
        cursor_.insertImage(format);
        return cursor_.blockNumber();
    }

    static QTextCharFormat charFormat(const Style& style) {
        QTextCharFormat format;
        format.setFontPointSize(style.size);
        format.setFontWeight(style.bold ? QFont::Bold : QFont::Normal);
        format.setForeground(QColor(detail::qtText(style.color)));
        return format;
    }

    QTextBlockFormat blockFormat(const Style& style) const {
        QTextBlockFormat format;
        format.setTopMargin(style.spaceBefore * unit_);
        format.setLineHeight(style.lineHeight, QTextBlockFormat::ProportionalHeight);
        if (style.centered) {
            format.setAlignment(Qt::AlignHCenter);
        }
        return format;
    }

    QSizeF page_;
    double unit_;
    const walkthrough::DocumentFormat& format_;
    std::unique_ptr<QTextDocument> doc_;
    QTextCursor cursor_;
    std::vector<Group> groups_;
    // The header's last block; nullopt without a header.
    std::optional<int> headerEnd_;
    bool empty_ = true;
};

} // namespace

Composer::Composer(QSizeF page, int dpi, walkthrough::DocumentFormat format)
    : page_(page), unit_(dpi / 72.0), format_(std::move(format)) {}

ComposedDocument Composer::compose(const walkthrough::Walkthrough& walkthrough) const {
    ComposedDocument composed = layout(walkthrough);
    composed.frames = paginate(*composed.doc, composed.groups);
    return composed;
}

ComposedDocument Composer::layout(const walkthrough::Walkthrough& walkthrough) const {
    return Build(page_, unit_, format_).layout(walkthrough);
}

ComposedDocument compose(const walkthrough::Walkthrough& walkthrough,
                         const walkthrough::DocumentFormat& format, const QPageLayout& layout) {
    const int dpi = layoutDpi();
    return Composer(textArea(layout, dpi), dpi, format).compose(walkthrough);
}

} // namespace fv::documents
