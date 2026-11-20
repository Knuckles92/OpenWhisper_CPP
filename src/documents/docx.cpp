#include "documents/docx.h"

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFontInfo>
#include <QImage>
#include <QPageLayout>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextTable>
#include <QUrl>
#include <QVariant>

#include <pugixml.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/utf8.h"
#include "documents/detail/text.h"
#include "documents/markdown_composer.h"
#include "documents/ooxml.h"
#include "documents/pdf.h"
#include "walkthrough/renderer.h"
#include "walkthrough/storage.h"

namespace fv::documents {

namespace {

constexpr int kEmuPerTwip = 635;
// Space above and below each paragraph in a table cell, in points.
constexpr double kCellSpacePt = 2.0;
constexpr char16_t kObjectReplacement = 0xFFFC; // U+FFFC OBJECT REPLACEMENT CHARACTER
constexpr char16_t kLineSeparator = 0x2028;     // U+2028 LINE SEPARATOR

// Drops the characters XML 1.0 cannot hold (C0 controls but tab, line feed and carriage
// return), which pugixml would write as character references Word refuses. lxml refuses them
// too, so python-docx raises ValueError on such text; this writes the rest of the words.
QString xmlChars(QString text) {
    text.removeIf(
        [](QChar c) { return c.unicode() < 0x20 && c != u'\t' && c != u'\n' && c != u'\r'; });
    return text;
}

// Python's round(): halves go to the even neighbour.
double pyRound(double value) {
    if (std::abs(value - std::trunc(value)) == 0.5) {
        return 2.0 * std::round(value / 2.0);
    }
    return std::round(value);
}

// python-docx's Pt(points): int(points * 12700), towards zero.
std::int64_t emu(double points) {
    return static_cast<std::int64_t>(points * kEmuPerPt);
}

// Length.twips: the EMU in twentieths of a point, rounded as Python rounds.
std::int64_t twips(std::int64_t emu) {
    return static_cast<std::int64_t>(pyRound(static_cast<double>(emu) / kEmuPerTwip));
}

// ST_HpsMeasure: the EMU in whole half-points, towards zero.
std::int64_t halfPoints(std::int64_t emu) {
    return static_cast<std::int64_t>(static_cast<double>(emu) / kEmuPerPt * 2);
}

// "1D1B19", as python-docx's RGBColor writes a colour.
std::string hex(const QColor& color) {
    return detail::utf8Text(QStringLiteral("%1%2%3")
                                .arg(color.red(), 2, 16, QChar(u'0'))
                                .arg(color.green(), 2, 16, QChar(u'0'))
                                .arg(color.blue(), 2, 16, QChar(u'0'))
                                .toUpper());
}

std::string hex(std::string_view color) {
    return hex(QColor(detail::qtText(color)));
}

// What str.isspace() is true for, so a run's text keeps its spaces as python-docx keeps them.
bool pySpace(QChar c) {
    const char16_t u = c.unicode();
    return c.isSpace() || (u >= 0x1C && u <= 0x1F);
}

// A border of `side`, `size` eighths of a point wide, `space` points off the words.
void border(pugi::xml_node parent, const char* side, int size, double space = 0.0) {
    pugi::xml_node edge = parent.append_child(side);
    edge.append_attribute("w:val").set_value("single");
    edge.append_attribute("w:sz").set_value(size);
    edge.append_attribute("w:space").set_value(static_cast<long long>(pyRound(space)));
    edge.append_attribute("w:color").set_value(hex(kFrameColor));
}

// Copies a QTextDocument's blocks, tables and images into a Word document body (_Writer).
class Writer {
public:
    // `unit` is the QTextDocument's layout units per point; `kept` the blocks that keep with
    // the next; `blockWidth` the room between the margins, in EMU.
    Writer(const QTextDocument& doc, double unit, std::set<int> kept, std::int64_t blockWidth)
        : doc_(doc), unit_(unit), kept_(std::move(kept)), blockWidth_(blockWidth),
          defaultFamily_(QFontInfo(doc.defaultFont()).family()),
          defaultSize_(doc.defaultFont().pointSizeF()),
          nextRelationship_(ooxml::firstFreeRelationship()) {}

