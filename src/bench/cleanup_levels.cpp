#include "bench/cleanup_levels.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

#include "base/utf8.h"
#include "bench/data/cleanup_golden.jsonl.h"
#include "dictation/cleanup_prompt.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/capture_service.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "text/cleanup_profiles.h"
#include "text/cleanup_prompts.h"

namespace fv::bench {

namespace {

const std::set<std::string, std::less<>>& known_keys() {
    static const std::set<std::string, std::less<>> keys = {
        "id",           "category",         "level",     "profile",   "destination", "input",
        "must_contain", "must_not_contain", "min_lines", "max_lines", "note"};
    return keys;
}

// repr() of a JSON value as Python prints the loaded object.
std::string py_repr(const Json& value) {
    if (value.is_string()) {
        return py::repr(value.get<std::string>());
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_array()) {
        std::string out = "[";
        for (std::size_t i = 0; i < value.size(); ++i) {
            out += (i == 0 ? "" : ", ") + py_repr(value[i]);
        }
        return out + "]";
    }
    return value.dump();
}

// Python truthiness of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string() || value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

bool is_stored_level(const Json& level) {
    if (!level.is_string()) {
        return false;
    }
    const auto name = level.get<std::string>();
    return std::any_of(text::STORED_LEVELS.begin(), text::STORED_LEVELS.end(),
                       [&](text::CleanupLevel stored) { return text::to_string(stored) == name; });
}

bool is_starter_profile(const Json& profile) {
    if (!profile.is_string()) {
        return false;
    }
    const auto id = profile.get<std::string>();
    const auto& starters = text::starter_profiles();
    return std::any_of(starters.begin(), starters.end(),
                       [&](const text::CleanupProfile& item) { return item.id == id; });
}

// _has: the phrase as a whole word run, any case.
bool has(std::string_view text, std::string_view phrase) {
    const py::Regex pattern("(?<!\\w)" + py::regex_escape(phrase) + "(?!\\w)", py::IGNORECASE);
    return pattern.search(text).has_value();
}

// str.splitlines's boundaries.
std::vector<std::string> split_lines(std::string_view text) {
    static const py::Regex boundary("\\r\\n|[\\n\\r\\v\\f\\x1c\\x1d\\x1e\\x85\\u2028\\u2029]");
    std::vector<std::string> lines;
    for (auto& piece : boundary.split(text)) {
        lines.push_back(piece.value_or(""));
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

std::vector<std::string> phrase_list(const Json& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const Json& item : value) {
            out.push_back(item.is_string() ? item.get<std::string>() : item.dump());
        }
    } else {
        out.push_back(value.is_string() ? value.get<std::string>() : value.dump());
    }
    return out;
}

std::string truncate(std::string_view text, std::int64_t code_points) {
    return py::slice(text, 0, code_points);
}

} // namespace

std::string_view golden_jsonl() {
    return embedded::bench_cleanup_golden;
}

std::vector<Json> load_cases(std::string_view jsonl) {
    std::vector<Json> cases;
    std::set<std::string, std::less<>> seen;
    std::size_t number = 0;
    for (const std::string& line : split_lines(jsonl)) {
        ++number;
        if (py::strip(line).empty()) {
            continue;
        }
        Json item = Json::parse(line);
        const Json id = item.value("id", Json(nullptr));
        const std::string name = truthy(id) ? (id.is_string() ? id.get<std::string>() : id.dump())
                                            : "line " + std::to_string(number);
        std::vector<std::string> problems;
        std::vector<std::string> unknown;
        for (const auto& [key, value] : item.items()) {
            (void)value;
            if (!known_keys().contains(key)) {
                unknown.push_back(key);
            }
        }
        if (!unknown.empty()) {
            std::sort(unknown.begin(), unknown.end());
            problems.push_back("unknown keys " + py_repr(Json(unknown)));
        }
        if (seen.contains(name)) {
            problems.push_back("duplicate id");
        }
        const Json input = item.value("input", Json(nullptr));
        if (!input.is_string() || py::strip(input.get<std::string>()).empty()) {
            problems.push_back("no input");
        }
        const bool has_level = item.contains("level");
        const bool has_profile = item.contains("profile");
        if (has_level == has_profile) {
            problems.push_back("needs exactly one of level and profile");
        } else if (has_level && !is_stored_level(item["level"])) {
            problems.push_back("unknown level " + py_repr(item["level"]));
        } else if (has_profile && !is_starter_profile(item["profile"])) {
            problems.push_back("unknown profile " + py_repr(item["profile"]));
        }
        const Json destination = item.value("destination", Json("app"));
        if (std::find(std::begin(kDestinations), std::end(kDestinations),
                      destination.is_string() ? destination.get<std::string>() : std::string()) ==
            std::end(kDestinations)) {
            problems.push_back("unknown destination " + py_repr(item["destination"]));
        }
        if (!truthy(item.value("must_contain", Json(nullptr))) &&
            !truthy(item.value("must_not_contain", Json(nullptr)))) {
            problems.push_back("checks nothing");
        }
        if (!problems.empty()) {
            std::string joined;
            for (const std::string& problem : problems) {
                joined += (joined.empty() ? "" : "; ") + problem;
            }
            throw std::invalid_argument(name + ": " + joined);
        }
        seen.insert(name);
        cases.push_back(std::move(item));
    }
    return cases;
}

std::vector<Json> load_cases() {
    return load_cases(golden_jsonl());
}

std::vector<Json> load_cases_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read " + path_to_utf8(path) + ".");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_cases(std::string_view(text));
}

