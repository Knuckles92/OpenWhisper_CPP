#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace fv {

// The palette the release notes are coloured from: token name (without "@") -> stylesheet
// value, as ui_theme's Palette holds them. Only the rendered style strings are resolved, never
// the notes, which carry "@user" mentions.
struct StyleTable {
    std::map<std::string, std::string, std::less<>> tokens;

    // Every "@token" in text substituted. Throws std::invalid_argument for a token the table
    // does not hold ("Unknown palette token '@x'").
    std::string resolve(std::string_view text) const;
};

// Qt rich text for a GitHub release body (UTF-8 Markdown), or "" when nothing survives. Handles
// the subset the notes use (headings, bullet and numbered lists, rules, fenced code, inline
// code, http(s) links, bold, italic) with inline styles Qt supports. Checksum sections, bare
// digest lines, "Full Changelog" footers and HTML comments are dropped; all text is escaped as
// Python's html.escape does (quotes included). Input beyond 6,000 code points is cut at the
// last line break before the limit.
std::string render_release_notes_html(std::string_view notes, const StyleTable& style);

} // namespace fv
