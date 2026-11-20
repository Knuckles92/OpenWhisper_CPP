#include "uimath/release_notes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv {
namespace {

using Lines = std::vector<std::string>;

constexpr std::string_view kMono = "'Cascadia Mono','Consolas','SF Mono',monospace";

// Colours are palette tokens, substituted when the notes are rendered.
constexpr std::string_view kPStyle = "margin:0px 0px 10px 0px;color:@text-body;line-height:140%;";
constexpr std::string_view kHStyle =
    "margin:2px 0px 6px 0px;color:@text-heading;font-size:13px;font-weight:600;";
constexpr std::string_view kUlStyle = "margin:0px 0px 10px 0px;-qt-list-indent:1;";
constexpr std::string_view kLiStyle = "margin:0px 0px 5px 0px;color:@text-body;line-height:140%;";

std::string pre_style() {
    return "margin:0px 0px 10px 0px;background-color:@bg;color:@text-soft;font-family:" +
           std::string(kMono) + ";font-size:12px;white-space:pre-wrap;";
}

std::string code_style() {
    return "background-color:@border;color:@text;font-family:" + std::string(kMono) +
           ";font-size:12px;";
}

constexpr std::string_view kLinkStyle = "color:@accent;text-decoration:none;";

// Code points, as Python's len() counts them.
constexpr std::size_t kInputLimit = 6000;

const py::Regex& token_re() {
    static const py::Regex re(R"(@([a-z][a-z0-9]*(?:-[a-z0-9]+)*))");
    return re;
}
const py::Regex& heading_re() {
    static const py::Regex re(R"(^(#{1,6})\s+(.+?)\s*#*$)");
    return re;
}
const py::Regex& bullet_re() {
    static const py::Regex re(R"(^[-*+]\s+(.*)$)");
    return re;
}
const py::Regex& ordered_re() {
    static const py::Regex re(R"(^\d+[.)]\s+(.*)$)");
    return re;
}
const py::Regex& rule_re() {
    static const py::Regex re(R"(^(-{3,}|\*{3,}|_{3,})$)");
    return re;
}
const py::Regex& fence_re() {
    static const py::Regex re(R"(^```|^~~~)");
    return re;
}
const py::Regex& link_re() {
    static const py::Regex re(R"(\[([^\]]+)\]\((https?://[^)\s]+)\))");
    return re;
}
const py::Regex& hash_token_re() {
    static const py::Regex re(R"((?<![0-9a-z])[0-9a-f]{32,}(?![0-9a-z]))", py::IGNORECASE);
    return re;
}
// Sections aimed at manual verification rather than at "what changed".
const py::Regex& dropped_section_re() {
    static const py::Regex re(
        R"(^(sha-?\d*|checksums?|hashes|verification|verify(ing)?( the)?( download| files?)?)"
        R"(|artifacts?|assets?|downloads?)\b[:\s]*$)",
        py::IGNORECASE);
    return re;
}
const py::Regex& dropped_line_re() {
    static const py::Regex re(R"(^\s*(\*\*)?full changelog(\*\*)?\s*:|^\s*<!--)", py::IGNORECASE);
    return re;
}
const py::Regex& inline_code_re() {
    static const py::Regex re(R"(`([^`]+)`)");
    return re;
}
const py::Regex& bold_re() {
    static const py::Regex re(R"(\*\*(\S(?:[^*]*\S)?)\*\*)");
    return re;
}
const py::Regex& star_italic_re() {
    static const py::Regex re(R"((?<![\w*])\*(\S(?:[^*]*\S)?)\*(?![\w*]))");
    return re;
}
const py::Regex& underscore_italic_re() {
    static const py::Regex re(R"((?<![\w_])_(\S(?:[^_]*\S)?)_(?![\w_]))");
    return re;
}

bool matches(const py::Regex& re, std::string_view text) {
    return re.match(text).has_value();
}

// Unix line endings, trimmed, and cut at a line break near the input limit.
std::string normalized_text(std::string_view notes) {
    std::string text;
    text.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (notes[i] == '\r') {
            text.push_back('\n');
            if (i + 1 < notes.size() && notes[i + 1] == '\n') {
                ++i;
            }
        } else {
            text.push_back(notes[i]);
        }
    }
    text = py::strip(text);
    if (text.empty()) {
        return text;
    }
    if (py::len(text) > kInputLimit) {
        text = py::slice(text, 0, static_cast<std::int64_t>(kInputLimit));
        // str.rsplit("\n", 1)[0]: everything before the last line break, or all of it.
        if (const std::size_t cut = text.rfind('\n'); cut != std::string::npos) {
            text.resize(cut);
        }
    }
    return text;
}

