#pragma once

#include <pugixml.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "archive/zip_reader.h"

// A .docx read back as python-docx reads one in OpenWhisper's tests: its parts parsed with
// pugixml, and the few python-docx accessors those tests use.
namespace fv::documents::test {

inline std::string readEntry(archive::ZipReader& reader, std::string_view name) {
    std::string out;
    reader.read_entry(name, [&](std::span<const std::byte> block) {
        out.append(reinterpret_cast<const char*>(block.data()), block.size());
    });
    return out;
}

inline pugi::xml_document parseXml(std::string_view text) {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed =
        doc.load_buffer(text.data(), text.size(), pugi::parse_default | pugi::parse_ws_pcdata);
    INFO(parsed.description());
    REQUIRE(parsed);
    return doc;
}

class WordPackage {
public:
    explicit WordPackage(const std::filesystem::path& path) {
        archive::ZipReader reader(path);
        for (const archive::EntryInfo& info : reader.infolist()) {
            entries.push_back(info.name);
        }
        for (const std::string& name : entries) {
            parts[name] = readEntry(reader, name);
        }
        document = parseXml(parts.at("word/document.xml"));
        styles = parseXml(parts.at("word/styles.xml"));
        core = parseXml(parts.at("docProps/core.xml"));
        relationships = parseXml(parts.at("word/_rels/document.xml.rels"));
    }

    pugi::xml_node body() const { return document.child("w:document").child("w:body"); }

    // Document.paragraphs: the body's own paragraphs, not a table's.
    std::vector<pugi::xml_node> paragraphs() const { return children(body(), "w:p"); }
    std::vector<pugi::xml_node> tables() const { return children(body(), "w:tbl"); }
    // Document.inline_shapes: every picture, tables' included.
    std::vector<pugi::xml_node> pictures() const {
        std::vector<pugi::xml_node> found;
        for (const pugi::xpath_node& node : body().select_nodes(".//wp:inline")) {
            found.push_back(node.node());
        }
        return found;
    }
    pugi::xml_node section() const { return body().child("w:sectPr"); }
    pugi::xml_node style(std::string_view id) const {
        for (pugi::xml_node style = styles.child("w:styles").child("w:style"); style;
             style = style.next_sibling("w:style")) {
            if (style.attribute("w:styleId").value() == id) {
                return style;
            }
        }
        return {};
    }
    std::string coreText(const char* name) const {
        return core.child("cp:coreProperties").child(name).text().get();
    }
    // The target of the document's relationship `id`.
    std::string target(std::string_view id) const {
        for (pugi::xml_node node = relationships.child("Relationships").child("Relationship"); node;
             node = node.next_sibling("Relationship")) {
            if (node.attribute("Id").value() == id) {
                return node.attribute("Target").value();
            }
        }
        return {};
    }

    static std::vector<pugi::xml_node> children(pugi::xml_node parent, const char* name) {
        std::vector<pugi::xml_node> found;
        for (pugi::xml_node child = parent.child(name); child; child = child.next_sibling(name)) {
            found.push_back(child);
        }
        return found;
    }

