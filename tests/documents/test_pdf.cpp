// from tests/test_walkthrough_document.py (the PDF half). Page text and metadata are read back
// with PDFium when OPENWHISPER_CPP_TEST_PDFIUM names it; which line and frame went on which page is
// always checked through the painter's own PageRecord.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QLocale>
#include <QSizeF>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <tuple>

#include "base/json.h"
#include "documents/composer.h"
#include "documents/pdf.h"
#include "documents_support.h"
#include "walkthrough/renderer.h"

namespace fv::documents::test {
namespace {

using walkthrough::DocumentFormat;

std::string meta(std::string_view rest) {
    return "Tue, Oct 20, 2026, 2:32 PM · " + std::string(rest);
}

std::vector<std::string> letters(const std::vector<std::string>& pages) {
    std::vector<std::string> all;
    for (const std::string& page : pages) {
        std::string compact;
        for (char c : page) {
            if (c != ' ') {
                compact += c;
            }
        }
        all.push_back(compact);
    }
    return all;
}

std::string joined(const std::vector<std::string>& pages) {
    std::string text;
    for (std::size_t index = 0; index < pages.size(); ++index) {
        text += (index ? " " : "") + pages[index];
    }
    return text;
}

walkthrough::Walkthrough oneShot(const TempFolder& tmp) {
    const auto shot = png(tmp / "shot-1.png", 800, 450);
    return walkthroughOf(
        136.4, {section("The update card is grayed out.", shot, 41.3), section("That's it.")});
}

TEST_CASE("pdf holds the words and captions as text", "[test_walkthrough_document]") {
    TempFolder tmp;
    const WrittenPdf pdf = writePdf(oneShot(tmp), tmp.path(), {}, letter());

    CHECK(pdf.path == tmp / "walkthrough.pdf");
    CHECK(std::filesystem::is_regular_file(pdf.path));
    const std::vector<std::string> expected{"Walkthrough",
                                            meta("2:16 of speech · 1 screenshot"),
                                            about(),
                                            "The update card is grayed out.",
                                            "Screenshot 1 · 0:41",
                                            "That's it."};
    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        const std::string& page = pages[0];
        INFO(page);
        std::vector<std::size_t> positions;
        for (const std::string& text : expected) {
            positions.push_back(page.find(text));
            REQUIRE(positions.back() != std::string::npos);
        }
        CHECK(std::is_sorted(positions.begin(), positions.end()));
    }
}

TEST_CASE("three shots on letter and a4 keep each section on its page", "[pdf]") {
    // The fixture of the golden vectors' first two compose cases: the second screenshot is too
    // tall to stay under the first, so its section starts page 2, and the third starts page 3.
    TempFolder tmp;
    const auto walkthrough = walkthroughOf(
        136.4,
        {section("Okay, the update card is grayed out.", png(tmp / "a.png", 1600, 900), 41.3),
         section("Next, this label is stale.\nIt should say 2.8.", png(tmp / "b.png", 900, 1200),
                 72.6),
         section("And the footer overlaps.\n\nSecond paragraph about it.",
                 png(tmp / "c.png", 1280, 720), 101.0),
         section("That's it.")});
    const auto [paper, width, height] =
        GENERATE(std::tuple{QPageSize::Letter, 612, 792}, std::tuple{QPageSize::A4, 595, 842});

    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, QPageSize(paper));

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 3);
        CHECK(pages[0].starts_with("Walkthrough " + meta("2:16 of speech · 3 screenshots")));
        CHECK(pages[0].ends_with("Okay, the update card is grayed out. Screenshot 1 · 0:41"));
        CHECK(pages[1] == "Next, this label is stale. It should say 2.8. Screenshot 2 · 1:12");
        CHECK(pages[2] == "And the footer overlaps. Second paragraph about it. Screenshot 3 · "
                          "1:41 That's it.");
    }
    for (const PageRecord& page : pdf.pages) {
        CHECK(page.frames.size() == 1);
    }
    PdfiumReader& reader = pdfiumOrSkip();
    const auto info = reader.metadata(pdf.path);
    CHECK(info.at("Title") == "Walkthrough · Tue, Oct 20, 2026, 2:32 PM");
    CHECK(info.at("Creator") == std::string(identity().product));
    const auto [pageWidth, pageHeight] = reader.pageSize(pdf.path, 0);
    CHECK(std::lround(pageWidth) == width);
    CHECK(std::lround(pageHeight) == height);
}

