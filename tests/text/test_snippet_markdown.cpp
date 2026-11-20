// tests/test_personalize_s5_snippets.py, the light Markdown tests; plus the markdown_to_html and
// markdown_to_plain golden vectors of services.snippets.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

#include "golden/golden.h"
#include "text/snippet_markdown.h"

namespace text = fv::text;
namespace golden = fv::golden;

// from
// tests/test_personalize_s5_snippets.py::test_markdown_renders_emphasis_links_breaks_and_lists_and_escapes_the_rest
TEST_CASE("test_markdown_renders_emphasis_links_breaks_and_lists_and_escapes_the_rest",
          "[test_personalize_s5_snippets]") {
    const std::string source = "Hi *there* & **you** <script>\n"
                               "\n"
                               "- one\n"
                               "* [two](https://two.example.com/?a=1&b=2)\n"
                               "3. snake_case_name\n"
                               "4) see https://x.example.com/a_b.\n"
                               "Bye _for now_";
    CHECK(text::markdown_to_html(source) ==
          "Hi <i>there</i> &amp; <b>you</b> &lt;script&gt;"
          R"(<ul><li>one</li><li><a href="https://two.example.com/?a=1&amp;b=2">two</a></li></ul>)"
          R"(<ol start="3"><li>snake_case_name</li>)"
          R"(<li>see <a href="https://x.example.com/a_b">https://x.example.com/a_b</a>.</li></ol>)"
          "Bye <i>for now</i>");
}

// from tests/test_personalize_s5_snippets.py::test_unsafe_links_stay_literal_text
TEST_CASE("test_unsafe_links_stay_literal_text", "[test_personalize_s5_snippets]") {
    const std::string href = GENERATE(
        values<std::string>({"javascript:alert(1)", "file:///etc/passwd", "data:text/html,x"}));
    const std::string html = text::markdown_to_html("[click](" + href + ")");
    CHECK(html.find("<a") == std::string::npos);
    CHECK(html.find("click") != std::string::npos);
}

// from tests/test_personalize_s5_snippets.py::test_www_links_get_https
TEST_CASE("test_www_links_get_https", "[test_personalize_s5_snippets]") {
    CHECK(text::markdown_to_html("[site](www.example.com)") ==
          R"(<a href="https://www.example.com">site</a>)");
}

// from tests/test_personalize_s5_snippets.py::test_escaped_markers_stay_literal
TEST_CASE("test_escaped_markers_stay_literal", "[test_personalize_s5_snippets]") {
    CHECK(text::markdown_to_html(R"(\*not italic\*)") == "*not italic*");
    CHECK(text::markdown_to_plain(R"(\*not italic\*)") == "*not italic*");
}

// from tests/test_personalize_s5_snippets.py::test_plain_text_keeps_list_markers_and_drops_emphasis
TEST_CASE("test_plain_text_keeps_list_markers_and_drops_emphasis",
          "[test_personalize_s5_snippets]") {
    CHECK(text::markdown_to_plain("**Agenda**\n- *one*\n2. [two](https://two.example.com)") ==
          "Agenda\n- one\n2. two (https://two.example.com)");
}

TEST_CASE("escape_lines escapes each line and joins them with breaks", "[snippet_markdown]") {
    CHECK(text::escape_lines("a < b\r\nc & \"d\"\re") == "a &lt; b<br>c &amp; \"d\"<br>e");
    CHECK(text::escape_lines("").empty());
}

TEST_CASE("golden markdown_to_html", "[golden]") {
    golden::for_each("services.snippets", "markdown_to_html", [](const golden::Case& c) {
        CHECK(text::markdown_to_html(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden markdown_to_plain", "[golden]") {
    golden::for_each("services.snippets", "markdown_to_plain", [](const golden::Case& c) {
        CHECK(text::markdown_to_plain(c.arg<std::string>(0)) == c.result<std::string>());
    });
}
