#pragma once

#include <QColor>
#include <QPageLayout>
#include <QSizeF>
#include <QString>
#include <QTextBlock>

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include "documents/composer.h"
#include "walkthrough/document.h"

class QTextList;

// Free-form Markdown, such as an agent's write-up, laid out as a paginated QTextDocument in the
// walkthrough's look (services/walkthrough/document.py:1041-1275). Qt's own Markdown reader does
// the structure; this gives it the walkthrough's font, line spacing and screenshot sizing and
// framing, or the colours and font the write-up's Look asks for. Affinity: as composer.h.
namespace fv::documents {

// Space above a heading of levels 1 to 3, in points; other levels get kOtherHeadingSpace, and the
// first block has none.
inline constexpr std::array<double, 3> kHeadingSpace{0.0, 18.0, 14.0};
inline constexpr double kOtherHeadingSpace = 12.0;
// Space between a heading and what it heads, in points.
inline constexpr double kHeadingGap = 5.0;
inline constexpr int kBodyLineHeight = 140;

// The number ("2.", "b.") or bullet Qt would draw before `block`.
QString listMarker(const QTextList& list, const QTextBlock& block);
// The block's text without its images, stripped.
QString words(const QTextBlock& block);
// The colour `name` reads as, or nullopt for "" and what QColor can't read (logged).
std::optional<QColor> color(std::string_view name);
// Colours the block's words; links, which have their own colour, keep it.
void colorWords(const QTextBlock& block, const QColor& color);

class MarkdownComposer {
public:
    // `page` is the text area in layout units; images are read from `folder`.
    MarkdownComposer(QSizeF page, int dpi, std::filesystem::path folder,
                     walkthrough::Look look = {});

    // `text` laid out on pages; frames say where its screenshots ended up.
    ComposedDocument compose(std::string_view text) const;
    // `text`'s blocks, not yet fitted to pages, and its screenshots with the words that lead
    // into them.
    ComposedDocument layout(std::string_view text) const;

private:
    QSizeF page_;
    double unit_; // layout units per point
    std::filesystem::path folder_;
    std::optional<QColor> textColor_;
    std::optional<QColor> headingColor_;
    QString font_;
};

// `text` on `layout`'s page at the layout DPI, paginated.
ComposedDocument composeMarkdown(std::string_view text, const std::filesystem::path& folder,
                                 const walkthrough::Look& look, const QPageLayout& layout);

} // namespace fv::documents