TEST_CASE("pdf metadata names the walkthrough", "[test_walkthrough_document]") {
    TempFolder tmp;
    const WrittenPdf pdf = writePdf(walkthroughOf(5, {section("Hi.")}), tmp.path(), {}, letter());

    const auto info = pdfiumOrSkip().metadata(pdf.path);
    CHECK(info.at("Title") == "Walkthrough · Tue, Oct 20, 2026, 2:32 PM");
    CHECK(info.at("Creator") == std::string(identity().product));
}

TEST_CASE("a section that would split starts the next page", "[test_walkthrough_document]") {
    // Each portrait screenshot takes most of a page, so the second section cannot fit under the
    // first: its words move along with its caption.
    TempFolder tmp;
    const auto walkthrough =
        walkthroughOf(90, {section("First words.", png(tmp / "shot-1.png", 900, 1200), 10.0),
                           section("Second words.", png(tmp / "shot-2.png", 900, 1200), 50.0),
                           section("Last words.")});

    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 2);
        CHECK(contains(pages[0], "First words."));
        CHECK(contains(pages[0], "Screenshot 1 · 0:10"));
        CHECK_FALSE(contains(pages[0], "Second words."));
        CHECK_FALSE(contains(pages[0], "Screenshot 2"));
        CHECK(contains(pages[1], "Second words."));
        CHECK(contains(pages[1], "Screenshot 2 · 0:50"));
        CHECK(contains(pages[1], "Last words."));
    }
    CHECK(pdf.pages[0].frames.size() == 1);
    CHECK(pdf.pages[1].frames.size() == 1);
}

TEST_CASE("a screenshot without words moves whole", "[test_walkthrough_document]") {
    // Too tall for the room left under the first section, the second screenshot starts the next
    // page as wide as the text, not shrunk.
    TempFolder tmp;
    const auto walkthrough =
        walkthroughOf(90, {section("First words.", png(tmp / "shot-1.png", 1344, 900), 10.0),
                           section("", png(tmp / "shot-2.png", 1600, 900, "#30a060"), 20.0)});

    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 2);
        CHECK(contains(pages[1], "Screenshot 2 · 0:20"));
    }
    // Letter less 0.75 inch margins leaves 7 inches of text width: 672 layout units at 96 dpi.
    REQUIRE(pdf.pages[1].frames.size() == 1);
    CHECK(pdf.pages[1].frames[0].width() == Catch::Approx(7 * 96).margin(kSlack));
    if (PdfiumReader::get() != nullptr) {
        CHECK(widest(pdf.path, 1, "#30a060") == Catch::Approx(7 * 72).margin(3));
    }
}

TEST_CASE("a screenshot without words is never shrunk", "[test_walkthrough_document]") {
    // Wherever the first section ends, the screenshot after it starts its own section: it moves
    // on whole, or stays whole, but never shrinks.
    TempFolder tmp;
    const auto lone = png(tmp / "lone.png", 1600, 900);
    for (int height = 300; height < 460; height += 6) {
        const auto first =
            png(tmp / ("first-" + std::to_string(height) + ".png"), 1344, 2 * height);
        const Composer composer(QSizeF(672, 921), 96);

        const ComposedDocument composed = composer.compose(
            walkthroughOf(9, {section("First words.", first, 1.0), section("", lone, 2.0)}));

        INFO(height);
        REQUIRE(composed.frames.size() == 2);
        CHECK(composed.frames[1].size() == QSizeF(672, 378));
    }
}

