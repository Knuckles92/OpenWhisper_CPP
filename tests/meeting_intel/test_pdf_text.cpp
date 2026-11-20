// PDF text through PDFium loaded by path: the fixture PDFs with the owner's pdfium.dll
// (FIORIVOCE_TEST_PDFIUM, read-only; skipped when unset), and what happens without a library.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>

#include "intel_support.h"
#include "meeting_intel/pdf_text.h"
#include "testkit/testkit.h"

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

void use_test_pdfium() {
    const auto library = fv::testkit::get_env("FIORIVOCE_TEST_PDFIUM");
    if (!library || library->empty()) {
        SKIP("FIORIVOCE_TEST_PDFIUM is not set: PDFs are not read");
    }
    set_pdfium_path(fv::base_utf8::path_from_utf8(*library));
}

} // namespace

TEST_CASE("a PDF's text comes from PDFium", "[pdf_text]") {
    use_test_pdfium();
    const auto text = pdf_text(knowledge_fixtures() / "budget.pdf", 40);
    REQUIRE(text);
    CHECK(*text == "Budget review on Friday\nVenue costs were approved");
    // No page at all is read past the cap.
    CHECK(pdf_text(knowledge_fixtures() / "budget.pdf", 0) == std::optional<std::string>(""));
}

TEST_CASE("a PDF encrypted for its owner only opens with the empty password", "[pdf_text]") {
    use_test_pdfium();
    const auto text = pdf_text(knowledge_fixtures() / "owner-only.pdf", 40);
    REQUIRE(text);
    CHECK_THAT(*text, ContainsSubstring("Budget review on Friday"));
}

TEST_CASE("a PDF that needs a password reads as empty", "[pdf_text]") {
    use_test_pdfium();
    CHECK(pdf_text(knowledge_fixtures() / "locked.pdf", 40) == std::optional<std::string>(""));
}

TEST_CASE("a malformed PDF is an error", "[pdf_text]") {
    use_test_pdfium();
    CHECK_THROWS_AS(pdf_text(knowledge_fixtures() / "broken.pdf", 40), PdfError);
}

TEST_CASE("a PDF in a folder with a non-ASCII name is read", "[pdf_text]") {
    use_test_pdfium();
    TempFolder temp;
    const fs::path folder = temp.path() / fv::base_utf8::path_from_utf8("Fiori \xC3\xA8 voce");
    fs::create_directories(folder);
    fs::copy_file(knowledge_fixtures() / "budget.pdf", folder / "budget.pdf");
    const auto text = pdf_text(folder / "budget.pdf", 40);
    REQUIRE(text);
    CHECK_THAT(*text, ContainsSubstring("Venue costs"));
}

TEST_CASE("without PDFium a PDF has no text", "[pdf_text]") {
    set_pdfium_path(std::nullopt);
    CHECK_FALSE(pdfium_path());
    CHECK_FALSE(pdf_text(knowledge_fixtures() / "budget.pdf", 40));
    TempFolder temp;
    const fs::path fake = write_bytes(temp.path() / "pdfium.dll", "not a library");
    set_pdfium_path(fake);
    CHECK(pdfium_path() == std::optional<fs::path>(fake));
    // A library that cannot be loaded is skipped (and logged once), every time.
    CHECK_FALSE(pdf_text(knowledge_fixtures() / "budget.pdf", 40));
    CHECK_FALSE(pdf_text(knowledge_fixtures() / "budget.pdf", 40));
    set_pdfium_path(std::nullopt);
}

TEST_CASE("a missing PDF file is an error", "[pdf_text]") {
    TempFolder temp;
    CHECK_THROWS(pdf_text(temp.path() / "missing.pdf", 40));
}
