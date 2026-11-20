#include "focus/catalog.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>

#include "focus/data/app_catalog.json.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::focus::catalog {

namespace {

struct Tables {
    std::vector<KnownApp> apps;
    std::vector<KnownSite> sites;
    std::unordered_map<std::string, std::size_t> by_id;
    std::unordered_map<std::string, std::size_t> site_by_key;
    std::vector<std::pair<std::string, std::vector<std::string>>> featured;
    std::vector<std::string> suffixes;
    std::size_t title_limit = 0;
    py::Regex invisible;
    py::Regex separators;
    py::Regex counter;
    py::Regex more_pages;
};

py::Regex compile(const Json& entry) {
    return py::Regex(entry.at("pattern").get<std::string>(), entry.at("flags").get<unsigned>());
}

Tables load() {
    const Json document = Json::parse(embedded::focus_app_catalog);
    const Json& patterns = document.at("patterns");
    Tables tables{
        .apps = {},
        .sites = {},
        .by_id = {},
        .site_by_key = {},
        .featured = {},
        .suffixes = document.at("suffixes").get<std::vector<std::string>>(),
        .title_limit = document.at("title_limit").get<std::size_t>(),
        .invisible = compile(patterns.at("invisible")),
        .separators = compile(patterns.at("separators")),
        .counter = compile(patterns.at("counter")),
        .more_pages = compile(patterns.at("more_pages")),
    };
    for (const auto& app : document.at("apps")) {
        tables.apps.push_back(KnownApp{
            .name = app.at("name").get<std::string>(),
            .category = app.at("category").get<std::string>(),
            .surface = app.at("surface").get<std::string>(),
            .ids = app.at("ids").get<std::vector<std::string>>(),
            .browser = app.at("browser").get<bool>(),
        });
    }
    for (const auto& known : document.at("sites")) {
        tables.sites.push_back(KnownSite{
            .name = known.at("name").get<std::string>(),
            .category = known.at("category").get<std::string>(),
            .aliases = known.at("aliases").get<std::vector<std::string>>(),
        });
    }
    for (const auto& [key, index] : document.at("by_id").items()) {
        tables.by_id.emplace(key, index.get<std::size_t>());
    }
    for (const auto& [key, index] : document.at("site_by_key").items()) {
        tables.site_by_key.emplace(key, index.get<std::size_t>());
    }
    for (const auto& [category, names] : document.at("featured").items()) {
        tables.featured.emplace_back(category, names.get<std::vector<std::string>>());
    }
    return tables;
}

const Tables& tables() {
    static const Tables loaded = load();
    return loaded;
}

const KnownSite* site_by_key(const std::string& key) {
    const auto& all = tables();
    const auto found = all.site_by_key.find(key);
    return found == all.site_by_key.end() ? nullptr : &all.sites[found->second];
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string stem(std::string_view app_id) {
    std::string lowered = py::casefold(py::strip(app_id));
    for (const auto& suffix : tables().suffixes) {
        if (ends_with(lowered, suffix)) {
            return py::slice(lowered, 0, -static_cast<std::int64_t>(py::len(suffix)));
        }
    }
    return lowered;
}

// app_keys in the order Python builds its set. Python iterates that set in hash order, which
// is random per process; classify here takes the first match in this order instead.
std::vector<std::string> ordered_app_keys(const AppIdentity& identity) {
    std::vector<std::string> keys;
    for (std::string key :
         {py::casefold(py::strip(identity.app_id)), stem(identity.app_id),
          py::casefold(py::strip(identity.name)), py::casefold(app_name(identity.app_id))}) {
        if (!key.empty() && std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(std::move(key));
        }
    }
    return keys;
}

bool is_category(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return std::ranges::find(CATEGORIES, std::string_view(text)) != std::end(CATEGORIES);
}

} // namespace

const KnownApp* lookup(std::string_view app_id) {
    const auto& all = tables();
    const auto found = all.by_id.find(py::casefold(py::strip(app_id)));
    return found == all.by_id.end() ? nullptr : &all.apps[found->second];
}

const KnownSite* site(std::string_view name) {
    return site_by_key(py::casefold(py::strip(name)));
}

bool is_browser(std::string_view app_id) {
    const KnownApp* app = lookup(app_id);
    return app != nullptr && app->browser;
}

std::string pretty_name(std::string_view app_id) {
    std::string name = py::strip(app_id);
    const std::string lowered = py::casefold(name);
    for (const auto& suffix : tables().suffixes) {
        if (ends_with(lowered, suffix)) {
            name = py::slice(name, 0, -static_cast<std::int64_t>(py::len(suffix)));
            break;
        }
    }
    if (name.find('.') != std::string::npos && name.find(' ') == std::string::npos) {
        // A reverse-DNS id: com.example.FooBar names the app last.
        name = name.substr(name.rfind('.') + 1);
    }
    static const py::Regex joiners(R"([_-]+)");
    name = py::strip(joiners.sub(" ", name));
    if (!name.empty() && name == py::lower(name)) {
        std::string titled;
        for (const auto& word : py::split(name)) {
            if (!titled.empty()) {
                titled += ' ';
            }
            titled += py::upper(py::slice(word, 0, 1)) + py::slice(word, 1);
        }
        name = std::move(titled);
    }
    return name.empty() ? std::string(app_id) : name;
}

std::string app_name(std::string_view app_id, std::string_view fallback) {
    if (const KnownApp* app = lookup(app_id)) {
        return app->name;
    }
    std::string stripped = py::strip(fallback);
    return stripped.empty() ? pretty_name(app_id) : stripped;
}

std::string site_from_title(std::string_view title) {
    if (title.empty()) {
        return "";
    }
    const auto& all = tables();
    const std::string cleaned =
        all.invisible.sub("", py::slice(title, 0, static_cast<std::int64_t>(all.title_limit)));
    // The page's own name comes first and the browser and profile last; the site is usually
    // just before them, so the last known segment wins.
    const auto segments = all.separators.split(cleaned);
    for (const auto& segment : std::views::reverse(segments)) {
        const std::string key = py::casefold(py::strip(
            all.more_pages.sub("", all.counter.sub("", py::strip(segment.value_or(""))))));
        if (const KnownSite* known = site_by_key(key)) {
            return known->name;
        }
    }
    return "";
}

std::string title_hint(std::string_view app_id, std::string_view title) {
    return is_browser(app_id) ? site_from_title(title) : "";
}

std::set<std::string> app_keys(const AppIdentity& identity) {
    const auto keys = ordered_app_keys(identity);
    return {keys.begin(), keys.end()};
}

std::string site_key(const AppIdentity& identity) {
    return py::casefold(py::strip(identity.title_hint));
}

bool matches(const std::optional<AppIdentity>& identity, std::string_view value) {
    const std::string key = py::casefold(py::strip(value));
    if (key.empty() || !identity) {
        return false;
    }
    if (key == site_key(*identity)) {
        return true;
    }
    const auto keys = ordered_app_keys(*identity);
    return std::ranges::find(keys, key) != keys.end();
}

std::vector<Override> parse_overrides(const Json& raw) {
    std::vector<Override> pairs;
    if (!raw.is_array()) {
        return pairs;
    }
    std::set<std::string> seen;
    for (const auto& item : raw) {
        if (!item.is_object()) {
            continue;
        }
        const auto match_entry = item.find("match");
        const auto category_entry = item.find("category");
        if (match_entry == item.end() || !match_entry->is_string() ||
            category_entry == item.end() || !is_category(*category_entry)) {
            continue;
        }
        std::string match = py::slice(py::strip(match_entry->get_ref<const std::string&>()), 0,
                                      static_cast<std::int64_t>(MAX_MATCH_CHARS));
        std::string key = py::casefold(match);
        if (match.empty() || seen.contains(key)) {
            continue;
        }
        seen.insert(std::move(key));
        pairs.emplace_back(std::move(match), category_entry->get<std::string>());
        if (pairs.size() >= MAX_OVERRIDES) {
            break;
        }
    }
    return pairs;
}

std::optional<AppKind> classify(const std::optional<AppIdentity>& identity,
                                const std::vector<Override>& overrides) {
    if (!identity || identity->app_id.empty()) {
        return std::nullopt;
    }
    const KnownApp* app = lookup(identity->app_id);
    const std::string surface = app != nullptr ? app->surface : std::string(TEXT);
    std::string name = app_name(identity->app_id, identity->name);
    std::map<std::string, std::string> by_key;
    for (const auto& [match, category] : overrides) {
        by_key.insert_or_assign(py::casefold(match), category);
    }
    const std::string hint = site_key(*identity);
    if (!hint.empty()) {
        const KnownSite* known = site(hint);
        name = known != nullptr ? known->name : identity->title_hint;
        if (const auto found = by_key.find(hint); found != by_key.end()) {
            return AppKind{found->second, surface, name};
        }
        if (known != nullptr) {
            return AppKind{known->category, surface, name};
        }
    }
    for (const auto& key : ordered_app_keys(*identity)) {
        if (const auto found = by_key.find(key); found != by_key.end()) {
            return AppKind{found->second, surface, name};
        }
    }
    return AppKind{app != nullptr ? app->category : std::string(OTHER), surface, name};
}

std::string surface_for(const std::optional<AppIdentity>& identity) {
    const KnownApp* app = identity ? lookup(identity->app_id) : nullptr;
    return app != nullptr ? app->surface : std::string(TEXT);
}

bool is_terminal(const std::optional<AppIdentity>& identity) {
    return identity.has_value() && surface_for(identity) == TERMINAL;
}

std::vector<std::string> known_names() {
    std::set<std::string> names;
    for (const auto& app : tables().apps) {
        names.insert(app.name);
    }
    for (const auto& known : tables().sites) {
        names.insert(known.name);
    }
    std::vector<std::pair<std::string, std::string>> keyed;
    for (const auto& name : names) {
        keyed.emplace_back(py::casefold(name), name);
    }
    std::ranges::stable_sort(keyed, {}, &std::pair<std::string, std::string>::first);
    std::vector<std::string> sorted;
    for (auto& entry : keyed) {
        sorted.push_back(std::move(entry.second));
    }
    return sorted;
}

std::optional<std::string> builtin_category(std::string_view name) {
    const std::string key = py::casefold(py::strip(name));
    if (const KnownSite* known = site_by_key(key)) {
        return known->category;
    }
    for (const auto& app : tables().apps) {
        if (py::casefold(app.name) == key) {
            return app.category;
        }
    }
    return std::nullopt;
}

const std::vector<std::pair<std::string, std::vector<std::string>>>& featured() {
    return tables().featured;
}

const std::vector<KnownApp>& apps() {
    return tables().apps;
}

const std::vector<KnownSite>& sites() {
    return tables().sites;
}

} // namespace fv::focus::catalog
