#include "text/app_styles.h"

#include <algorithm>
#include <cstdint>

#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/detail/prompt_data.h"

namespace fv::text::app_styles {

namespace {

bool is_one_of(std::string_view value, const auto& choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

const Json& style_text(std::string_view table) {
    return detail::prompt_data().at("app_styles").at(std::string(table));
}

std::string lookup(std::string_view table, std::string_view key) {
    const Json& entries = style_text(table);
    const auto found = entries.find(std::string(key));
    return found == entries.end() ? std::string() : found->get<std::string>();
}

Json override_row(const std::string& match, const std::string& category) {
    Json row = Json::object();
    row["match"] = match;
    row["category"] = category;
    return row;
}

} // namespace

Json resolve_tones(const Json& settings) {
    const Json stored = keys::app_style_tones.read(detail::or_empty(settings));
    Json tones = Json::object();
    for (const std::string_view category : CATEGORIES) {
        const std::string key(category);
        std::string tone(FORMAL);
        if (stored.is_object()) {
            const auto found = stored.find(key);
            if (found != stored.end() && found->is_string() &&
                is_one_of(found->get_ref<const std::string&>(), TONES)) {
                tone = found->get<std::string>();
            }
        }
        tones[key] = tone;
    }
    return tones;
}

std::vector<focus::catalog::Override> resolve_overrides(const Json& settings) {
    return focus::catalog::parse_overrides(
        keys::app_style_overrides.read(detail::or_empty(settings)));
}

std::string category_for(const std::optional<focus::FocusSnapshot>& snapshot,
                         const Json& settings) {
    const auto& identity = snapshot ? snapshot->identity : std::nullopt;
    if (!identity || identity->is_self) {
        return "";
    }
    const auto kind = focus::catalog::classify(identity, resolve_overrides(settings));
    return kind ? kind->category : std::string();
}

std::optional<AppStyle> style_for(const std::optional<focus::FocusSnapshot>& snapshot,
                                  const Json& settings) {
    const Json& values = detail::or_empty(settings);
    if (!keys::app_styles_enabled.read(values)) {
        return std::nullopt;
    }
    const auto& identity = snapshot ? snapshot->identity : std::nullopt;
    if (!identity || identity->is_self) {
        return std::nullopt;
    }
    const auto kind = focus::catalog::classify(identity, resolve_overrides(values));
    if (!kind) {
        return std::nullopt;
    }
    return AppStyle{kind->category, resolve_tones(values).at(kind->category).get<std::string>(),
                    kind->name, kind->surface};
}

std::string prompt_block(const std::optional<AppStyle>& style) {
    if (!style) {
        return "";
    }
    std::vector<std::string> lines;
    std::string tone = lookup("tone_prompts", style->tone);
    if (!tone.empty()) {
        std::string writing = lookup("writing", style->category);
        if (writing.empty()) {
            writing = "a note";
        }
        constexpr std::string_view field = "{writing}";
        for (auto at = tone.find(field); at != std::string::npos;
             at = tone.find(field, at + writing.size())) {
            tone.replace(at, field.size(), writing);
        }
        lines.push_back(std::move(tone));
    }
    std::string surface = lookup("surface_prompts", style->surface);
    if (!surface.empty()) {
        lines.push_back(std::move(surface));
    }
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) {
            out += '\n';
        }
        out += line;
    }
    return out;
}

bool set_tone(Json& settings, std::string_view category, std::string_view tone) {
    if (!is_one_of(category, CATEGORIES) || !is_one_of(tone, TONES)) {
        throw py::ValueError("Unknown style " + py::repr(category) + ": " + py::repr(tone));
    }
    Json tones = resolve_tones(settings);
    const std::string key(keys::app_style_tones.key());
    const bool changed =
        tones.at(std::string(category)).get<std::string>() != tone || !settings.contains(key);
    tones[std::string(category)] = std::string(tone);
    settings[key] = std::move(tones);
    return changed;
}

void set_override(Json& settings, std::string_view match, std::string_view category) {
    const std::string name =
        py::slice(py::strip(match), 0, static_cast<std::int64_t>(focus::catalog::MAX_MATCH_CHARS));
    if (name.empty() || !is_one_of(category, CATEGORIES)) {
        throw py::ValueError("Choose an app and a style");
    }
    const std::string folded = py::casefold(name);
    Json kept = Json::array();
    for (const auto& [existing, chosen] : resolve_overrides(settings)) {
        if (py::casefold(existing) != folded) {
            kept.push_back(override_row(existing, chosen));
        }
    }
    if (focus::catalog::builtin_category(name) != std::string(category)) {
        kept.insert(kept.begin(), override_row(name, std::string(category)));
    }
    while (kept.size() > focus::catalog::MAX_OVERRIDES) {
        kept.erase(kept.size() - 1);
    }
    settings[std::string(keys::app_style_overrides.key())] = std::move(kept);
}

void remove_override(Json& settings, std::string_view match) {
    const std::string key = py::casefold(py::strip(match));
    Json kept = Json::array();
    for (const auto& [existing, chosen] : resolve_overrides(settings)) {
        if (py::casefold(existing) != key) {
            kept.push_back(override_row(existing, chosen));
        }
    }
    settings[std::string(keys::app_style_overrides.key())] = std::move(kept);
}

} // namespace fv::text::app_styles
