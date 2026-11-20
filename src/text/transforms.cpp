#include "text/transforms.h"

#include <algorithm>
#include <set>

#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/cleanup_config.h"
#include "text/detail/prompt_data.h"

namespace fv::text {

static_assert(MAX_TRANSFORMS == config::MAX_TEXT_TRANSFORMS);

namespace {

std::optional<std::string> required_text(const Json& item, std::string_view key) {
    const auto found = item.find(std::string(key));
    if (found == item.end() || !found->is_string()) {
        return std::nullopt;
    }
    std::string value = py::strip(found->get_ref<const std::string&>());
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// _hotkey: a normalized shortcut, or "" for anything but a non-blank string. Python also maps a
// parse failure to ""; hotkeys::normalize_hotkey never throws for text, but a later grammar
// that does keeps the same answer.
std::string hotkey_of(const Json& value, hotkeys::Vocabulary vocabulary) {
    if (!value.is_string() || py::strip(value.get_ref<const std::string&>()).empty()) {
        return "";
    }
    try {
        return hotkeys::normalize_hotkey(value.get_ref<const std::string&>(), vocabulary);
    } catch (const std::exception&) {
        return "";
    }
}

void store(Json& settings, const std::vector<Transform>& transforms) {
    Json list = Json::array();
    for (const Transform& item : transforms) {
        list.push_back(transform_to_json(item));
    }
    settings[std::string(keys::text_transforms.key())] = std::move(list);
}

} // namespace

Json transform_to_json(const Transform& transform) {
    Json out = Json::object();
    out["id"] = transform.id;
    out["name"] = transform.name;
    out["instruction"] = transform.instruction;
    out["hotkey"] = transform.hotkey;
    return out;
}

const std::vector<Transform>& starter_transforms() {
    static const std::vector<Transform> starters = [] {
        std::vector<Transform> out;
        for (const Json& row : detail::prompt_data().at("starter_transforms")) {
            out.push_back({row.at("id").get<std::string>(), row.at("name").get<std::string>(),
                           row.at("instruction").get<std::string>(),
                           row.at("hotkey").get<std::string>()});
        }
        return out;
    }();
    return starters;
}

std::vector<Transform> load_transforms(const Json& settings, hotkeys::Vocabulary vocabulary) {
    // An optional list: unset reads as null (the starters), anything but a list as [].
    const Json raw = keys::text_transforms.read(detail::or_empty(settings));
    if (raw.is_null()) {
        return starter_transforms();
    }
    std::vector<Transform> transforms;
    std::set<std::string> seen;
    for (const Json& item : raw) {
        if (!item.is_object()) {
            continue;
        }
        auto id = required_text(item, "id");
        auto name = required_text(item, "name");
        auto instruction = required_text(item, "instruction");
        if (!id || !name || !instruction) {
            continue;
        }
        if (!seen.insert(*id).second) {
            continue;
        }
        const auto hotkey = item.find("hotkey");
        transforms.push_back(
            {std::move(*id), std::move(*name), std::move(*instruction),
             hotkey == item.end() ? std::string() : hotkey_of(*hotkey, vocabulary)});
        if (transforms.size() >= config::MAX_TEXT_TRANSFORMS) {
            break;
        }
    }
    return transforms;
}

std::optional<Transform> find_transform(const Json& settings, std::string_view transform_id,
                                        hotkeys::Vocabulary vocabulary) {
    for (Transform& transform : load_transforms(settings, vocabulary)) {
        if (transform.id == transform_id) {
            return std::move(transform);
        }
    }
    return std::nullopt;
}

void validate_transform(const Transform& transform, const Json& settings,
                        const ShortcutContext& context) {
    const std::string name = py::strip(transform.name);
    if (name.empty()) {
        throw py::ValueError("Give the transform a name.");
    }
    if (py::len(name) > NAME_MAX_CHARS) {
        throw py::ValueError("Use a name with " + std::to_string(NAME_MAX_CHARS) +
                             " characters or fewer.");
    }
    if (py::strip(transform.instruction).empty()) {
        throw py::ValueError("Add an instruction for how to change the text.");
    }
    const std::vector<Transform> existing = load_transforms(settings, context.vocabulary);
    const std::string folded = py::casefold(name);
    for (const Transform& other : existing) {
        if (other.id != transform.id && py::casefold(other.name) == folded) {
            throw py::ValueError("A transform with that name already exists.");
        }
    }
    const bool is_new = std::none_of(existing.begin(), existing.end(),
                                     [&](const Transform& t) { return t.id == transform.id; });
    if (is_new && existing.size() >= config::MAX_TEXT_TRANSFORMS) {
        throw py::ValueError("You can keep up to " + std::to_string(config::MAX_TEXT_TRANSFORMS) +
                             " transforms. Delete one to add another.");
    }
    const std::string conflict =
        transform_hotkey_conflict(transform.hotkey, settings, transform.id, context);
    if (!conflict.empty()) {
        throw py::ValueError("That shortcut is already used by " + conflict + ". Choose another.");
    }
}

void save_transform(Json& settings, const Transform& transform, const ShortcutContext& context) {
    const Transform cleaned{py::strip(transform.id), py::strip(transform.name),
                            py::strip(transform.instruction),
                            hotkey_of(Json(transform.hotkey), context.vocabulary)};
    if (cleaned.id.empty()) {
        throw py::ValueError("This transform has no id.");
    }
    validate_transform(cleaned, settings, context);
    std::vector<Transform> transforms = load_transforms(settings, context.vocabulary);
    const auto found = std::find_if(transforms.begin(), transforms.end(),
                                    [&](const Transform& t) { return t.id == cleaned.id; });
    if (found == transforms.end()) {
        transforms.push_back(cleaned);
    } else {
        *found = cleaned;
    }
    store(settings, transforms);
}

void delete_transform(Json& settings, std::string_view transform_id,
                      hotkeys::Vocabulary vocabulary) {
    std::vector<Transform> kept;
    for (Transform& transform : load_transforms(settings, vocabulary)) {
        if (transform.id != transform_id) {
            kept.push_back(std::move(transform));
        }
    }
    store(settings, kept);
}

} // namespace fv::text
