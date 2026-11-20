#include "walkthrough/document.h"

#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "base/identity.h"
#include "base/json.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "walkthrough/files.h"
#include "walkthrough/storage.h"

namespace fv::walkthrough {

namespace {

constexpr std::string_view kOrder = "Each screenshot comes right after the words that describe it.";

// A line that would open a heading, quote, list or rule in Markdown.
const py::Regex& block_marker() {
    static const py::Regex regex(R"(^([ \t]*)([#>+*-]|\d{1,9}[.)](?=\s|$)))", py::MULTILINE);
    return regex;
}

const py::Regex& link_needs_brackets() {
    static const py::Regex regex(R"([\s()])");
    return regex;
}

const py::Regex& blank_line() {
    static const py::Regex regex(R"(\n\s*\n)");
    return regex;
}

// os.path.abspath: absolute, normalised, the platform's separators, no trailing separator.
std::filesystem::path abspath(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    absolute.make_preferred();
    if (absolute.has_relative_path() && !absolute.has_filename()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

bool same_name(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    return py::lower(path_to_utf8(a)) == py::lower(path_to_utf8(b));
#else
    return a == b;
#endif
}

std::int64_t whole_seconds(double seconds) {
    if (std::isnan(seconds)) {
        throw std::invalid_argument("cannot convert float NaN to integer");
    }
    if (std::isinf(seconds)) {
        throw std::overflow_error("cannot convert float infinity to integer");
    }
    if (seconds >= 9.0e18) {
        return std::int64_t{9'000'000'000'000'000'000};
    }
    return seconds <= 0 ? 0 : static_cast<std::int64_t>(seconds);
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            out += separator;
        }
        out += items[index];
    }
    return out;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::filesystem::filesystem_error("Could not write", path,
                                                std::make_error_code(std::errc::permission_denied));
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        throw std::filesystem::filesystem_error("Could not write", path,
                                                std::make_error_code(std::errc::io_error));
    }
}

} // namespace

std::chrono::zoned_seconds local_moment(const std::chrono::time_zone* zone,
                                        std::chrono::local_seconds started_at) {
    const auto info = zone->get_info(started_at);
    if (info.result == std::chrono::local_info::nonexistent) {
        // A skipped time is read with the offset before the jump (CPython's local_to_seconds
        // with fold 0), so 02:30 in a spring-forward gap becomes 03:30 after it.
        return std::chrono::zoned_seconds(
            zone, std::chrono::sys_seconds{started_at.time_since_epoch() - info.first.offset});
    }
    return std::chrono::zoned_seconds(zone, started_at, std::chrono::choose::earliest);
}

std::vector<Part> parts(const Walkthrough& walkthrough) {
    std::vector<Part> out;
    int shots = 0;
    for (const auto& section : walkthrough.sections) {
        std::string words = tidy(section.text);
        int number = 0;
        if (section.shot) {
            number = ++shots;
        } else if (words.empty()) {
            continue;
        }
        out.push_back({std::move(words), section.shot, number, section.kept_at_s});
    }
    return out;
}

int shot_count(const std::vector<Part>& parts) {
    int count = 0;
    for (const auto& part : parts) {
        count += part.number != 0 ? 1 : 0;
    }
    return count;
}

std::string tidy(std::string_view text) {
    std::string lines;
    lines.reserve(text.size());
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '\r') {
            lines.push_back('\n');
            if (at + 1 < text.size() && text[at + 1] == '\n') {
                ++at;
            }
        } else {
            lines.push_back(text[at]);
        }
    }
    return py::strip(lines);
}

std::vector<std::string> paragraphs(std::string_view words) {
    std::vector<std::string> out;
    for (const auto& piece : blank_line().split(words)) {
        std::string paragraph = py::strip(piece.value_or(""));
        if (!paragraph.empty()) {
            out.push_back(std::move(paragraph));
        }
    }
    return out;
}

std::string clock(double seconds) {
    const std::int64_t whole = whole_seconds(seconds);
    const std::int64_t hours = whole / 3600;
    const std::int64_t rest = whole % 3600;
    if (hours != 0) {
        return std::format("{}:{:02}:{:02}", hours, rest / 60, rest % 60);
    }
    return std::format("{}:{:02}", rest / 60, rest % 60);
}