    // Every block in order; a table's blocks as that table.
    void blocks(pugi::xml_node body) {
        QTextBlock block = doc_.begin();
        while (block.isValid()) {
            QTextTable* table = QTextCursor(block).currentTable();
            if (table == nullptr) {
                paragraph(block, body);
                block = block.next();
                continue;
            }
            writeTable(*table, body);
            block = table->lastCursorPosition().block().next();
        }
    }

    const std::vector<ooxml::Relationship>& relationships() const { return relationships_; }
    const std::vector<ooxml::Media>& media() const { return media_; }

private:
    // A table ruled as in the PDF: above, under each row, firmer under the header.
    void writeTable(const QTextTable& table, pugi::xml_node body) {
        const int rows = table.rows();
        const int columns = table.columns();
        // python-docx shares the width between the margins out evenly.
        const std::int64_t column = columns > 0 ? blockWidth_ / columns : 0;
        const long long columnTwips = twips(column);
        pugi::xml_node tbl = body.append_child("w:tbl");
        pugi::xml_node properties = tbl.append_child("w:tblPr");
        pugi::xml_node width = properties.append_child("w:tblW");
        width.append_attribute("w:type").set_value("auto");
        width.append_attribute("w:w").set_value(0);
        pugi::xml_node look = properties.append_child("w:tblLook");
        look.append_attribute("w:firstColumn").set_value("1");
        look.append_attribute("w:firstRow").set_value("1");
        look.append_attribute("w:lastColumn").set_value("0");
        look.append_attribute("w:lastRow").set_value("0");
        look.append_attribute("w:noHBand").set_value("0");
        look.append_attribute("w:noVBand").set_value("1");
        look.append_attribute("w:val").set_value("04A0");
        // After tblLook, where word.py appends it.
        pugi::xml_node borders = properties.append_child("w:tblBorders");
        for (const char* side : {"w:top", "w:bottom", "w:insideH"}) {
            border(borders, side, kHairline);
        }
        pugi::xml_node grid = tbl.append_child("w:tblGrid");
        for (int index = 0; index < columns; ++index) {
            grid.append_child("w:gridCol").append_attribute("w:w").set_value(columnTwips);
        }
        for (int row = 0; row < rows; ++row) {
            pugi::xml_node tr = tbl.append_child("w:tr");
            for (int index = 0; index < columns; ++index) {
                pugi::xml_node tc = tr.append_child("w:tc");
                pugi::xml_node cellProperties = tc.append_child("w:tcPr");
                pugi::xml_node cellWidth = cellProperties.append_child("w:tcW");
                cellWidth.append_attribute("w:type").set_value("dxa");
                cellWidth.append_attribute("w:w").set_value(columnTwips);
                if (row == 0) {
                    border(cellProperties.append_child("w:tcBorders"), "w:bottom", kHairline * 2);
                }
                const QTextTableCell cell = table.cellAt(row, index);
                QTextBlock block = cell.firstCursorPosition().block();
                const int last = cell.lastCursorPosition().block().blockNumber();
                int copied = 0;
                while (block.isValid() && block.blockNumber() <= last) {
                    paragraph(block, tc);
                    ++copied;
                    block = block.next();
                }
                // A new cell comes with an empty paragraph; the copied ones replace it.
                if (copied == 0) {
                    tc.append_child("w:p");
                }
                for (pugi::xml_node p = tc.child("w:p"); p; p = p.next_sibling("w:p")) {
                    cellSpacing(p);
                }
            }
        }
    }

    // A cell's paragraph gets kCellSpacePt above and below, whatever its block asked for.
    static void cellSpacing(pugi::xml_node p) {
        pugi::xml_node properties = p.child("w:pPr");
        if (!properties) {
            properties = p.prepend_child("w:pPr");
        }
        pugi::xml_node spacing = properties.child("w:spacing");
        if (!spacing) {
            spacing = properties.append_child("w:spacing");
        }
        const long long space = twips(emu(kCellSpacePt));
        for (const char* name : {"w:before", "w:after"}) {
            pugi::xml_attribute attribute = spacing.attribute(name);
            if (!attribute) {
                attribute = spacing.append_attribute(name);
            }
            attribute.set_value(space);
        }
    }

