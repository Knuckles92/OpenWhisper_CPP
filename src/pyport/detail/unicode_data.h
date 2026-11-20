#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fv::py::detail {

// Bits of PropertyRecord::flags, in the order tools/gen/unicode_tables.py writes them.
enum : std::uint16_t {
    kAlpha = 1 << 0,
    kDecimal = 1 << 1,
    kDigit = 1 << 2,
    kNumeric = 1 << 3,
    kSpace = 1 << 4,
    kLower = 1 << 5,
    kUpper = 1 << 6,
    kTitle = 1 << 7,
    kCased = 1 << 8,
    kCaseIgnorable = 1 << 9,
    kPrintable = 1 << 10,
    kXidStart = 1 << 11,
    kXidContinue = 1 << 12,
};

struct PropertyRecord {
    std::uint16_t flags;
    std::uint8_t category; // index of the name category_name() returns
};

struct PropertyRun {
    char32_t first;
    std::uint16_t record;
};

struct CaseMapping {
    char32_t cp;
    char32_t mapped[3]; // zero-padded
};

const PropertyRecord& properties(char32_t cp);
const char* category_name(std::uint8_t category);

enum class CaseKind { Lower, Upper, Title, Fold };

// Writes the full mapping of cp into out and returns how many code points it has (1 to 3);
// a code point without a mapping maps to itself.
std::size_t map_case(char32_t cp, CaseKind kind, char32_t out[3]);

// Inclusive code point ranges where pred holds, for building regex classes.
struct CodeRange {
    char32_t first;
    char32_t last;
};
std::span<const CodeRange> word_ranges();    // re's \w
std::span<const CodeRange> decimal_ranges(); // re's \d
std::span<const CodeRange> space_ranges();   // re's \s

} // namespace fv::py::detail
