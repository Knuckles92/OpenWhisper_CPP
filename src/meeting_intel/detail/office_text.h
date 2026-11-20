#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

// The text python-docx, python-pptx and openpyxl give the knowledge folder, read from the
// package's zip and XML directly. Each throws (archive::BadArchive, std::runtime_error) for a file
// those libraries would refuse; the caller treats any throw as "no text".
// Affinity: thread-safe (no shared state).
namespace fv::meeting_intel::detail {

// _extract_docx: the body's paragraphs (runs and hyperlinks: text, tabs, line breaks), then every
// body table row as its non-empty cells joined by " | " (a cell spanning columns repeats, a
// vertically merged cell shows the cell it continues), all stripped, non-empty, joined by "\n".
std::string extract_docx(const std::filesystem::path& path);
// _extract_pptx: slide by slide in presentation order, the text of every top-level shape that
// has a text frame (paragraphs joined by "\n", a line break as "\v"), stripped, non-empty, joined
// by "\n".
std::string extract_pptx(const std::filesystem::path& path);
// _extract_xlsx: every worksheet's title, then its first `max_rows` rows (rows missing from the
// sheet count) as the non-empty values of their first `max_cols` columns joined by " | ".
// Values are the cached results openpyxl reads with data_only: shared and inline strings,
// numbers as Python prints int or float, booleans, errors, and date-formatted numbers as
// Python's datetime, time or timedelta text.
std::string extract_xlsx(const std::filesystem::path& path, std::size_t max_rows,
                         std::size_t max_cols);

} // namespace fv::meeting_intel::detail
