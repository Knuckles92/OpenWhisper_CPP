#include "documents/ooxml.h"

#include <pugixml.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

#include "archive/zip_writer.h"
#include "documents/data/word_numbering.xml.h"
#include "documents/data/word_styles.xml.h"
#include "documents/data/word_styles_with_effects.xml.h"
#include "documents/data/word_thumbnail.jpeg.h"

namespace fv::documents::ooxml {

namespace {

// zipfile's writestr gives a file entry these permissions (0o600 << 16).
constexpr std::uint32_t kEntryAttributes = 0600u << 16;

constexpr std::string_view kStylesRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";
constexpr std::string_view kSettingsRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings";
constexpr std::string_view kWebSettingsRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/webSettings";
constexpr std::string_view kFontTableRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/fontTable";
constexpr std::string_view kThemeRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme";

constexpr std::string_view kStylesWithEffectsRelationship =
    "http://schemas.microsoft.com/office/2007/relationships/stylesWithEffects";
constexpr std::string_view kCustomXmlRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXml";
constexpr std::string_view kNumberingRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering";

// The template's customXml item (a bibliography source list) and its properties, which
// python-docx carries as they are.
constexpr std::string_view kCustomXmlItem =
    "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
    R"(<b:Sources xmlns:b="http://schemas.openxmlformats.org/officeDocument/2006/bibliography" )"
    R"(xmlns="http://schemas.openxmlformats.org/officeDocument/2006/bibliography" )"
    R"(SelectedStyle="/APA.XSL" StyleName="APA"/>)"
    "\n";
constexpr std::string_view kCustomXmlItemProps =
    "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
    R"(<ds:datastoreItem xmlns:ds="http://schemas.openxmlformats.org/officeDocument/2006/customXml" )"
    R"(ds:itemID="{EF278816-EC6F-A645-907D-7F25AECB1D4A}">)"
    "\n  <ds:schemaRefs>\n"
    R"(    <ds:schemaRef ds:uri="http://schemas.openxmlformats.org/officeDocument/2006/bibliography"/>)"
    "\n  </ds:schemaRefs>\n</ds:datastoreItem>\n";

// The template's docProps/core.xml; title, creator, last editor and description are filled in.
// Its dates are the template's: python-docx leaves them.
constexpr std::string_view kCore =
    R"(<cp:coreProperties )"
    R"(xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" )"
    R"(xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" )"
    R"(xmlns:dcmitype="http://purl.org/dc/dcmitype/" )"
    R"(xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">)"
    R"(<dc:title/><dc:subject/><dc:creator/><cp:keywords/><dc:description/>)"
    R"(<cp:lastModifiedBy/><cp:revision>1</cp:revision>)"
    R"(<dcterms:created xsi:type="dcterms:W3CDTF">2013-12-23T23:15:00Z</dcterms:created>)"
    R"(<dcterms:modified xsi:type="dcterms:W3CDTF">2013-12-23T23:15:00Z</dcterms:modified>)"
    R"(<cp:category/></cp:coreProperties>)";

constexpr std::string_view kContentTypesNs =
    "http://schemas.openxmlformats.org/package/2006/content-types";
constexpr std::string_view kPackageRelationshipsNs =
    "http://schemas.openxmlformats.org/package/2006/relationships";

class StringWriter final : public pugi::xml_writer {
public:
    explicit StringWriter(std::string& out) : out_(out) {}
    void write(const void* data, std::size_t size) override {
        out_.append(static_cast<const char*>(data), size);
    }

private:
    std::string& out_;
};

pugi::xml_document parse(std::string_view xml) {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw std::logic_error(std::string("A Word template part does not parse: ") +
                               parsed.description());
    }
    return doc;
}

