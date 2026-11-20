#include "walkthrough/styles.h"

#include <format>
#include <set>

#include "base/ids.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "walkthrough/write_up_names.h"

namespace fv::walkthrough {

namespace {

Json array_of(const std::vector<WriteUpStyle>& styles) {
    Json out = Json::array();
    for (const auto& style : styles) {
        out.push_back(style.to_dict());
    }
    return out;
}

} // namespace

Json WriteUpStyle::to_dict() const {
    Json out = Json::object();
    out["id"] = id;
    out["name"] = name;
    out["instructions"] = instructions;
    return out;
}

const WriteUpStyle& default_style() {
    static const WriteUpStyle style{"walkthrough", "Walkthrough",
                                    std::string(write_up::DEFAULT_INSTRUCTIONS)};
    return style;
}

const std::array<WriteUpStyle, 4>& starter_styles() {
    static const std::array<WriteUpStyle, 4> styles{
        WriteUpStyle{"step-by-step", "Step-by-step guide",
                     "Turn this into a how-to guide someone could follow on their own. Give it a "
                     "short title, then numbered steps in the order I did them, each saying what "
                     "to do and what you should see, with the screenshot for that step right "
                     "under it. Tidy my wording and drop filler, but keep every detail someone "
                     "would need to repeat it."},
        WriteUpStyle{"bug-report", "Bug report",
                     "Write this up as a bug report. Start with a short title and a "
                     "one-sentence summary of the problem, then the steps to reproduce it, what "
                     "happened, and what I expected instead. Put each screenshot where it shows "
                     "the problem best. Keep exact names, numbers and error messages as I said "
                     "or showed them, and don't guess at causes I didn't mention."},
        WriteUpStyle{"design-feedback", "Design feedback",
                     "Write this up as design feedback on the screens I'm showing. Give it a "
                     "short title, then one section per change I'm suggesting: what's there now, "
                     "what I'd change and why, with the screenshot that shows it. End with a "
                     "short list of the proposed changes. Tidy my wording and keep my reasoning, "
                     "but don't add ideas I didn't have."},
        WriteUpStyle{"agent-task", "Task for an agent",
                     "Write this up as a brief for a coding agent to act on. Give it a short "
                     "title, then the goal in a sentence or two, what to change and where, as I "
                     "described it, and how to tell it's done. Put each screenshot beside the "
                     "part it explains. Keep the exact names of screens, buttons and files, and "
                     "don't add requirements I didn't state."},
    };
    return styles;
}

std::vector<WriteUpStyle> saved_styles(const Json& settings) {
    const Json snapshot = settings.is_object() ? settings : Json::object();
    const Json raw = keys::walkthrough_write_up_styles.read(snapshot);
    if (raw.is_null()) {
        return {starter_styles().begin(), starter_styles().end()};
    }
    std::vector<WriteUpStyle> styles;
    std::set<std::string, std::less<>> seen;
    for (const auto& item : raw) {
        if (!item.is_object()) {
            continue;
        }
        std::array<std::string, 3> values;
        bool usable = true;
        const std::array<std::string_view, 3> fields{"id", "name", "instructions"};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto found = item.find(fields[index]);
            if (found == item.end() || !found->is_string()) {
                usable = false;
                break;
            }
            values[index] = py::strip(found->get<std::string>());
            usable = usable && !values[index].empty();
        }
        if (!usable || seen.contains(values[0]) || values[0] == default_style().id) {
            continue;
        }
        seen.insert(values[0]);
        styles.push_back({values[0], values[1], values[2]});
    }
    if (styles.size() > MAX_STYLES) {
        styles.resize(MAX_STYLES);
    }
    return styles;
}

std::vector<WriteUpStyle> load_styles(const Json& settings) {
    std::vector<WriteUpStyle> styles{default_style()};
    for (auto& style : saved_styles(settings)) {
        styles.push_back(std::move(style));
    }
    return styles;
}

std::optional<WriteUpStyle> style_named(std::string_view name,
                                        std::span<const WriteUpStyle> styles) {
    const std::string folded = py::casefold(py::strip(name));
    for (const auto& style : styles) {
        if (py::casefold(style.name) == folded) {
            return style;
        }
    }
    return std::nullopt;
}

std::optional<WriteUpStyle> style_named(const Json& settings, std::string_view name) {
    return style_named(name, load_styles(settings));
}

std::optional<WriteUpStyle> style_for(std::string_view instructions,
                                      std::span<const WriteUpStyle> styles) {
    std::string text = py::strip(instructions);
    if (text.empty()) {
        text = write_up::DEFAULT_INSTRUCTIONS;
    }
    for (const auto& style : styles) {
        if (style.instructions == text) {
            return style;
        }
    }
    return std::nullopt;
}

std::optional<WriteUpStyle> style_for(const Json& settings, std::string_view instructions) {
    return style_for(instructions, load_styles(settings));
}

WriteUpStyle save_style(Json& settings, std::string_view name, std::string_view instructions,
                        std::string_view id) {
    const std::string trimmed_name = py::strip(name);
    const std::string trimmed_instructions = py::strip(instructions);
    if (trimmed_name.empty()) {
        throw py::ValueError("Give the style a name.");
    }
    if (py::len(trimmed_name) > MAX_NAME) {
        throw py::ValueError(std::format("Use a name with {} characters or fewer.", MAX_NAME));
    }
    if (trimmed_instructions.empty()) {
        throw py::ValueError("Add the instructions for the write-up you want.");
    }
    if (py::casefold(trimmed_name) == py::casefold(default_style().name)) {
        throw py::ValueError(std::format("\"{}\" is the built-in style. Choose another name.",
                                         default_style().name));
    }
    auto styles = saved_styles(settings);
    const auto existing = style_named(trimmed_name, styles);
    if (!existing && styles.size() >= MAX_STYLES) {
        throw py::ValueError(
            std::format("You can keep up to {} styles. Delete one first.", MAX_STYLES));
    }
    std::string style_id;
    if (existing) {
        style_id = existing->id;
    } else if (!id.empty()) {
        style_id = std::string(id);
    } else {
        style_id = uuid4_hex().substr(0, 12);
    }
    WriteUpStyle style{style_id, trimmed_name, trimmed_instructions};
    if (!existing) {
        styles.push_back(style);
    } else {
        for (auto& each : styles) {
            if (each == *existing) {
                each = style;
                break;
            }
        }
    }
    if (!settings.is_object()) {
        settings = Json::object();
    }
    settings[std::string(keys::walkthrough_write_up_styles.key())] = array_of(styles);
    return style;
}

void delete_style(Json& settings, std::string_view id) {
    std::vector<WriteUpStyle> kept;
    for (auto& style : saved_styles(settings)) {
        if (style.id != id) {
            kept.push_back(std::move(style));
        }
    }
    if (!settings.is_object()) {
        settings = Json::object();
    }
    settings[std::string(keys::walkthrough_write_up_styles.key())] = array_of(kept);
}

} // namespace fv::walkthrough
