// from tests/test_walkthrough_word.py: a walkthrough and its write-up as Word documents, read
// back with ZipReader and pugixml as the Python tests read them with python-docx.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QFontInfo>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "documents/docx.h"
#include "documents/markdown_composer.h"
#include "documents/painting.h"
#include "documents_support.h"
#include "docx_reader.h"
#include "walkthrough/renderer.h"
#include "walkthrough/session.h"

namespace fv::documents::test {
namespace {

std::filesystem::path shot(const TempFolder& tmp, std::string_view name) {
    return png(tmp / name, 400, 240);
}

std::vector<std::string> texts(const WordPackage& word) {
    std::vector<std::string> found;
    for (pugi::xml_node paragraph : word.paragraphs()) {
        if (std::string text = paragraphText(paragraph); !text.empty()) {
            found.push_back(std::move(text));
        }
    }
    return found;
}

pugi::xml_node directRun(pugi::xml_node paragraph, std::size_t index) {
    const auto runs = WordPackage::children(paragraph, "w:r");
    REQUIRE(runs.size() > index);
    return runs[index];
}

TEST_CASE("a walkthrough reads as its pdf does with each section kept on a page",
          "[test_walkthrough_word]") {
    TempFolder tmp;
    const auto walkthrough = walkthroughOf(
        80, {section("Open the menu.", shot(tmp, "shot-1.png"), 4.0),
             section("Pick the backend.\n\nThen save.", shot(tmp, "shot-2.png"), 72.0),
             section("That's all.")});

    const auto path = writeDocx(walkthrough, tmp.path(), {}, letter());

    CHECK(path == tmp / "walkthrough.docx");
    const WordPackage word(path);
    CHECK(texts(word) == std::vector<std::string>{
                             "Walkthrough",
                             "Tue, Oct 20, 2026, 2:32 PM · 1:20 of speech · 2 screenshots",
                             about(),
                             "Open the menu.",
                             "Screenshot 1 · 0:04",
                             "Pick the backend.",
                             "Then save.",
                             "Screenshot 2 · 1:12",
                             "That's all.",
                         });
    CHECK(word.pictures().size() == 2);
    // Letter, inside the PDF's margins (0.75 inch at the sides, 0.7 at the top and bottom), in
    // twentieths of a point; pictures no wider than the text.
    const pugi::xml_node size = word.section().child("w:pgSz");
    CHECK(attributeInt(size, "w:w") == 612 * 20);
    CHECK(attributeInt(size, "w:h") == 792 * 20);
    const pugi::xml_node margins = word.section().child("w:pgMar");
    CHECK(attributeInt(margins, "w:left") == 1080);
    CHECK(attributeInt(margins, "w:right") == 1080);
    CHECK(attributeInt(margins, "w:top") == 1008);
    CHECK(attributeInt(margins, "w:bottom") == 1008);
    const std::int64_t textWidth = (612 * 20 - 1080 - 1080) * 635;
    for (pugi::xml_node picture : word.pictures()) {
        CHECK(attributeInt(picture.child("wp:extent"), "cx") <= textWidth);
        // 400 pixels at 1/96 inch are 300 points.
        CHECK(attributeInt(picture.child("wp:extent"), "cx") == 300 * kEmuPerPt);
        CHECK(attributeInt(picture.child("wp:extent"), "cy") == 180 * kEmuPerPt);
        const pugi::xml_node line = picture.select_node(".//pic:spPr/a:ln").node();
        CHECK(std::string_view(line.attribute("w").value()) == "7620");
        CHECK(std::string_view(
                  line.child("a:solidFill").child("a:srgbClr").attribute("val").value()) ==
              "DCD5CB");
    }
    // A section's words, screenshot and caption hold together; the caption lets the next
    // section go to another page.
    const auto paragraphs = byText(word.paragraphs());
    CHECK(keepWithNext(paragraphs.at("Pick the backend.")) == true);
    CHECK(keepWithNext(paragraphs.at("Then save.")) == true);
    CHECK_FALSE(keepWithNext(paragraphs.at("Screenshot 2 · 1:12")).value_or(false));
    CHECK(word.coreText("dc:title") == "Walkthrough · Tue, Oct 20, 2026, 2:32 PM");
    CHECK(word.coreText("dc:creator").empty());
    CHECK(word.coreText("cp:lastModifiedBy").empty());
    CHECK(word.coreText("dc:description") == "Made by " + std::string(identity().product));
}

TEST_CASE("the documents format settings apply to word too", "[test_walkthrough_word]") {
    TempFolder tmp;
    const auto walkthrough = walkthroughOf(5, {section("Words.", shot(tmp, "shot-1.png"), 4.0)});
    const walkthrough::DocumentFormat noExtras{"Bug report", false, false, false};

    const WordPackage word(writeDocx(walkthrough, tmp.path(), noExtras));

    CHECK(texts(word) == std::vector<std::string>{"Bug report", "Words."});
    CHECK(word.pictures().size() == 1);
    CHECK(word.coreText("dc:title") == "Bug report · Tue, Oct 20, 2026, 2:32 PM");
}

TEST_CASE("a write up keeps its structure links and look", "[test_walkthrough_word]") {
    TempFolder tmp;
    shot(tmp, "shot-1.png");
    const std::string text = R"(# Changing the backend

Open **Settings** and follow [the guide](https://example.com/guide).

![The menu](shot-1.png)

## Steps

- Pick one
- Save it

> Restart after.

```
backend = "remote"
```

| Name | Value |
|---|---|
| speed | fast |

![Gone](missing.png)
)";

    const auto path =
        writeMarkdownDocx(text, tmp / "write-up.docx", tmp.path(), "Changing the backend",
                          std::nullopt, walkthrough::Look{"", "#5b2a86", ""});

    CHECK(path == tmp / "write-up.docx");
    const WordPackage word(path);
    const auto paragraphs = byText(word.paragraphs());

    const pugi::xml_node title = paragraphs.at("Changing the backend");
    const pugi::xml_node outline = properties(title).child("w:outlineLvl");
    REQUIRE(outline);
    CHECK(std::string_view(outline.attribute("w:val").value()) == "0");
    const pugi::xml_node titleRun = directRun(title, 0).child("w:rPr");
    CHECK(std::string_view(titleRun.child("w:color").attribute("w:val").value()) == "5B2A86");
    CHECK(onOff(titleRun.child("w:b")) == true);
    CHECK(
        std::string_view(
            properties(paragraphs.at("Steps")).child("w:outlineLvl").attribute("w:val").value()) ==
        "1");
    // The link is a real one, around its words.
    const pugi::xml_node body = paragraphs.at("Open Settings and follow the guide.");
    bool boldSettings = false;
    for (pugi::xml_node run : WordPackage::children(body, "w:r")) {
        boldSettings = boldSettings || (runText(run) == "Settings" &&
                                        onOff(run.child("w:rPr").child("w:b")).value_or(false));
    }
    CHECK(boldSettings);
    std::vector<std::string> addresses;
    for (pugi::xml_node link : WordPackage::children(body, "w:hyperlink")) {
        addresses.push_back(word.target(link.attribute("r:id").value()));
    }
    CHECK(addresses == std::vector<std::string>{"https://example.com/guide"});
    // List markers hang in the indent, as in the PDF.
    const pugi::xml_node item = paragraphs.at("•\tPick one");
    const pugi::xml_node indent = properties(item).child("w:ind");
    CHECK(attributeInt(indent, "w:hanging") > 0);
    CHECK(attributeInt(indent, "w:left") > 0);
    CHECK(properties(paragraphs.at("Restart after.")).child("w:pBdr"));
    const pugi::xml_node code = paragraphs.at("backend = \"remote\"");
    CHECK(std::string_view(properties(code).child("w:shd").attribute("w:fill").value()) ==
          "F5F1EB");
    REQUIRE(word.tables().size() == 1);
    std::vector<std::vector<std::string>> cells;
    for (pugi::xml_node row : WordPackage::children(word.tables()[0], "w:tr")) {
        auto& texts = cells.emplace_back();
        for (pugi::xml_node cell : WordPackage::children(row, "w:tc")) {
            texts.push_back(cellText(cell));
        }
    }
    CHECK(cells == std::vector<std::vector<std::string>>{{"Name", "Value"}, {"speed", "fast"}});
    // Only the screenshot that exists; the heading keeps with what it heads.
    CHECK(word.pictures().size() == 1);
    CHECK(std::string_view(word.pictures()[0].child("wp:docPr").attribute("descr").value()) ==
          "The menu");
    CHECK(keepWithNext(paragraphs.at("Steps")) == true);
    CHECK(word.coreText("dc:title") == "Changing the backend");
}

TEST_CASE("word documents are written beside the pdf and copied as files",
          "[test_walkthrough_word]") {
    TempFolder tmp;
    walkthrough::WalkthroughResult result;
    result.folder = tmp.path();
    result.pdf = tmp / "write-up.pdf";
    result.markdown = tmp / "write-up.md";
    result.clipboard_text = "text";
    result.duration_s = 1.0;

    using Copy = std::pair<std::vector<std::filesystem::path>, std::string>;
    CHECK(result.docx() == tmp / "write-up.docx");
    CHECK(result.clipboard("word") == Copy{{result.docx()}, ""});
    CHECK(result.clipboard("pdf") == Copy{{result.pdf}, ""});
    CHECK(result.clipboard("both") == Copy{{result.pdf}, "text"});
    CHECK(result.clipboard("agent") == Copy{{}, "text"});
}

TEST_CASE("the word document is sized in points like the pdf", "[test_walkthrough_word]") {
    TempFolder tmp;

    const WordPackage word(writeDocx(walkthroughOf(5, {section("Words.")}), tmp.path()));

    const pugi::xml_node normal = word.style("Normal").child("w:rPr");
    CHECK(std::string_view(normal.child("w:sz").attribute("w:val").value()) == "22");
    // The words' font, as the PDF draws them.
    const std::string family = utf8(QFontInfo(documentFont()).family());
    CHECK(normal.child("w:rFonts").attribute("w:ascii").value() == family);
    CHECK(normal.child("w:rFonts").attribute("w:hAnsi").value() == family);
    const pugi::xml_node spacing = word.style("Normal").child("w:pPr").child("w:spacing");
    CHECK(std::string_view(spacing.attribute("w:after").value()) == "0");
    CHECK(std::string_view(spacing.attribute("w:line").value()) == "240");
}

TEST_CASE("agent copy includes selected pdf and action instructions", "[test_walkthrough_word]") {
    const std::string filename = GENERATE("walkthrough.pdf", "write-up.pdf", "write-up-1.pdf");
    TempFolder tmp;
    const auto folder = tmp / "Walkthrough with spaces";
    walkthrough::WalkthroughResult result;
    result.folder = folder;
    result.pdf = folder / filename;
    result.markdown = folder / "document.md";
    result.clipboard_text = "Markdown body";
    result.duration_s = 1.0;

    const auto [files, prompt] = result.clipboard("agent_task");

    CHECK(files.empty());
    CHECK(contains(prompt, path_to_utf8(std::filesystem::weakly_canonical(result.pdf))));
    CHECK(contains(prompt, "screenshots"));
    CHECK(contains(prompt, "carry out the tasks and changes"));
    CHECK(contains(prompt, "verify the result"));
    CHECK_FALSE(contains(prompt, "Markdown body"));
}

// --- the package itself ----------------------------------------------------------------------

TEST_CASE("a word package is a zip without zip64 extra fields", "[docx]") {
    TempFolder tmp;
    const auto path =
        writeDocx(walkthroughOf(5, {section("Words.", shot(tmp, "shot-1.png"), 2.0)}), tmp.path());

    const std::vector<RawEntry> entries = rawEntries(path);

    std::vector<std::string> names;
    for (const RawEntry& entry : entries) {
        INFO(entry.name);
        names.push_back(entry.name);
        CHECK_FALSE(entry.centralZip64);
        CHECK_FALSE(entry.localZip64);
        CHECK((entry.madeBy & 0xFF) == 20);
    }
    CHECK(names == std::vector<std::string>{
                       "[Content_Types].xml", "_rels/.rels", "docProps/core.xml",
                       "docProps/app.xml", "word/document.xml", "word/_rels/document.xml.rels",
                       "word/styles.xml", "word/stylesWithEffects.xml", "word/settings.xml",
                       "word/webSettings.xml", "word/fontTable.xml", "word/theme/theme1.xml",
                       "customXml/item1.xml", "customXml/_rels/item1.xml.rels",
                       "customXml/itemProps1.xml", "word/numbering.xml", "word/media/image1.png",
                       "docProps/thumbnail.jpeg"});
    const WordPackage word(path);
    const QImage image =
        QImage::fromData(QByteArray::fromStdString(word.parts.at("word/media/image1.png")));
    CHECK(image.size() == QSize(400, 240));
}

TEST_CASE("the same picture twice is one part and links are related once", "[docx]") {
    TempFolder tmp;
    png(tmp / "a.png", 320, 200);
    png(tmp / "b.png", 320, 200);
    png(tmp / "c.png", 200, 320);
    const std::string text = "[one](https://example.com/x) and [two](https://example.com/x) and "
                             "[three](https://example.com/y)\n\n![A](a.png)\n\n![B](b.png)\n\n"
                             "![C](c.png)\n";

    const WordPackage word(writeMarkdownDocx(text, tmp / "w.docx", tmp.path(), "Links", letter()));

    std::vector<std::string> media;
    for (const std::string& name : word.entries) {
        if (name.starts_with("word/media/")) {
            media.push_back(name);
        }
    }
    CHECK(media == std::vector<std::string>{"word/media/image1.png", "word/media/image2.png"});
    std::vector<std::string> embeds;
    std::vector<std::string> shapes;
    for (pugi::xml_node picture : word.pictures()) {
        embeds.push_back(
            word.target(picture.select_node(".//a:blip").node().attribute("r:embed").value()));
        shapes.push_back(picture.child("wp:docPr").attribute("id").value());
    }
    CHECK(embeds ==
          std::vector<std::string>{"media/image1.png", "media/image1.png", "media/image2.png"});
    CHECK(shapes == std::vector<std::string>{"1", "2", "3"});
    std::vector<std::string> links;
    for (pugi::xml_node link : WordPackage::children(word.paragraphs()[0], "w:hyperlink")) {
        links.push_back(word.target(link.attribute("r:id").value()));
    }
    CHECK(links == std::vector<std::string>{"https://example.com/x", "https://example.com/x",
                                            "https://example.com/y"});
    std::size_t external = 0;
    for (pugi::xml_node node :
         WordPackage::children(word.relationships.child("Relationships"), "Relationship")) {
        external += std::string_view(node.attribute("TargetMode").value()) == "External";
    }
    CHECK(external == 2);
}

TEST_CASE("runs keep line breaks and tabs", "[docx]") {
    TempFolder tmp;
    // A single line break inside a section's paragraph is U+2028 in the composed document.
    const WordPackage word(writeDocx(walkthroughOf(5, {section("First line \nsecond\tline")}),
                                     tmp.path(),
                                     walkthrough::DocumentFormat{"", false, false, true}));

    const auto paragraphs = word.paragraphs();
    REQUIRE(paragraphs.size() == 1);
    const pugi::xml_node run = directRun(paragraphs[0], 0);
    std::vector<std::string> pieces;
    for (pugi::xml_node child : run.children()) {
        if (std::string_view(child.name()) == "w:rPr") {
            continue;
        }
        pieces.push_back(child.name() + std::string("|") + child.text().get() + "|" +
                         child.attribute("xml:space").value());
    }
    CHECK(pieces == std::vector<std::string>{"w:t|First line |preserve", "w:br||", "w:t|second|",
                                             "w:tab||", "w:t|line|"});
}

TEST_CASE("characters xml cannot hold are left out of the words", "[docx]") {
    TempFolder tmp;
    // C0 controls in a transcript; pugixml would write them as &#1; and the like, which no
    // XML 1.0 reader (Word included) accepts.
    const auto path = writeDocx(walkthroughOf(5, {section("Page\x01one\x02two\x07.")}), tmp.path(),
                                walkthrough::DocumentFormat{"", false, false, true});

    const WordPackage word(path);
    REQUIRE(word.paragraphs().size() == 1);
    CHECK(paragraphText(word.paragraphs()[0]) == "Pageonetwo.");
    CHECK(word.parts.at("word/document.xml").find("&#") == std::string::npos);
}

TEST_CASE("an unwritable word document raises a document error", "[docx]") {
    TempFolder tmp;
    const auto path = tmp / "missing" / "w.docx";

    CHECK_THROWS_AS(writeMarkdownDocx("Words.", path, tmp.path(), "t"), walkthrough::DocumentError);
    try {
        writeMarkdownDocx("Words.", path, tmp.path(), "t");
    } catch (const walkthrough::DocumentError& error) {
        CHECK(std::string_view(error.what())
                  .starts_with("Could not write " + path_to_utf8(path) + ": "));
    }
    CHECK_FALSE(std::filesystem::exists(path));
}

} // namespace
} // namespace fv::documents::test