void addRelationship(pugi::xml_node root, std::string_view id, std::string_view type,
                     std::string_view target, bool external) {
    pugi::xml_node relationship = root.append_child("Relationship");
    relationship.append_attribute("Id").set_value(id);
    relationship.append_attribute("Type").set_value(type);
    relationship.append_attribute("Target").set_value(target);
    if (external) {
        relationship.append_attribute("TargetMode").set_value("External");
    }
}

pugi::xml_node relationshipsRoot(pugi::xml_document& doc) {
    pugi::xml_node root = doc.append_child("Relationships");
    root.append_attribute("xmlns").set_value(kPackageRelationshipsNs);
    return root;
}

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

} // namespace

// The template parts python-docx 1.2.0 saves unchanged, as tools/parity/cases/
// services.walkthrough.word.py records them (template_parts).
const std::string_view kAppXml = R"ooxml(<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties" xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
  <Template>Normal.dotm</Template>
  <TotalTime>0</TotalTime>
  <Pages>1</Pages>
  <Words>0</Words>
  <Characters>0</Characters>
  <Application>Microsoft Macintosh Word</Application>
  <DocSecurity>0</DocSecurity>
  <Lines>0</Lines>
  <Paragraphs>0</Paragraphs>
  <ScaleCrop>false</ScaleCrop>
  <HeadingPairs>
    <vt:vector size="2" baseType="variant">
      <vt:variant>
        <vt:lpstr>Title</vt:lpstr>
      </vt:variant>
      <vt:variant>
        <vt:i4>1</vt:i4>
      </vt:variant>
    </vt:vector>
  </HeadingPairs>
  <TitlesOfParts>
    <vt:vector size="1" baseType="lpstr">
      <vt:lpstr/>
    </vt:vector>
  </TitlesOfParts>
  <Manager/>
  <Company/>
  <LinksUpToDate>false</LinksUpToDate>
  <CharactersWithSpaces>0</CharactersWithSpaces>
  <SharedDoc>false</SharedDoc>
  <HyperlinkBase/>
  <HyperlinksChanged>false</HyperlinksChanged>
  <AppVersion>14.0000</AppVersion>
</Properties>
)ooxml";
const std::string_view kSettingsXml =
    R"ooxml(<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<w:settings xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" xmlns:o="urn:schemas-microsoft-com:office:office" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:m="http://schemas.openxmlformats.org/officeDocument/2006/math" xmlns:v="urn:schemas-microsoft-com:vml" xmlns:w10="urn:schemas-microsoft-com:office:word" xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" xmlns:sl="http://schemas.openxmlformats.org/schemaLibrary/2006/main" mc:Ignorable="w14"><w:zoom w:val="bestFit"/><w:proofState w:spelling="clean" w:grammar="clean"/><w:defaultTabStop w:val="720"/><w:characterSpacingControl w:val="doNotCompress"/><w:savePreviewPicture/><w:compat><w:useFELayout/><w:compatSetting w:name="compatibilityMode" w:uri="http://schemas.microsoft.com/office/word" w:val="14"/><w:compatSetting w:name="overrideTableStyleFontSizeAndJustification" w:uri="http://schemas.microsoft.com/office/word" w:val="1"/><w:compatSetting w:name="enableOpenTypeFeatures" w:uri="http://schemas.microsoft.com/office/word" w:val="1"/><w:compatSetting w:name="doNotFlipMirrorIndents" w:uri="http://schemas.microsoft.com/office/word" w:val="1"/></w:compat><w:rsids><w:rsidRoot w:val="00B47730"/><w:rsid w:val="00034616"/><w:rsid w:val="0006063C"/><w:rsid w:val="0015074B"/><w:rsid w:val="0029639D"/><w:rsid w:val="00326F90"/><w:rsid w:val="00AA1D8D"/><w:rsid w:val="00B47730"/><w:rsid w:val="00CB0664"/><w:rsid w:val="00FC693F"/></w:rsids><m:mathPr><m:mathFont m:val="Cambria Math"/><m:brkBin m:val="before"/><m:brkBinSub m:val="--"/><m:smallFrac m:val="0"/><m:dispDef/><m:lMargin m:val="0"/><m:rMargin m:val="0"/><m:defJc m:val="centerGroup"/><m:wrapIndent m:val="1440"/><m:intLim m:val="subSup"/><m:naryLim m:val="undOvr"/></m:mathPr><w:themeFontLang w:val="en-US" w:eastAsia="ja-JP"/><w:clrSchemeMapping w:bg1="light1" w:t1="dark1" w:bg2="light2" w:t2="dark2" w:accent1="accent1" w:accent2="accent2" w:accent3="accent3" w:accent4="accent4" w:accent5="accent5" w:accent6="accent6" w:hyperlink="hyperlink" w:followedHyperlink="followedHyperlink"/><w:doNotAutoCompressPictures/><w:shapeDefaults><o:shapedefaults v:ext="edit" spidmax="1027"/><o:shapelayout v:ext="edit"><o:idmap v:ext="edit" data="1"/></o:shapelayout></w:shapeDefaults><w:decimalSymbol w:val="."/><w:listSeparator w:val=","/><w14:docId w14:val="24062061"/><w14:defaultImageDpi w14:val="300"/></w:settings>)ooxml";