    void paragraph(const QTextBlock& block, pugi::xml_node container) {
        pugi::xml_node p = container.append_child("w:p");
        // w:pPr's children in the order the schema requires; Word refuses a file with them out
        // of order: keepNext, pBdr, shd, spacing, ind, jc, outlineLvl.
        pugi::xml_node properties = p.append_child("w:pPr");
        const QTextBlockFormat format = block.blockFormat();
        const int level = format.headingLevel();
        // A screenshot section's blocks but its last, and headings, keep with the next.
        pugi::xml_node keepNext = properties.append_child("w:keepNext");
        if (!(level || kept_.contains(block.blockNumber()))) {
            keepNext.append_attribute("w:val").set_value("0");
        }
        if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            border(properties.append_child("w:pBdr"), "w:bottom", kHairline);
        } else if (format.hasProperty(QTextFormat::BlockCodeFence)) {
            pugi::xml_node shading = properties.append_child("w:shd");
            shading.append_attribute("w:val").set_value("clear");
            shading.append_attribute("w:color").set_value("auto");
            shading.append_attribute("w:fill").set_value(hex(kCodeFill));
        } else if (format.hasProperty(QTextFormat::BlockQuoteLevel)) {
            border(properties.append_child("w:pBdr"), "w:left", kHairline, kQuoteGapPt);
        }
        pugi::xml_node spacing = properties.append_child("w:spacing");
        spacing.append_attribute("w:before").set_value(twips(emu(format.topMargin() / unit_)));
        spacing.append_attribute("w:after").set_value(twips(emu(format.bottomMargin() / unit_)));
        if (format.lineHeightType() == QTextBlockFormat::ProportionalHeight) {
            // python-docx: Emu(multiple * Twips(240)), 240 twips being single spacing.
            const double multiple = format.lineHeight() / 100.0;
            const auto line =
                static_cast<std::int64_t>(multiple * static_cast<double>(240 * kEmuPerTwip));
            spacing.append_attribute("w:line").set_value(twips(line));
            spacing.append_attribute("w:lineRule").set_value("auto");
        }
        const double left = format.leftMargin() + format.indent() * doc_.indentWidth();
        pugi::xml_node indent;
        if (left != 0.0) {
            indent = properties.append_child("w:ind");
            indent.append_attribute("w:left").set_value(twips(emu(left / unit_)));
        }
        if (format.textIndent() != 0.0) {
            // Negative hangs a list item's marker; Word tabs to the indent after it.
            if (!indent) {
                indent = properties.append_child("w:ind");
            }
            const std::int64_t first = emu(format.textIndent() / unit_);
            if (first < 0) {
                indent.append_attribute("w:hanging").set_value(twips(-first));
            } else {
                indent.append_attribute("w:firstLine").set_value(twips(first));
            }
        }
        if (const char* alignment = jc(format.alignment())) {
            properties.append_child("w:jc").append_attribute("w:val").set_value(alignment);
        }
        if (level) {
            // Word's navigation pane and table of contents list it.
            properties.append_child("w:outlineLvl")
                .append_attribute("w:val")
                .set_value(std::min(level, 9) - 1);
        }
        runs(block, p);
    }

    static const char* jc(Qt::Alignment alignment) {
        if (alignment & Qt::AlignHCenter) {
            return "center";
        }
        if (alignment & Qt::AlignJustify) {
            return "both";
        }
        if (alignment & Qt::AlignRight) {
            return "right";
        }
        return nullptr;
    }

    void runs(const QTextBlock& block, pugi::xml_node p) {
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (format.isImageFormat()) {
                picture(p, format.toImageFormat());
                continue;
            }
            QString text = xmlChars(fragment.text());
            text.remove(QChar(kObjectReplacement));
            text.replace(QChar(kLineSeparator), QChar(u'\n'));
            if (text.isEmpty()) {
                continue;
            }
            pugi::xml_node parent = p;
            if (const QString href = format.anchorHref(); !href.isEmpty()) {
                parent = p.append_child("w:hyperlink");
                parent.append_attribute("r:id").set_value(
                    hyperlink(detail::utf8Text(xmlChars(href))));
            }
            pugi::xml_node run = parent.append_child("w:r");
            runFormat(run.append_child("w:rPr"), format);
            appendText(run, text);
        }
    }

