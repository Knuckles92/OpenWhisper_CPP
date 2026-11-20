// from tests/test_walkthrough_write_up.py (the write-up as a PDF). The write-up's agent half is
// fv_walkthrough's.
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QSizeF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>

#include <algorithm>
#include <map>
#include <set>

#include "documents/markdown_composer.h"
#include "documents/painting.h"
#include "documents/pdf.h"
#include "documents_support.h"

namespace fv::documents::test {
namespace {

using walkthrough::Look;

std::vector<QTextBlock> blocks(const QTextDocument& doc) {
    std::vector<QTextBlock> all;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        all.push_back(block);
    }
    return all;
}

std::vector<QTextFragment> fragments(const QTextBlock& block) {
    std::vector<QTextFragment> all;
    for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
        all.push_back(iterator.fragment());
    }
    return all;
}

// The document's text as the painter's record has it, pages joined by new lines (_text).
std::string recordedText(const WrittenPdf& pdf) {
    std::string text;
    for (std::size_t index = 0; index < pdf.pages.size(); ++index) {
        text += (index ? "\n" : "") + recordText(pdf.pages[index], "\n");
    }
    return text;
}

// _text: the painter's record always, and PDFium's reading of the file when it is there.
std::vector<std::pair<std::string, std::string>> texts(const WrittenPdf& pdf) {
    std::vector<std::pair<std::string, std::string>> all{{"painted lines", recordedText(pdf)}};
    if (PdfiumReader* reader = PdfiumReader::get()) {
        std::string text;
        const auto pages = reader->pageTexts(pdf.path);
        for (std::size_t index = 0; index < pages.size(); ++index) {
            text += (index ? "\n" : "") + pages[index];
        }
        all.emplace_back("PDFium", text);
    }
    return all;
}

std::string purple() {
    return utf8(QColor(QStringLiteral("purple")).name());
}

TEST_CASE("the look colours the words but not links", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    const Look look{"purple", "#5b2a86", "no such font"};
    const ComposedDocument composed =
        MarkdownComposer(QSizeF(600, 800), 96, tmp.path(), look)
            .compose("# Title\n\nPlain [a link](https://example.com) words.\n\n1. A step");

    // As a dict built fragment by fragment: a later fragment of the same text wins.
    std::map<std::string, std::string> colors;
    for (const QTextBlock& block : blocks(*composed.doc)) {
        for (const QTextFragment& fragment : fragments(block)) {
            colors[utf8(fragment.text())] = utf8(fragment.charFormat().foreground().color().name());
        }
    }
    CHECK(colors.at("Title") == "#5b2a86");
    CHECK(colors.at("Plain ") == purple());
    CHECK(colors.at(" words.") == purple());
    CHECK(colors.at("a link") != purple());
    CHECK(colors.at("a link") != "#5b2a86");
    CHECK(std::any_of(colors.begin(), colors.end(), [](const auto& entry) {
        return entry.first.ends_with("A step") && entry.second == purple();
    }));
}

TEST_CASE("a colour qt cant read leaves the words as they were", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    const LogCapture log;
    const ComposedDocument composed =
        MarkdownComposer(QSizeF(600, 800), 96, tmp.path(), Look{"ultraviolet", "", ""})
            .compose("Words.");

    const auto all = fragments(composed.doc->begin());
    REQUIRE(all.size() == 1);
    CHECK_FALSE(all[0].charFormat().hasProperty(QTextFormat::ForegroundBrush));
    CHECK(contains(log.text(), "Ignored a write-up colour Qt can't read: 'ultraviolet'"));
}

TEST_CASE("purple text comes out purple in the pdf", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    std::string text = "# Purple\n\n";
    for (int n = 0; n < 40; ++n) {
        text += "Words in purple. ";
    }

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "t",
                                            std::nullopt, Look{"purple", "", ""});

    const QImage page = pdfiumOrSkip().render(pdf.path, 0);
    // The page isn't filled: only what was painted has any alpha.
    std::set<QRgb> inked;
    for (int y = 0; y < page.height(); y += 2) {
        for (int x = 0; x < page.width(); x += 2) {
            if (qAlpha(page.pixel(x, y))) {
                inked.insert(QColor(page.pixel(x, y)).rgb());
            }
        }
    }
    CHECK(inked == std::set<QRgb>{QColor(QStringLiteral("purple")).rgb()});
}

TEST_CASE("any markdown lays out with its screenshots framed", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    png(tmp / "shot-1.png", 1600, 900);
    const std::string text =
        "# Title\n\n1. First step\n2. Second step\n\n![A wide one](shot-1.png)\n\nAfter.";

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "Title");

    for (const auto& [reader, words] : texts(pdf)) {
        INFO(reader << ": " << words);
        for (const char* expected : {"Title", "First step", "Second step", "After."}) {
            CHECK(contains(words, expected));
        }
    }
    REQUIRE(pdf.pages.size() == 1);
    CHECK(pdf.pages[0].frames.size() == 1);
}

