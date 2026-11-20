#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// meeting/context_folder.py: consent-gated, bounded keyword search of the knowledge folder the
// user chose. Policy lives here so no caller can bypass consent, containment in the folder, the
// caps, or the rule that sg_ ids in file text never reach a model (they would be repaired onto
// the live meeting's evidence).
// Affinity: thread-safe (one text cache under a mutex); blocks on the file system and PDFium,
// never on the GUI thread.
namespace fv::meeting_intel {

inline constexpr std::int64_t kFolderDefaultHitLimit = 10;
inline constexpr std::int64_t kFolderMaxHitLimit = 20;
inline constexpr std::size_t kFolderMaxSnippetChars = 180;
inline constexpr std::size_t kFolderMaxPassageChars = 800;
inline constexpr std::size_t kFolderMaxTotalChars = 4000;
// Bytes: plain text files, and the Office and PDF documents whose text is extracted.
inline constexpr std::uintmax_t kFolderMaxFileBytes = 2 * 1024 * 1024;
inline constexpr std::uintmax_t kFolderMaxDocBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kFolderMaxExtractedChars = 50'000;
// Accepted files a search reads at most.
inline constexpr std::size_t kFolderMaxFilesScanned = 200;
inline constexpr int kFolderMaxPdfPages = 40;
inline constexpr std::size_t kFolderMaxXlsxRows = 200;
inline constexpr std::size_t kFolderMaxXlsxCols = 20;
// Extracted texts kept, oldest inserted dropped first.
inline constexpr std::size_t kFolderMaxCacheEntries = 64;

// {ok, disabled?, text, hits}: with `relative_path`, a passage of at most 800 characters of that
// file (around the query's first token when there is one); otherwise the files that match the
// query's tokens best, as hits {ref "file:<relpath>:<n>", path (POSIX, relative), title, snippet}
// within 4000 characters of text. Never an absolute path. `settings` is read on every call; a
// failed read leaves the search off. `limit` is int()'d (10 when that fails) and clamped to
// 1..20; an infinite float throws py::OverflowError, as Python's int() does.
Json search_context_files(const std::function<Json()>& settings, std::string_view query,
                          std::string_view relative_path = {},
                          const Json& limit = Json(kFolderDefaultHitLimit));
// Drops the extraction cache (a settings change, tests).
void clear_context_folder_cache();
// Whether the user opted in; false (logged by type) when the settings cannot be read.
bool context_folder_enabled(const std::function<Json()>& settings);

namespace detail {

// _query_tokens: every [A-Za-z0-9_]{2,} run, lower-cased.
std::vector<std::string> query_tokens(std::string_view query);
// _score: +8 for each token in the lower-cased path, +min(count, 10) for its occurrences in the
// lower-cased text, +12 when every token is in the path; 0 when no token is anywhere.
int score(std::string_view relpath, std::string_view text, const std::vector<std::string>& tokens);
// _snippet: a clip of max_chars around the first token found (starting max(24, max_chars / 5)
// code points before it), or of the head.
std::string snippet(std::string_view text, const std::vector<std::string>& tokens,
                    std::size_t max_chars);
// _safe_join: the file `relative_path` names inside `root` (resolved), or nullopt for an empty,
// rooted, "~", ".." or dot-part path, a link, or anything that resolves outside the root.
std::optional<std::filesystem::path> safe_join(const std::filesystem::path& root,
                                               std::string_view relative_path);
// _relative_display: the POSIX path of `path` (resolved) below `root`, or nullopt when it is not
// below it or passes through a hidden or skipped folder.
std::optional<std::string> relative_display(const std::filesystem::path& root,
                                            const std::filesystem::path& path);
// _extract_plain over a file's bytes: "" when a NUL is in the first 8 KiB; UTF-8 (a BOM dropped);
// else Latin-1 when at least 85 % of the first 4 KiB is printable ASCII, tab or newline.
std::string extract_plain(std::string_view bytes);
// _extract_text: the file's text by its suffix, sanitized and clipped to 50,000 characters; ""
// for a skipped type or size, a failed extraction, or a PDF without PDFium.
std::string extract_text(const std::filesystem::path& path);

} // namespace detail

} // namespace fv::meeting_intel
