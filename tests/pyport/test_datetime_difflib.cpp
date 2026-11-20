// Direct tests of pyport/datetime.h and pyport/difflib.h.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pyport/datetime.h"
#include "pyport/difflib.h"
#include "pyport/errors.h"

using namespace fv::py;

TEST_CASE("fromisoformat fills the fields", "[pyport]") {
    const DateTime dt = fromisoformat("2026-11-14T10:20:30.5-03:30");
    CHECK(dt.year == 2026);
    CHECK(dt.month == 11);
    CHECK(dt.day == 14);
    CHECK(dt.hour == 10);
    CHECK(dt.microsecond == 500000);
    CHECK(dt.utc_offset_us == -(3 * 3600 + 30 * 60) * 1000000LL);
    CHECK(dt.date() == Date{2026, 11, 14});
    CHECK(fromisoformat("2026-11-14T10:20:30Z").utc_offset_us == 0);
    CHECK(!fromisoformat("2026-11-14").utc_offset_us.has_value());
}

TEST_CASE("isoformat round-trips and honours timespec", "[pyport]") {
    const DateTime dt = fromisoformat("2026-11-14T10:20:30.123456+05:30");
    CHECK(dt.isoformat() == "2026-11-14T10:20:30.123456+05:30");
    CHECK(dt.isoformat(' ', "milliseconds") == "2026-11-14 10:20:30.123+05:30");
    CHECK(dt.isoformat('T', "hours") == "2026-11-14T10+05:30");
    CHECK_THROWS_AS(dt.isoformat('T', "nanoseconds"), ValueError);
    CHECK(date_fromisoformat("2026-W46-6").isoformat() == "2026-11-14");
}

TEST_CASE("invalid strings raise Python's ValueError", "[pyport]") {
    try {
        fromisoformat("bad");
        FAIL("no error");
    } catch (const ValueError& e) {
        CHECK(std::string(e.what()) == "Invalid isoformat string: 'bad'");
    }
    CHECK_THROWS_AS(fromisoformat("2026-02-30"), ValueError);
    CHECK_THROWS_AS(date_fromisoformat("2026-11-14T00"), ValueError);
}

TEST_CASE("SequenceMatcher reproduces difflib", "[pyport]") {
    SequenceMatcher m("qabxcd", "abycdf");
    const std::vector<Opcode> expected = {
        {OpTag::Delete, 0, 1, 0, 0}, {OpTag::Equal, 1, 3, 0, 2},  {OpTag::Replace, 3, 4, 2, 3},
        {OpTag::Equal, 4, 6, 3, 5},  {OpTag::Insert, 6, 6, 5, 6},
    };
    CHECK(m.get_opcodes() == expected);
    CHECK(SequenceMatcher("abcd", "bcde").ratio() == 0.75);
    CHECK(SequenceMatcher("", "").ratio() == 1.0);
    WordSequenceMatcher words({"a", "b", "c"}, {"a", "c"});
    CHECK(words.ratio() == 0.8);
}

TEST_CASE("autojunk ignores popular elements of a long b", "[pyport]") {
    // dictation_stats.py passes autojunk=False; the default drops popular anchors from 200 up,
    // and only the extension of an empty match at the start still finds the first run.
    const std::string a = std::string(100, 'a') + "b" + std::string(100, 'a');
    const std::string b = std::string(100, 'a') + "c" + std::string(100, 'a');
    CHECK(SequenceMatcher(a, b, false).ratio() == 0.9950248756218906);
    CHECK(SequenceMatcher(a, b, true).ratio() == 0.4975124378109453);
    const std::string short_b = std::string(99, 'a') + "c" + std::string(99, 'a');
    CHECK(SequenceMatcher(a, short_b, true).ratio() == SequenceMatcher(a, short_b, false).ratio());
}

TEST_CASE("get_close_matches orders by score then by string", "[pyport]") {
    const std::vector<std::string> words = {"ape", "apple", "peach", "puppy"};
    CHECK(get_close_matches("appel", words) == std::vector<std::string>{"apple", "ape"});
    CHECK_THROWS_AS(get_close_matches("x", words, 0), ValueError);
    CHECK_THROWS_AS(get_close_matches("x", words, 3, 1.5), ValueError);
}
