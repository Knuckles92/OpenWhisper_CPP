// Ported from tests/test_markdown_render.py, plus the golden vectors of
// ui_qt.utils.markdown_render (MarkdownStyle).
#include <catch2/catch_test_macros.hpp>

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextTable>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "ui_theme/markdown_render.h"
#include "ui_theme/palette.h"

using fv::ui::MarkdownStyle;

namespace {

const QString kSample = QStringLiteral(R"(# Title

Intro with **bold**, `code`, and a [link](https://example.com).

## recording.mp3

- one
- two

> quoted

```
line one
line two
```

| a | b |
|---|---|
| 1 | 2 |
)");

QString hexOf(const std::string& token) {
    return fv::ui::tokenColor(token).name();
}

QTextBlock blockNamed(const QTextDocument& document, const QString& text) {
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (block.text() == text) {
            return block;
        }
    }
    throw std::runtime_error("no block with text " + text.toStdString());
}

std::vector<QTextFragment> fragments(const QTextBlock& block) {
    std::vector<QTextFragment> out;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        out.push_back(it.fragment());
    }
    return out;
}

// The block formats read below only borrow the document, so it is kept alive by the caller.
std::unique_ptr<QTextDocument> render(const QString& text,
                                      const MarkdownStyle& style = MarkdownStyle::PREVIEW) {
    auto document = std::make_unique<QTextDocument>();
    fv::ui::renderMarkdown(*document, text, style);
    return document;
}

MarkdownStyle styleArg(const fv::golden::Case& c) {
    const fv::Json& which = c.args().at(0);
    if (which == "preview") {
        return MarkdownStyle::PREVIEW;
    }
    if (which == "reader") {
        return MarkdownStyle::READER;
    }
    return MarkdownStyle{.bodyPt = which.at(0).get<double>(),
                         .lineHeight = 100,
                         .paragraphGap = which.at(1).get<int>(),
                         .indentPx = which.at(2).get<int>(),
                         .codeBackground = "#000000",
                         .quoteBackground = "#000000",
                         .headingScale = which.at(3).get<std::vector<double>>()};
}

} // namespace

TEST_CASE("MarkdownStyle heading_pt replays the golden vectors", "[golden][markdown_render]") {
    fv::golden::for_each("ui_qt.utils.markdown_render", "heading_pt",
                         [](const fv::golden::Case& c) {
                             CHECK(styleArg(c).headingPt(c.arg<int>(1)) == c.result<double>());
                         });
}

TEST_CASE("MarkdownStyle code_pt replays the golden vectors", "[golden][markdown_render]") {
    fv::golden::for_each("ui_qt.utils.markdown_render", "code_pt", [](const fv::golden::Case& c) {
        CHECK(styleArg(c).codePt() == c.result<double>());
    });
}