TEST_CASE("list numbers and bullets are in the pdf", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    const std::string text = "1. First step\n2. Second step\n\n- A point\n  - A smaller one";

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "t");

    for (const auto& [reader, words] : texts(pdf)) {
        INFO(reader << ": " << words);
        for (const char* expected :
             {"1. First step", "2. Second step", "• A point", "• A smaller one"}) {
            CHECK(contains(words, expected));
        }
    }
}

TEST_CASE("a screenshot in a list keeps to its column", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    png(tmp / "shot-1.png", 1600, 900);
    const QSizeF page(600, 800);
    const ComposedDocument composed =
        MarkdownComposer(page, 96, tmp.path()).compose("1. A step\n\n   ![wide](shot-1.png)\n");

    const QTextBlock block = composed.doc->findBlockByNumber(1);
    const double left = block.blockFormat().indent() * composed.doc->indentWidth();
    const QTextImageFormat image = block.begin().fragment().charFormat().toImageFormat();
    CHECK(left > 0);
    CHECK(image.width() <= page.width() - left);
}

TEST_CASE("a tables cells are placed below the title", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    const ComposedDocument composed =
        MarkdownComposer(QSizeF(600, 800), 96, tmp.path())
            .compose(
                "# Title\n\nWords.\n\n| Setting | Value |\n| --- | --- |\n| Agent | Codex |\n");

    const QTextDocument& doc = *composed.doc;
    const QTextBlock title = doc.begin();
    const double titleBottom = origin(doc, title).y() + title.layout()->boundingRect().height();
    std::vector<QTextBlock> cells;
    for (const QTextBlock& block : blocks(doc)) {
        if (QTextCursor(block).currentTable() != nullptr) {
            cells.push_back(block);
        }
    }
    REQUIRE_FALSE(cells.empty());
    for (const QTextBlock& cell : cells) {
        CHECK(origin(doc, cell).y() > titleBottom);
    }
}

TEST_CASE("a missing screenshot is left out and html is only text", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    const std::string text = "Before\n\n![gone](shot-9.png)\n\n<b>bold?</b>\n\nAfter";

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "t");

    for (const auto& [reader, words] : texts(pdf)) {
        INFO(reader << ": " << words);
        CHECK(contains(words, "Before"));
        CHECK(contains(words, "After"));
        CHECK(contains(words, "<b>bold?</b>"));
    }
    for (const PageRecord& page : pdf.pages) {
        CHECK(page.frames.empty());
    }
}

TEST_CASE("a code block is shaded although lines are painted one by one", "[pdf]") {
    // Painting line by line leaves out what Qt draws beside the lines; the marks put the code
    // shading back, across the text width.
    TempFolder tmp;
    const std::string text = "Words.\n\n```\ncode line\nanother\n```\n\nAfter.";

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "t", letter());

    REQUIRE(pdf.pages.size() == 1);
    CHECK(contains(flat(recordText(pdf.pages[0])), "code line another"));
    // Letter less its margins is 7 inches wide; the shading reaches the right edge.
    // Outside CHECK: a SKIP raised inside an assertion counts as its failure.
    const int shaded = widest(pdf.path, 0, "#f5f1eb");
    CHECK(shaded > 6 * 72);
}

TEST_CASE("a screenshot keeps its heading and words on its page", "[test_walkthrough_write_up]") {
    TempFolder tmp;
    png(tmp / "shot-1.png", 800, 1200);
    std::string filler;
    for (int n = 0; n < 9; ++n) {
        filler += (n ? "\n\n" : "") + ("Paragraph " + std::to_string(n) + " ");
        for (int word = 0; word < 40; ++word) {
            filler += "word ";
        }
    }
    const std::string text =
        "# Title\n\n" + filler + "\n\n## Lead\n\nThe words about it.\n\n![tall](shot-1.png)";

    const WrittenPdf pdf = writeMarkdownPdf(text, tmp / "write-up.pdf", tmp.path(), "t");

    std::vector<std::pair<std::string, std::vector<std::string>>> readers;
    std::vector<std::string> recorded;
    for (const PageRecord& page : pdf.pages) {
        recorded.push_back(recordText(page, "\n"));
    }
    readers.emplace_back("painted lines", recorded);
    if (PdfiumReader* reader = PdfiumReader::get()) {
        readers.emplace_back("PDFium", reader->pageTexts(pdf.path));
    }
    for (const auto& [reader, pages] : readers) {
        INFO(reader);
        const auto found = std::find_if(pages.begin(), pages.end(), [](const std::string& page) {
            return contains(page, "The words about it.");
        });
        REQUIRE(found != pages.end());
        const auto leadPage = static_cast<std::size_t>(found - pages.begin());
        CHECK(leadPage == pages.size() - 1);
        CHECK(leadPage > 0);
        CHECK(contains(pages[leadPage], "Lead"));
    }
    CHECK(pdf.pages.back().frames.size() == 1);
}

} // namespace
} // namespace fv::documents::test