// A bare digest, optionally with a file name before it.
bool is_checksum_line(std::string_view stripped) {
    if (stripped.empty()) {
        return false;
    }
    return hash_token_re().search(stripped).has_value() && py::split(stripped).size() <= 3;
}

// Checksum sections and lines dropped; fenced code is kept as written.
Lines drop_verification(const Lines& lines) {
    Lines kept;
    std::size_t skip_level = 0;
    bool in_fence = false;
    for (const std::string& line : lines) {
        const std::string stripped = py::strip(line);
        if (!in_fence) {
            if (const std::optional<py::Match> heading = heading_re().match(stripped)) {
                const std::size_t level = py::len(heading->group(1));
                if (skip_level != 0 && level <= skip_level) {
                    skip_level = 0;
                }
                if (matches(dropped_section_re(), py::strip(heading->group(2)))) {
                    skip_level = level;
                    continue;
                }
            }
        }
        if (matches(fence_re(), stripped)) {
            in_fence = !in_fence;
        }
        if (skip_level != 0) {
            continue;
        }
        if (!in_fence && (matches(dropped_line_re(), line) || is_checksum_line(stripped))) {
            continue;
        }
        kept.push_back(line);
    }
    return kept;
}

Lines relevant_lines(std::string_view notes) {
    const std::string text = normalized_text(notes);
    if (text.empty()) {
        return {};
    }
    Lines kept = drop_verification(py::split_on(text, "\n"));
    while (!kept.empty() && py::strip(kept.front()).empty()) {
        kept.erase(kept.begin());
    }
    while (!kept.empty() && py::strip(kept.back()).empty()) {
        kept.pop_back();
    }
    return kept;
}

bool starts_block(std::string_view stripped) {
    return matches(heading_re(), stripped) || matches(bullet_re(), stripped) ||
           matches(ordered_re(), stripped) || matches(rule_re(), stripped) ||
           matches(fence_re(), stripped);
}

std::string inline_html(std::string_view text, const StyleTable& style) {
    std::vector<std::string> spans;
    const std::string code_open = "<code style=\"" + style.resolve(code_style()) + "\">";
    std::string out = inline_code_re().sub(
        [&](const py::Match& match) {
            spans.push_back(code_open + "&nbsp;" + py::html_escape(match.group(1)) +
                            "&nbsp;</code>");
            return std::string(1, '\0') + std::to_string(spans.size() - 1) + std::string(1, '\0');
        },
        text);
    out = py::html_escape(out);
    out = link_re().sub("<a href=\"\\2\" style=\"" + style.resolve(kLinkStyle) + "\">\\1</a>", out);
    out = bold_re().sub("<b>\\1</b>", out);
    out = star_italic_re().sub("<i>\\1</i>", out);
    out = underscore_italic_re().sub("<i>\\1</i>", out);
    for (std::size_t position = 0; position < spans.size(); ++position) {
        const std::string marker =
            std::string(1, '\0') + std::to_string(position) + std::string(1, '\0');
        for (std::size_t at = out.find(marker); at != std::string::npos;
             at = out.find(marker, at + spans[position].size())) {
            out.replace(at, marker.size(), spans[position]);
        }
    }
    return out;
}