    std::vector<std::string> entries;
    std::map<std::string, std::string> parts;
    pugi::xml_document document;
    pugi::xml_document styles;
    pugi::xml_document core;
    pugi::xml_document relationships;
};

// Run.text: w:t as written, w:tab a tab, w:br and w:cr a line break.
inline std::string runText(pugi::xml_node run) {
    std::string text;
    for (pugi::xml_node child : run.children()) {
        const std::string_view name = child.name();
        if (name == "w:t") {
            text += child.text().get();
        } else if (name == "w:tab") {
            text += '\t';
        } else if (name == "w:br" || name == "w:cr") {
            text += '\n';
        }
    }
    return text;
}

// Paragraph.text: its runs' text, hyperlinks' included.
inline std::string paragraphText(pugi::xml_node paragraph) {
    std::string text;
    for (pugi::xml_node child : paragraph.children()) {
        const std::string_view name = child.name();
        if (name == "w:r") {
            text += runText(child);
        } else if (name == "w:hyperlink") {
            for (pugi::xml_node run : WordPackage::children(child, "w:r")) {
                text += runText(run);
            }
        }
    }
    return text;
}

// _Cell.text: its paragraphs' text, one per line.
inline std::string cellText(pugi::xml_node cell) {
    std::string text;
    bool first = true;
    for (pugi::xml_node paragraph : WordPackage::children(cell, "w:p")) {
        if (!first) {
            text += '\n';
        }
        first = false;
        text += paragraphText(paragraph);
    }
    return text;
}

// An on/off property such as w:keepNext or w:b: nullopt when absent, else its w:val.
inline std::optional<bool> onOff(pugi::xml_node element) {
    if (!element) {
        return std::nullopt;
    }
    const std::string_view value = element.attribute("w:val").value();
    return !(value == "0" || value == "false" || value == "off");
}

inline pugi::xml_node properties(pugi::xml_node paragraph) {
    return paragraph.child("w:pPr");
}

inline std::optional<bool> keepWithNext(pugi::xml_node paragraph) {
    return onOff(properties(paragraph).child("w:keepNext"));
}

// The paragraphs with text, by their text (later ones win, as in a dict comprehension).
inline std::map<std::string, pugi::xml_node> byText(const std::vector<pugi::xml_node>& paragraphs) {
    std::map<std::string, pugi::xml_node> found;
    for (pugi::xml_node paragraph : paragraphs) {
        if (std::string text = paragraphText(paragraph); !text.empty()) {
            found[text] = paragraph;
        }
    }
    return found;
}

inline std::int64_t attributeInt(pugi::xml_node node, const char* name) {
    return node.attribute(name).as_llong();
}

// Whether each entry's local header and central record carry a Zip64 extra field (id 1).
struct RawEntry {
    std::string name;
    std::uint16_t madeBy = 0;
    bool centralZip64 = false;
    bool localZip64 = false;
};

inline std::vector<RawEntry> rawEntries(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string zip((std::istreambuf_iterator<char>(in)), {});
    const auto u16 = [&](std::size_t at) {
        std::uint16_t value = 0;
        std::memcpy(&value, zip.data() + at, 2);
        return value;
    };
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value = 0;
        std::memcpy(&value, zip.data() + at, 4);
        return value;
    };
    const auto hasZip64 = [&](std::size_t at, std::size_t length) {
        for (std::size_t field = at; field + 4 <= at + length; field += 4 + u16(field + 2)) {
            if (u16(field) == 1) {
                return true;
            }
        }
        return false;
    };
    const std::size_t end = zip.rfind(std::string("PK\x05\x06", 4));
    REQUIRE(end != std::string::npos);
    // No Zip64 end-of-directory locator either.
    REQUIRE((end < 20 || u32(end - 20) != 0x07064b50));
    std::size_t at = u32(end + 16);
    std::vector<RawEntry> entries;
    for (std::uint16_t index = 0; index < u16(end + 10); ++index) {
        REQUIRE(u32(at) == 0x02014b50);
        const std::uint16_t name = u16(at + 28);
        const std::uint16_t extra = u16(at + 30);
        const std::uint16_t comment = u16(at + 32);
        RawEntry entry;
        entry.name = zip.substr(at + 46, name);
        entry.madeBy = u16(at + 4);
        entry.centralZip64 = hasZip64(at + 46 + name, extra);
        const std::size_t local = u32(at + 42);
        REQUIRE(u32(local) == 0x04034b50);
        entry.localZip64 = hasZip64(local + 30 + u16(local + 26), u16(local + 28));
        entries.push_back(std::move(entry));
        at += 46 + name + extra + comment;
    }
    return entries;
}

} // namespace fv::documents::test
