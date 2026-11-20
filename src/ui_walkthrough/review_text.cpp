#include "ui_walkthrough/review_text.h"

#include <array>
#include <exception>
#include <format>
#include <regex>
#include <system_error>

#include "base/json.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "walkthrough/files.h"

namespace fv::walkthrough_review {

namespace {

using namespace std::chrono;

constexpr std::array<std::string_view, 7> kWeekdays{"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                    "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12> kMonths{"January",   "February", "March",    "April",
                                                   "May",       "June",     "July",     "August",
                                                   "September", "October",  "November", "December"};

struct Parts {
    year_month_day date;
    hh_mm_ss<seconds> clock;
};

Parts parts(local_seconds when) {
    const auto midnight = floor<days>(when);
    return {year_month_day{midnight}, hh_mm_ss<seconds>{when - midnight}};
}

std::string_view weekday_name(year_month_day date) {
    return kWeekdays[weekday{local_days{date}}.c_encoding()];
}

std::string_view month_name(year_month_day date) {
    return kMonths[static_cast<unsigned>(date.month()) - 1];
}

// %a and %b: the first three letters of the English names.
std::string_view abbreviated(std::string_view name) {
    return name.substr(0, 3);
}

std::optional<local_seconds> wall_time(int year_, int month_, int day_, int hour, int minute,
                                       int second) {
    const year_month_day date{std::chrono::year{year_},
                              std::chrono::month{static_cast<unsigned>(month_)},
                              std::chrono::day{static_cast<unsigned>(day_)}};
    if (year_ < 1 || !date.ok() || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    return local_days{date} + hours{hour} + minutes{minute} + seconds{second};
}

// The first `count` code points of UTF-8 `text`, as Python's str slicing cuts.
std::string first_code_points(std::string_view text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t taken = 0; at < text.size() && taken < count; ++taken) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
            ++at;
        }
    }
    return std::string(text.substr(0, at));
}

// datetime.strptime(name, "%Y-%m-%d_%H-%M-%S"): _strptime's patterns for each directive,
// matched from the start, then the whole text must be used.
std::optional<local_seconds> from_folder_name(std::string_view name) {
    static const std::regex pattern(
        R"((\d\d\d\d)-(1[0-2]|0[1-9]|[1-9])-(3[01]|[12]\d|0[1-9]|[1-9]| [1-9])_)"
        R"((2[0-3]|[0-1]\d|\d)-([0-5]\d|\d)-(6[0-1]|[0-5]\d|\d))",
        std::regex::ECMAScript | std::regex::icase);
    const std::string text(name);
    std::smatch found;
    if (!std::regex_search(text, found, pattern, std::regex_constants::match_continuous) ||
        static_cast<std::size_t>(found.length(0)) != text.size()) {
        return std::nullopt;
    }
    const auto number = [&](int group) {
        return std::stoi(found.str(group));
    };
    return wall_time(number(1), number(2), number(3), number(4), number(5), number(6));
}

std::optional<local_seconds> from_draft(const std::filesystem::path& folder) {
    try {
        const Json draft = walkthrough::read_json(folder / path_from_utf8(walkthrough::DRAFT_NAME));
        const Json& value = draft.at("started_at");
        if (!value.is_string()) {
            return std::nullopt;
        }
        const auto when = py::fromisoformat(value.get<std::string>());
        // An aware time keeps its own wall time, as Python formats it.
        return wall_time(when.year, when.month, when.day, when.hour, when.minute, when.second);
    } catch (const std::exception&) {
        // OSError, ValueError, KeyError or TypeError in Python: the folder's name says instead.
        return std::nullopt;
    }
}

std::string title_text(local_seconds when) {
    const auto [date, clock] = parts(when);
    return std::format("{}, {} {} · {}", weekday_name(date), month_name(date),
                       static_cast<unsigned>(date.day()), time(when));
}

} // namespace

std::optional<local_seconds> started(const std::filesystem::path& folder,
                                     const walkthrough::Walkthrough* document) {
    if (document != nullptr) {
        return document->started_at.get_local_time();
    }
    if (auto when = from_draft(folder)) {
        return when;
    }
    return from_folder_name(first_code_points(path_to_utf8(folder.filename()), 19));
}