const std::string_view kWebSettingsXml =
    R"ooxml(<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<w:webSettings xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" mc:Ignorable="w14">
  <w:allowPNG/>
  <w:doNotSaveAsSingleFile/>
</w:webSettings>
)ooxml";
const std::string_view kFontTableXml =
    R"ooxml(<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<w:fonts xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" mc:Ignorable="w14">
  <w:font w:name="Symbol">
    <w:panose1 w:val="00000000000000000000"/>
    <w:charset w:val="02"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="00000000" w:usb1="10000000" w:usb2="00000000" w:usb3="00000000" w:csb0="80000000" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="Times New Roman">
    <w:panose1 w:val="02020603050405020304"/>
    <w:charset w:val="00"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="E0002AFF" w:usb1="C0007841" w:usb2="00000009" w:usb3="00000000" w:csb0="000001FF" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="Cambria">
    <w:panose1 w:val="02040503050406030204"/>
    <w:charset w:val="00"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="E00002FF" w:usb1="400004FF" w:usb2="00000000" w:usb3="00000000" w:csb0="0000019F" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="ＭＳ 明朝">
    <w:panose1 w:val="00000000000000000000"/>
    <w:charset w:val="80"/>
    <w:family w:val="roman"/>
    <w:notTrueType/>
    <w:pitch w:val="fixed"/>
    <w:sig w:usb0="00000001" w:usb1="08070000" w:usb2="00000010" w:usb3="00000000" w:csb0="00020000" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="Calibri">
    <w:panose1 w:val="020F0502020204030204"/>
    <w:charset w:val="00"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="E10002FF" w:usb1="4000ACFF" w:usb2="00000009" w:usb3="00000000" w:csb0="0000019F" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="ＭＳ ゴシック">
    <w:panose1 w:val="00000000000000000000"/>
    <w:charset w:val="80"/>
    <w:family w:val="modern"/>
    <w:notTrueType/>
    <w:pitch w:val="fixed"/>
    <w:sig w:usb0="00000001" w:usb1="08070000" w:usb2="00000010" w:usb3="00000000" w:csb0="00020000" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="Courier">
    <w:panose1 w:val="02000500000000000000"/>
    <w:charset w:val="00"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="00000003" w:usb1="00000000" w:usb2="00000000" w:usb3="00000000" w:csb0="00000001" w:csb1="00000000"/>
  </w:font>
  <w:font w:name="Arial">
    <w:panose1 w:val="020B0604020202020204"/>
    <w:charset w:val="00"/>
    <w:family w:val="auto"/>
    <w:pitch w:val="variable"/>
    <w:sig w:usb0="E0002AFF" w:usb1="C0007843" w:usb2="00000009" w:usb3="00000000" w:csb0="000001FF" w:csb1="00000000"/>
  </w:font>