TEST_CASE("the first section stays on the first page with the title",
          "[test_walkthrough_document]") {
    // Its words and a portrait screenshot overflow the page under the header. Moving it would
    // leave the title alone, so the screenshot shrinks to fit.
    TempFolder tmp;
    std::string words;
    for (int n = 0; n < 20; ++n) {
        words += "Some context before the first screenshot. ";
    }
    const auto walkthrough = walkthroughOf(
        90, {section(words, png(tmp / "shot-1.png", 900, 1200), 61.0), section("Done.")});

    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        CHECK(contains(pages[0], "Walkthrough"));
        CHECK(count(pages[0], "Some context before the first screenshot.") == 20);
        CHECK(contains(pages[0], "Screenshot 1 · 1:01"));
    }
    CHECK(pdf.pages[0].frames.size() == 1);
}

TEST_CASE("a section longer than a page flows on", "[test_walkthrough_document]") {
    TempFolder tmp;
    std::string words;
    for (int n = 0; n < 150; ++n) {
        words += "An explanation that keeps going. ";
    }
    const auto walkthrough = walkthroughOf(
        600, {section("Short start.", png(tmp / "shot-1.png", 900, 500), 5.0),
              section(words, png(tmp / "shot-2.png", 900, 1200), 300.0), section("Done.")});

    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        CHECK(pages.size() >= 2);
        CHECK(count(joined(pages), "An explanation that keeps going.") == 150);
        // PDFium reads a page with only a line or two on it as vertical text, one letter per
        // line; only the letters' order is left to compare there.
        const std::string all = joined(letters(pages));
        const auto caption = all.find("Screenshot2·5:00");
        REQUIRE(caption != std::string::npos);
        CHECK(caption < all.find("Done."));
    }
}

TEST_CASE("write pdf works on a worker thread", "[test_walkthrough_document]") {
    TempFolder tmp;
    const auto shot = png(tmp / "shot-1.png", 640, 360);
    const auto walkthrough = walkthroughOf(20, {section("Written by a worker.", shot, 5.0)});

    std::packaged_task<WrittenPdf()> task(
        [&] { return writePdf(walkthrough, tmp.path(), {}, letter()); });
    std::future<WrittenPdf> done = task.get_future();
    std::thread worker(std::move(task));
    const bool finished = done.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
    worker.join();

    REQUIRE(finished);
    const WrittenPdf pdf = done.get();
    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(contains(pages[0], "Written by a worker."));
        CHECK(contains(pages[0], "Screenshot 1 · 0:05"));
    }
}

TEST_CASE("a walkthrough without screenshots is text only", "[test_walkthrough_document]") {
    TempFolder tmp;
    const WrittenPdf pdf = writePdf(walkthroughOf(52, {section("First paragraph.\n\nSecond one.")}),
                                    tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(contains(pages[0], "0:52 of speech · no screenshots"));
        CHECK(contains(pages[0], "First paragraph."));
        CHECK(contains(pages[0], "Second one."));
        CHECK_FALSE(contains(pages[0], "Each screenshot comes right after"));
    }
}

TEST_CASE("a walkthrough with nothing said still writes its header",
          "[test_walkthrough_document]") {
    TempFolder tmp;
    const auto walkthrough = walkthroughOf(3, {section(" ")});
    const std::string line = meta("0:03 of speech · no screenshots");

    const auto markdown = walkthrough::write_markdown(walkthrough, tmp.path());
    const auto manifest = walkthrough::write_manifest(walkthrough, tmp.path());
    const WrittenPdf pdf = writePdf(walkthrough, tmp.path(), {}, letter());

    std::ifstream markdownIn(markdown, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(markdownIn),
                           std::istreambuf_iterator<char>()};
    CHECK(text == "# Walkthrough\n\n_" + line + ". " + aboutShotless() + "_\n");
    std::ifstream manifestIn(manifest, std::ios::binary);
    const Json parsed = Json::parse(
        std::string{std::istreambuf_iterator<char>(manifestIn), std::istreambuf_iterator<char>()});
    CHECK(parsed.at("sections") == Json::array());
    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(pages[0] == "Walkthrough " + line + " " + aboutShotless());
    }
}

