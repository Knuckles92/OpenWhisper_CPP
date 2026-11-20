#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A walkthrough written out as Markdown and a JSON manifest, and what every format shares
// (services/walkthrough/document.py:66-365). One continuous recording with screenshots kept along
// the way: each screenshot closes a section, the words said since the previous screenshot first,
// then the screenshot they describe; speech after the last screenshot is a trailing section.
// walkthrough.md is for terminal agents and editors, walkthrough.json keeps the sections as data
// for a later write-up. The PDF and Word layouts are fv_documents' (renderer.h); the helpers
// below are what they share with the Markdown.
namespace fv::walkthrough {

inline constexpr std::string_view TITLE = "Walkthrough";
inline constexpr int MANIFEST_VERSION = 1;

// Which paper the PDF and Word documents use; Auto is the locale's (Letter in the US, else A4).
enum class PaperSize { Auto, Letter, A4 };

// What was said up to one kept screenshot, or after the last one.
struct Section {
    std::string text; // may be ""
    // PNG of the screenshot that closes this section; nullopt for trailing speech.
    std::optional<std::filesystem::path> shot;
    // Seconds into the recording when that screenshot was kept.
    std::optional<double> kept_at_s;
    // A screenshot Review left out of the documents; kept so it can return.
    std::optional<std::filesystem::path> omitted;
    // What the user held Hold for note to say here: for the AI write-up only, so never in the
    // documents, only in the manifest.
    std::vector<std::string> notes;

    friend bool operator==(const Section&, const Section&) = default;
};

// One recording's sections, in the order they were spoken.
struct Walkthrough {
    // Local time with its zone (the manifest writes its UTC offset).
    std::chrono::zoned_seconds started_at;
    double duration_s = 0; // recording length in seconds
    std::vector<Section> sections;
};

// What the documents show besides the words and screenshots. The defaults are the full layout;
// Settings > Dictation > Walkthroughs changes them. The manifest always keeps everything.
struct DocumentFormat {
    std::string title{TITLE}; // "" leaves the heading out
    bool details = true;      // the line with the date, the length and the screenshot count
    bool about = true;        // the note saying what the document is and how to read it
    bool captions = true;     // "Screenshot 2 · 1:12" under each screenshot

    friend bool operator==(const DocumentFormat&, const DocumentFormat&) = default;
};

// A write-up's colours and font, which its Markdown can't say. "" keeps the walkthrough's own;
// a colour is anything QColor reads ("purple", "#5b2a86").
struct Look {
    std::string text_color;
    std::string heading_color; // "" gives headings the text's colour
    std::string font;

    friend bool operator==(const Look&, const Look&) = default;
};

// started_at as a moment in `zone`, as Python's astimezone() reads a naive time with fold 0: an
// ambiguous time is its first occurrence, a skipped one is read with the offset before the jump.
std::chrono::zoned_seconds local_moment(const std::chrono::time_zone* zone,
                                        std::chrono::local_seconds started_at);

// A section as written out: its tidied words and its screenshot's number (_Part).
struct Part {
    std::string words;
    std::optional<std::filesystem::path> shot;
    int number = 0; // 1 for the first screenshot; 0 without one
    std::optional<double> kept_at_s;

    friend bool operator==(const Part&, const Part&) = default;
};

// The sections worth writing, each screenshot numbered in order of appearance; a section with
// neither words nor a screenshot is dropped.
std::vector<Part> parts(const Walkthrough& walkthrough);
int shot_count(const std::vector<Part>& parts);
// `text` with "\n" line breaks and without surrounding whitespace.
std::string tidy(std::string_view text);
// `words` split at blank lines, each paragraph stripped, empty ones dropped.
std::vector<std::string> paragraphs(std::string_view words);

// "1:12", or "1:02:03" from an hour on, in whole seconds; negatives read 0:00.
std::string clock(double seconds);
// "Tue, Oct 20, 2026, 2:32 PM", with English names whatever the locale (_when).
std::string when_text(std::chrono::local_seconds started_at);
// "Tue, Oct 20, 2026, 2:32 PM · 2:16 of speech · 3 screenshots" (_meta).
std::string meta_line(const Walkthrough& walkthrough, int shots);
// "Spoken notes with screenshots, transcribed by <product>." and, with screenshots, how to read
// them (_about).
std::string about_text(int shots);
// The lines under the title that `format` keeps, in order (_header).
std::vector<std::string> header_lines(const Walkthrough& walkthrough, int shots,
                                      const DocumentFormat& format);
// "Screenshot 2" + before_time + clock + after_time, or just "Screenshot 2" without a known
// time (_label).
std::string shot_label(const Part& part, std::string_view before_time,
                       std::string_view after_time = "");
// `path` relative to `folder` with forward slashes, both made absolute first; nullopt outside it.
// Windows compares the names without regard to case, as Python's PureWindowsPath does (_relative).
std::optional<std::string> relative_to_folder(const std::filesystem::path& path,
                                              const std::filesystem::path& folder);
// Backslashes a marker that opens a line ("#", ">", "-", "+", "*", "1." or "12)"), so speech
// never turns into Markdown structure (_escape_markers).
std::string escape_markers(std::string_view words);
// A link destination for `path`: relative to `folder`, in <> when it holds whitespace or
// parentheses, or the full path, always in <>, with the platform's separators (_image_target).
std::string image_target(const std::filesystem::path& path, const std::filesystem::path& folder,
                         bool absolute);
// The manifest's name for a screenshot: relative to `folder`, else its full path (_shot_name).
std::optional<std::string> shot_name(const std::optional<std::filesystem::path>& shot,
                                     const std::filesystem::path& folder);

// The walkthrough as Markdown, each section's words above its screenshot. Image links are
// relative to `folder`, where walkthrough.md sits beside its screenshots; `absolute` links full
// paths instead, for Markdown read elsewhere. Without captions a screenshot keeps its label as
// alt text. LF line endings; the text ends in one "\n".
std::string markdown(const Walkthrough& walkthrough, const std::filesystem::path& folder,
                     bool absolute, const DocumentFormat& format = {});
// Writes folder/walkthrough.md as UTF-8 with LF line endings, its images linked relative to it.
// Throws std::filesystem::filesystem_error.
std::filesystem::path write_markdown(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder,
                                     const DocumentFormat& format = {});
// Writes folder/walkthrough.json atomically: the sections as data for a later write-up,
// screenshots by name within `folder` (null for trailing speech), times in seconds, the start
// with its UTC offset, held notes with their section. Empty sections are dropped unless Review
// holds a screenshot for them or they hold notes.
std::filesystem::path write_manifest(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder);

} // namespace fv::walkthrough
