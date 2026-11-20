#include "ui_theme/markdown_render.h"

#include <QColor>
#include <QFont>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <vector>

#include "pyport/number.h"
#include "ui_theme/palette.h"

namespace fv::ui {
namespace {

// Palette tokens, read when a document is styled so a re-render after a theme change picks up
// the new colours.
constexpr char kHeading[] = "text-heading";
constexpr char kQuoteText[] = "text-soft";
constexpr char kCodeText[] = "text";
constexpr char kLink[] = "accent-soft";
constexpr char kBorder[] = "border";

QStringList monoFamilies() {
    return {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("SF Mono"),
            QStringLiteral("Menlo"), QStringLiteral("monospace")};
}

// A palette token, or a literal colour for a one-off style.
QColor colorOf(const std::string& value) {
    if (value.starts_with('#') || value.starts_with("rgb")) {
        return QColor::fromString(QString::fromStdString(value));
    }
    return tokenColor(value);
}

bool isCodeBlock(const QTextBlockFormat& format) {
    return format.hasProperty(QTextFormat::BlockCodeFence) || format.nonBreakableLines();
}

// What the importer made of a block, as far as styling cares.
struct BlockKind {
    int headingLevel;
    bool isCode;
    bool isQuote;

    static BlockKind of(const QTextBlockFormat& format) {
        return {format.headingLevel(), isCodeBlock(format),
                format.hasProperty(QTextFormat::BlockQuoteLevel)};
    }
};

void mergeOverRange(QTextCursor& cursor, int position, int length, const QTextCharFormat& format) {
    cursor.setPosition(position);
    cursor.setPosition(position + length, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(format);
}

// Not BlockUnderCursor: that selection reaches back over the separator into the previous block
// and restyles its last character too.
void mergeOverBlock(QTextCursor& cursor, const QTextBlock& block, const QTextCharFormat& format) {
    mergeOverRange(cursor, block.position(), block.length() - 1, format);
}

QTextCharFormat codeFormat(const MarkdownStyle& style, const std::string* background) {
    QTextCharFormat format;
    format.setFontFamilies(monoFamilies());
    format.setFontFixedPitch(true);
    format.setFontPointSize(style.codePt());
    format.setForeground(tokenColor(kCodeText));
    if (background != nullptr && !background->empty()) {
        format.setBackground(colorOf(*background));
    }
    return format;
}

// The line height, margins and background the block's kind calls for.
QTextBlockFormat blockSpacing(const QTextBlock& block, const BlockKind& kind,
                              const MarkdownStyle& style, bool inTable) {
    const int level = kind.headingLevel;
    const int gap = style.paragraphGap;
    QTextBlockFormat merged;
    merged.setLineHeight(style.lineHeight, QTextBlockFormat::ProportionalHeight);
    if (inTable) {
        merged.setTopMargin(0);
        merged.setBottomMargin(0);
    } else if (level != 0) {
        merged.setTopMargin(gap * (level <= 2 ? 1.5 : 1.0));
        merged.setBottomMargin(gap * 0.6);
    } else if (kind.isCode) {
        // Each fenced line is its own block; only the run's last line gets the gap, so the
        // lines read as one slab.
        merged.setLineHeight(125, QTextBlockFormat::ProportionalHeight);
        merged.setBackground(colorOf(style.codeBackground));
        merged.setTopMargin(0);
        const QTextBlock next = block.next();
        const bool endsRun = !(next.isValid() && isCodeBlock(next.blockFormat()));
        merged.setBottomMargin(endsRun ? gap : 0);
    } else if (kind.isQuote) {
        merged.setBackground(colorOf(style.quoteBackground));
        merged.setLeftMargin(style.indentPx);
        merged.setTopMargin(0);
        merged.setBottomMargin(gap);
    } else if (block.textList() != nullptr) {
        merged.setTopMargin(0);
        merged.setBottomMargin(gap * 0.5);
    } else {
        merged.setTopMargin(0);
        merged.setBottomMargin(gap);
    }
    if (block.position() == 0) {
        merged.setTopMargin(0);
    }
    return merged;
}

// Inline code and links inside an ordinary block.
void styleInlineRuns(QTextCursor& cursor, const QTextBlock& block, const MarkdownStyle& style) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment piece = it.fragment();
        const QTextCharFormat pieceFormat = piece.charFormat();
        if (pieceFormat.fontFixedPitch()) {
            mergeOverRange(cursor, piece.position(), piece.length(),
                           codeFormat(style, &style.codeBackground));
        } else if (pieceFormat.isAnchor()) {
            QTextCharFormat link;
            link.setForeground(tokenColor(kLink));
            link.setFontUnderline(false);
            mergeOverRange(cursor, piece.position(), piece.length(), link);
        }
    }
}

void styleBlock(QTextCursor& cursor, const QTextBlock& block, const MarkdownStyle& style,
                bool inTable) {
    const BlockKind kind = BlockKind::of(block.blockFormat());
    const QTextBlockFormat merged = blockSpacing(block, kind, style, inTable);
    cursor.setPosition(block.position());
    cursor.mergeBlockFormat(merged);

    if (block.length() <= 1) {
        return;
    }
    if (kind.headingLevel != 0) {
        QTextCharFormat heading;
        heading.setFontPointSize(style.headingPt(kind.headingLevel));
        heading.setFontWeight(QFont::DemiBold);
        heading.setForeground(tokenColor(kHeading));
        mergeOverBlock(cursor, block, heading);
        return;
    }
    if (kind.isCode) {
        mergeOverBlock(cursor, block, codeFormat(style, nullptr));
        return;
    }
    if (kind.isQuote) {
        QTextCharFormat quote;
        quote.setForeground(tokenColor(kQuoteText));
        mergeOverBlock(cursor, block, quote);
    }
    styleInlineRuns(cursor, block, style);
}

void styleTable(QTextTable* table, const MarkdownStyle& style) {
    QTextTableFormat format = table->format();
    format.setBorder(1);
    format.setBorderBrush(tokenColor(kBorder));
    format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    format.setBorderCollapse(true);
    format.setCellPadding(std::max(4.0, style.paragraphGap * 0.75));
    format.setCellSpacing(0);
    format.setTopMargin(style.paragraphGap * 0.5);
    format.setBottomMargin(style.paragraphGap);
    table->setFormat(format);

    const QColor headerBackground = colorOf(style.codeBackground);
    for (int column = 0; column < table->columns(); ++column) {
        QTextTableCell cell = table->cellAt(0, column);
        if (!cell.isValid()) {
            continue;
        }
        QTextCharFormat cellFormat = cell.format();
        cellFormat.setBackground(headerBackground);
        cell.setFormat(cellFormat);
    }
}

void styleDocument(QTextDocument& document, const MarkdownStyle& style) {
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    std::vector<QTextTable*> tables;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextTable* table = QTextCursor(block).currentTable();
        if (table != nullptr && std::find(tables.begin(), tables.end(), table) == tables.end()) {
            tables.push_back(table);
        }
        styleBlock(cursor, block, style, table != nullptr);
    }
    for (QTextTable* table : tables) {
        styleTable(table, style);
    }
    cursor.endEditBlock();
}

} // namespace

