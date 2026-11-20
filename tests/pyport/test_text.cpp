// Direct tests of pyport/text.h and pyport/unicode.h.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

using namespace fv::py;

TEST_CASE("len counts code points, not bytes", "[pyport]") {
    std::string e200;
    for (int i = 0; i < 200; ++i) {
        e200 += "\xc3\xa9"; // é
    }
    CHECK(len(e200) == 200);
    CHECK(e200.size() == 400);
    CHECK(len("\xf0\x9f\x98\x80") == 1); // one astral code point
    CHECK(len("") == 0);
}

TEST_CASE("casefold is full folding", "[pyport]") {
    CHECK(casefold("stra\xc3\x9f"
                   "e") == "strasse");
    CHECK(casefold("\xef\xac\x81") == "fi"); // ﬁ
    CHECK(casefold("\xce\xa3\xcf\x83\xcf\x82") == "\xcf\x83\xcf\x83\xcf\x83");
}

TEST_CASE("lower applies the final sigma rule", "[pyport]") {
    CHECK(lower("\xce\x91\xce\xa3") == "\xce\xb1\xcf\x82");                 // ΑΣ -> ας
    CHECK(lower("\xce\x91\xce\xa3\xce\x91") == "\xce\xb1\xcf\x83\xce\xb1"); // σ inside a word
    CHECK(lower("\xce\xa3") == "\xcf\x83");                                 // alone: not final
    CHECK(lower("\xc4\xb0") == "i\xcc\x87");                                // İ -> i + U+0307
}

TEST_CASE("upper, title and capitalize use the full mappings", "[pyport]") {
    CHECK(upper("stra\xc3\x9f"
                "e") == "STRASSE");
    CHECK(title("hello wORLD") == "Hello World");
    CHECK(title("\xc7\x86"
                "emal") == "\xc7\x85"
                           "emal"); // ǆ -> ǅ
    CHECK(capitalize("hELLO") == "Hello");
    CHECK(capitalize("\xef\xac\x81x") == "Fix"); // ﬁ titlecases to "Fi"
}

TEST_CASE("strip and split use Unicode whitespace", "[pyport]") {
    CHECK(strip("\xc2\xa0x\xe2\x80\x83") == "x");
    CHECK(lstrip("  x ") == "x ");
    CHECK(rstrip("  x ") == "  x");
    CHECK(strip("xxhixx", "x") == "hi");
    CHECK(split("  a b\xe2\x80\x83"
                "c  ") == std::vector<std::string>{"a", "b", "c"});
    CHECK(split("  a b  c  ", 1) == std::vector<std::string>{"a", "b  c  "});
    CHECK(split("") == std::vector<std::string>{});
    CHECK(split_on("a,,b", ",") == std::vector<std::string>{"a", "", "b"});
    CHECK_THROWS_AS(split_on("a", ""), ValueError);
}

TEST_CASE("slice indexes code points", "[pyport]") {
    const std::string s = "a\xf0\x9f\x98\x80"
                          "b";
    CHECK(slice(s, 1, 2) == "\xf0\x9f\x98\x80");
    CHECK(slice(s, -1) == "b");
    CHECK(slice(s, 5, 9).empty());
}

TEST_CASE("the is-predicates follow CPython", "[pyport]") {
    CHECK(isdecimal("\xd9\xa1\xd9\xa2")); // Arabic-Indic digits
    CHECK(!isdecimal("\xc2\xb2"));        // ² is a digit, not decimal
    CHECK(isdigit("\xc2\xb2"));
    CHECK(isnumeric("\xe2\x85\xa7")); // Ⅷ
    CHECK(!isalpha(""));
    CHECK(isprintable(""));
    CHECK(!isprintable("\xc2\xad")); // soft hyphen is Cf
    CHECK(isupper("ABC1"));
    CHECK(!isupper("ABc"));
    CHECK(istitle("Title Case"));
    CHECK(!istitle("TItle"));
    CHECK(isidentifier("_n\xc3\xa4me"));
    CHECK(!isidentifier("1x"));
    CHECK(isspace("\x1c\x1f"));
}

TEST_CASE("html_escape and repr", "[pyport]") {
    CHECK(html_escape("<a href=\"x\">'&'</a>") ==
          "&lt;a href=&quot;x&quot;&gt;&#x27;&amp;&#x27;&lt;/a&gt;");
    CHECK(html_escape("\"'", false) == "\"'");
    CHECK(repr("it's") == "\"it's\"");
    CHECK(repr("a\nb") == "'a\\nb'");
    CHECK(repr("\x7f") == "'\\x7f'");
    CHECK(repr("\xc3\xa9") == "'\xc3\xa9'");
}

TEST_CASE("code points decode leniently and round-trip", "[pyport]") {
    CHECK(to_code_points("a\xff"
                         "b") == std::u32string{U'a', 0xFFFD, U'b'});
    // A lone surrogate in three bytes, as json.loads gives for "\ud83d", is kept.
    CHECK(to_code_points("\xed\xa0\xbd") == std::u32string{0xD83D});
    CHECK(to_utf8(U"\U0001F600") == "\xf0\x9f\x98\x80");
    CHECK(general_category(U'A') == "Lu");
    CHECK(general_category(0x0378) == "Cn");
    CHECK(decimal_value(0x0663) == 3);
    CHECK(decimal_value(U'x') == std::nullopt);
    CHECK(unidata_version == "15.0.0");
}