std::string day(local_seconds when, year_month_day today_) {
    const auto date = parts(when).date;
    const auto apart = (local_days{today_} - local_days{date}).count();
    if (apart == 0) {
        return "Today";
    }
    if (apart == 1) {
        return "Yesterday";
    }
    if (date.year() == today_.year()) {
        return std::format("{}, {} {}", abbreviated(weekday_name(date)),
                           abbreviated(month_name(date)), static_cast<unsigned>(date.day()));
    }
    return std::format("{} {}, {}", abbreviated(month_name(date)),
                       static_cast<unsigned>(date.day()), static_cast<int>(date.year()));
}

std::string time(local_seconds when) {
    const auto clock = parts(when).clock;
    const auto hour = clock.hours().count();
    const auto twelve = hour % 12 == 0 ? 12 : hour % 12;
    return std::format("{}:{:02} {}", twelve, clock.minutes().count(), hour < 12 ? "AM" : "PM");
}

std::string shots(int count) {
    return std::format("{} screenshot{}", count, count == 1 ? "" : "s");
}

std::string written_at(std::optional<local_seconds> modified, year_month_day today_) {
    if (!modified) {
        return "";
    }
    const auto date = parts(*modified).date;
    if (date == today_) {
        return time(*modified);
    }
    return std::format("{} {}, {}", abbreviated(month_name(date)),
                       static_cast<unsigned>(date.day()), time(*modified));
}

std::optional<local_seconds> modified_at(const std::filesystem::path& file) {
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        return std::nullopt;
    }
    const auto moment = floor<seconds>(clock_cast<system_clock>(stamp));
    return floor<seconds>(current_zone()->to_local(moment));
}

year_month_day today() {
    return year_month_day{floor<days>(current_zone()->to_local(system_clock::now()))};
}

Header header(const HeaderInput& input) {
    Header out;
    const bool has = input.folder.has_value();
    if (input.started) {
        out.title = title_text(*input.started);
    } else if (has) {
        out.title = path_to_utf8(input.folder->filename());
    }
    if (input.sections) {
        int count = 0;
        for (const bool shot : *input.sections) {
            count += shot ? 1 : 0;
        }
        out.meta = std::format("{} · {}", shots(count), walkthrough::clock(input.duration_s));
    } else {
        out.meta = input.draft ? "Not finished yet" : "";
    }
    // A written-up walkthrough says which version shows with its switch.
    out.pill = input.draft ? "Interrupted" : "As spoken";
    out.pill_tone = input.draft ? "warning" : "plain";
    out.pill_shown = has && (input.draft || !input.written);
    out.version_bar_shown = input.written && input.sections.has_value();
    out.version_bar_index = input.spoken ? 1 : 0;
    const std::string redo = input.written ? "Redo" : "Write up";
    out.retry = input.written ? "Redo AI write-up" : "Write up with AI";
    out.style_menu = redo + " as";
    out.prompt = redo + " with a new prompt…";
    out.title_shown = has;
    out.notice = std::string(input.spoken ? SPOKEN_HINT : WRITE_UP_HINT);
    return out;
}

Versions versions(std::span<const int> earlier, std::optional<int> version, bool spoken,
                  bool loaded, const std::function<std::string(std::optional<int>)>& written_at_) {
    Versions out;
    out.shown = loaded && !spoken && !earlier.empty();
    if (!out.shown) {
        return out;
    }
    const int latest = earlier.back() + 1;
    // Python's `version or latest`: a version 0 reads as the latest too.
    const auto number_of = [&](std::optional<int> chosen) {
        return chosen && *chosen != 0 ? *chosen : latest;
    };
    out.text = std::format("Version {} of {}  ▾", number_of(version), latest);
    std::vector<std::optional<int>> order{std::nullopt};
    for (auto at = earlier.rbegin(); at != earlier.rend(); ++at) {
        order.emplace_back(*at);
    }
    for (const auto& each : order) {
        std::string label = std::format("Version {}", number_of(each));
        const std::string when = written_at_(each);
        if (!when.empty()) {
            label += " · " + when;
        }
        if (!each) {
            label += " (latest)";
        }
        out.rows.push_back({each, std::move(label), each == version});
    }
    return out;
}

std::optional<std::string_view> copied(std::string_view format) {
    if (format == "pdf") {
        return "PDF copied.";
    }
    if (format == "agent_task") {
        return "Instructions copied. Paste them, then drag the PDF into your chat.";
    }
    if (format == "agent") {
        return "Markdown copied.";
    }
    if (format == "word") {
        return "Word document copied.";
    }
    return std::nullopt;
}

} // namespace fv::walkthrough_review
