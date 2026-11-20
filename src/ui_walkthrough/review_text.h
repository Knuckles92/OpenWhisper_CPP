#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "walkthrough/document.h"

// What the storyboard says about a walkthrough (ui_qt/widgets/walkthrough_review.py:77-93,
// 302-331, 1085-1148): when it started, its header, the write-up version picker and the notices.
// Qt-free; every date and time is local wall time, with English names whatever the locale.
// Affinity: plain functions; started() and modified_at() read the disk, so any thread.
namespace fv::walkthrough_review {

inline constexpr std::string_view SPOKEN_HINT =
    "Fix any words, drag a row to move a screenshot and its words, or switch a screenshot off to "
    "leave it out. Original screenshots are kept.";
inline constexpr std::string_view WRITE_UP_HINT =
    "Fix the write-up's words or drag a row to move a screenshot. Saving rebuilds its PDF and "
    "Word document.";
// What the notice says once Review's work is done, unless the work says otherwise.
inline constexpr std::string_view SAVED = "Saved. Copy it again whenever you're ready.";

// The two versions of a written-up walkthrough, in the switch's order: title, then tooltip.
struct VersionChoice {
    std::string_view title;
    std::string_view tip;
};
inline constexpr VersionChoice VERSIONS[] = {
    {"AI write-up", "What the agent wrote; choose a saved write-up with the version menu"},
    {"As spoken", "Your words as you said them, a section per screenshot"},
};

// When the walkthrough in `folder` started (_started): the loaded document's start, else
// draft.json's started_at, else the first 19 characters of the folder's name as
// %Y-%m-%d_%H-%M-%S, read as Python's strptime reads them; nullopt when none of these says.
// A start with a UTC offset keeps its own wall time.
std::optional<std::chrono::local_seconds> started(const std::filesystem::path& folder,
                                                  const walkthrough::Walkthrough* document);

// "Today", "Yesterday", "Tue, Oct 21" in today's year, else "Oct 21, 2025" (_day).
std::string day(std::chrono::local_seconds when, std::chrono::year_month_day today);
// "3:05 PM": %I:%M %p without its leading zero (_time).
std::string time(std::chrono::local_seconds when);
// "1 screenshot", "3 screenshots" (_shots).
std::string shots(int count);

// When a write-up last changed, as the version menu says it (_written_at): its time today,
// else "Oct 20, 3:40 PM"; "" when it is unknown.
std::string written_at(std::optional<std::chrono::local_seconds> modified,
                       std::chrono::year_month_day today);
// A file's last write in local wall time, or nullopt when it cannot be read.
std::optional<std::chrono::local_seconds> modified_at(const std::filesystem::path& file);
// Today's date on this computer.
std::chrono::year_month_day today();

// What the header shows about the folder on display (_show_header).
struct HeaderInput {
    std::optional<std::filesystem::path> folder; // nullopt: nothing on display
    std::optional<std::chrono::local_seconds> started;
    // The loaded document's sections, true where one has a screenshot, and its length in
    // seconds; nullopt when no document could be loaded.
    std::optional<std::vector<bool>> sections;
    double duration_s = 0;
    bool draft = false;
    bool written = false; // written up
    bool spoken = true;   // the words as spoken show, not the write-up
};

struct Header {
    std::string title;
    bool title_shown = false;
    std::string meta;
    std::string pill;
    std::string pill_tone; // "plain" or "warning"
    bool pill_shown = false;
    bool version_bar_shown = false;
    int version_bar_index = 1; // 0 the write-up, 1 as spoken
    std::string retry;         // "Redo AI write-up" or "Write up with AI"
    std::string style_menu;    // "Redo as" or "Write up as"
    std::string prompt;        // "Redo with a new prompt…" or "Write up with a new prompt…"
    std::string notice;        // the hint, when no work is running
};
Header header(const HeaderInput& input);

// One row of the write-up version menu, newest first.
struct VersionRow {
    std::optional<int> version; // nullopt for the latest
    std::string label;          // "Version 2 · 3:05 PM (latest)"
    bool checked = false;
};
// The version picker while a write-up with earlier versions shows (_show_versions): its text,
// "Version 1 of 2  ▾", and the menu's rows. `version` is the earlier one showing (nullopt: the
// latest); `written_at` gives each version's time ("" for none). Empty rows and text when the
// picker hides: nothing loaded, the words as spoken, or no earlier write-up.
struct Versions {
    bool shown = false;
    std::string text;
    std::vector<VersionRow> rows;
};
Versions versions(std::span<const int> earlier, std::optional<int> version, bool spoken,
                  bool loaded, const std::function<std::string(std::optional<int>)>& written_at);

// What the notice says after copying in a walkthrough_copy_format (_COPIED); nullopt for a
// format the storyboard does not copy in ("both").
std::optional<std::string_view> copied(std::string_view format);

} // namespace fv::walkthrough_review
