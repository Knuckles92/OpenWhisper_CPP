// QtDocumentRenderer, the walkthrough::DocumentRenderer fv_walkthrough's library and write-up
// write their PDFs through.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include "documents/qt_renderer.h"
#include "documents_support.h"
#include "docx_reader.h"
#include "walkthrough/renderer.h"
#include "walkthrough/write_up_names.h"

namespace fv::documents::test {
namespace {

TEST_CASE("the installed qt renderer writes the walkthrough pdf into its folder", "[renderer]") {
    TempFolder tmp;
    const auto shot = png(tmp / "shot-1.png", 800, 450);
    const auto walkthrough =
        walkthroughOf(20, {section("Words for the renderer.", shot, 5.0), section("Done.")});
    walkthrough::set_document_renderer(std::make_shared<QtDocumentRenderer>());
    // Removed however the test ends, so no later test in this process finds it installed.
    struct Uninstall {
        ~Uninstall() { walkthrough::set_document_renderer(nullptr); }
    } uninstall;

    const auto path = walkthrough::document_renderer().write_pdf(walkthrough, tmp.path(), {},
                                                                 walkthrough::PaperSize::Letter);

    CHECK(path == tmp / "walkthrough.pdf");
    CHECK(std::filesystem::file_size(path) > 0);
    if (PdfiumReader* reader = PdfiumReader::get()) {
        const auto pages = reader->pageTexts(path);
        REQUIRE(pages.size() == 1);
        CHECK(contains(flat(pages[0]), "Words for the renderer. Screenshot 1 · 0:05 Done."));
        const auto [width, height] = reader->pageSize(path, 0);
        CHECK(std::lround(width) == 612);
        CHECK(std::lround(height) == 792);
    }
}

TEST_CASE("the qt renderer writes a write-up pdf at its target", "[renderer]") {
    TempFolder tmp;
    png(tmp / "shot-1.png", 640, 360);
    QtDocumentRenderer renderer;
    const auto target = tmp / std::string(walkthrough::write_up::PDF_NAME);

    renderer.write_markdown_pdf("# Bug report\n\nIt hangs.\n\n![one](shot-1.png)\n", target,
                                tmp.path(), "Bug report", {}, walkthrough::PaperSize::A4);

    CHECK(std::filesystem::file_size(target) > 0);
    if (PdfiumReader* reader = PdfiumReader::get()) {
        CHECK(reader->metadata(target).at("Title") == "Bug report");
        const auto [width, height] = reader->pageSize(target, 0);
        CHECK(std::lround(width) == 595);
        CHECK(std::lround(height) == 842);
    }
}

TEST_CASE("the qt renderer reports an unwritable pdf as a document error", "[renderer]") {
    TempFolder tmp;
    QtDocumentRenderer renderer;

    CHECK_THROWS_AS(renderer.write_markdown_pdf("Words.", tmp / "missing" / "write-up.pdf",
                                                tmp.path(), "t", {}, walkthrough::PaperSize::Auto),
                    walkthrough::DocumentError);
}

TEST_CASE("the paper setting names the page size", "[renderer]") {
    CHECK_FALSE(pageSizeFor(walkthrough::PaperSize::Auto).has_value());
    CHECK(pageSizeFor(walkthrough::PaperSize::Letter)->id() == QPageSize::Letter);
    CHECK(pageSizeFor(walkthrough::PaperSize::A4)->id() == QPageSize::A4);
}

TEST_CASE("the qt renderer writes word documents on the paper asked for", "[renderer]") {
    TempFolder tmp;
    QtDocumentRenderer renderer;

    const auto path = renderer.write_docx(walkthroughOf(5, {section("Hi.")}), tmp.path(), {},
                                          walkthrough::PaperSize::A4);
    const auto target = tmp / std::string(walkthrough::write_up::DOCX_NAME);
    renderer.write_markdown_docx("# Bug report\n\nIt hangs.\n", target, tmp.path(), "Bug report",
                                 {}, walkthrough::PaperSize::Letter);

    CHECK(path == tmp / "walkthrough.docx");
    CHECK(paragraphText(WordPackage(path).paragraphs().back()) == "Hi.");
    CHECK(attributeInt(WordPackage(path).section().child("w:pgSz"), "w:w") == 11900);
    const WordPackage writeUp(target);
    CHECK(paragraphText(writeUp.paragraphs()[0]) == "Bug report");
    CHECK(attributeInt(writeUp.section().child("w:pgSz"), "w:w") == 12240);
    CHECK(writeUp.coreText("dc:title") == "Bug report");
}

TEST_CASE("the qt renderer reports an unwritable word document as a document error", "[renderer]") {
    TempFolder tmp;
    QtDocumentRenderer renderer;

    CHECK_THROWS_AS(renderer.write_markdown_docx("Hi.", tmp / "missing" / "w.docx", tmp.path(), "t",
                                                 {}, walkthrough::PaperSize::Auto),
                    walkthrough::DocumentError);
    CHECK_THROWS_AS(renderer.write_docx(walkthroughOf(5, {section("Hi.")}), tmp / "missing", {},
                                        walkthrough::PaperSize::Auto),
                    walkthrough::DocumentError);
}

} // namespace
} // namespace fv::documents::test
