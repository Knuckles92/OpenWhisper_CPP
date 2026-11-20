#include "walkthrough/library.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <system_error>

#include "base/clock.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "walkthrough/detail/patterns.h"
#include "walkthrough/detail/temp_folder.h"
#include "walkthrough/detail/text_io.h"
#include "walkthrough/files.h"
#include "walkthrough/finish.h"
#include "walkthrough/renderer.h"
#include "walkthrough/storage.h"

namespace fv::walkthrough {

namespace {

constexpr std::string_view kOriginalName = "walkthrough.original.json";
constexpr int kMaxRetentionDays = 3650;
constexpr double kSecondsPerDay = 86400;

const py::Regex& folder_pattern() {
    static const py::Regex regex(R"(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(?:-\d+)?\Z)");
    return regex;
}

// An earlier write-up, kept when a newer one took its place: write-up-1.md is the first.
const py::Regex& earlier_pattern() {
    static const py::Regex regex(R"(write-up-([1-9]\d*)\.(?:md|pdf|docx)\Z)");
    return regex;
}

// What a walkthrough folder may hold besides shot-<n>.png and write-up-<n>.*.
const std::set<std::string, std::less<>>& fixed_names() {
    static const std::set<std::string, std::less<>> names{
        std::string(DRAFT_NAME), std::string(AUDIO_NAME),   "walkthrough.json", "walkthrough.md",
        "walkthrough.pdf",       "walkthrough.docx",        "write-up.md",      "write-up.pdf",
        "write-up.docx",         std::string(kOriginalName)};
    return names;
}

bool is_link(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool present(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

std::string name_of(const std::filesystem::path& path) {
    return path_to_utf8(path.has_filename() ? path.filename() : path.parent_path().filename());
}

std::filesystem::path child(const std::filesystem::path& folder, std::string_view name) {
    return folder / path_from_utf8(name);
}

// type(value).__name__ of a JSON value as Python loads it.
std::string_view type_name(const Json& value) {
    if (value.is_null()) {
        return "NoneType";
    }
    if (value.is_boolean()) {
        return "bool";
    }
    if (value.is_number_integer()) {
        return "int";
    }
    if (value.is_number_float()) {
        return "float";
    }
    if (value.is_string()) {
        return "str";
    }
    return value.is_array() ? "list" : "dict";
}

[[noreturn]] void key_error(std::string_view key) {
    throw py::PyError("KeyError", py::repr(key));
}

// mapping[key]: KeyError when absent, TypeError when `mapping` is no dict.
const Json& item(const Json& mapping, std::string_view key) {
    if (!mapping.is_object()) {
        throw py::TypeError(std::format("'{}' object is not subscriptable", type_name(mapping)));
    }
    const auto found = mapping.find(key);
    if (found == mapping.end()) {
        key_error(key);
    }
    return *found;
}

// mapping.get(key, fallback); AttributeError when `mapping` is no dict.
Json get(const Json& mapping, std::string_view key, const Json& fallback = nullptr) {
    if (!mapping.is_object()) {
        throw py::PyError("AttributeError",
                          std::format("'{}' object has no attribute 'get'", type_name(mapping)));
    }
    const auto found = mapping.find(key);
    return found == mapping.end() ? fallback : *found;
}

// Iterating a JSON value as Python does: a list's items, a dict's keys.
std::vector<Json> items_of(const Json& value) {
    std::vector<Json> out;
    if (value.is_array()) {
        out.assign(value.begin(), value.end());
    } else if (value.is_object()) {
        for (const auto& [key, ignored] : value.items()) {
            out.emplace_back(key);
        }
    } else if (value.is_string()) {
        const std::string text = value.get<std::string>();
        for (std::size_t at = 0; at < py::len(text); ++at) {
            out.emplace_back(
                py::slice(text, static_cast<std::int64_t>(at), static_cast<std::int64_t>(at) + 1));
        }
    } else {
        throw py::TypeError(std::format("'{}' object is not iterable", type_name(value)));
    }
    return out;
}

// str(value) of a JSON scalar; containers are written as JSON.
std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    if (value.is_number_integer()) {
        return value.dump();
    }
    return py::dumps(value);
}

// _seconds: float(value), finite and not negative.
double seconds(const Json& value) {
    const auto number = py::py_float(value);
    if (!number) {
        if (value.is_string()) {
            throw py::ValueError("could not convert string to float: " +
                                 py::repr(value.get<std::string>()));
        }
        throw py::TypeError(std::format("float() argument must be a string or a real number, "
                                        "not '{}'",
                                        type_name(value)));
    }
    if (!std::isfinite(*number) || *number < 0) {
        throw py::ValueError("Invalid walkthrough time");
    }
    return *number;
}

bool fullmatch(const py::Regex& regex, const Json& value) {
    if (!value.is_string()) {
        throw py::TypeError(
            std::format("expected string or bytes-like object, got '{}'", type_name(value)));
    }
    return regex.fullmatch(value.get<std::string>()).has_value();
}

// _shot_file: the screenshot `name` names inside `folder`; nullopt for no name, or for a missing
// one that isn't `required`.
std::optional<std::filesystem::path> shot_file(const std::filesystem::path& folder,
                                               const Json& name, bool required = true) {
    if (!name.is_null() &&
        !(name.is_string() && detail::shot_pattern().fullmatch(name.get<std::string>()))) {
        throw py::ValueError("Invalid screenshot name");
    }
    if (name.is_null()) {
        return std::nullopt;
    }
    try {
        return local_file(folder, name.get<std::string>());
    } catch (const py::ValueError&) {
        if (required) {
            throw;
        }
        return std::nullopt; // an omitted PNG deleted by hand just can't come back
    }
}

// The wall time of a datetime.fromisoformat value.
std::chrono::local_seconds wall_time(const py::DateTime& value) {
    return std::chrono::local_days{std::chrono::year{value.year} /
                                   static_cast<unsigned>(value.month) /
                                   static_cast<unsigned>(value.day)} +
           std::chrono::hours{value.hour} + std::chrono::minutes{value.minute} +
           std::chrono::seconds{value.second};
}

py::DateTime parse_started(const Json& value) {
    if (!value.is_string()) {
        throw py::TypeError("fromisoformat: argument must be str");
    }
    return py::fromisoformat(value.get<std::string>());
}

// A stored start as a moment in this computer's zone: one with a UTC offset is that instant,
// a naive one this computer's wall time (as astimezone() reads it).
std::chrono::zoned_seconds started_moment(const Json& value) {
    const auto parsed = parse_started(value);
    const auto* zone = system_clock().zone();
    const auto wall = wall_time(parsed);
    if (parsed.utc_offset_us) {
        const auto offset = std::chrono::floor<std::chrono::seconds>(
            std::chrono::microseconds{*parsed.utc_offset_us});
        return std::chrono::zoned_seconds(
            zone, std::chrono::sys_seconds{wall.time_since_epoch() - offset});
    }
    return local_moment(zone, wall);
}

// A draft's start as wall time on this computer.
std::chrono::local_seconds started_local(const Json& value) {
    const auto parsed = parse_started(value);
    const auto wall = wall_time(parsed);
    if (!parsed.utc_offset_us) {
        return wall;
    }
    const auto offset =
        std::chrono::floor<std::chrono::seconds>(std::chrono::microseconds{*parsed.utc_offset_us});
    return system_clock().zone()->to_local(
        std::chrono::sys_seconds{wall.time_since_epoch() - offset});
}

// os.stat().st_mtime: seconds since the epoch.
double modified_s(const std::filesystem::path& path) {
    const auto written = std::filesystem::last_write_time(path);
    const auto utc = std::chrono::clock_cast<std::chrono::system_clock>(written);
    return std::chrono::duration<double>(utc.time_since_epoch()).count();
}

double now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Python's except (OSError, ValueError, KeyError, TypeError).
template <class Fn> bool ignoring_expected_errors(Fn&& fn) {
    try {
        fn();
        return true;
    } catch (const std::system_error&) {
    } catch (const py::PyError& error) {
        const auto type = error.type_name();
        if (!dynamic_cast<const py::ValueError*>(&error) && type != "KeyError" &&
            type != "TypeError") {
            throw;
        }
    }
    return false;
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

} // namespace

Walkthrough load_document(const std::filesystem::path& folder) {
    const Json value = read_json(child(folder, MANIFEST_NAME));
    if (!py::py_equal(get(value, "version"), Json(MANIFEST_VERSION))) {
        throw py::ValueError("Unsupported walkthrough version");
    }
    std::vector<Section> sections;
    for (const auto& section : items_of(item(value, "sections"))) {
        const auto shot = shot_file(folder, get(section, "shot"));
        const auto omitted = shot_file(folder, get(section, "omitted_shot"), false);
        const Json stamp = get(section, "kept_at_s");
        const Json notes = get(section, "notes", Json::array());
        const bool strings = notes.is_array() && std::ranges::all_of(notes, [](const Json& note) {
                                 return note.is_string();
                             });
        if (!strings) {
            throw py::ValueError("Invalid walkthrough notes");
        }
        Section loaded;
        loaded.text = py_str(item(section, "text"));
        loaded.shot = shot;
        if (!stamp.is_null()) {
            loaded.kept_at_s = seconds(stamp);
        }
        // A section that has both keeps only its screenshot.
        loaded.omitted = shot ? std::nullopt : omitted;
        for (const auto& note : notes) {
            loaded.notes.push_back(note.get<std::string>());
        }
        sections.push_back(std::move(loaded));
    }
    auto started = started_moment(item(value, "started_at"));
    const double duration = seconds(item(value, "duration_s"));
    return Walkthrough{started, duration, std::move(sections)};
}

std::vector<Note> held_notes(const Walkthrough& walkthrough) {
    std::vector<Note> notes;
    int shown = 0;
    for (const auto& section : walkthrough.sections) {
        for (const auto& text : section.notes) {
            notes.push_back({text, shown});
        }
        if (section.shot) {
            ++shown;
        }
    }
    return notes;
}

bool written_up(const std::filesystem::path& folder) {
    return is_file(child(folder, write_up::PDF_NAME)) &&
           is_file(child(folder, write_up::MARKDOWN_NAME));
}

std::array<std::string, 3> write_up_names(std::optional<int> version) {
    const std::string stem = version ? std::format("write-up-{}", *version) : "write-up";
    return {stem + ".md", stem + ".pdf", stem + ".docx"};
}

std::vector<int> earlier_write_ups(const std::filesystem::path& folder) {
    if (!written_up(folder)) {
        return {};
    }
    std::set<int> found;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const std::string name = path_to_utf8(entry.path().filename());
        const auto match = earlier_pattern().fullmatch(name);
        if (!match || !name.ends_with(".md")) {
            continue;
        }
        const auto version = py::py_int(match->group(1));
        if (!version || *version > std::numeric_limits<int>::max()) {
            continue; // a number this large was never written by a write-up
        }
        const auto [md, pdf, docx] = write_up_names(static_cast<int>(*version));
        if (is_file(child(folder, pdf)) && is_file(child(folder, md))) {
            found.insert(static_cast<int>(*version));
        }
    }
    return {found.begin(), found.end()};
}

WalkthroughResult load_result(const std::filesystem::path& folder, bool spoken,
                              std::optional<int> version) {
    const Walkthrough walkthrough = load_document(folder);
    std::vector<std::filesystem::path> shots;
    for (const auto& section : walkthrough.sections) {
        if (section.shot) {
            shots.push_back(*section.shot);
        }
    }
    const bool written = !spoken && written_up(folder);
    std::string md_name(MARKDOWN_NAME);
    std::string pdf_name(PDF_NAME);
    if (written) {
        const auto names = write_up_names(version);
        md_name = names[0];
        pdf_name = names[1];
    }
    const auto pdf = local_file(folder, pdf_name);
    const auto md = local_file(folder, md_name);
    // Image paths are rebuilt from the names on disk; another computer's paths are never kept.
    std::set<std::string, std::less<>> names;
    for (const auto& shot : shots) {
        names.insert(path_to_utf8(shot.filename()));
    }
    const std::string text = detail::image_pattern().sub(
        [&](const py::Match& match) -> std::string {
            const std::string name = detail::strip_chars(match.group(2), "<>");
            if (!names.contains(name)) {
                return "";
            }
            return std::format("![{}]({})", match.group(1),
                               image_target(child(folder, name), folder, true));
        },
        detail::read_text(md));
    std::vector<std::string> parts;
    int number = 0;
    for (const auto& section : walkthrough.sections) {
        std::string words = py::strip(section.text);
        if (!words.empty()) {
            parts.push_back(std::move(words));
        }
        if (section.shot) {
            parts.push_back(std::format("[Screenshot {}]", ++number));
        }
    }
    WalkthroughResult result;
    result.folder = folder;
    result.pdf = pdf;
    result.markdown = md;
    result.clipboard_text = text;
    result.history_text = join(parts, "\n\n");
    result.shots = static_cast<int>(shots.size());
    result.duration_s = walkthrough.duration_s;
    if (!shots.empty()) {
        result.first_shot = shots.front();
    }
    result.written_up = written;
    return result;
}

std::vector<std::filesystem::path> entries(const std::filesystem::path& root) {
    if (!present(root) || is_link(root)) {
        return {};
    }
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto& path = entry.path();
        std::error_code error;
        if (!std::filesystem::is_directory(path, error) || is_link(path) ||
            !folder_pattern().fullmatch(path_to_utf8(path.filename()))) {
            continue;
        }
        if (is_file(child(path, MANIFEST_NAME)) || is_file(child(path, DRAFT_NAME))) {
            found.push_back(path);
        }
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) {
        return path_to_utf8(a.filename()) > path_to_utf8(b.filename());
    });
    return found;
}

