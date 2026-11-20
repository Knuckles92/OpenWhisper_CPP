// Golden vectors of services.walkthrough.word: the Word packages python-docx writes for fixed
// walkthroughs and write-ups (tools/parity/cases/services.walkthrough.word.py says what each
// field is), compared part by part as XML trees. Relationship ids compare by what they point
// to; texts compare after rebranding. Every part of python-docx's template is carried, so the
// entries, content types and relationships match whole; styles.xml compares as the case file's
// _styles cuts it (test_sweep_word_template.cpp compares it whole with python-docx's).
#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QImage>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "documents/docx.h"
#include "documents/ooxml.h"
#include "documents_support.h"
#include "docx_reader.h"
#include "golden/golden.h"
#include "pyport/datetime.h"

namespace fv::documents::test {
namespace {

constexpr std::string_view kModule = "services.walkthrough.word";

// The styles the case file's _styles keeps besides w:docDefaults (_KEPT_STYLES).
const std::set<std::string> kKeptStyles{"Normal", "DefaultParagraphFont", "TableNormal", "NoList"};

using Resolve = std::function<std::string(std::string_view)>;

pugi::xml_document parseCompared(std::string_view text) {
    pugi::xml_document doc;
    // A w:t that holds only spaces keeps them.
    const pugi::xml_parse_result parsed = doc.load_buffer(
        text.data(), text.size(), pugi::parse_default | pugi::parse_ws_pcdata_single);
    INFO(parsed.description());
    REQUIRE(parsed);
    return doc;
}

// Id -> "type target" for a relationships part.
Resolve resolver(const std::string& relationships) {
    std::map<std::string, std::string> targets;
    const pugi::xml_document doc = parseCompared(relationships);
    for (pugi::xml_node node : doc.child("Relationships").children("Relationship")) {
        targets[node.attribute("Id").value()] =
            std::string(node.attribute("Type").value()) + " " + node.attribute("Target").value();
    }
    return [targets](std::string_view id) {
        const auto found = targets.find(std::string(id));
        return found == targets.end() ? "unknown " + std::string(id) : found->second;
    };
}

// Every difference between two element trees, by path.
void compare(pugi::xml_node expected, pugi::xml_node actual, const std::string& path,
             const Resolve& expectedIds, const Resolve& actualIds,
             std::vector<std::string>& differences) {
    if (std::string_view(expected.name()) != actual.name()) {
        differences.push_back(path + ": element " + expected.name() + " became " + actual.name());
        return;
    }
    const std::string here = path + "/" + expected.name();
    const auto attributes = [](pugi::xml_node node, const Resolve& ids) {
        std::map<std::string, std::string> found;
        for (pugi::xml_attribute attribute : node.attributes()) {
            const std::string name = attribute.name();
            found[name] =
                (name == "r:id" || name == "r:embed") ? ids(attribute.value()) : attribute.value();
        }
        return found;
    };
    const auto want = attributes(expected, expectedIds);
    const auto got = attributes(actual, actualIds);
    if (want != got) {
        std::string text = here + ": attributes";
        for (const auto& [name, value] : want) {
            text += " " + name + "=" + value;
        }
        text += " became";
        for (const auto& [name, value] : got) {
            text += " " + name + "=" + value;
        }
        differences.push_back(text);
    }
    if (std::string_view(expected.text().get()) != actual.text().get()) {
        differences.push_back(here + ": text '" + expected.text().get() + "' became '" +
                              actual.text().get() + "'");
    }
    std::vector<pugi::xml_node> wantChildren;
    std::vector<pugi::xml_node> gotChildren;
    for (pugi::xml_node child : expected.children()) {
        if (child.type() == pugi::node_element) {
            wantChildren.push_back(child);
        }
    }
    for (pugi::xml_node child : actual.children()) {
        if (child.type() == pugi::node_element) {
            gotChildren.push_back(child);
        }
    }
    if (wantChildren.size() != gotChildren.size()) {
        std::string text = here + ": children";
        for (pugi::xml_node child : wantChildren) {
            text += std::string(" ") + child.name();
        }
        text += " became";
        for (pugi::xml_node child : gotChildren) {
            text += std::string(" ") + child.name();
        }
        differences.push_back(text);
    }
    for (std::size_t index = 0; index < std::min(wantChildren.size(), gotChildren.size());
         ++index) {
        compare(wantChildren[index], gotChildren[index], here + "[" + std::to_string(index) + "]",
                expectedIds, actualIds, differences);
    }
}

void expectSameTree(std::string_view label, pugi::xml_node expected, pugi::xml_node actual,
                    const Resolve& expectedIds = {}, const Resolve& actualIds = {}) {
    const Resolve none = [](std::string_view id) {
        return std::string(id);
    };
    std::vector<std::string> differences;
    compare(expected, actual, "", expectedIds ? expectedIds : none, actualIds ? actualIds : none,
            differences);
    INFO(label);
    for (std::size_t index = 0; index < std::min<std::size_t>(differences.size(), 12); ++index) {
        INFO(differences[index]);
    }
    CHECK(differences.empty());
}

// A relationships part as a set of "type target mode".
std::set<std::string> relationshipSet(const std::string& text) {
    std::set<std::string> found;
    const pugi::xml_document doc = parseCompared(text);
    for (pugi::xml_node node : doc.child("Relationships").children("Relationship")) {
        const std::string target = node.attribute("Target").value();
        found.insert(std::string(node.attribute("Type").value()) + " " + target + " " +
                     node.attribute("TargetMode").value());
    }
    return found;
}

// [Content_Types].xml as a set of "Default ext type" and "Override part type".
std::set<std::string> contentTypes(const std::string& text) {
    std::set<std::string> found;
    const pugi::xml_document doc = parseCompared(text);
    for (pugi::xml_node node : doc.child("Types").children()) {
        const std::string kind = node.name();
        const std::string key = kind == "Default" ? node.attribute("Extension").value()
                                                  : node.attribute("PartName").value();
        found.insert(kind + " " + key + " " + node.attribute("ContentType").value());
    }
    return found;
}

void expectPackage(const golden::Case& c, const std::filesystem::path& path) {
    INFO(c.label());
    const Json expected = c.result<Json>();
    const WordPackage word(path);

    std::vector<std::string> entries;
    for (const Json& entry : expected.at("entries")) {
        entries.push_back(entry.get<std::string>());
    }
    // python-docx writes the pictures last but for the thumbnail; so does FioriVoce.
    CHECK(word.entries == entries);
    CHECK_FALSE(expected.at("zip64").get<bool>());
    for (const RawEntry& entry : rawEntries(path)) {
        CHECK_FALSE((entry.centralZip64 || entry.localZip64));
    }

    const Json& parts = expected.at("parts");
    const auto part = [&](const char* name) {
        return golden::rebrand(parts.at(name).get<std::string>());
    };

    const std::set<std::string> wantTypes = contentTypes(part("[Content_Types].xml"));
    CHECK(contentTypes(word.parts.at("[Content_Types].xml")) == wantTypes);
    CHECK(relationshipSet(word.parts.at("_rels/.rels")) == relationshipSet(part("_rels/.rels")));
    CHECK(relationshipSet(word.parts.at("word/_rels/document.xml.rels")) ==
          relationshipSet(part("word/_rels/document.xml.rels")));

    const pugi::xml_document wantCore = parseCompared(part("docProps/core.xml"));
    expectSameTree("docProps/core.xml", wantCore.document_element(),
                   parseCompared(word.parts.at("docProps/core.xml")).document_element());

    const pugi::xml_document wantDocument = parseCompared(part("word/document.xml"));
    const pugi::xml_document gotDocument = parseCompared(word.parts.at("word/document.xml"));
    expectSameTree("word/document.xml", wantDocument.document_element(),
                   gotDocument.document_element(), resolver(part("word/_rels/document.xml.rels")),
                   resolver(word.parts.at("word/_rels/document.xml.rels")));

    // The styles a package uses: the document defaults and the four default styles.
    CHECK(expected.at("styles_used").empty());
    const pugi::xml_document wantStyles =
        parseCompared(golden::rebrand(expected.at("styles").get<std::string>()));
    const pugi::xml_document gotStyles = parseCompared(word.parts.at("word/styles.xml"));
    std::vector<pugi::xml_node> want(wantStyles.document_element().children().begin(),
                                     wantStyles.document_element().children().end());
    std::vector<pugi::xml_node> got;
    for (pugi::xml_node child : gotStyles.document_element().children()) {
        if (std::string_view(child.name()) == "w:docDefaults" ||
            (std::string_view(child.name()) == "w:style" &&
             kKeptStyles.contains(child.attribute("w:styleId").value()))) {
            got.push_back(child);
        }
    }
    REQUIRE(want.size() == got.size());
    for (std::size_t index = 0; index < want.size(); ++index) {
        expectSameTree("word/styles.xml", want[index], got[index]);
    }

    for (const auto& [name, size] : expected.at("media").items()) {
        INFO(name);
        const QImage image =
            QImage::fromData(QByteArray::fromStdString(word.parts.at(std::string(name))));
        CHECK(image.width() == size.at(0).get<int>());
        CHECK(image.height() == size.at(1).get<int>());
    }
}

std::chrono::local_seconds localFromIso(const std::string& iso) {
    const auto value = py::fromisoformat(iso);
    return local(value.year, static_cast<unsigned>(value.month), static_cast<unsigned>(value.day),
                 value.hour, value.minute, value.second);
}

std::optional<QPageSize> paperOf(const Json& paper) {
    if (paper.is_null()) {
        return std::nullopt;
    }
    REQUIRE((paper == "letter" || paper == "a4"));
    return QPageSize(paper == "letter" ? QPageSize::Letter : QPageSize::A4);
}

TEST_CASE("the word writer writes walkthroughs as OpenWhisper's",
          "[golden][test_walkthrough_word]") {
    golden::for_each(kModule, "write_docx", [](const golden::Case& c) {
        TempFolder tmp;
        const Json value = c.arg<Json>(1);
        walkthrough::Walkthrough walkthrough{
            walkthrough::local_moment(std::chrono::locate_zone("America/Los_Angeles"),
                                      localFromIso(value.at("started_at").get<std::string>())),
            value.at("duration_s").get<double>(),
            {}};
        int number = 0;
        for (const Json& item : value.at("sections")) {
            ++number;
            const Json& shot = item.at(1);
            std::optional<std::filesystem::path> path;
            if (shot.is_array()) {
                path = png(tmp / ("shot-" + std::to_string(number) + ".png"), shot.at(0).get<int>(),
                           shot.at(1).get<int>());
            }
            const Json& kept = item.at(2);
            walkthrough.sections.push_back(
                section(item.at(0).get<std::string>(), path,
                        kept.is_null() ? std::nullopt : std::optional(kept.get<double>())));
        }
        walkthrough::DocumentFormat format;
        if (const Json chosen = c.arg<Json>(2); !chosen.is_null()) {
            format = {chosen.at(0).get<std::string>(), chosen.at(1).get<bool>(),
                      chosen.at(2).get<bool>(), chosen.at(3).get<bool>()};
        }

        expectPackage(c, writeDocx(walkthrough, tmp.path(), format, paperOf(c.arg<Json>(0))));
    });
}

TEST_CASE("the word writer writes write ups as OpenWhisper's", "[golden][test_walkthrough_word]") {
    golden::for_each(kModule, "write_markdown_docx", [](const golden::Case& c) {
        TempFolder tmp;
        for (const Json& image : c.arg<Json>(2)) {
            png(tmp / image.at(0).get<std::string>(), image.at(1).get<int>(),
                image.at(2).get<int>());
        }
        walkthrough::Look look;
        if (const Json chosen = c.arg<Json>(3); !chosen.is_null()) {
            look = {chosen.at(0).get<std::string>(), chosen.at(1).get<std::string>(),
                    chosen.at(2).get<std::string>()};
        }

        expectPackage(c, writeMarkdownDocx(c.arg<std::string>(1), tmp / "write-up.docx", tmp.path(),
                                           c.arg<std::string>(4), paperOf(c.arg<Json>(0)), look));
    });
}

TEST_CASE("the part comparison sees every kind of difference", "[docx]") {
    const Resolve ids = [](std::string_view id) {
        return "target of " + std::string(id);
    };
    const auto differences = [&](std::string_view expected, std::string_view actual) {
        const pugi::xml_document want = parseCompared(expected);
        const pugi::xml_document got = parseCompared(actual);
        std::vector<std::string> found;
        compare(want.document_element(), got.document_element(), "", ids, ids, found);
        return found.size();
    };
    const std::string_view base = R"(<w:p a="1"><w:r r:id="rId1"><w:t> </w:t></w:r></w:p>)";
    CHECK(differences(base, base) == 0);
    CHECK(differences(base, R"(<w:p a="1"><w:r r:id="rId1"><w:t>x</w:t></w:r></w:p>)") == 1);
    CHECK(differences(base, R"(<w:p a="2"><w:r r:id="rId1"><w:t> </w:t></w:r></w:p>)") == 1);
    CHECK(differences(base, R"(<w:p a="1"><w:r r:id="rId2"><w:t> </w:t></w:r></w:p>)") == 1);
    CHECK(differences(base, R"(<w:p a="1"><w:r r:id="rId1"><w:t> </w:t><w:br/></w:r></w:p>)") == 1);
    CHECK(differences(base, R"(<w:p a="1"><w:x r:id="rId1"><w:t> </w:t></w:x></w:p>)") == 1);
}

TEST_CASE("the template parts are python-docx's", "[golden][test_walkthrough_word]") {
    golden::for_each(kModule, "template_parts", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(expected.at("docProps/app.xml").get<std::string>() == ooxml::kAppXml);
        CHECK(expected.at("word/settings.xml").get<std::string>() == ooxml::kSettingsXml);
        CHECK(expected.at("word/webSettings.xml").get<std::string>() == ooxml::kWebSettingsXml);
        CHECK(expected.at("word/fontTable.xml").get<std::string>() == ooxml::kFontTableXml);
        CHECK(expected.at("word/theme/theme1.xml").get<std::string>() == ooxml::kThemeXml);
    });
}

} // namespace
} // namespace fv::documents::test