TEST_CASE("an unreadable screenshot keeps its words and caption", "[test_walkthrough_document]") {
    TempFolder tmp;
    const auto broken = tmp / "shot-1.png";
    {
        std::ofstream out(broken, std::ios::binary);
        out << "not a png";
    }
    const LogCapture log;

    const WrittenPdf pdf = writePdf(walkthroughOf(20, {section("These words stay.", broken, 7.0)}),
                                    tmp.path(), {}, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(contains(pages[0], "These words stay."));
        CHECK(contains(pages[0], "Screenshot 1 · 0:07"));
    }
    CHECK(pdf.pages[0].frames.empty());
    CHECK(contains(log.text(), "Could not read screenshot"));
}

TEST_CASE("an unwritable pdf raises", "[test_walkthrough_document]") {
    TempFolder tmp;
    const auto missing = tmp / "missing";

    try {
        writePdf(walkthroughOf(5, {section("Hi.")}), missing, {}, letter());
        FAIL("writePdf wrote into a folder that does not exist");
    } catch (const walkthrough::DocumentError& error) {
        CHECK(std::string(error.what()) ==
              "Could not write " + path_to_utf8(missing / "walkthrough.pdf"));
    }
}

TEST_CASE("a folder named beyond ascii holds its pdf and screenshots", "[pdf]") {
    // Paths cross into Qt as UTF-8, never in the system's code page.
    TempFolder tmp;
    const auto folder = tmp / "Durchgänge 走查";
    REQUIRE(std::filesystem::create_directory(folder));
    const auto shot = png(folder / path_from_utf8("écran-1.png"), 800, 450);

    const WrittenPdf pdf = writePdf(walkthroughOf(20, {section("Les mots restent.", shot, 7.0)}),
                                    folder, {}, letter());
    const WrittenPdf writeUp = writeMarkdownPdf(
        "Avant\n\n![une](écran-1.png)\n", folder / path_from_utf8("rédigé.pdf"), folder, "Rédigé");

    CHECK(pdf.path == folder / "walkthrough.pdf");
    CHECK(std::filesystem::is_regular_file(pdf.path));
    REQUIRE(pdf.pages.size() == 1);
    CHECK(pdf.pages[0].frames.size() == 1);
    CHECK(std::filesystem::is_regular_file(writeUp.path));
    REQUIRE(writeUp.pages.size() == 1);
    CHECK(writeUp.pages[0].frames.size() == 1);
    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(contains(pages[0], "Les mots restent. Screenshot 1 · 0:07"));
    }
    if (PdfiumReader* reader = PdfiumReader::get()) {
        CHECK(reader->metadata(writeUp.path).at("Title") == "Rédigé");
    }
}

TEST_CASE("paper follows the locale unless given", "[test_walkthrough_document]") {
    struct Row {
        QLocale::MeasurementSystem measurement;
        std::optional<QPageSize::PageSizeId> pageSize;
        int width;
        int height;
    };
    const Row row = GENERATE(Row{QLocale::ImperialUSSystem, std::nullopt, 612, 792},
                             Row{QLocale::MetricSystem, std::nullopt, 595, 842},
                             Row{QLocale::ImperialUKSystem, std::nullopt, 595, 842},
                             Row{QLocale::ImperialUSSystem, QPageSize::A4, 595, 842});
    // Python swaps QLocale for the call; here the locale's paper is given the way writePdf
    // takes it when none is chosen.
    const QPageSize paper = row.pageSize ? QPageSize(*row.pageSize) : pageSizeFor(row.measurement);
    const QSizeF points = pageLayout(paper).fullRectPoints().size();
    CHECK(std::lround(points.width()) == row.width);
    CHECK(std::lround(points.height()) == row.height);

    TempFolder tmp;
    const WrittenPdf pdf = writePdf(walkthroughOf(5, {section("Hi.")}), tmp.path(), {}, paper);

    const auto [width, height] = pdfiumOrSkip().pageSize(pdf.path, 0);
    CHECK(std::lround(width) == row.width);
    CHECK(std::lround(height) == row.height);
}