WalkthroughResult save_review(const std::filesystem::path& folder,
                              std::span<const std::string> texts, const std::vector<bool>& keep,
                              std::span<const int> order, const Json& settings) {
    const Walkthrough original = load_document(folder);
    const std::size_t count = original.sections.size();
    std::vector<int> saved_order;
    if (order.empty()) {
        for (std::size_t index = 0; index < count; ++index) {
            saved_order.push_back(static_cast<int>(index));
        }
    } else {
        saved_order.assign(order.begin(), order.end());
    }
    std::vector<int> sorted = saved_order;
    std::ranges::sort(sorted);
    bool every_once = sorted.size() == count;
    for (std::size_t index = 0; every_once && index < count; ++index) {
        every_once = sorted[index] == static_cast<int>(index);
    }
    if (texts.size() != count || keep.size() != count || !every_once) {
        throw py::ValueError("The document changed; reopen Review");
    }
    Walkthrough updated{original.started_at, original.duration_s, {}};
    for (std::size_t row = 0; row < count; ++row) {
        Section section = original.sections[static_cast<std::size_t>(saved_order[row])];
        const auto image = section.shot ? section.shot : section.omitted;
        section.text = texts[row];
        section.shot = keep[row] ? image : std::nullopt;
        section.omitted = keep[row] ? std::nullopt : image;
        updated.sections.push_back(std::move(section));
    }
    const auto [format, paper] = document_settings(settings);
    // Rendered in isolation first: a failed PDF cannot destroy the saved document.
    const detail::TemporaryFolder staging(".review-", folder);
    const auto& stage = staging.path();
    Walkthrough staged = updated;
    for (std::size_t index = 0; index < staged.sections.size(); ++index) {
        auto& section = staged.sections[index];
        const auto& source = updated.sections[index];
        if (section.shot) {
            section.shot = stage / section.shot->filename();
        }
        if (section.omitted) {
            section.omitted = stage / section.omitted->filename();
        }
        if (const auto image = source.shot ? source.shot : source.omitted) {
            std::filesystem::copy_file(*image, stage / image->filename(),
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }
    write_markdown(staged, stage, format);
    auto& renderer = document_renderer();
    renderer.write_pdf(staged, stage, format, paper);
    renderer.write_docx(staged, stage, format, paper);
    write_manifest(staged, stage);
    const auto original_manifest = child(folder, kOriginalName);
    if (!present(original_manifest)) {
        atomic_json(original_manifest, read_json(child(folder, MANIFEST_NAME)));
    }
    const std::vector<std::string> names{std::string(MARKDOWN_NAME), std::string(PDF_NAME),
                                         std::string(DOCX_NAME), std::string(MANIFEST_NAME)};
    publish_files(stage, folder, names);
    return load_result(folder, true);
}

std::vector<std::pair<std::string, std::string>> write_up_parts(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> parts;
    std::vector<std::string> words;
    for (auto& line : detail::splitlines(text)) {
        if (detail::image_pattern().fullmatch(py::strip(line))) {
            parts.emplace_back(detail::strip_chars(join(words, "\n"), "\n"), std::move(line));
            words.clear();
        } else {
            words.push_back(std::move(line));
        }
    }
    std::string rest = detail::strip_chars(join(words, "\n"), "\n");
    if (!py::strip(rest).empty() || parts.empty()) {
        parts.emplace_back(std::move(rest), "");
    }
    return parts;
}

std::string join_write_up(std::span<const std::pair<std::string, std::string>> parts) {
    std::vector<std::string> blocks;
    for (const auto& [words, line] : parts) {
        if (!py::strip(words).empty()) {
            blocks.push_back(detail::strip_chars(words, "\n"));
        }
        if (!line.empty()) {
            blocks.push_back(line);
        }
    }
    return join(blocks, "\n\n");
}

std::optional<std::filesystem::path> write_up_shot(const std::filesystem::path& folder,
                                                   std::string_view line) {
    const auto match = detail::image_pattern().fullmatch(py::strip(line));
    if (!match) {
        return std::nullopt;
    }
    try {
        return shot_file(folder, Json(detail::strip_chars(match->group(2), "<>")), false);
    } catch (const py::ValueError&) {
        return std::nullopt; // not one of this walkthrough's screenshots
    }
}

WalkthroughResult save_write_up(const std::filesystem::path& folder, std::string_view text,
                                std::optional<int> version, const Json& settings) {
    const auto listed = [&] {
        const auto earlier = earlier_write_ups(folder);
        return std::ranges::find(earlier, *version) != earlier.end();
    };
    if (!written_up(folder) || (version && !listed())) {
        throw py::ValueError("This walkthrough has no write-up to save");
    }
    // The look a spoken note asked for is never saved with a write-up.
    return write_up::write(load_result(folder, false, version), py::strip(text), Look{}, settings,
                           version);
}

WalkthroughResult retry_write_up(const std::filesystem::path& folder,
                                 std::optional<std::string> instructions, const Json& settings,
                                 const CancelToken& cancel, write_up::AgentTaskRunner run) {
    const WalkthroughResult result = load_result(folder, true);
    const Walkthrough walkthrough = load_document(folder);
    std::vector<std::filesystem::path> shots;
    for (const auto& section : walkthrough.sections) {
        if (section.shot) {
            shots.push_back(*section.shot);
        }
    }
    if (!instructions) {
        instructions = keys::walkthrough_write_up_instructions.read(settings);
    }
    const auto notes = held_notes(walkthrough);
    return write_up::write_up(
        result, shots, keys::walkthrough_write_up_agent.read(settings), py::strip(*instructions),
        keys::walkthrough_write_up_notes.read(settings), notes, settings, cancel, std::move(run));
}

int attach_recovered_audio(const std::filesystem::path& root,
                           const std::filesystem::path& recordings) {
    int count = 0;
    for (const auto& folder : entries(root)) {
        if (present(child(folder, MANIFEST_NAME)) || present(child(folder, AUDIO_NAME))) {
            continue;
        }
        ignoring_expected_errors([&] {
            const Json draft = read_json(child(folder, DRAFT_NAME));
            const Json journal = get(draft, "journal_id", "");
            static const py::Regex journal_pattern("[0-9a-f]{32}");
            if (!fullmatch(journal_pattern, journal)) {
                return;
            }
            // recordings.glob("recording_*-recovered-<journal>.wav"), which ignores case on
            // Windows.
            unsigned flags = 0;
#ifdef _WIN32
            flags = py::IGNORECASE;
#endif
            const py::Regex wanted(
                "recording_.*-recovered-" + journal.get<std::string>() + R"(\.wav\Z)", flags);
            std::vector<std::string> matches;
            std::error_code error;
            for (std::filesystem::directory_iterator it(recordings, error), end;
                 !error && it != end; it.increment(error)) {
                const std::string name = path_to_utf8(it->path().filename());
                if (wanted.fullmatch(name)) {
                    matches.push_back(name);
                }
            }
            if (matches.size() != 1) {
                return;
            }
            const auto source = local_file(recordings, matches.front());
            const auto temporary = folder / ".audio.tmp";
            std::filesystem::copy_file(source, temporary,
                                       std::filesystem::copy_options::overwrite_existing);
            std::filesystem::rename(temporary, child(folder, AUDIO_NAME));
            ++count;
        });
    }
    return count;
}

std::unique_ptr<WalkthroughSession> recovery_session(const std::filesystem::path& folder) {
    const Json draft = read_json(child(folder, DRAFT_NAME));
    if (!py::py_equal(get(draft, "version"), Json(1))) {
        throw py::ValueError("Unsupported draft version");
    }
    local_file(folder, AUDIO_NAME);
    WalkthroughSession::Recovered recovered;
    recovered.started_at = started_local(item(draft, "started_at"));
    recovered.folder = folder;
    // Only screenshots that were written; no writer is needed.
    double last = 0.0;
    for (const auto& shot : items_of(item(draft, "shots"))) {
        const Json& name = item(shot, "shot");
        if (!fullmatch(detail::shot_pattern(), name)) {
            throw py::ValueError("Invalid screenshot name");
        }
        const double stamp = seconds(item(shot, "kept_at_s"));
        if (stamp < last) {
            throw py::ValueError("Unordered screenshot times");
        }
        last = stamp;
        std::filesystem::path path;
        try {
            path = local_file(folder, name.get<std::string>());
        } catch (const py::ValueError&) {
            continue; // a crash may precede the PNG write
        }
        if (!detail::readable_png(path)) {
            continue;
        }
        recovered.shots.emplace_back(std::move(path), stamp);
    }
    // Held for the agent, so cut out of the sections; null runs to the end.
    double end = 0.0;
    for (const auto& note : items_of(get(draft, "notes_s", Json::array()))) {
        const auto pair = items_of(note);
        if (pair.size() != 2) {
            throw py::ValueError(pair.size() > 2 ? "too many values to unpack (expected 2)"
                                                 : std::format("not enough values to unpack "
                                                               "(expected 2, got {})",
                                                               pair.size()));
        }
        const double start = seconds(pair[0]);
        const double stop =
            pair[1].is_null() ? std::numeric_limits<double>::infinity() : seconds(pair[1]);
        if (start < end || stop < start) {
            throw py::ValueError("Unordered note times");
        }
        end = stop;
        recovered.notes_s.emplace_back(start, stop);
    }
    return WalkthroughSession::recovered(std::move(recovered));
}

int retention_days(const std::filesystem::path& root) {
    int days = 0;
    ignoring_expected_errors([&] {
        const Json value = item(read_json(child(root, PREFERENCES_NAME)), "days");
        if (value.is_number_integer() && !value.is_number_unsigned() &&
            value.get<std::int64_t>() >= 0 && value.get<std::int64_t>() <= kMaxRetentionDays) {
            days = static_cast<int>(value.get<std::int64_t>());
        } else if (value.is_number_unsigned() && value.get<std::uint64_t>() <= kMaxRetentionDays) {
            days = static_cast<int>(value.get<std::uint64_t>());
        }
    });
    return days;
}

void set_retention(const std::filesystem::path& root, int days) {
    if (days < 0 || days > kMaxRetentionDays) {
        throw py::ValueError("Keep captures for 0\xE2\x80\x93"
                             "3650 days; 0 means forever");
    }
    std::filesystem::create_directories(root);
    Json value = Json::object();
    value["days"] = days;
    atomic_json(child(root, PREFERENCES_NAME), value);
}

std::vector<std::filesystem::path> expired_entries(const std::filesystem::path& root,
                                                   std::optional<double> now) {
    const int days = retention_days(root);
    if (days == 0) {
        return {};
    }
    const double cutoff = now.value_or(now_s()) - days * kSecondsPerDay;
    std::vector<std::filesystem::path> expired;
    for (const auto& folder : entries(root)) {
        const auto manifest = child(folder, MANIFEST_NAME);
        if (!present(child(folder, DRAFT_NAME)) && is_file(manifest) &&
            modified_s(manifest) < cutoff) {
            expired.push_back(folder);
        }
    }
    return expired;
}

void delete_entry(const std::filesystem::path& folder) {
    // Links and unexpected files are refused instead of deleting user data recursively.
    if (is_link(folder) || !folder_pattern().fullmatch(name_of(folder))) {
        throw py::ValueError("Not a walkthrough folder");
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        files.push_back(entry.path());
    }
    for (const auto& path : files) {
        const std::string name = path_to_utf8(path.filename());
        if (is_link(path) || !is_file(path) ||
            (!fixed_names().contains(name) && !detail::shot_pattern().fullmatch(name) &&
             !earlier_pattern().fullmatch(name))) {
            throw py::ValueError("Folder contains extra files; remove it in your file manager");
        }
    }
    for (const auto& path : files) {
        std::filesystem::remove(path);
    }
    std::filesystem::remove(folder);
}

int prune(const std::filesystem::path& root) {
    int removed = 0;
    for (const auto& folder : expired_entries(root)) {
        try {
            delete_entry(folder);
            ++removed;
        } catch (const std::system_error&) {
            continue;
        } catch (const py::ValueError&) {
            continue;
        }
    }
    return removed;
}

} // namespace fv::walkthrough
