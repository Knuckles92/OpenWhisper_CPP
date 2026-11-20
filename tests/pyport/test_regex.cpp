// Direct tests of pyport/regex.h: the Match API and Python's syntax over PCRE2.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pyport/regex.h"

using namespace fv::py;

TEST_CASE("match positions are code point indices", "[pyport]") {
    const Regex re(R"(\w+)");
    const auto m = re.search("\xf0\x9f\x98\x80 \xc3\xa9t\xc3\xa9", 0);
    REQUIRE(m.has_value());
    CHECK(m->start() == 2);
    CHECK(m->end() == 5);
    CHECK(m->byte_start() == 5);
    CHECK(m->byte_end() == 10);
    CHECK(m->group() == "\xc3\xa9t\xc3\xa9");
    CHECK(re.search("ab cd", 2)->group() == "cd"); // pos counts code points
}

TEST_CASE("groups, names and expand", "[pyport]") {
    const Regex re(R"((?P<key>\w+)=(\d+)?)");
    REQUIRE(re.group_count() == 2);
    REQUIRE(re.group_index().at("key") == 1);
    const auto m = re.match("name=");
    REQUIRE(m.has_value());
    CHECK(m->group("key") == "name");
    CHECK(!m->matched(2));
    CHECK(m->group_if(2) == std::nullopt);
    CHECK(m->group(2).empty());
    CHECK(m->groups() == std::vector<std::optional<std::string>>{"name", std::nullopt});
    CHECK(m->expand(R"(\g<key>:\2!)") == "name:!");
    CHECK_THROWS_AS(m->group(5), IndexError);
}

TEST_CASE("a match outlives its regex and its text", "[pyport]") {
    std::optional<Match> m;
    {
        const Regex re("(b)");
        std::string text = "abc";
        m = re.search(text);
        text = "zzz";
    }
    REQUIRE(m.has_value());
    CHECK(m->group(1) == "b");
    CHECK(m->expand(R"([\1])") == "[b]");
}

TEST_CASE("match and fullmatch anchor as Python's", "[pyport]") {
    const Regex re("a|ab");
    CHECK(re.match("abc")->group() == "a");
    CHECK(re.fullmatch("ab")->group() == "ab");
    CHECK(!re.fullmatch("abc").has_value());
    CHECK(!re.match("cab").has_value());
}

TEST_CASE("finditer, sub with a function, subn and split", "[pyport]") {
    const Regex re(R"(\d+)");
    const auto all = re.finditer("a1b22c333");
    REQUIRE(all.size() == 3);
    CHECK(all[2].start() == 6);
    CHECK(re.sub([](const Match& m) { return "<" + m.group() + ">"; }, "a1b22") == "a<1>b<22>");
    CHECK(re.subn("#", "a1b22c333", 2) == std::pair<std::string, std::size_t>{"a#b#c333", 2});
    CHECK(re.split("a1b22c", 1) == std::vector<std::optional<std::string>>{"a", "b22c"});
    CHECK(Regex("x*").findall("axb") == std::vector<std::string>{"", "x", "", ""});
    CHECK_THROWS_AS(Regex("(a)(b)").findall("ab"), RegexError);
    CHECK(Regex("(a)(b)").findall_groups("abab") ==
          std::vector<std::vector<std::string>>{{"a", "b"}, {"a", "b"}});
}

TEST_CASE("Python's escapes and classes are translated", "[pyport]") {
    // U+0301 is a combining mark: \w in PCRE2 10.43+ but not in Python.
    CHECK(Regex(R"([^\w]+)").split("cafe\xcc\x81 x") ==
          std::vector<std::optional<std::string>>{"cafe", "x"});
    CHECK(Regex(R"(\s)").search("\x1c").has_value()); // Python's \s includes U+001C
    CHECK(Regex(R"(\d)").search("\xc2\xb2") == std::nullopt);
    CHECK(Regex(R"(\u00e9\U0001F600\x41)")
              .search("\xc3\xa9\xf0\x9f\x98\x80"
                      "A")
              .has_value());
    CHECK(Regex(R"(a\Z)").search("a\n") == std::nullopt);
    CHECK(Regex(R"(a$)").search("a\n").has_value());
    CHECK(Regex(R"(\bx\b)").sub("y", "x xx \xc3\xa9x x") == "y xx \xc3\xa9x y");
    CHECK(Regex("A", IGNORECASE).search("a").has_value());
    CHECK(Regex("(?im)^b").search("a\nB").has_value());
}

TEST_CASE("patterns with several word classes stay within PCRE2's size limit", "[pyport]") {
    // Each \w becomes a class of about 750 ranges; release_notes.py's emphasis pattern has two.
    const Regex emphasis(R"((?<![\w*])\*(\S(?:[^*]*\S)?)\*(?![\w*]))");
    CHECK(emphasis.sub(R"(<i>\1</i>)", "a *b c* d*e*") == "a <i>b c</i> d*e*");
    CHECK_NOTHROW(Regex(R"(\w+\W+\w+\W+\w+\W+\w+\W+\w+\W+\w+\b)"));
}

TEST_CASE("patterns and templates Python refuses are refused", "[pyport]") {
    CHECK_THROWS_AS(Regex(R"(\q)"), RegexError);
    CHECK_THROWS_AS(Regex("(unclosed"), RegexError);
    CHECK_THROWS_AS(Regex(R"(\x4)"), RegexError);
    CHECK_THROWS_AS(Regex(R"(\N{BULLET})"), RegexError); // not supported, see Regex
    const Regex re("(a)");
    CHECK_THROWS_AS(re.sub(R"(\2)", "zzz"), RegexError); // checked before matching
    CHECK_THROWS_AS(re.sub(R"(\g<name>)", "a"), IndexError);
}

TEST_CASE("errors quote what Python's re quotes", "[pyport]") {
    const auto message = [](auto&& make) {
        try {
            make();
        } catch (const RegexError& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    CHECK(message([] { Regex(R"([\777])"); }) ==
          R"(octal escape value \777 outside of range 0-0o377)");
    CHECK(message([] { Regex(R"(\477)"); }) ==
          R"(octal escape value \477 outside of range 0-0o377)");
    const Regex re("(a)");
    CHECK(message([&] { re.sub(R"(\477)", "a"); }) ==
          R"(octal escape value \477 outside of range 0-0o377)");
    // int(name) is quoted whole, without its leading zeros.
    CHECK(message([&] { re.sub(R"(\g<0123>)", "a"); }) == "invalid group reference 123");
    CHECK(re.sub(R"(\g<01>)", "a") == "a");
    CHECK(message([] { Regex("(?P>n)"); }) == "unknown extension ?P>");
    CHECK(message([] { Regex("(?\xc3\xa9)"); }) == "unknown extension ?\xc3\xa9");
}

TEST_CASE("IGNORECASE leaves \\w and \\W to each character's own category", "[pyport]") {
    // U+0345 is Mn (not \w) though it case-folds to iota; sre never folds a category test.
    CHECK(!Regex(R"(\w)", IGNORECASE).search("\xcd\x85").has_value());
    CHECK(Regex(R"(\W)", IGNORECASE).search("\xcd\x85").has_value());
    CHECK(!Regex(R"(\W)", IGNORECASE).search("\xce\xb9").has_value());
    CHECK(Regex(R"(\w+)", IGNORECASE).search("Ab")->group() == "Ab");
}

TEST_CASE("regex_escape escapes what re.escape does", "[pyport]") {
    CHECK(regex_escape("a.b*c d\xc3\xa9_-") == "a\\.b\\*c\\ d\xc3\xa9_\\-");
}