</w:fonts>
)ooxml";
const std::string_view kThemeXml = R"ooxml(<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme">
  <a:themeElements>
    <a:clrScheme name="Office">
      <a:dk1>
        <a:sysClr val="windowText" lastClr="000000"/>
      </a:dk1>
      <a:lt1>
        <a:sysClr val="window" lastClr="FFFFFF"/>
      </a:lt1>
      <a:dk2>
        <a:srgbClr val="1F497D"/>
      </a:dk2>
      <a:lt2>
        <a:srgbClr val="EEECE1"/>
      </a:lt2>
      <a:accent1>
        <a:srgbClr val="4F81BD"/>
      </a:accent1>
      <a:accent2>
        <a:srgbClr val="C0504D"/>
      </a:accent2>
      <a:accent3>
        <a:srgbClr val="9BBB59"/>
      </a:accent3>
      <a:accent4>
        <a:srgbClr val="8064A2"/>
      </a:accent4>
      <a:accent5>
        <a:srgbClr val="4BACC6"/>
      </a:accent5>
      <a:accent6>
        <a:srgbClr val="F79646"/>
      </a:accent6>
      <a:hlink>
        <a:srgbClr val="0000FF"/>
      </a:hlink>
      <a:folHlink>
        <a:srgbClr val="800080"/>
      </a:folHlink>
    </a:clrScheme>
    <a:fontScheme name="Office">
      <a:majorFont>
        <a:latin typeface="Calibri"/>
        <a:ea typeface=""/>
        <a:cs typeface=""/>
        <a:font script="Jpan" typeface="ＭＳ ゴシック"/>
        <a:font script="Hang" typeface="맑은 고딕"/>
        <a:font script="Hans" typeface="宋体"/>
        <a:font script="Hant" typeface="新細明體"/>
        <a:font script="Arab" typeface="Times New Roman"/>
        <a:font script="Hebr" typeface="Times New Roman"/>
        <a:font script="Thai" typeface="Angsana New"/>
        <a:font script="Ethi" typeface="Nyala"/>
        <a:font script="Beng" typeface="Vrinda"/>
        <a:font script="Gujr" typeface="Shruti"/>
        <a:font script="Khmr" typeface="MoolBoran"/>
        <a:font script="Knda" typeface="Tunga"/>
        <a:font script="Guru" typeface="Raavi"/>
        <a:font script="Cans" typeface="Euphemia"/>
        <a:font script="Cher" typeface="Plantagenet Cherokee"/>
        <a:font script="Yiii" typeface="Microsoft Yi Baiti"/>
        <a:font script="Tibt" typeface="Microsoft Himalaya"/>
        <a:font script="Thaa" typeface="MV Boli"/>
        <a:font script="Deva" typeface="Mangal"/>
        <a:font script="Telu" typeface="Gautami"/>
        <a:font script="Taml" typeface="Latha"/>
        <a:font script="Syrc" typeface="Estrangelo Edessa"/>
        <a:font script="Orya" typeface="Kalinga"/>
        <a:font script="Mlym" typeface="Kartika"/>
        <a:font script="Laoo" typeface="DokChampa"/>
        <a:font script="Sinh" typeface="Iskoola Pota"/>
        <a:font script="Mong" typeface="Mongolian Baiti"/>
        <a:font script="Viet" typeface="Times New Roman"/>
        <a:font script="Uigh" typeface="Microsoft Uighur"/>
        <a:font script="Geor" typeface="Sylfaen"/>
      </a:majorFont>
      <a:minorFont>
        <a:latin typeface="Cambria"/>
        <a:ea typeface=""/>
        <a:cs typeface=""/>
        <a:font script="Jpan" typeface="ＭＳ 明朝"/>
        <a:font script="Hang" typeface="맑은 고딕"/>
        <a:font script="Hans" typeface="宋体"/>
        <a:font script="Hant" typeface="新細明體"/>
        <a:font script="Arab" typeface="Arial"/>
        <a:font script="Hebr" typeface="Arial"/>
        <a:font script="Thai" typeface="Cordia New"/>
        <a:font script="Ethi" typeface="Nyala"/>
        <a:font script="Beng" typeface="Vrinda"/>
        <a:font script="Gujr" typeface="Shruti"/>
        <a:font script="Khmr" typeface="DaunPenh"/>
        <a:font script="Knda" typeface="Tunga"/>
        <a:font script="Guru" typeface="Raavi"/>
        <a:font script="Cans" typeface="Euphemia"/>
        <a:font script="Cher" typeface="Plantagenet Cherokee"/>
        <a:font script="Yiii" typeface="Microsoft Yi Baiti"/>
        <a:font script="Tibt" typeface="Microsoft Himalaya"/>
        <a:font script="Thaa" typeface="MV Boli"/>
        <a:font script="Deva" typeface="Mangal"/>
        <a:font script="Telu" typeface="Gautami"/>
        <a:font script="Taml" typeface="Latha"/>
        <a:font script="Syrc" typeface="Estrangelo Edessa"/>
        <a:font script="Orya" typeface="Kalinga"/>
        <a:font script="Mlym" typeface="Kartika"/>
        <a:font script="Laoo" typeface="DokChampa"/>
        <a:font script="Sinh" typeface="Iskoola Pota"/>
        <a:font script="Mong" typeface="Mongolian Baiti"/>
        <a:font script="Viet" typeface="Arial"/>
        <a:font script="Uigh" typeface="Microsoft Uighur"/>
        <a:font script="Geor" typeface="Sylfaen"/>
      </a:minorFont>
    </a:fontScheme>
    <a:fmtScheme name="Office">
      <a:fillStyleLst>
        <a:solidFill>
          <a:schemeClr val="phClr"/>
        </a:solidFill>
        <a:gradFill rotWithShape="1">
          <a:gsLst>
            <a:gs pos="0">
              <a:schemeClr val="phClr">
                <a:tint val="50000"/>
                <a:satMod val="300000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="35000">
              <a:schemeClr val="phClr">
                <a:tint val="37000"/>
                <a:satMod val="300000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="100000">
              <a:schemeClr val="phClr">
                <a:tint val="15000"/>
                <a:satMod val="350000"/>
              </a:schemeClr>
            </a:gs>
          </a:gsLst>
          <a:lin ang="16200000" scaled="1"/>
        </a:gradFill>
        <a:gradFill rotWithShape="1">
          <a:gsLst>
            <a:gs pos="0">
              <a:schemeClr val="phClr">
                <a:tint val="100000"/>
                <a:shade val="100000"/>
                <a:satMod val="130000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="100000">
              <a:schemeClr val="phClr">
                <a:tint val="50000"/>
                <a:shade val="100000"/>
                <a:satMod val="350000"/>
              </a:schemeClr>
            </a:gs>
          </a:gsLst>
          <a:lin ang="16200000" scaled="0"/>
        </a:gradFill>
      </a:fillStyleLst>
      <a:lnStyleLst>
        <a:ln w="9525" cap="flat" cmpd="sng" algn="ctr">
          <a:solidFill>
            <a:schemeClr val="phClr">
              <a:shade val="95000"/>
              <a:satMod val="105000"/>
            </a:schemeClr>
          </a:solidFill>
          <a:prstDash val="solid"/>
        </a:ln>
        <a:ln w="25400" cap="flat" cmpd="sng" algn="ctr">
          <a:solidFill>
            <a:schemeClr val="phClr"/>
          </a:solidFill>
          <a:prstDash val="solid"/>
        </a:ln>
        <a:ln w="38100" cap="flat" cmpd="sng" algn="ctr">
          <a:solidFill>
            <a:schemeClr val="phClr"/>
          </a:solidFill>
          <a:prstDash val="solid"/>
        </a:ln>
      </a:lnStyleLst>
      <a:effectStyleLst>
        <a:effectStyle>
          <a:effectLst>
            <a:outerShdw blurRad="40000" dist="20000" dir="5400000" rotWithShape="0">
              <a:srgbClr val="000000">
                <a:alpha val="38000"/>
              </a:srgbClr>
            </a:outerShdw>
          </a:effectLst>
        </a:effectStyle>
        <a:effectStyle>
          <a:effectLst>
            <a:outerShdw blurRad="40000" dist="23000" dir="5400000" rotWithShape="0">
              <a:srgbClr val="000000">
                <a:alpha val="35000"/>
              </a:srgbClr>
            </a:outerShdw>
          </a:effectLst>
        </a:effectStyle>
        <a:effectStyle>
          <a:effectLst>
            <a:outerShdw blurRad="40000" dist="23000" dir="5400000" rotWithShape="0">
              <a:srgbClr val="000000">
                <a:alpha val="35000"/>
              </a:srgbClr>
            </a:outerShdw>
          </a:effectLst>
          <a:scene3d>
            <a:camera prst="orthographicFront">
              <a:rot lat="0" lon="0" rev="0"/>
            </a:camera>
            <a:lightRig rig="threePt" dir="t">
              <a:rot lat="0" lon="0" rev="1200000"/>
            </a:lightRig>
          </a:scene3d>
          <a:sp3d>
            <a:bevelT w="63500" h="25400"/>
          </a:sp3d>
        </a:effectStyle>
      </a:effectStyleLst>
      <a:bgFillStyleLst>
        <a:solidFill>
          <a:schemeClr val="phClr"/>
        </a:solidFill>
        <a:gradFill rotWithShape="1">
          <a:gsLst>
            <a:gs pos="0">
              <a:schemeClr val="phClr">
                <a:tint val="40000"/>
                <a:satMod val="350000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="40000">
              <a:schemeClr val="phClr">
                <a:tint val="45000"/>
                <a:shade val="99000"/>
                <a:satMod val="350000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="100000">
              <a:schemeClr val="phClr">
                <a:shade val="20000"/>
                <a:satMod val="255000"/>
              </a:schemeClr>
            </a:gs>
          </a:gsLst>
          <a:path path="circle">
            <a:fillToRect l="50000" t="-80000" r="50000" b="180000"/>
          </a:path>
        </a:gradFill>
        <a:gradFill rotWithShape="1">
          <a:gsLst>
            <a:gs pos="0">
              <a:schemeClr val="phClr">
                <a:tint val="80000"/>
                <a:satMod val="300000"/>
              </a:schemeClr>
            </a:gs>
            <a:gs pos="100000">
              <a:schemeClr val="phClr">
                <a:shade val="30000"/>
                <a:satMod val="200000"/>
              </a:schemeClr>
            </a:gs>
          </a:gsLst>
          <a:path path="circle">
            <a:fillToRect l="50000" t="50000" r="50000" b="50000"/>
          </a:path>
        </a:gradFill>
      </a:bgFillStyleLst>
    </a:fmtScheme>
  </a:themeElements>
  <a:objectDefaults>
    <a:spDef>
      <a:spPr/>
      <a:bodyPr/>
      <a:lstStyle/>
      <a:style>
        <a:lnRef idx="1">
          <a:schemeClr val="accent1"/>
        </a:lnRef>
        <a:fillRef idx="3">
          <a:schemeClr val="accent1"/>
        </a:fillRef>
        <a:effectRef idx="2">
          <a:schemeClr val="accent1"/>
        </a:effectRef>
        <a:fontRef idx="minor">
          <a:schemeClr val="lt1"/>
        </a:fontRef>
      </a:style>
    </a:spDef>
    <a:lnDef>
      <a:spPr/>
      <a:bodyPr/>
      <a:lstStyle/>
      <a:style>
        <a:lnRef idx="2">
          <a:schemeClr val="accent1"/>
        </a:lnRef>
        <a:fillRef idx="0">
          <a:schemeClr val="accent1"/>
        </a:fillRef>
        <a:effectRef idx="1">
          <a:schemeClr val="accent1"/>
        </a:effectRef>
        <a:fontRef idx="minor">
          <a:schemeClr val="tx1"/>
        </a:fontRef>
      </a:style>
    </a:lnDef>
  </a:objectDefaults>
  <a:extraClrSchemeLst/>