TEST_CASE("without a page size the pdf takes the system locale's paper", "[pdf]") {
    const bool us = QLocale::system().measurementSystem() == QLocale::ImperialUSSystem;
    CHECK(localPageSize().id() == (us ? QPageSize::Letter : QPageSize::A4));
    CHECK(pageLayout().pageSize().id() == localPageSize().id());

    TempFolder tmp;
    const WrittenPdf pdf = writePdf(walkthroughOf(5, {section("Hi.")}), tmp.path());

    const auto [width, height] = pdfiumOrSkip().pageSize(pdf.path, 0);
    CHECK(std::lround(width) == (us ? 612 : 595));
    CHECK(std::lround(height) == (us ? 792 : 842));
}

TEST_CASE("pdf without a header or captions holds only the words", "[test_walkthrough_document]") {
    TempFolder tmp;
    const DocumentFormat bare{"", false, false, false};

    const WrittenPdf pdf = writePdf(oneShot(tmp), tmp.path(), bare, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(pages[0] == "The update card is grayed out. That's it.");
    }
    // The file keeps a name for viewers' title bars.
    const std::string title = pdfiumOrSkip().metadata(pdf.path).at("Title");
    CHECK(title.starts_with("Walkthrough · "));
}

TEST_CASE("pdf uses the chosen title", "[test_walkthrough_document]") {
    TempFolder tmp;
    const DocumentFormat titled{"Checkout bugs", true, false, true};

    const WrittenPdf pdf = writePdf(oneShot(tmp), tmp.path(), titled, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        REQUIRE(pages.size() == 1);
        CHECK(pages[0].starts_with("Checkout bugs Tue, Oct 20, 2026"));
        CHECK_FALSE(contains(pages[0], aboutShotless()));
        CHECK(contains(pages[0], "Screenshot 1 · 0:41"));
    }
}

TEST_CASE("without captions a section still stays on one page", "[test_walkthrough_document]") {
    TempFolder tmp;
    std::string words;
    for (int n = 0; n < 12; ++n) {
        words += "Some context before the second screenshot. ";
    }
    const auto first = png(tmp / "shot-1.png", 900, 700, "#c03030");
    const auto second = png(tmp / "shot-2.png", 900, 700, "#30c030");
    const DocumentFormat noCaptions{std::string(walkthrough::TITLE), true, true, false};

    const WrittenPdf pdf =
        writePdf(walkthroughOf(40, {section("First.", first, 5.0), section(words, second, 30.0),
                                    section("Done.")}),
                 tmp.path(), noCaptions, letter());

    for (const auto& [reader, pages] : readings(pdf)) {
        INFO(reader);
        const auto found = std::find_if(pages.begin(), pages.end(), [](const std::string& page) {
            return contains(page, "second screenshot");
        });
        REQUIRE(found != pages.end());
        const auto secondPage = static_cast<std::size_t>(found - pages.begin());
        // The second screenshot, the lower of the two frames, is on that page.
        std::vector<QRectF> frames;
        for (const PageRecord& page : pdf.pages) {
            frames.insert(frames.end(), page.frames.begin(), page.frames.end());
        }
        REQUIRE(frames.size() == 2);
        const auto& onPage = pdf.pages[secondPage].frames;
        CHECK(std::any_of(onPage.begin(), onPage.end(),
                          [&](const QRectF& frame) { return frame == frames.back(); }));
        if (reader == "PDFium") {
            CHECK(widest(pdf.path, static_cast<int>(secondPage), "#30c030") > 0);
        }
    }
}

} // namespace
} // namespace fv::documents::test
