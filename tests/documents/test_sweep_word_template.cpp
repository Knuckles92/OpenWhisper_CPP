// Every Word package carries python-docx's whole default template, as OpenWhisper's do
// (services/walkthrough/word.py writes from docx.Document()). The fixtures under
// tests/fixtures/documents are word/styles.xml and word/numbering.xml of a package python-docx
// 1.2.0 saved after word.py's Normal settings, with Calibri at 11 pt.
#include <catch2/catch_test_macros.hpp>

#include <pugixml.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "documents/docx.h"
#include "documents/ooxml.h"
#include "documents_support.h"
#include "docx_reader.h"
#include "testkit/testkit.h"

namespace fv::documents::test {
namespace {

std::string fixture(const char* name) {
    std::ifstream in(testkit::source_root() / "tests" / "fixtures" / "documents" / name,
                     std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path shot(const TempFolder& tmp, std::string_view name) {
    return png(tmp / name, 400, 240);
}

// Both texts as XML trees: element names, attributes and texts, whitespace-only text ignored.
bool sameTree(pugi::xml_node want, pugi::xml_node got, std::string& where) {
    if (std::string_view(want.name()) != got.name() ||
        std::string_view(want.text().get()) != got.text().get()) {
        where = std::string(want.name()) + " / " + got.name();
        return false;
    }
    auto wantAttribute = want.first_attribute();
    auto gotAttribute = got.first_attribute();
    for (; wantAttribute && gotAttribute; wantAttribute = wantAttribute.next_attribute(),
                                          gotAttribute = gotAttribute.next_attribute()) {
        if (std::string_view(wantAttribute.name()) != gotAttribute.name() ||
            std::string_view(wantAttribute.value()) != gotAttribute.value()) {
            where = std::string(want.name()) + "@" + wantAttribute.name();
            return false;
        }
    }
    if (wantAttribute || gotAttribute) {
        where = std::string(want.name()) + ": attribute count";
        return false;
    }
    auto wantChild = want.first_child();
    auto gotChild = got.first_child();
    const auto element = [](pugi::xml_node node) {
        while (node && node.type() != pugi::node_element) {
            node = node.next_sibling();
        }
        return node;
    };
    for (wantChild = element(wantChild), gotChild = element(gotChild); wantChild && gotChild;
         wantChild = element(wantChild.next_sibling()),
        gotChild = element(gotChild.next_sibling())) {
        if (!sameTree(wantChild, gotChild, where)) {
            where = std::string(want.name()) + " > " + where;
            return false;
        }
    }
    if (wantChild || gotChild) {
        where = std::string(want.name()) + ": child count";
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("styles.xml is python-docx's template with word.py's Normal style", "[docx]") {
    const std::string styles = ooxml::stylesXml("Calibri", 22);
    CHECK(styles.starts_with(ooxml::kDeclaration));
    pugi::xml_document want;
    pugi::xml_document got;
    REQUIRE(want.load_string(fixture("python_docx_styles.xml").c_str()));
    REQUIRE(got.load_string(styles.c_str()));
    std::string where;
    const bool same = sameTree(want.document_element(), got.document_element(), where);
    INFO(where);
    CHECK(same);
    // Word's Styles gallery reads these: the latent styles and the template's 164 styles.
    CHECK(got.document_element().child("w:latentStyles"));
    CHECK(std::distance(got.document_element().children("w:style").begin(),
                        got.document_element().children("w:style").end()) == 164);
}

TEST_CASE("a word package holds every part of python-docx's template", "[docx]") {
    TempFolder tmp;
    const auto path =
        writeDocx(walkthroughOf(5, {section("Words.", shot(tmp, "shot-1.png"), 2.0)}), tmp.path());
    const WordPackage word(path);
    CHECK(word.entries ==
          std::vector<std::string>{
              "[Content_Types].xml", "_rels/.rels", "docProps/core.xml", "docProps/app.xml",
              "word/document.xml", "word/_rels/document.xml.rels", "word/styles.xml",
              "word/stylesWithEffects.xml", "word/settings.xml", "word/webSettings.xml",
              "word/fontTable.xml", "word/theme/theme1.xml", "customXml/item1.xml",
              "customXml/_rels/item1.xml.rels", "customXml/itemProps1.xml", "word/numbering.xml",
              "word/media/image1.png", "docProps/thumbnail.jpeg"});
    pugi::xml_document numbering;
    pugi::xml_document wantNumbering;
    REQUIRE(numbering.load_string(word.parts.at("word/numbering.xml").c_str()));
    REQUIRE(wantNumbering.load_string(fixture("python_docx_numbering.xml").c_str()));
    std::string where;
    const bool same =
        sameTree(wantNumbering.document_element(), numbering.document_element(), where);
    INFO(where);
    CHECK(same);
    CHECK(word.parts.at("word/stylesWithEffects.xml").size() == 438131);
    // The template's JPEG as python-docx copies it: SOI, then EOI and one NUL byte.
    const std::string& thumbnail = word.parts.at("docProps/thumbnail.jpeg");
    REQUIRE(thumbnail.size() == 8324);
    CHECK(thumbnail.starts_with("\xFF\xD8"));
    CHECK(thumbnail.ends_with(std::string("\xFF\xD9\0", 3)));
    CHECK(word.parts.at("customXml/_rels/item1.xml.rels").find(R"(Target="itemProps1.xml")") !=
          std::string::npos);
}

} // namespace fv::documents::test