// The fenced block opening at index, or "" when it holds only checksums; index moves past its
// closing fence.
std::string render_fence(const Lines& lines, std::size_t& index, const StyleTable& style) {
    const std::size_t total = lines.size();
    ++index;
    Lines buffer;
    while (index < total && !matches(fence_re(), py::strip(lines[index]))) {
        buffer.push_back(lines[index]);
        ++index;
    }
    ++index;
    bool any_body = false;
    bool all_checksums = true;
    for (const std::string& row : buffer) {
        const std::string stripped = py::strip(row);
        if (stripped.empty()) {
            continue;
        }
        any_body = true;
        if (!is_checksum_line(stripped)) {
            all_checksums = false;
        }
    }
    if (!any_body || all_checksums) {
        return "";
    }
    std::string joined;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (i > 0) {
            joined += '\n';
        }
        joined += buffer[i];
    }
    return "<pre style=\"" + style.resolve(pre_style()) + "\">" +
           py::html_escape(py::strip(joined, "\n")) + "</pre>";
}

// The lines from index up to the next blank line or block, joined by spaces.
std::string collect_paragraph(const Lines& lines, std::size_t& index) {
    std::string paragraph = py::strip(lines[index]);
    ++index;
    while (index < lines.size()) {
        const std::string next = py::strip(lines[index]);
        if (next.empty() || starts_block(next)) {
            break;
        }
        paragraph += ' ';
        paragraph += next;
        ++index;
    }
    return paragraph;
}

Lines collect_list(const Lines& lines, std::size_t& index, const py::Regex& pattern) {
    Lines items;
    while (index < lines.size()) {
        const std::string& raw = lines[index];
        const std::string stripped = py::strip(raw);
        if (const std::optional<py::Match> match = pattern.match(stripped)) {
            items.push_back(match->group(1));
            ++index;
            continue;
        }
        // An indented continuation belongs to the item above it.
        if (!items.empty() && !stripped.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
            items.back() += ' ';
            items.back() += stripped;
            ++index;
            continue;
        }
        break;
    }
    return items;
}

std::string blocks_to_html(const Lines& lines, const StyleTable& style) {
    std::string html;
    std::size_t index = 0;
    const std::size_t total = lines.size();
    while (index < total) {
        const std::string stripped = py::strip(lines[index]);
        if (stripped.empty()) {
            ++index;
            continue;
        }
        if (matches(fence_re(), stripped)) {
            html += render_fence(lines, index, style);
            continue;
        }
        if (const std::optional<py::Match> heading = heading_re().match(stripped)) {
            html += "<p style=\"" + style.resolve(kHStyle) + "\">" +
                    inline_html(heading->group(2), style) + "</p>";
            ++index;
            continue;
        }
        if (matches(rule_re(), stripped)) {
            html += "<hr />";
            ++index;
            continue;
        }
        bool listed = false;
        for (const auto& [pattern, tag] :
             {std::pair<const py::Regex*, std::string_view>{&bullet_re(), "ul"},
              {&ordered_re(), "ol"}}) {
            if (!matches(*pattern, stripped)) {
                continue;
            }
            const Lines items = collect_list(lines, index, *pattern);
            std::string rendered;
            for (const std::string& item : items) {
                rendered += "<li style=\"" + style.resolve(kLiStyle) + "\">" +
                            inline_html(item, style) + "</li>";
            }
            html += "<" + std::string(tag) + " style=\"" + std::string(kUlStyle) + "\">" +
                    rendered + "</" + std::string(tag) + ">";
            listed = true;
            break;
        }
        if (!listed) {
            const std::string paragraph = collect_paragraph(lines, index);
            html += "<p style=\"" + style.resolve(kPStyle) + "\">" + inline_html(paragraph, style) +
                    "</p>";
        }
    }
    return html;
}

} // namespace

std::string StyleTable::resolve(std::string_view text) const {
    if (text.find('@') == std::string_view::npos) {
        return std::string(text);
    }
    return token_re().sub(
        [this](const py::Match& match) {
            const std::string name = match.group(1);
            const auto found = tokens.find(name);
            if (found == tokens.end()) {
                throw std::invalid_argument("Unknown palette token '@" + name + "'");
            }
            return found->second;
        },
        text);
}

std::string render_release_notes_html(std::string_view notes, const StyleTable& style) {
    const Lines lines = relevant_lines(notes);
    if (lines.empty()) {
        return "";
    }
    return blocks_to_html(lines, style);
}

} // namespace fv