    // The run's look, in w:rPr's schema order: rFonts, b, i, strike, color, sz, u.
    void runFormat(pugi::xml_node properties, const QTextCharFormat& format) const {
        const QFont font = format.font();
        if (format.fontFixedPitch() || !format.fontFamilies().toStringList().isEmpty()) {
            if (const QString family = QFontInfo(font).family(); family != defaultFamily_) {
                pugi::xml_node fonts = properties.append_child("w:rFonts");
                const std::string name = detail::utf8Text(family);
                fonts.append_attribute("w:ascii").set_value(name);
                fonts.append_attribute("w:hAnsi").set_value(name);
            }
        }
        if (font.weight() >= 600) {
            properties.append_child("w:b");
        }
        if (font.italic()) {
            properties.append_child("w:i");
        }
        if (font.strikeOut()) {
            properties.append_child("w:strike");
        }
        if (format.hasProperty(QTextFormat::ForegroundBrush)) {
            properties.append_child("w:color").append_attribute("w:val").set_value(
                hex(format.foreground().color()));
        }
        if (font.pointSizeF() > 0 && font.pointSizeF() != defaultSize_) {
            properties.append_child("w:sz").append_attribute("w:val").set_value(
                halfPoints(emu(font.pointSizeF())));
        }
        if (font.underline()) {
            properties.append_child("w:u").append_attribute("w:val").set_value("single");
        }
    }

    // A tab becomes w:tab and a line break w:br; the rest goes in w:t, which keeps its spaces
    // when it starts or ends with one.
    static void appendText(pugi::xml_node run, const QString& text) {
        QString pending;
        const auto flush = [&] {
            if (pending.isEmpty()) {
                return;
            }
            pugi::xml_node t = run.append_child("w:t");
            if (pySpace(pending.front()) || pySpace(pending.back())) {
                t.append_attribute("xml:space").set_value("preserve");
            }
            t.text().set(detail::utf8Text(pending));
            pending.clear();
        };
        for (const QChar c : text) {
            if (c == u'\t') {
                flush();
                run.append_child("w:tab");
            } else if (c == u'\n' || c == u'\r') {
                flush();
                run.append_child("w:br");
            } else {
                pending += c;
            }
        }
        flush();
    }

    // The image at the size the PDF shows it, framed in the same hairline.
    void picture(pugi::xml_node p, const QTextImageFormat& format) {
        const QImage image =
            doc_.resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>();
        if (image.isNull()) {
            return;
        }
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        // PNG: lossless, as in the PDF.
        image.save(&buffer, "PNG");
        buffer.close();
        const std::string id = imagePart(data);
        const long long width = emu(format.width() / unit_);
        const long long height = emu(format.height() / unit_);
        const int shape = nextShape_++;

        pugi::xml_node drawing =
            p.append_child("w:r").append_child("w:drawing").append_child("wp:inline");
        drawing.append_attribute("xmlns:a").set_value(ooxml::kDrawingNs);
        drawing.append_attribute("xmlns:pic").set_value(ooxml::kPictureNs);
        extent(drawing.append_child("wp:extent"), width, height);
        pugi::xml_node properties = drawing.append_child("wp:docPr");
        properties.append_attribute("id").set_value(shape);
        properties.append_attribute("name").set_value("Picture " + std::to_string(shape));
        if (const QString alt = xmlChars(format.property(QTextFormat::ImageAltText).toString());
            !alt.isEmpty()) {
            properties.append_attribute("descr").set_value(detail::utf8Text(alt));
        }
        drawing.append_child("wp:cNvGraphicFramePr")
            .append_child("a:graphicFrameLocks")
            .append_attribute("noChangeAspect")
            .set_value("1");
        pugi::xml_node graphic = drawing.append_child("a:graphic").append_child("a:graphicData");
        graphic.append_attribute("uri").set_value(ooxml::kPictureNs);
        pugi::xml_node pic = graphic.append_child("pic:pic");
        pugi::xml_node names = pic.append_child("pic:nvPicPr");
        pugi::xml_node name = names.append_child("pic:cNvPr");
        name.append_attribute("id").set_value("0");
        name.append_attribute("name").set_value("image.png");
        names.append_child("pic:cNvPicPr");
        pugi::xml_node fill = pic.append_child("pic:blipFill");
        fill.append_child("a:blip").append_attribute("r:embed").set_value(id);
        fill.append_child("a:stretch").append_child("a:fillRect");
        pugi::xml_node shapeProperties = pic.append_child("pic:spPr");
        pugi::xml_node transform = shapeProperties.append_child("a:xfrm");
        pugi::xml_node offset = transform.append_child("a:off");
        offset.append_attribute("x").set_value("0");
        offset.append_attribute("y").set_value("0");
        extent(transform.append_child("a:ext"), width, height);
        shapeProperties.append_child("a:prstGeom").append_attribute("prst").set_value("rect");
        pugi::xml_node line = shapeProperties.append_child("a:ln");
        line.append_attribute("w").set_value(
            static_cast<long long>(pyRound(kFrameWidthPt * kEmuPerPt)));
        line.append_child("a:solidFill")
            .append_child("a:srgbClr")
            .append_attribute("val")
            .set_value(hex(kFrameColor));
    }

