#pragma once

#include <string>
#include <vector>

#include <QString>

class QTextDocument;

namespace fv::ui {

// Typography for one rendering context. Colours that depend on the surface behind the text
// travel as palette tokens (or a literal "#hex"): an inline-code chip must contrast with the
// transcript pane and with the reading window, which are different greys.
struct MarkdownStyle {
    double bodyPt;
    int lineHeight;   // percent, proportional
    int paragraphGap; // px
    int indentPx;
    std::string codeBackground;
    std::string quoteBackground;
    std::vector<double> headingScale{1.55, 1.3, 1.15, 1.05, 1.0, 1.0};
    QString family = QStringLiteral("Segoe UI");

    // Point size of a heading level; levels past the table reuse its last scale. Rounded to one
    // decimal as Python's round(x, 1).
    double headingPt(int level) const;
    double codePt() const;
    // The same style with body size (one decimal), paragraph gap (at least 2) and indent (at
    // least 12) multiplied by factor; colours are kept.
    MarkdownStyle scaled(double factor) const;

    // The transcript pane inside a tab (on @surface) and the reading window (on @bg).
    static const MarkdownStyle PREVIEW;
    static const MarkdownStyle READER;
};

// Replaces document's content with text rendered as GitHub-flavoured Markdown, then writes the
// sizes, margins and colours of style (and of currentPalette()) onto the block, character and
// table formats, which Qt's importer leaves at its defaults.
void renderMarkdown(QTextDocument& document, const QString& text, const MarkdownStyle& style);

} // namespace fv::ui
