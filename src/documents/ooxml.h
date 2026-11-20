#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pugi {
class xml_document;
}

// The parts of a Word package (.docx) around its document.xml, as python-docx writes them from
// its default template (docx/templates/default.docx, python-docx 1.2.0): every part of the
// template, its relationship ids and its entry order, so Word's Styles gallery offers the
// template's styles as OpenWhisper's files do.
//
// Affinity: any thread; plain functions over their arguments.
namespace fv::documents::ooxml {

inline constexpr std::string_view kDeclaration =
    "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n";

inline constexpr std::string_view kWordNs =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
inline constexpr std::string_view kRelationshipsNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kDrawingNs =
    "http://schemas.openxmlformats.org/drawingml/2006/main";
inline constexpr std::string_view kPictureNs =
    "http://schemas.openxmlformats.org/drawingml/2006/picture";
inline constexpr std::string_view kWordDrawingNs =
    "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";

inline constexpr std::string_view kImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
inline constexpr std::string_view kHyperlinkRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";

// The template's w:document element with every namespace it declares, without its body.
inline constexpr std::string_view kDocumentOpen =
    R"(<w:document xmlns:wpc="http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas" )"
    R"(xmlns:mo="http://schemas.microsoft.com/office/mac/office/2008/main" )"
    R"(xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" )"
    R"(xmlns:mv="urn:schemas-microsoft-com:mac:vml" xmlns:o="urn:schemas-microsoft-com:office:office" )"
    R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" )"
    R"(xmlns:m="http://schemas.openxmlformats.org/officeDocument/2006/math" )"
    R"(xmlns:v="urn:schemas-microsoft-com:vml" )"
    R"(xmlns:wp14="http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing" )"
    R"(xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" )"
    R"(xmlns:w10="urn:schemas-microsoft-com:office:word" )"
    R"(xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" )"
    R"(xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" )"
    R"(xmlns:wpg="http://schemas.microsoft.com/office/word/2010/wordprocessingGroup" )"
    R"(xmlns:wpi="http://schemas.microsoft.com/office/word/2010/wordprocessingInk" )"
    R"(xmlns:wne="http://schemas.microsoft.com/office/word/2006/wordml" )"
    R"(xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" )"
    R"(mc:Ignorable="w14 wp14"/>)";

// The template's parts, as python-docx saves them.
extern const std::string_view kAppXml;
extern const std::string_view kSettingsXml;
extern const std::string_view kWebSettingsXml;
extern const std::string_view kFontTableXml;
extern const std::string_view kThemeXml;

// One relationship of a part.
struct Relationship {
    std::string id;
    std::string_view type;
    std::string target;
    bool external = false;
};

// One image of the document, under word/media/.
struct Media {
    std::string name; // "image1.png"
    std::vector<std::byte> bytes;
};

// `doc` as python-docx writes a part: the declaration, then the XML with no whitespace added.
std::string serialize(const pugi::xml_document& doc);
// word/styles.xml: the template's, its Normal style in
// `family` at `halfPoints` half-points, with no space after and single line spacing.
std::string stylesXml(std::string_view family, int halfPoints);
// docProps/core.xml: the template's, with `title`, no author or last editor, and `comments`.
std::string coreXml(std::string_view title, std::string_view comments);
// [Content_Types].xml for the parts this package holds; PNG pictures when `png`.
std::string contentTypesXml(bool png);
// _rels/.rels: the document, the two property parts and the thumbnail.
std::string packageRelationshipsXml();
// word/_rels/document.xml.rels: the template parts the document uses, then `added`.
std::string documentRelationshipsXml(const std::vector<Relationship>& added);
// The first id documentRelationshipsXml leaves free for `added`.
int firstFreeRelationship();

// Writes the package to `path`: `document` (word/document.xml), its relationships, `styles`,
// `core`, the template parts and `media`, deflated, with no Zip64 extra fields. Throws
// std::filesystem::filesystem_error or std::runtime_error when the file cannot be written, and
// leaves no file behind then.
void writePackage(const std::filesystem::path& path, std::string_view document,
                  const std::vector<Relationship>& relationships, std::string_view styles,
                  std::string_view core, const std::vector<Media>& media);

} // namespace fv::documents::ooxml