    static void extent(pugi::xml_node node, long long width, long long height) {
        node.append_attribute("cx").set_value(width);
        node.append_attribute("cy").set_value(height);
    }

    std::string relate(std::string_view type, std::string target, bool external) {
        std::string id = "rId" + std::to_string(nextRelationship_++);
        relationships_.push_back({id, type, std::move(target), external});
        return id;
    }

    // One relationship per address, however often it is linked.
    std::string hyperlink(const std::string& url) {
        if (const auto found = links_.find(url); found != links_.end()) {
            return found->second;
        }
        return links_[url] = relate(ooxml::kHyperlinkRelationship, url, true);
    }

    // One part per distinct picture, as python-docx keeps one per SHA-1.
    std::string imagePart(const QByteArray& png) {
        const QByteArray digest = QCryptographicHash::hash(png, QCryptographicHash::Sha1);
        if (const auto found = images_.find(digest); found != images_.end()) {
            return found->second;
        }
        ooxml::Media part;
        part.name = "image" + std::to_string(media_.size() + 1) + ".png";
        const auto* bytes = reinterpret_cast<const std::byte*>(png.constData());
        part.bytes.assign(bytes, bytes + png.size());
        std::string id = relate(ooxml::kImageRelationship, "media/" + part.name, false);
        media_.push_back(std::move(part));
        return images_[digest] = id;
    }

    const QTextDocument& doc_;
    double unit_;
    std::set<int> kept_;
    std::int64_t blockWidth_;
    QString defaultFamily_;
    double defaultSize_;
    int nextRelationship_;
    int nextShape_ = 1;
    std::vector<ooxml::Relationship> relationships_;
    std::vector<ooxml::Media> media_;
    std::map<std::string, std::string> links_;
    std::map<QByteArray, std::string> images_;
};

// The template's section, on `layout`'s page and margins.
void section(pugi::xml_node body, const QPageLayout& layout) {
    const QSizeF page = layout.pageSize().size(QPageSize::Point);
    const QMarginsF margins = layout.margins(QPageLayout::Point);
    pugi::xml_node properties = body.append_child("w:sectPr");
    properties.append_attribute("w:rsidR").set_value("00FC693F");
    properties.append_attribute("w:rsidRPr").set_value("0006063C");
    properties.append_attribute("w:rsidSect").set_value("00034616");
    pugi::xml_node size = properties.append_child("w:pgSz");
    size.append_attribute("w:w").set_value(twips(emu(page.width())));
    size.append_attribute("w:h").set_value(twips(emu(page.height())));
    pugi::xml_node margin = properties.append_child("w:pgMar");
    margin.append_attribute("w:top").set_value(twips(emu(margins.top())));
    margin.append_attribute("w:right").set_value(twips(emu(margins.right())));
    margin.append_attribute("w:bottom").set_value(twips(emu(margins.bottom())));
    margin.append_attribute("w:left").set_value(twips(emu(margins.left())));
    margin.append_attribute("w:header").set_value("720");
    margin.append_attribute("w:footer").set_value("720");
    margin.append_attribute("w:gutter").set_value("0");
    properties.append_child("w:cols").append_attribute("w:space").set_value("720");
    properties.append_child("w:docGrid").append_attribute("w:linePitch").set_value("360");
}

