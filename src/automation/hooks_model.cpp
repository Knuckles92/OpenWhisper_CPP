#include "automation/hooks_model.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <utility>

#include "automation/catalog.h"
#include "automation/detail/pypath.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/url.h"
#include "base/identity.h"
#include "base/ids.h"
#include "net/address.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "settings/keys.h"

namespace fv::automation::hooks {

namespace {

// _text: a stripped string, or "" for anything else.
std::string text_of(const Json& item, std::string_view key) {
    const Json& value = detail::get(item, key);
    return value.is_string() ? py::strip(value.get_ref<const std::string&>()) : std::string();
}

// _strings: the string items of a list, cut to `limit`.
std::vector<std::string> strings_of(const Json& value, std::size_t limit) {
    std::vector<std::string> items;
    if (!value.is_array()) {
        return items;
    }
    for (const Json& item : value) {
        if (item.is_string() && items.size() < limit) {
            items.push_back(item.get<std::string>());
        }
    }
    return items;
}

// _bool: a stored bool, else true.
bool bool_of(const Json& value) {
    return value.is_boolean() ? value.get<bool>() : true;
}

// _int: an integer clamped into [low, high]; a bool or anything else reads as the default.
std::int64_t int_of(const Json& value, std::int64_t fallback, std::int64_t low, std::int64_t high) {
    if (!value.is_number_integer()) {
        return fallback;
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        return high; // beyond int64, so above every limit here
    }
    return std::clamp(value.get<std::int64_t>(), low, high);
}

const Json& rows_of(const Json& settings, std::string_view key) {
    static const Json kEmpty = Json::array();
    const Json& raw = detail::get(settings, key);
    return raw.is_array() ? raw : kEmpty;
}

Json strings_json(const std::vector<std::string>& items) {
    Json list = Json::array();
    for (const auto& item : items) {
        list.push_back(item);
    }
    return list;
}

bool is_stage(std::string_view stage) {
    return std::ranges::find(STAGES, stage) != std::end(STAGES);
}

bool is_filter_kind(std::string_view kind) {
    return std::ranges::find(FILTER_KINDS, kind) != std::end(FILTER_KINDS);
}

template <class Item>
void check_name(std::string_view raw_name, const std::vector<Item>& others,
                std::string_view item_id, std::string_view noun) {
    const std::string name = py::strip(raw_name);
    if (name.empty()) {
        throw py::ValueError(std::format("Give the {} a name.", noun));
    }
    if (py::len(name) > NAME_MAX_CHARS) {
        throw py::ValueError(
            std::format("Use a name with {} characters or fewer.", NAME_MAX_CHARS));
    }
    const std::string folded = py::casefold(name);
    for (const Item& other : others) {
        if (other.id != item_id && py::casefold(other.name) == folded) {
            throw py::ValueError(std::format("A {} with that name already exists.", noun));
        }
    }
}

template <class Item>
void check_room(const std::vector<Item>& others, std::string_view item_id, std::size_t limit,
                std::string_view noun) {
    const bool is_new =
        std::ranges::none_of(others, [&](const Item& other) { return other.id == item_id; });
    if (is_new && others.size() >= limit) {
        throw py::ValueError(
            std::format("You can keep up to {} {}s. Delete one to add another.", limit, noun));
    }
}

void check_events(const std::vector<std::string>& events) {
    if (catalog::known_types(events).empty()) {
        throw py::ValueError("Choose at least one event.");
    }
}

void check_program(std::string_view path, const std::vector<std::string>& args) {
    if (path.empty()) {
        throw py::ValueError("Choose the program to run.");
    }
    if (!detail::isabs(path)) {
        throw py::ValueError("Choose the program with its full path.");
    }
    const bool too_long = std::ranges::any_of(
        args, [](const std::string& arg) { return py::len(arg) > ARG_MAX_CHARS; });
    if (args.size() > MAX_ARGS || too_long) {
        throw py::ValueError(
            std::format("Use at most {} arguments of {} characters.", MAX_ARGS, ARG_MAX_CHARS));
    }
}

void require_id(std::string_view item_id, std::string_view noun) {
    if (py::strip(item_id).empty()) {
        throw py::ValueError(std::format("This {} has no id.", noun));
    }
}

// _replace: the item in place of the one with its id, else appended.
template <class Item> Json replaced(std::vector<Item> items, Item item) {
    const auto found =
        std::ranges::find_if(items, [&](const Item& other) { return other.id == item.id; });
    if (found == items.end()) {
        items.push_back(std::move(item));
    } else {
        *found = std::move(item);
    }
    Json rows = Json::array();
    for (const Item& each : items) {
        rows.push_back(each.to_dict());
    }
    return rows;
}

template <class Item> Json without(const std::vector<Item>& items, std::string_view item_id) {
    Json rows = Json::array();
    for (const Item& each : items) {
        if (each.id != item_id) {
            rows.push_back(each.to_dict());
        }
    }
    return rows;
}

std::string key_of(const auto& setting) {
    return std::string(setting.key());
}

} // namespace

std::string new_id() {
    return random_hex(12);
}

std::string webhook_secret_name(std::string_view hook_id) {
    return std::format("{}WEBHOOK_SECRET_{}", identity().env_prefix, hook_id);
}

Json Webhook::to_dict() const {
    Json data = Json::object();
    data["id"] = id;
    data["name"] = name;
    data["url"] = url;
    data["events"] = strings_json(events);
    data["enabled"] = enabled;
    return data;
}

Json Program::to_dict() const {
    Json data = Json::object();
    data["id"] = id;
    data["name"] = name;
    data["program"] = program;
    data["args"] = strings_json(args);
    data["events"] = strings_json(events);
    data["enabled"] = enabled;
    data["timeout_s"] = timeout_s;
    return data;
}

Json TextFilter::to_dict() const {
    Json data = Json::object();
    data["id"] = id;
    data["name"] = name;
    data["kind"] = kind;
    data["stage"] = stage;
    data["url"] = url;
    data["program"] = program;
    data["args"] = strings_json(args);
    data["timeout_ms"] = timeout_ms;
    data["enabled"] = enabled;
    return data;
}

std::vector<Webhook> load_webhooks(const Json& settings) {
    std::vector<Webhook> hooks;
    std::set<std::string> seen;
    for (const Json& item : rows_of(settings, keys::automation_webhooks.key())) {
        if (!item.is_object()) {
            continue;
        }
        Webhook hook{text_of(item, "id"), text_of(item, "name"), text_of(item, "url")};
        if (hook.id.empty() || hook.name.empty() || hook.url.empty() || seen.contains(hook.id)) {
            continue;
        }
        seen.insert(hook.id);
        hook.events =
            catalog::known_types(strings_of(detail::get(item, "events"), MAX_EVENT_NAMES));
        hook.enabled = bool_of(detail::get(item, "enabled"));
        hooks.push_back(std::move(hook));
        if (hooks.size() >= MAX_WEBHOOKS) {
            break;
        }
    }
    return hooks;
}

std::vector<Program> load_programs(const Json& settings) {
    std::vector<Program> programs;
    std::set<std::string> seen;
    for (const Json& item : rows_of(settings, keys::automation_programs.key())) {
        if (!item.is_object()) {
            continue;
        }
        Program program{text_of(item, "id"), text_of(item, "name"), text_of(item, "program")};
        if (program.id.empty() || program.name.empty() || program.program.empty() ||
            seen.contains(program.id)) {
            continue;
        }
        seen.insert(program.id);
        program.args = strings_of(detail::get(item, "args"), MAX_ARGS);
        program.events =
            catalog::known_types(strings_of(detail::get(item, "events"), MAX_EVENT_NAMES));
        program.enabled = bool_of(detail::get(item, "enabled"));
        program.timeout_s = int_of(detail::get(item, "timeout_s"), PROGRAM_TIMEOUT_DEFAULT_S, 1,
                                   PROGRAM_TIMEOUT_MAX_S);
        programs.push_back(std::move(program));
        if (programs.size() >= MAX_PROGRAMS) {
            break;
        }
    }
    return programs;
}

std::vector<TextFilter> load_filters(const Json& settings) {
    std::vector<TextFilter> filters;
    std::set<std::string> seen;
    for (const Json& item : rows_of(settings, keys::automation_filters.key())) {
        if (!item.is_object()) {
            continue;
        }
        TextFilter text_filter{text_of(item, "id"), text_of(item, "name"), text_of(item, "kind"),
                               text_of(item, "stage")};
        if (text_filter.id.empty() || text_filter.name.empty() || seen.contains(text_filter.id) ||
            !is_filter_kind(text_filter.kind) || !is_stage(text_filter.stage)) {
            continue;
        }
        text_filter.url = text_of(item, "url");
        text_filter.program = text_of(item, "program");
        if ((text_filter.kind == FILTER_HTTP && text_filter.url.empty()) ||
            (text_filter.kind == FILTER_PROGRAM && text_filter.program.empty())) {
            continue;
        }
        seen.insert(text_filter.id);
        text_filter.args = strings_of(detail::get(item, "args"), MAX_ARGS);
        text_filter.timeout_ms = int_of(detail::get(item, "timeout_ms"), FILTER_TIMEOUT_DEFAULT_MS,
                                        FILTER_TIMEOUT_MIN_MS, FILTER_TIMEOUT_MAX_MS);
        text_filter.enabled = bool_of(detail::get(item, "enabled"));
        filters.push_back(std::move(text_filter));
        if (filters.size() >= MAX_FILTERS) {
            break;
        }
    }
    return filters;
}

void validate_url(std::string_view url) {
    if (url.empty() || py::len(url) > URL_MAX_CHARS) {
        throw py::ValueError("Enter the address to send to.");
    }
    detail::UrlParts parts;
    try {
        parts = detail::urlsplit(url);
    } catch (const py::ValueError&) {
        throw py::ValueError("That address isn't valid.");
    }
    if ((parts.scheme != "http" && parts.scheme != "https") || !parts.hostname()) {
        throw py::ValueError("Use an address that starts with http:// or https://.");
    }
    // any(ch.isspace() for ch in url)
    for (const char32_t cp : py::to_code_points(url)) {
        if (py::is_space(cp)) {
            throw py::ValueError("Addresses can't contain spaces.");
        }
    }
}

bool url_is_private(std::string_view url) {
    std::string host;
    try {
        host = detail::urlsplit(url).hostname().value_or("");
    } catch (const py::ValueError&) {
        return false;
    }
    if (host == "localhost") {
        return true;
    }
    for (const std::string_view suffix : {".localhost", ".local", ".lan", ".home.arpa"}) {
        if (host.ends_with(suffix)) {
            return true;
        }
    }
    const auto address = net::parse_address(host);
    if (!address) {
        return false;
    }
    return net::is_loopback(*address) || net::is_private(*address) || net::is_link_local(*address);
}

void validate_webhook(const Webhook& hook, const Json& settings) {
    const auto existing = load_webhooks(settings);
    check_name(hook.name, existing, hook.id, "webhook");
    validate_url(hook.url);
    check_events(hook.events);
    check_room(existing, hook.id, MAX_WEBHOOKS, "webhook");
}

void validate_program(const Program& program, const Json& settings) {
    const auto existing = load_programs(settings);
    check_name(program.name, existing, program.id, "program");
    check_program(program.program, program.args);
    check_events(program.events);
    if (program.timeout_s < 1 || program.timeout_s > PROGRAM_TIMEOUT_MAX_S) {
        throw py::ValueError(
            std::format("Use a time limit from 1 to {} seconds.", PROGRAM_TIMEOUT_MAX_S));
    }
    check_room(existing, program.id, MAX_PROGRAMS, "program");
}

void validate_filter(const TextFilter& text_filter, const Json& settings) {
    const auto existing = load_filters(settings);
    check_name(text_filter.name, existing, text_filter.id, "filter");
    if (!is_filter_kind(text_filter.kind)) {
        throw py::ValueError("Choose whether the filter is a web address or a program.");
    }
    if (!is_stage(text_filter.stage)) {
        throw py::ValueError("Choose when the filter runs.");
    }
    if (text_filter.kind == FILTER_HTTP) {
        validate_url(text_filter.url);
    } else {
        check_program(text_filter.program, text_filter.args);
    }
    if (text_filter.timeout_ms < FILTER_TIMEOUT_MIN_MS ||
        text_filter.timeout_ms > FILTER_TIMEOUT_MAX_MS) {
        throw py::ValueError(std::format("Use a time limit from {} to {} ms.",
                                         FILTER_TIMEOUT_MIN_MS, FILTER_TIMEOUT_MAX_MS));
    }
    check_room(existing, text_filter.id, MAX_FILTERS, "filter");
}

void save_webhook(Json& settings, const Webhook& hook) {
    Webhook clean{py::strip(hook.id), py::strip(hook.name), py::strip(hook.url),
                  catalog::known_types(hook.events), hook.enabled};
    require_id(clean.id, "webhook");
    validate_webhook(clean, settings);
    settings[key_of(keys::automation_webhooks)] = replaced(load_webhooks(settings), clean);
}

void save_program(Json& settings, const Program& program) {
    Program clean{py::strip(program.id),
                  py::strip(program.name),
                  py::strip(program.program),
                  program.args,
                  catalog::known_types(program.events),
                  program.enabled,
                  program.timeout_s};
    require_id(clean.id, "program");
    validate_program(clean, settings);
    settings[key_of(keys::automation_programs)] = replaced(load_programs(settings), clean);
}

void save_filter(Json& settings, const TextFilter& text_filter) {
    TextFilter clean{
        py::strip(text_filter.id), py::strip(text_filter.name), text_filter.kind,
        text_filter.stage,         py::strip(text_filter.url),  py::strip(text_filter.program),
        text_filter.args,          text_filter.timeout_ms,      text_filter.enabled};
    require_id(clean.id, "filter");
    validate_filter(clean, settings);
    settings[key_of(keys::automation_filters)] = replaced(load_filters(settings), clean);
}

void delete_webhook(Json& settings, std::string_view hook_id) {
    settings[key_of(keys::automation_webhooks)] = without(load_webhooks(settings), hook_id);
}

void delete_program(Json& settings, std::string_view program_id) {
    settings[key_of(keys::automation_programs)] = without(load_programs(settings), program_id);
}

void delete_filter(Json& settings, std::string_view filter_id) {
    settings[key_of(keys::automation_filters)] = without(load_filters(settings), filter_id);
}

std::optional<Webhook> find_webhook(const Json& settings, std::string_view hook_id) {
    for (Webhook& hook : load_webhooks(settings)) {
        if (hook.id == hook_id) {
            return std::move(hook);
        }
    }
    return std::nullopt;
}

std::optional<Program> find_program(const Json& settings, std::string_view program_id) {
    for (Program& program : load_programs(settings)) {
        if (program.id == program_id) {
            return std::move(program);
        }
    }
    return std::nullopt;
}

std::optional<TextFilter> find_filter(const Json& settings, std::string_view filter_id) {
    for (TextFilter& text_filter : load_filters(settings)) {
        if (text_filter.id == filter_id) {
            return std::move(text_filter);
        }
    }
    return std::nullopt;
}

} // namespace fv::automation::hooks
