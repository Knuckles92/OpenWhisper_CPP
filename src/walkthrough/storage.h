#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "base/paths.h"

// Where walkthroughs and annotated screenshots are saved (services/walkthrough/storage.py):
// a folder per walkthrough under <data root>/captures, named for when it started, and
// screenshots taken outside a walkthrough in captures/screenshots, named for when they were
// taken. A second capture in the same second gets "-2", "-3" and so on.
namespace fv::walkthrough {

inline constexpr std::string_view CAPTURES_FOLDER = "captures";
inline constexpr std::string_view SCREENSHOTS_FOLDER = "screenshots";
// The finished document in a capture folder.
inline constexpr std::string_view MARKDOWN_NAME = "walkthrough.md";
inline constexpr std::string_view PDF_NAME = "walkthrough.pdf";
// The same document for Word.
inline constexpr std::string_view DOCX_NAME = "walkthrough.docx";
inline constexpr std::string_view MANIFEST_NAME = "walkthrough.json";

// <data root>/captures.
std::filesystem::path captures_root(const Paths& paths);

// Creates `root` if needed, then claims root/2026-10-21_14-32-05, or the first free "-2", "-3"
// name after it, by creating it: two walkthroughs never share a folder, and a file of that
// name is passed over. Throws std::filesystem::filesystem_error when root cannot be created.
std::filesystem::path new_capture_folder(const std::filesystem::path& root,
                                         std::chrono::local_seconds started_at);

// A free path root/screenshots/2026-10-21_14-32-05.png, or the first free "-2", "-3" name
// after it. The folder is created, the file is not: the caller saves the image there.
std::filesystem::path new_screenshot_path(const std::filesystem::path& root,
                                          std::chrono::local_seconds taken_at);

// "2026-10-21_14-32-05" (strftime "%Y-%m-%d_%H-%M-%S").
std::string stamp(std::chrono::local_seconds when);

} // namespace fv::walkthrough