std::string case_label(const Json& item) {
    if (item.contains("level")) {
        return item["level"].get<std::string>();
    }
    return "profile:" + item.at("profile").get<std::string>();
}

std::string compose_prompt(const Json& item) {
    std::optional<focus::AppIdentity> identity;
    if (item.value("destination", std::string("app")) == "app") {
        focus::AppIdentity notepad;
        notepad.app_id = "notepad.exe";
        notepad.name = "Notepad";
        notepad.platform = "windows";
        identity = notepad;
    }
    dictation::DictationJob job;
    job.focus = focus::FocusRequest::resolved(focus::FocusSnapshot{identity, std::nullopt});
    const Json settings{{"transcript_cleanup_enabled", true},
                        {"transcript_cleanup_level", item.value("level", std::string("medium"))}};
    std::optional<text::CleanupProfile> profile;
    if (item.contains("profile")) {
        for (const auto& starter : text::starter_profiles()) {
            if (starter.id == item["profile"].get<std::string>()) {
                profile = starter;
            }
        }
    }
    const dictation::PreparedText prepared =
        dictation::prepare_text(item.at("input").get<std::string>(), &job, settings);
    return dictation::compose_cleanup_prompt(settings, &job, profile ? &*profile : nullptr, {},
                                             prepared);
}

std::vector<std::string> check(const Json& item, std::string_view output) {
    std::vector<std::string> failures;
    for (const Json& wanted : item.value("must_contain", Json::array())) {
        const auto options = phrase_list(wanted);
        if (std::none_of(options.begin(), options.end(),
                         [&](const std::string& option) { return has(output, option); })) {
            std::string joined;
            for (const std::string& option : options) {
                joined += (joined.empty() ? "" : " / ") + option;
            }
            failures.push_back("missing " + py::repr(joined));
        }
    }
    for (const Json& unwanted : item.value("must_not_contain", Json::array())) {
        const std::string phrase =
            unwanted.is_string() ? unwanted.get<std::string>() : unwanted.dump();
        if (has(output, phrase)) {
            failures.push_back("contains " + py::repr(phrase));
        }
    }
    std::int64_t lines = 0;
    for (const std::string& line : split_lines(output)) {
        if (!py::strip(line).empty()) {
            ++lines;
        }
    }
    if (item.contains("min_lines") && lines < item["min_lines"].get<std::int64_t>()) {
        failures.push_back(std::to_string(lines) + " lines, wanted at least " +
                           std::to_string(item["min_lines"].get<std::int64_t>()));
    }
    if (item.contains("max_lines") && lines > item["max_lines"].get<std::int64_t>()) {
        failures.push_back(std::to_string(lines) + " lines, wanted at most " +
                           std::to_string(item["max_lines"].get<std::int64_t>()));
    }
    return failures;
}

Json CaseResult::to_json() const {
    return Json{{"id", id},
                {"label", label},
                {"category", category},
                {"passed", passed},
                {"failures", failures},
                {"output", output},
                {"elapsed_s", elapsed_s}};
}

std::vector<CaseResult> run(const std::vector<Json>& cases, const Cleaner& cleaner, double delay_s,
                            const std::function<void(double)>& sleep) {
    std::vector<CaseResult> results;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Json& item = cases[index];
        if (index != 0 && delay_s > 0) {
            if (sleep) {
                sleep(delay_s);
            } else {
                std::this_thread::sleep_for(std::chrono::duration<double>(delay_s));
            }
        }
        const auto started = std::chrono::steady_clock::now();
        const std::string output =
            cleaner.cleanup(item.at("input").get<std::string>(), compose_prompt(item));
        const auto error = cleaner.last_error();
        CaseResult result;
        result.id = item.at("id").get<std::string>();
        result.label = case_label(item);
        result.category = item.value("category", std::string());
        // Provider error bodies can end in account details; the start says what went wrong.
        result.failures = error && !error->empty()
                              ? std::vector<std::string>{"cleanup failed: " + truncate(*error, 120)}
                              : check(item, output);
        result.passed = result.failures.empty();
        result.output = output;
        result.elapsed_s = py::round_ndigits(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 2);
        results.push_back(std::move(result));
    }
    return results;
}

Json CleanupSummary::to_json() const {
    const auto groups = [](const auto& map) {
        Json out = Json::object();
        for (const auto& [key, counts] : map) {
            out[key] = Json::array({counts.first, counts.second});
        }
        return out;
    };
    return Json{{"passed", passed},
                {"total", total},
                {"by_label", groups(by_label)},
                {"by_category", groups(by_category)}};
}

CleanupSummary summarize(const std::vector<CaseResult>& results) {
    CleanupSummary summary;
    for (const CaseResult& result : results) {
        for (auto* group : {&summary.by_label, &summary.by_category}) {
            auto& counts = (*group)[group == &summary.by_label ? result.label : result.category];
            counts.first += result.passed ? 1 : 0;
            counts.second += 1;
        }
        summary.passed += result.passed ? 1 : 0;
    }
    summary.total = results.size();
    return summary;
}

std::string rate(std::size_t passed, std::size_t total) {
    if (total == 0) {
        return "0/0";
    }
    return std::to_string(passed) + "/" + std::to_string(total) + " (" +
           py::format_fixed(100.0 * static_cast<double>(passed) / static_cast<double>(total), 0) +
           "%)";
}

} // namespace fv::bench
