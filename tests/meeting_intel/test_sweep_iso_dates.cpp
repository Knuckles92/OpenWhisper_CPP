// XLSX cells of type "d" (ISO 8601, written by Excel in strict files) read as openpyxl 3.1.5's
// from_ISO8601 gives them to _extract_xlsx. The fixtures are workbooks whose "d" cells were
// written by hand; the expected text is what OpenWhisper's _extract_xlsx printed for them.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "meeting_intel/detail/office_text.h"
#include "testkit/testkit.h"

namespace {

std::filesystem::path fixture(const char* name) {
    return fv::testkit::source_root() / "tests" / "fixtures" / "meeting_intel" / "iso_dates" / name;
}

} // namespace

TEST_CASE("xlsx ISO date cells read as openpyxl's dates, times and durations", "[office_text]") {
    CHECK(fv::meeting_intel::detail::extract_xlsx(fixture("iso_dates.xlsx"), 100, 20) ==
          "Dates\nLabel\nrow0 | 2024-01-15\nrow1 | 2024-01-15 10:30:00\n"
          "row2 | 2024-01-15 10:30:00.500000\nrow3 | 10:30:00\nrow4 | 10:30:15.123000\n"
          "row5 | 1 day, 1:30:00\nrow6 | 0:00:01.500000\nrow7 | 1:30:00\n"
          "row8 | 2024-01-15 10:30:00\nrow9 | 2 days, 1:00:00.001000");
}

TEST_CASE("an xlsx ISO date that is no date fails the workbook as openpyxl does", "[office_text]") {
    // "2024-02-30": ValueError "day is out of range for month".
    CHECK_THROWS_AS(
        fv::meeting_intel::detail::extract_xlsx(fixture("iso_date_invalid.xlsx"), 100, 20),
        std::runtime_error);
}
