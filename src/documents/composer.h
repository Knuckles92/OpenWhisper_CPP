#pragma once

#include <QMarginsF>
#include <QPageLayout>
#include <QRectF>
#include <QSizeF>
#include <QTextDocument>

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "walkthrough/document.h"

// A walkthrough laid out as a paginated QTextDocument, the page the PDF paints and the Word
// writer copies (services/walkthrough/document.py:365-415, 734-870). The document measures in
// layout units, `dpi` to the inch; styles give lengths in points.
//
// Affinity: any thread, once the QGuiApplication exists (for its fonts). Uses QImage, QPainter,
// QTextDocument and QPdfWriter only, never QPixmap or a widget, so documents are made on
// worker threads. Each call builds and deletes its own QTextDocument on the calling thread.
namespace fv::documents {

// About 0.7 inch around the text, a little more at the sides.
inline constexpr QMarginsF kMarginsIn{0.75, 0.7, 0.75, 0.7};
// The tallest a screenshot is shown, as a share of the text area's height.
inline constexpr double kMaxImageHeight = 0.62;
// The most a screenshot shrinks to keep its section on one page.
inline constexpr double kMinImageScale = 0.5;
inline constexpr std::array<std::string_view, 3> kFontFamilies{"Segoe UI", "Noto Sans",
                                                               "DejaVu Sans"};
// A hairline around each screenshot, so a light one keeps its edge on white.
inline constexpr std::string_view kFrameColor = "#dcd5cb";
inline constexpr double kFrameWidthPt = 0.6;
// Layout units of give at page edges, where layout positions are rounded.
inline constexpr double kSlack = 0.5;
// In a write-up: a list marker's room in its item's indent, code shading and how far a quote's
// bar stands off its words; points.
inline constexpr double kListHangPt = 16.0;
inline constexpr std::string_view kCodeFill = "#f5f1eb";
inline constexpr double kCodePadPt = 4.0;
inline constexpr double kQuoteGapPt = 9.0;

// How one kind of paragraph looks; lengths in points.
struct Style {
    double size;
    std::string_view color;
    bool bold = false;
    double spaceBefore = 0.0;
    int lineHeight = 100; // percent of single spacing
    bool centered = false;
};

inline constexpr std::string_view kInk = "#1d1b19";
inline constexpr std::string_view kMuted = "#6b645c";
inline constexpr Style kTitleStyle{22, kInk, true};
inline constexpr Style kMetaStyle{9.5, kMuted, false, 3};
inline constexpr Style kAboutStyle{9.5, kMuted, false, 1};
// A section's first paragraph; the space above it sets sections apart.
inline constexpr Style kWordsStyle{11, kInk, false, 16, 145};
inline constexpr Style kMoreWordsStyle{11, kInk, false, 6, 145};
inline constexpr Style kImageStyle{11, kInk, false, 6, 100, true};
// A screenshot with no words above it starts its section.
inline constexpr Style kLoneImageStyle{11, kInk, false, kWordsStyle.spaceBefore, 100, true};
inline constexpr Style kCaptionStyle{8.5, "#8a8278", false, 3, 100, true};

// One screenshot section's blocks, which belong on one page (_Group).
struct Group {
    int first;                // its first block; the title's for the section under the header
    std::optional<int> image; // the screenshot's block; nullopt when it could not be read
    int last;                 // the caption's block, or the screenshot's without captions

    friend bool operator==(const Group&, const Group&) = default;
};

// A laid-out document, its screenshot sections and, once paginated, where each screenshot that
// could be read ended up, in document coordinates (one frame per group with an image).
struct ComposedDocument {
    std::unique_ptr<QTextDocument> doc;
    std::vector<Group> groups;
    std::vector<QRectF> frames;
};

// Lays a walkthrough out (_Composer). `page` is the text area in layout units.
class Composer {
public:
    Composer(QSizeF page, int dpi, walkthrough::DocumentFormat format = {});

    // The walkthrough laid out on pages; frames say where its screenshots ended up.
    ComposedDocument compose(const walkthrough::Walkthrough& walkthrough) const;
    // The walkthrough's blocks, not yet fitted to pages, and its screenshot sections (what the
    // Word writer copies).
    ComposedDocument layout(const walkthrough::Walkthrough& walkthrough) const;

private:
    QSizeF page_;
    double unit_; // layout units per point
    walkthrough::DocumentFormat format_;
};

// The walkthrough on `layout`'s page at the layout DPI, paginated.
ComposedDocument compose(const walkthrough::Walkthrough& walkthrough,
                         const walkthrough::DocumentFormat& format, const QPageLayout& layout);

} // namespace fv::documents
