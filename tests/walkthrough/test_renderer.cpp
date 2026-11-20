#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>

#include "support.h"
#include "walkthrough/renderer.h"
#include "walkthrough/testing/fake_renderer.h"
#include "walkthrough/write_up_names.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using namespace std::chrono_literals;

namespace {

Walkthrough sample() {
    return {local_moment(std::chrono::locate_zone("America/Los_Angeles"),
                         std::chrono::local_days{std::chrono::year{2026} / 10 / 20} + 14h),
            12.0,
            {Section{"Words.", std::nullopt, std::nullopt, std::nullopt, {}}}};
}

} // namespace

TEST_CASE("no renderer is a logic error until one is installed", "[renderer]") {
    set_document_renderer(nullptr);
    CHECK_THROWS_AS(document_renderer(), std::logic_error);

    auto fake = std::make_shared<FakeRenderer>();
    set_document_renderer(fake);
    CHECK(&document_renderer() == fake.get());

    // A replaced renderer stays alive for whoever still holds it.
    DocumentRenderer& held = document_renderer();
    set_document_renderer(std::make_shared<FakeRenderer>());
    fake.reset();
    CHECK(&held != &document_renderer());
    CHECK_NOTHROW(dynamic_cast<FakeRenderer&>(held).calls());
    set_document_renderer(nullptr);
}

TEST_CASE("the fake renderer writes a marker per call and records it", "[renderer]") {
    const TempFolder tmp;
    FakeRenderer renderer;
    const DocumentFormat format{"Bug report", true, false, true};

    const auto pdf = renderer.write_pdf(sample(), tmp.path(), format, PaperSize::Letter);
    const auto docx = renderer.write_docx(sample(), tmp.path(), format, PaperSize::A4);
    renderer.write_markdown_pdf("# Title\n", tmp / "write-up.pdf", tmp.path(), "Title",
                                Look{"purple", "", "Georgia"}, PaperSize::Auto);

    CHECK(pdf == tmp / "walkthrough.pdf");
    CHECK(docx == tmp / "walkthrough.docx");
    CHECK(read_file(pdf) == "fake pdf\nBug report");
    CHECK(read_file(tmp / "write-up.pdf") == "fake markdown pdf\nTitle");
    const auto calls = renderer.calls();
    REQUIRE(calls.size() == 3);
    CHECK(calls[0].kind == FakeRenderer::Kind::Pdf);
    CHECK(calls[0].format == format);
    CHECK(calls[0].paper == PaperSize::Letter);
    CHECK(calls[0].sections == 1);
    CHECK(calls[1].paper == PaperSize::A4);
    CHECK(calls[2].markdown == "# Title\n");
    CHECK(calls[2].look == Look{"purple", "", "Georgia"});
}

TEST_CASE("the fake renderer can be told to fail", "[renderer]") {
    const TempFolder tmp;
    FakeRenderer renderer;
    renderer.fail(FakeRenderer::Kind::Docx, "Word could not be written");

    CHECK_NOTHROW(renderer.write_pdf(sample(), tmp.path(), {}, PaperSize::Auto));
    CHECK_THROWS_AS(renderer.write_docx(sample(), tmp.path(), {}, PaperSize::Auto), DocumentError);
    CHECK_FALSE(std::filesystem::exists(tmp / "walkthrough.docx"));

    renderer.fail(std::nullopt, "");
    CHECK_THROWS_AS(
        renderer.write_markdown_docx("x", tmp / "w.docx", tmp.path(), "", {}, PaperSize::Auto),
        DocumentError);
    renderer.stop_failing();
    CHECK_NOTHROW(
        renderer.write_markdown_docx("x", tmp / "w.docx", tmp.path(), "", {}, PaperSize::Auto));
    CHECK(renderer.calls().size() == 4);
}

TEST_CASE("the write-up's names and default words", "[renderer]") {
    CHECK(write_up::MARKDOWN_NAME == "write-up.md");
    CHECK(write_up::PDF_NAME == "write-up.pdf");
    CHECK(write_up::DOCX_NAME == "write-up.docx");
    CHECK(write_up::WRITE_UP_AGENT_SETTING == "Settings → Dictation → Walkthroughs");
    CHECK(write_up::DEFAULT_INSTRUCTIONS.starts_with(
        "Here are some screenshots and a transcript of me talking through them. "));
    CHECK(write_up::DEFAULT_INSTRUCTIONS.ends_with("put each screenshot where it helps most."));
}