TEST_CASE("MarkdownStyle scaled replays the golden vectors", "[golden][markdown_render]") {
    fv::golden::for_each("ui_qt.utils.markdown_render", "scaled", [](const fv::golden::Case& c) {
        const MarkdownStyle style = styleArg(c).scaled(c.arg<double>(1));
        const fv::Json got =
            fv::Json::array({style.bodyPt, style.lineHeight, style.paragraphGap, style.indentPx,
                             style.codeBackground, style.quoteBackground, style.headingScale});
        CHECK(fv::golden::equal(c.raw().at("result"), got));
    });
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_headings_scale_from_the_body_size
TEST_CASE("headings scale from the body size", "[test_markdown_render]") {
    const auto document = render(kSample);
    const QTextBlock title = blockNamed(*document, QStringLiteral("Title"));
    const QTextBlock section = blockNamed(*document, QStringLiteral("recording.mp3"));
    CHECK(title.blockFormat().headingLevel() == 1);
    const double titlePt = fragments(title).at(0).charFormat().fontPointSize();
    const double sectionPt = fragments(section).at(0).charFormat().fontPointSize();
    CHECK(titlePt == MarkdownStyle::PREVIEW.headingPt(1));
    CHECK(titlePt > sectionPt);
    CHECK(sectionPt == MarkdownStyle::PREVIEW.headingPt(2));
    CHECK(sectionPt > MarkdownStyle::PREVIEW.bodyPt);
}

// from
// tests/test_markdown_render.py::TestRenderMarkdown::test_inline_code_gets_a_chip_and_links_the_accent
TEST_CASE("inline code gets a chip and links the accent", "[test_markdown_render]") {
    const auto document = render(kSample);
    const QTextBlock intro =
        blockNamed(*document, QStringLiteral("Intro with bold, code, and a link."));
    std::map<QString, QTextCharFormat> byText;
    for (const QTextFragment& fragment : fragments(intro)) {
        byText[fragment.text()] = fragment.charFormat();
    }
    const QTextCharFormat code = byText.at(QStringLiteral("code"));
    CHECK(code.background().color().name() == hexOf(MarkdownStyle::PREVIEW.codeBackground));
    CHECK(code.fontFixedPitch());
    CHECK(byText.at(QStringLiteral("bold")).fontWeight() >
          byText.at(QStringLiteral(", ")).fontWeight());
    const QTextCharFormat link = byText.at(QStringLiteral("link"));
    CHECK(link.isAnchor());
    CHECK(link.foreground().color().name() == hexOf("accent-soft"));
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_colours_follow_the_active_palette
TEST_CASE("colours follow the active palette", "[test_markdown_render]") {
    fv::ui::setCurrentPalette(fv::ui::lightPalette());
    const auto document = render(kSample);
    const QTextBlock title = blockNamed(*document, QStringLiteral("Title"));
    const QString heading = fragments(title).at(0).charFormat().foreground().color().name();
    const QString quote =
        blockNamed(*document, QStringLiteral("quoted")).blockFormat().background().color().name();
    fv::ui::setCurrentPalette(fv::ui::darkPalette());
    CHECK(heading == fv::ui::lightPalette().color("text-heading").name());
    CHECK(quote == fv::ui::lightPalette().color(MarkdownStyle::PREVIEW.quoteBackground).name());
    CHECK(heading != fv::ui::darkPalette().color("text-heading").name());
}

// from
// tests/test_markdown_render.py::TestRenderMarkdown::test_fenced_code_is_one_slab_with_the_gap_after_its_last_line
TEST_CASE("fenced code is one slab with the gap after its last line", "[test_markdown_render]") {
    const auto document = render(kSample);
    const QTextBlockFormat first = blockNamed(*document, QStringLiteral("line one")).blockFormat();
    const QTextBlockFormat last = blockNamed(*document, QStringLiteral("line two")).blockFormat();
    CHECK(first.background().color().name() == hexOf(MarkdownStyle::PREVIEW.codeBackground));
    CHECK(first.bottomMargin() == 0);
    CHECK(last.bottomMargin() == MarkdownStyle::PREVIEW.paragraphGap);
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_quotes_are_tinted_and_indented
TEST_CASE("quotes are tinted and indented", "[test_markdown_render]") {
    const auto document = render(kSample);
    const QTextBlockFormat quote = blockNamed(*document, QStringLiteral("quoted")).blockFormat();
    CHECK(quote.hasProperty(QTextFormat::BlockQuoteLevel));
    CHECK(quote.background().color().name() == hexOf(MarkdownStyle::PREVIEW.quoteBackground));
    CHECK(quote.leftMargin() == MarkdownStyle::PREVIEW.indentPx);
}

// from
// tests/test_markdown_render.py::TestRenderMarkdown::test_paragraph_spacing_comes_from_the_style_not_the_importer
TEST_CASE("paragraph spacing comes from the style not the importer", "[test_markdown_render]") {
    const auto document = render(QStringLiteral("first paragraph\n\nsecond paragraph"));
    const QTextBlockFormat first =
        blockNamed(*document, QStringLiteral("first paragraph")).blockFormat();
    const QTextBlockFormat second =
        blockNamed(*document, QStringLiteral("second paragraph")).blockFormat();
    CHECK(first.topMargin() == 0);
    CHECK(first.bottomMargin() == MarkdownStyle::PREVIEW.paragraphGap);
    CHECK(second.topMargin() == 0);
    CHECK(first.lineHeight() == MarkdownStyle::PREVIEW.lineHeight);
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_table_header_row_is_shaded
TEST_CASE("table header row is shaded", "[test_markdown_render]") {
    const auto document = render(kSample);
    const QTextBlock header = blockNamed(*document, QStringLiteral("a"));
    const QTextBlock cellBlock = blockNamed(*document, QStringLiteral("1"));
    CHECK(header.blockFormat().bottomMargin() == 0);
    QTextTable* table = QTextCursor(header).currentTable();
    REQUIRE(table != nullptr);
    CHECK(table->cellAt(0, 0).format().background().color().name() ==
          hexOf(MarkdownStyle::PREVIEW.codeBackground));
    CHECK(table->cellAt(1, 0).format().background().style() == Qt::NoBrush);
    CHECK(QTextCursor(cellBlock).currentTable() == table);
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_plain_prose_survives_untouched
TEST_CASE("plain prose survives untouched", "[test_markdown_render]") {
    const QString text = QStringLiteral("Paid $5 * 3 for snake_case_names and #1 priority.");
    CHECK(render(text)->toPlainText() == text);
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_empty_text_leaves_an_empty_document
TEST_CASE("empty text leaves an empty document", "[test_markdown_render]") {
    CHECK(render(QString())->isEmpty());
}

// from tests/test_markdown_render.py::TestRenderMarkdown::test_default_font_follows_the_style
TEST_CASE("default font follows the style", "[test_markdown_render]") {
    const auto document = render(QStringLiteral("hello"), MarkdownStyle::READER);
    CHECK(document->defaultFont().pointSizeF() == MarkdownStyle::READER.bodyPt);
    CHECK(document->indentWidth() == MarkdownStyle::READER.indentPx);
}

// from
// tests/test_markdown_render.py::TestMarkdownStyle::test_scaled_multiplies_lengths_and_keeps_colours
TEST_CASE("scaled multiplies lengths and keeps colours", "[test_markdown_render]") {
    const MarkdownStyle larger = MarkdownStyle::READER.scaled(1.5);
    CHECK(larger.bodyPt == MarkdownStyle::READER.bodyPt * 1.5);
    CHECK(larger.paragraphGap == 18);
    CHECK(larger.codeBackground == MarkdownStyle::READER.codeBackground);
    CHECK(larger.headingPt(1) > MarkdownStyle::READER.headingPt(1));
}

// from
// tests/test_markdown_render.py::TestMarkdownStyle::test_heading_levels_past_the_table_reuse_the_last_scale
TEST_CASE("heading levels past the table reuse the last scale", "[test_markdown_render]") {
    const MarkdownStyle style{.bodyPt = 10,
                              .lineHeight = 100,
                              .paragraphGap = 4,
                              .indentPx = 10,
                              .codeBackground = "#000000",
                              .quoteBackground = "#000000",
                              .headingScale = {2.0, 1.5}};
    CHECK(style.headingPt(1) == 20);
    CHECK(style.headingPt(2) == 15);
    CHECK(style.headingPt(6) == 15);
}