const MarkdownStyle MarkdownStyle::PREVIEW{.bodyPt = 13,
                                           .lineHeight = 135,
                                           .paragraphGap = 8,
                                           .indentPx = 22,
                                           .codeBackground = "code-on-surface",
                                           .quoteBackground = "quote-on-surface"};

const MarkdownStyle MarkdownStyle::READER{.bodyPt = 15,
                                          .lineHeight = 155,
                                          .paragraphGap = 12,
                                          .indentPx = 26,
                                          .codeBackground = "code-on-bg",
                                          .quoteBackground = "quote-on-bg"};

double MarkdownStyle::headingPt(int level) const {
    const int count = static_cast<int>(headingScale.size());
    const int index = std::max(1, std::min(level, count)) - 1;
    return py::round_ndigits(bodyPt * headingScale.at(static_cast<std::size_t>(index)), 1);
}

double MarkdownStyle::codePt() const {
    return py::round_ndigits(bodyPt * 0.9, 1);
}

MarkdownStyle MarkdownStyle::scaled(double factor) const {
    MarkdownStyle copy = *this;
    copy.bodyPt = py::round_ndigits(bodyPt * factor, 1);
    copy.paragraphGap = std::max(2, static_cast<int>(py::round_half_even(paragraphGap * factor)));
    copy.indentPx = std::max(12, static_cast<int>(py::round_half_even(indentPx * factor)));
    return copy;
}

void renderMarkdown(QTextDocument& document, const QString& text, const MarkdownStyle& style) {
    QFont font(style.family);
    font.setPointSizeF(style.bodyPt);
    document.setDefaultFont(font);
    document.setIndentWidth(style.indentPx);
    document.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    styleDocument(document, style);
}

} // namespace fv::ui