std::filesystem::path write(const QTextDocument& doc, const std::vector<Group>& groups,
                            const std::filesystem::path& path, const QPageLayout& layout, int dpi,
                            std::string_view title) {
    // Each screenshot section on one page: all but its last block keep with the next.
    std::set<int> kept;
    for (const Group& group : groups) {
        for (int number = group.first; number < group.last; ++number) {
            kept.insert(number);
        }
    }
    const QSizeF page = layout.pageSize().size(QPageSize::Point);
    const QMarginsF margins = layout.margins(QPageLayout::Point);
    // The width python-docx reads back from the section, in whole twips.
    const std::int64_t blockWidth =
        (twips(emu(page.width())) - twips(emu(margins.left())) - twips(emu(margins.right()))) *
        kEmuPerTwip;

    pugi::xml_document document;
    document.load_buffer(ooxml::kDocumentOpen.data(), ooxml::kDocumentOpen.size());
    pugi::xml_node body = document.first_child().append_child("w:body");
    Writer writer(doc, dpi / 72.0, std::move(kept), blockWidth);
    writer.blocks(body);
    section(body, layout);

    const QFont font = doc.defaultFont();
    const std::string styles =
        ooxml::stylesXml(detail::utf8Text(QFontInfo(font).family()),
                         static_cast<int>(halfPoints(emu(font.pointSizeF()))));
    const std::string core = ooxml::coreXml(title, "Made by " + std::string(identity().product));
    // minizip-ng makes a missing folder for the file; python-docx fails there, as the PDF does.
    if (std::error_code error;
        !path.parent_path().empty() && !std::filesystem::is_directory(path.parent_path(), error)) {
        throw walkthrough::DocumentError("Could not write " + path_to_utf8(path) +
                                         ": No such file or directory");
    }
    try {
        ooxml::writePackage(path, ooxml::serialize(document), writer.relationships(), styles, core,
                            writer.media());
    } catch (const std::exception& error) {
        throw walkthrough::DocumentError("Could not write " + path_to_utf8(path) + ": " +
                                         error.what());
    }
    return path;
}

} // namespace

std::filesystem::path writeDocx(const walkthrough::Walkthrough& walkthrough,
                                const std::filesystem::path& folder,
                                const walkthrough::DocumentFormat& format,
                                const std::optional<QPageSize>& pageSize) {
    const QPageLayout layout = pageLayout(pageSize);
    const int dpi = layoutDpi();
    const std::string title =
        (format.title.empty() ? std::string(walkthrough::TITLE) : format.title) + " · " +
        walkthrough::when_text(walkthrough.started_at.get_local_time());
    const ComposedDocument composed =
        Composer(textArea(layout, dpi), dpi, format).layout(walkthrough);
    return write(*composed.doc, composed.groups, folder / path_from_utf8(walkthrough::DOCX_NAME),
                 layout, dpi, title);
}

std::filesystem::path writeMarkdownDocx(std::string_view text, const std::filesystem::path& path,
                                        const std::filesystem::path& folder, std::string_view title,
                                        const std::optional<QPageSize>& pageSize,
                                        const walkthrough::Look& look) {
    const QPageLayout layout = pageLayout(pageSize);
    const int dpi = layoutDpi();
    const ComposedDocument composed =
        MarkdownComposer(textArea(layout, dpi), dpi, folder, look).layout(text);
    return write(*composed.doc, composed.groups, path, layout, dpi, title);
}

} // namespace fv::documents
