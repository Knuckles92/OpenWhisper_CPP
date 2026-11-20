// Ported from tests/test_release_notes.py, plus the golden vectors of ui_qt.utils.release_notes
// (made with the dark palette current).
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "base/json.h"
#include "golden/golden.h"
#include "testkit/testkit.h"
#include "uimath/release_notes.h"

namespace {

// The dark palette fv_ui_theme resolves with: uimath cannot link the Qt library, so the replay
// reads the same generated data file.
const fv::StyleTable& darkStyle() {
    static const fv::StyleTable table = [] {
        std::ifstream file(fv::testkit::source_root() / "src" / "ui_theme" / "data" /
                           "palettes.json");
        if (!file) {
            throw std::runtime_error("src/ui_theme/data/palettes.json is missing");
        }
        std::stringstream text;
        text << file.rdbuf();
        const fv::Json document = fv::Json::parse(text.str());
        fv::StyleTable style;
        for (const auto& [token, value] : document.at("palettes").at("dark").items()) {
            style.tokens.emplace(token, value.get<std::string>());
        }
        return style;
    }();
    return table;
}

std::string render(std::string_view notes) {
    return fv::render_release_notes_html(notes, darkStyle());
}

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t found = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("render_release_notes_html replays the golden vectors", "[golden][release_notes]") {
    fv::golden::for_each("ui_qt.utils.release_notes", "render_release_notes_html",
                         [](const fv::golden::Case& c) {
                             // None stands for "", as Python's (notes or "") reads it.
                             const auto notes = c.arg<std::optional<std::string>>(0);
                             CHECK(render(notes.value_or("")) == c.result<std::string>());
                         });
}

TEST_CASE("an unknown style token is an error", "[release_notes]") {
    fv::StyleTable empty;
    CHECK(empty.resolve("no tokens") == "no tokens");
    CHECK_THROWS_AS(empty.resolve("color:@text;"), std::invalid_argument);
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_empty_notes_render_nothing
TEST_CASE("empty notes render nothing", "[test_release_notes]") {
    CHECK(render("") == "");
    CHECK(render("   \n\n") == "");
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_bullets_become_a_list
TEST_CASE("bullets become a list", "[test_release_notes]") {
    const std::string html = render("- First item\n- Second item\n");
    CHECK(count(html, "<li") == 2);
    CHECK(contains(html, "First item"));
    CHECK_FALSE(contains(html, "- First item"));
}

// from
// tests/test_release_notes.py::TestRenderReleaseNotes::test_numbered_items_become_an_ordered_list
TEST_CASE("numbered items become an ordered list", "[test_release_notes]") {
    const std::string html = render("1. First\n2. Second\n");
    CHECK(contains(html, "<ol"));
    CHECK(count(html, "<li") == 2);
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_heading_marks_are_not_shown
TEST_CASE("heading marks are not shown", "[test_release_notes]") {
    const std::string html = render("### Highlights\n\nSomething changed.");
    CHECK(contains(html, "Highlights"));
    CHECK_FALSE(contains(html, "###"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_inline_markup_becomes_tags
TEST_CASE("inline markup becomes tags", "[test_release_notes]") {
    const std::string html = render("An **Active** badge, `gemini-3.7`, and *emphasis*.");
    CHECK(contains(html, "<b>Active</b>"));
    CHECK(contains(html, "<code"));
    CHECK(contains(html, "gemini-3.7"));
    CHECK(contains(html, "<i>emphasis</i>"));
    CHECK_FALSE(contains(html, "**"));
    CHECK_FALSE(contains(html, "`"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_links_keep_their_text_and_target
TEST_CASE("links keep their text and target", "[test_release_notes]") {
    const std::string html = render("See [the notes](https://example.com/x).");
    CHECK(contains(html, "href=\"https://example.com/x\""));
    CHECK(contains(html, ">the notes</a>"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_checksum_section_is_dropped
TEST_CASE("checksum section is dropped", "[test_release_notes]") {
    std::string hash;
    for (int i = 0; i < 8; ++i) {
        hash += "e4706b1d";
    }
    const std::string html = render("Real change here.\n\n### SHA-256\n\n```\n" + hash + "\n```\n");
    CHECK(contains(html, "Real change here."));
    CHECK_FALSE(contains(html, "SHA-256"));
    CHECK_FALSE(contains(html, "e4706b1d"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_bare_checksum_lines_are_dropped
TEST_CASE("bare checksum lines are dropped", "[test_release_notes]") {
    std::string digest;
    for (int i = 0; i < 32; ++i) {
        digest += "ab";
    }
    const std::string html = render("Update notes.\n\nFioriVoce-Setup.exe " + digest + "\n");
    CHECK(contains(html, "Update notes."));
    CHECK_FALSE(contains(html, digest));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_full_changelog_footer_is_dropped
TEST_CASE("full changelog footer is dropped", "[test_release_notes]") {
    const std::string html =
        render("A change.\n\n**Full Changelog**: https://example.com/compare\n");
    CHECK(contains(html, "A change."));
    CHECK_FALSE(contains(html, "Full Changelog"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_html_in_notes_is_escaped
TEST_CASE("html in notes is escaped", "[test_release_notes]") {
    const std::string html = render("Use <script>alert(1)</script> carefully");
    CHECK_FALSE(contains(html, "<script>"));
    CHECK(contains(html, "&lt;script&gt;"));
}

// from
// tests/test_release_notes.py::TestRenderReleaseNotes::test_fenced_code_survives_when_it_is_not_a_digest
TEST_CASE("fenced code survives when it is not a digest", "[test_release_notes]") {
    const std::string html = render("Run it:\n\n```\ngit pull --ff-only\n```\n");
    CHECK(contains(html, "<pre"));
    CHECK(contains(html, "git pull --ff-only"));
}

// from tests/test_release_notes.py::TestRenderReleaseNotes::test_paragraph_lines_are_joined
TEST_CASE("paragraph lines are joined", "[test_release_notes]") {
    const std::string html = render("One sentence\nwrapped over lines.\n");
    CHECK(contains(html, "One sentence wrapped over lines."));
    CHECK(count(html, "<p") == 1);
}
