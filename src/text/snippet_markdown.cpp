#include "text/snippet_markdown.h"

#include <array>
#include <cstddef>
#include <format>
#include <vector>

#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::text {

namespace {

const py::Regex& list_item() {
    static const py::Regex pattern(
        R"(^ {0,3}(?:(?P<bullet>[-*+])|(?P<number>\d{1,9})[.)])[ \t]+(?P<body>.*)$)");
    return pattern;
}

struct ListGroups {
    std::size_t bullet, number, body;
};

const ListGroups& list_groups() {
    static const ListGroups indices = [] {
        const auto& names = list_item().group_index();
        return ListGroups{names.at("bullet"), names.at("number"), names.at("body")};
    }();
    return indices;
}

// The inline alternatives in priority order: an escaped marker, [label](href), a bare web
// address (without trailing punctuation), **bold**, __bold__, *italic*, _italic_.
const py::Regex& inline_pattern() {
    static const py::Regex pattern(R"re(\\(?P<escaped>[\\`*_\[\]()#+\-.!]))re"
                                   R"re(|\[(?P<label>[^\]\n]+)\]\((?P<href>[^)\s]+)\))re"
                                   R"re(|(?P<url>https?://[^\s<>\"]*[^\s<>\".,;:!?)\]']))re"
                                   R"re(|\*\*(?P<bold>(?!\s).+?(?<!\s))\*\*)re"
                                   R"re(|(?<!\w)__(?P<bold_>(?!\s).+?(?<!\s))__(?!\w))re"
                                   R"re(|\*(?P<italic>(?![\s*])[^*\n]+?(?<!\s))\*)re"
                                   R"re(|(?<!\w)_(?P<italic_>(?![\s_])[^_\n]+?(?<!\s))_(?!\w))re");
    return pattern;
}

struct InlineGroups {
    std::size_t escaped, label, href, url, bold, bold_, italic, italic_;
};

const InlineGroups& groups() {
    static const InlineGroups indices = [] {
        const auto& names = inline_pattern().group_index();
        return InlineGroups{names.at("escaped"), names.at("label"),  names.at("href"),
                            names.at("url"),     names.at("bold"),   names.at("bold_"),
                            names.at("italic"),  names.at("italic_")};
    }();
    return indices;
}

std::string escape(std::string_view text, bool quote = true) {
    return py::html_escape(text, quote);
}

std::string safe_href(const std::string& href) {
    static constexpr std::array<std::string_view, 4> kSafeSchemes = {"http://", "https://",
                                                                     "mailto:", "tel:"};
    const std::string lowered = py::lower(href);
    for (const std::string_view scheme : kSafeSchemes) {
        if (lowered.starts_with(scheme)) {
            return href;
        }
    }
    if (lowered.starts_with("www.")) {
        return "https://" + href;
    }
    return {};
}

// The group that took part of `first` or else `second` (Python's `m[a] or m[b]`).
std::string either(const py::Match& match, std::size_t first, std::size_t second) {
    return match.matched(first) ? match.group(first) : match.group(second);
}

std::string inline_html(std::string_view text) {
    const InlineGroups& g = groups();
    std::string out;
    std::size_t cursor = 0;
    for (const py::Match& match : inline_pattern().finditer(text)) {
        out += escape(text.substr(cursor, match.byte_start() - cursor), false);
        cursor = match.byte_end();
        if (match.matched(g.escaped)) {
            out += escape(match.group(g.escaped), false);
        } else if (match.matched(g.label)) {
            const std::string href = safe_href(match.group(g.href));
            if (!href.empty()) {
                out += std::format(R"(<a href="{}">{}</a>)", escape(href),
                                   inline_html(match.group(g.label)));
            } else {
                out += escape(match.group(), false);
            }
        } else if (match.matched(g.url)) {
            const std::string url = match.group(g.url);
            out += std::format(R"(<a href="{}">{}</a>)", escape(url), escape(url, false));
        } else if (match.matched(g.bold) || match.matched(g.bold_)) {
            out += "<b>" + inline_html(either(match, g.bold, g.bold_)) + "</b>";
        } else {
            out += "<i>" + inline_html(either(match, g.italic, g.italic_)) + "</i>";
        }
    }
    out += escape(text.substr(cursor), false);
    return out;
}

std::string inline_plain(std::string_view text) {
    const InlineGroups& g = groups();
    std::string out;
    std::size_t cursor = 0;
    for (const py::Match& match : inline_pattern().finditer(text)) {
        out += text.substr(cursor, match.byte_start() - cursor);
        cursor = match.byte_end();
        if (match.matched(g.escaped)) {
            out += match.group(g.escaped);
        } else if (match.matched(g.label)) {
            const std::string label = inline_plain(match.group(g.label));
            const std::string href = match.group(g.href);
            if (safe_href(href).empty()) {
                out += match.group();
            } else if (label == href) {
                out += href;
            } else {
                out += label + " (" + href + ")";
            }
        } else if (match.matched(g.url)) {
            out += match.group(g.url);
        } else if (match.matched(g.bold) || match.matched(g.bold_)) {
            out += inline_plain(either(match, g.bold, g.bold_));
        } else {
            out += inline_plain(either(match, g.italic, g.italic_));
        }
    }
    out += text.substr(cursor);
    return out;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::vector<std::string> lines(std::string_view text) {
    return py::split_on(replace_all(replace_all(std::string(text), "\r\n", "\n"), "\r", "\n"),
                        "\n");
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

} // namespace

std::string markdown_to_plain(std::string_view text) {
    const ListGroups& g = list_groups();
    std::vector<std::string> out;
    for (const std::string& line : lines(text)) {
        const auto item = list_item().match(line);
        if (!item) {
            out.push_back(inline_plain(line));
        } else {
            out.push_back(line.substr(0, item->byte_start(g.body)) +
                          inline_plain(item->group(g.body)));
        }
    }
    return join(out, "\n");
}

std::string markdown_to_html(std::string_view text) {
    struct Block {
        std::string kind; // "text", "ul" or "ol"
        std::string attributes;
        std::vector<std::string> lines;
    };
    const ListGroups& g = list_groups();
    std::vector<Block> blocks;
    for (const std::string& line : lines(py::strip(text, "\r\n"))) {
        const auto item = list_item().match(line);
        const std::string kind = !item ? "text" : (item->matched(g.bullet) ? "ul" : "ol");
        std::string content = item ? inline_html(item->group(g.body)) : inline_html(line);
        if (!blocks.empty() && blocks.back().kind == kind) {
            blocks.back().lines.push_back(std::move(content));
            continue;
        }
        const std::string first =
            item && item->matched(g.number) ? item->group(g.number) : std::string("1");
        std::string attributes;
        if (first != "1") {
            attributes = std::format(R"( start="{}")", py::py_int(first).value_or(0));
        }
        blocks.push_back({kind, std::move(attributes), {std::move(content)}});
    }
    std::string out;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        Block& block = blocks[index];
        if (block.kind == "text") {
            // Lists are blocks already, so blank lines beside one only add gaps.
            std::vector<std::string>& text_lines = block.lines;
            if (index > 0) {
                while (!text_lines.empty() && py::strip(text_lines.front()).empty()) {
                    text_lines.erase(text_lines.begin());
                }
            }
            if (index + 1 < blocks.size()) {
                while (!text_lines.empty() && py::strip(text_lines.back()).empty()) {
                    text_lines.pop_back();
                }
            }
            out += join(text_lines, "<br>");
        } else {
            out += "<" + block.kind + block.attributes + ">";
            for (const std::string& item : block.lines) {
                out += "<li>" + item + "</li>";
            }
            out += "</" + block.kind + ">";
        }
    }
    return out;
}

std::string escape_lines(std::string_view text) {
    std::vector<std::string> escaped;
    for (const std::string& line : lines(text)) {
        escaped.push_back(escape(line, false));
    }
    return join(escaped, "<br>");
}

} // namespace fv::text