</a:theme>
)ooxml";

std::string serialize(const pugi::xml_document& doc) {
    std::string out(kDeclaration);
    StringWriter writer(out);
    doc.save(writer, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
    return out;
}

std::string stylesXml(std::string_view family, int halfPoints) {
    // word.py sets Normal's font name and size, no space after and single line spacing:
    // python-docx adds the w:pPr and then the w:rPr after the style's w:rsid.
    pugi::xml_document doc = parse(embedded::documents_word_styles);
    pugi::xml_node normal =
        doc.child("w:styles").find_child_by_attribute("w:style", "w:styleId", "Normal");
    if (!normal) {
        throw std::logic_error("The Word template has no Normal style");
    }
    pugi::xml_node spacing = normal.append_child("w:pPr").append_child("w:spacing");
    spacing.append_attribute("w:after").set_value("0");
    spacing.append_attribute("w:line").set_value("240");
    spacing.append_attribute("w:lineRule").set_value("auto");
    pugi::xml_node properties = normal.append_child("w:rPr");
    pugi::xml_node fonts = properties.append_child("w:rFonts");
    fonts.append_attribute("w:ascii").set_value(family);
    fonts.append_attribute("w:hAnsi").set_value(family);
    properties.append_child("w:sz").append_attribute("w:val").set_value(halfPoints);
    return serialize(doc);
}

std::string coreXml(std::string_view title, std::string_view comments) {
    pugi::xml_document doc = parse(kCore);
    const pugi::xml_node root = doc.child("cp:coreProperties");
    root.child("dc:title").text().set(title);
    root.child("dc:description").text().set(comments);
    return serialize(doc);
}

std::string contentTypesXml(bool png) {
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("Types");
    root.append_attribute("xmlns").set_value(kContentTypesNs);
    const auto addDefault = [&](const char* extension, const char* type) {
        pugi::xml_node node = root.append_child("Default");
        node.append_attribute("Extension").set_value(extension);
        node.append_attribute("ContentType").set_value(type);
    };
    const auto addOverride = [&](const char* part, const char* type) {
        pugi::xml_node node = root.append_child("Override");
        node.append_attribute("PartName").set_value(part);
        node.append_attribute("ContentType").set_value(type);
    };
    // python-docx sorts both lists.
    addDefault("jpeg", "image/jpeg");
    if (png) {
        addDefault("png", "image/png");
    }
    addDefault("rels", "application/vnd.openxmlformats-package.relationships+xml");
    addDefault("xml", "application/xml");
    addOverride("/customXml/itemProps1.xml",
                "application/vnd.openxmlformats-officedocument.customXmlProperties+xml");
    addOverride("/docProps/app.xml",
                "application/vnd.openxmlformats-officedocument.extended-properties+xml");
    addOverride("/docProps/core.xml", "application/vnd.openxmlformats-package.core-properties+xml");
    addOverride("/word/document.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
    addOverride("/word/fontTable.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.fontTable+xml");
    addOverride("/word/numbering.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml");
    addOverride("/word/settings.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml");
    addOverride("/word/styles.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml");
    addOverride("/word/stylesWithEffects.xml", "application/vnd.ms-word.stylesWithEffects+xml");
    addOverride("/word/theme/theme1.xml",
                "application/vnd.openxmlformats-officedocument.theme+xml");
    addOverride("/word/webSettings.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.webSettings+xml");
    return serialize(doc);
}

std::string packageRelationshipsXml() {
    // The template's ids, in python-docx's order.
    pugi::xml_document doc;
    const pugi::xml_node root = relationshipsRoot(doc);
    addRelationship(root, "rId3",
                    "http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
                    "core-properties",
                    "docProps/core.xml", false);
    addRelationship(root, "rId4",
                    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
                    "extended-properties",
                    "docProps/app.xml", false);
    addRelationship(root, "rId1",
                    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
                    "officeDocument",
                    "word/document.xml", false);
    addRelationship(root, "rId2",
                    "http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
                    "thumbnail",
                    "docProps/thumbnail.jpeg", false);
    return serialize(doc);
}

std::string customXmlRelationshipsXml() {
    pugi::xml_document doc;
    addRelationship(relationshipsRoot(doc), "rId1",
                    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
                    "customXmlProps",
                    "itemProps1.xml", false);
    return serialize(doc);
}

std::string documentRelationshipsXml(const std::vector<Relationship>& added) {
    pugi::xml_document doc;
    const pugi::xml_node root = relationshipsRoot(doc);
    // The template's ids, in python-docx's order.
    addRelationship(root, "rId3", kStylesRelationship, "styles.xml", false);
    addRelationship(root, "rId4", kStylesWithEffectsRelationship, "stylesWithEffects.xml", false);
    addRelationship(root, "rId5", kSettingsRelationship, "settings.xml", false);
    addRelationship(root, "rId6", kWebSettingsRelationship, "webSettings.xml", false);
    addRelationship(root, "rId7", kFontTableRelationship, "fontTable.xml", false);
    addRelationship(root, "rId8", kThemeRelationship, "theme/theme1.xml", false);
    addRelationship(root, "rId1", kCustomXmlRelationship, "../customXml/item1.xml", false);
    addRelationship(root, "rId2", kNumberingRelationship, "numbering.xml", false);
    for (const Relationship& relationship : added) {
        addRelationship(root, relationship.id, relationship.type, relationship.target,
                        relationship.external);
    }
    return serialize(doc);
}

int firstFreeRelationship() {
    return 9;
}

void writePackage(const std::filesystem::path& path, std::string_view document,
                  const std::vector<Relationship>& relationships, std::string_view styles,
                  std::string_view core, const std::vector<Media>& media) {
    archive::ZipWriter zip = archive::ZipWriter::create(path, {.zip64 = false});
    const auto add = [&](std::string_view name, std::string_view part) {
        zip.add_bytes(name, bytes(part), archive::Method::Deflated, kEntryAttributes);
    };
    // python-docx's order: the template's parts, the pictures, then the thumbnail.
    add("[Content_Types].xml", contentTypesXml(!media.empty()));
    add("_rels/.rels", packageRelationshipsXml());
    add("docProps/core.xml", core);
    add("docProps/app.xml", kAppXml);
    add("word/document.xml", document);
    add("word/_rels/document.xml.rels", documentRelationshipsXml(relationships));
    add("word/styles.xml", styles);
    add("word/stylesWithEffects.xml", embedded::documents_word_styles_with_effects);
    add("word/settings.xml", kSettingsXml);
    add("word/webSettings.xml", kWebSettingsXml);
    add("word/fontTable.xml", kFontTableXml);
    add("word/theme/theme1.xml", kThemeXml);
    add("customXml/item1.xml", kCustomXmlItem);
    add("customXml/_rels/item1.xml.rels", customXmlRelationshipsXml());
    add("customXml/itemProps1.xml", kCustomXmlItemProps);
    add("word/numbering.xml", serialize(parse(embedded::documents_word_numbering)));
    for (const Media& image : media) {
        zip.add_bytes("word/media/" + image.name, image.bytes, archive::Method::Deflated,
                      kEntryAttributes);
    }
    add("docProps/thumbnail.jpeg", embedded::documents_word_thumbnail);
    zip.close();
}

} // namespace fv::documents::ooxml