std::string when_text(std::chrono::local_seconds started_at) {
    static constexpr std::array<std::string_view, 7> kDays{"Sun", "Mon", "Tue", "Wed",
                                                           "Thu", "Fri", "Sat"};
    static constexpr std::array<std::string_view, 12> kMonths{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto day = std::chrono::floor<std::chrono::days>(started_at);
    const std::chrono::year_month_day date{day};
    const std::chrono::weekday weekday{day};
    const std::chrono::hh_mm_ss time{started_at - day};
    const auto hour = time.hours().count();
    const auto twelve = hour % 12 == 0 ? 12 : hour % 12;
    // strftime("%I:%M %p").lstrip("0").
    return std::format("{}, {} {}, {}, {}:{:02} {}", kDays[weekday.c_encoding()],
                       kMonths[static_cast<unsigned>(date.month()) - 1],
                       static_cast<unsigned>(date.day()), static_cast<int>(date.year()), twelve,
                       time.minutes().count(), hour < 12 ? "AM" : "PM");
}

std::string meta_line(const Walkthrough& walkthrough, int shots) {
    const std::string count = shots == 0   ? std::string("no screenshots")
                              : shots == 1 ? std::string("1 screenshot")
                                           : std::format("{} screenshots", shots);
    return std::format("{} · {} of speech · {}", when_text(walkthrough.started_at.get_local_time()),
                       clock(walkthrough.duration_s), count);
}

std::string about_text(int shots) {
    std::string about =
        std::format("Spoken notes with screenshots, transcribed by {}.", identity().product);
    if (shots != 0) {
        about += " ";
        about += kOrder;
    }
    return about;
}

std::vector<std::string> header_lines(const Walkthrough& walkthrough, int shots,
                                      const DocumentFormat& format) {
    std::vector<std::string> lines;
    if (format.details) {
        lines.push_back(meta_line(walkthrough, shots));
    }
    if (format.about) {
        lines.push_back(about_text(shots));
    }
    return lines;
}

std::string shot_label(const Part& part, std::string_view before_time,
                       std::string_view after_time) {
    std::string label = std::format("Screenshot {}", part.number);
    if (!part.kept_at_s) {
        return label;
    }
    return std::format("{}{}{}{}", label, before_time, clock(*part.kept_at_s), after_time);
}

std::optional<std::string> relative_to_folder(const std::filesystem::path& path,
                                              const std::filesystem::path& folder) {
    const auto absolute = abspath(path);
    const auto base = abspath(folder);
    auto at = absolute.begin();
    for (const auto& component : base) {
        if (at == absolute.end() || !same_name(*at, component)) {
            return std::nullopt;
        }
        ++at;
    }
    std::vector<std::string> rest;
    for (; at != absolute.end(); ++at) {
        rest.push_back(path_to_utf8(*at));
    }
    if (rest.empty()) {
        return ".";
    }
    return join(rest, "/");
}

std::string escape_markers(std::string_view words) {
    return block_marker().sub(
        [](const py::Match& match) {
            const std::string marker = match.group(2);
            return match.group(1) + marker.substr(0, marker.size() - 1) + "\\" +
                   marker.substr(marker.size() - 1);
        },
        words);
}

std::string image_target(const std::filesystem::path& path, const std::filesystem::path& folder,
                         bool absolute) {
    const auto relative = absolute ? std::nullopt : relative_to_folder(path, folder);
    if (!relative) {
        return "<" + path_to_utf8(abspath(path)) + ">";
    }
    if (link_needs_brackets().search(*relative)) {
        return "<" + *relative + ">";
    }
    return *relative;
}

std::optional<std::string> shot_name(const std::optional<std::filesystem::path>& shot,
                                     const std::filesystem::path& folder) {
    if (!shot) {
        return std::nullopt;
    }
    auto relative = relative_to_folder(*shot, folder);
    if (relative && !relative->empty()) {
        return relative;
    }
    return path_to_utf8(abspath(*shot));
}

std::string markdown(const Walkthrough& walkthrough, const std::filesystem::path& folder,
                     bool absolute, const DocumentFormat& format) {
    const auto written = parts(walkthrough);
    const int shots = shot_count(written);
    std::vector<std::string> lines;
    if (!format.title.empty()) {
        lines.push_back("# " + format.title);
        lines.emplace_back();
    }
    const auto header = header_lines(walkthrough, shots, format);
    if (!header.empty()) {
        // The details line has no full stop of its own; the note does.
        lines.push_back("_" + join(header, ". ") + "_");
        lines.emplace_back();
    }
    for (const auto& part : written) {
        if (!part.words.empty()) {
            lines.push_back(escape_markers(part.words));
            lines.emplace_back();
        }
        if (part.shot) {
            lines.push_back(std::format("![{}]({})", shot_label(part, " (", ")"),
                                        image_target(*part.shot, folder, absolute)));
            lines.emplace_back();
        }
    }
    return join(lines, "\n");
}

std::filesystem::path write_markdown(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder,
                                     const DocumentFormat& format) {
    const auto path = folder / path_from_utf8(MARKDOWN_NAME);
    write_text(path, markdown(walkthrough, folder, false, format));
    return path;
}

std::filesystem::path write_manifest(const Walkthrough& walkthrough,
                                     const std::filesystem::path& folder) {
    const auto local = walkthrough.started_at.get_local_time();
    const auto offset = walkthrough.started_at.get_info().offset;
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{local - day};
    py::DateTime started;
    started.year = static_cast<int>(date.year());
    started.month = static_cast<int>(static_cast<unsigned>(date.month()));
    started.day = static_cast<int>(static_cast<unsigned>(date.day()));
    started.hour = static_cast<int>(time.hours().count());
    started.minute = static_cast<int>(time.minutes().count());
    started.second = static_cast<int>(time.seconds().count());
    started.utc_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(offset).count();

    Json sections = Json::array();
    for (const auto& section : walkthrough.sections) {
        std::string text = tidy(section.text);
        if (!section.shot && !section.omitted && text.empty() && section.notes.empty()) {
            // As in the documents, an empty section is dropped, unless Review is holding a
            // screenshot for it or it holds notes for a write-up.
            continue;
        }
        Json item = Json::object();
        item["text"] = std::move(text);
        const auto shot = shot_name(section.shot, folder);
        item["shot"] = shot ? Json(*shot) : Json(nullptr);
        item["kept_at_s"] = section.kept_at_s ? Json(*section.kept_at_s) : Json(nullptr);
        // Older readers ignore this and see a section without a screenshot.
        if (section.omitted) {
            item["omitted_shot"] = *shot_name(section.omitted, folder);
        }
        if (!section.notes.empty()) {
            item["notes"] = section.notes;
        }
        sections.push_back(std::move(item));
    }
    Json manifest = Json::object();
    manifest["version"] = MANIFEST_VERSION;
    manifest["started_at"] = started.isoformat('T', "seconds");
    manifest["duration_s"] = walkthrough.duration_s;
    manifest["sections"] = std::move(sections);
    const auto path = folder / path_from_utf8(MANIFEST_NAME);
    atomic_json(path, manifest);
    return path;
}

} // namespace fv::walkthrough
