#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "focus/snapshot.h"

// What we know about apps: their names, kinds of writing and sites. Apps are keyed by Windows
// executable name, macOS bundle id or Linux window class, all compared case-insensitively. A
// browser's window title is read only to name the site it shows (Gmail, Slack, Google Docs...);
// the title itself is never kept, logged or returned. The tables are generated from
// OpenWhisper's services/focus_context/catalog.py into data/app_catalog.json
// (tools/gen/app_catalog.py).
namespace fv::focus::catalog {

// Same values as the app styles' categories and surfaces.
inline constexpr std::string_view EMAIL = "email";
inline constexpr std::string_view WORK = "work";
inline constexpr std::string_view PERSONAL = "personal";
inline constexpr std::string_view OTHER = "other";
inline constexpr std::string_view CATEGORIES[] = {EMAIL, WORK, PERSONAL, OTHER};

inline constexpr std::string_view TEXT = "text";
inline constexpr std::string_view CODE = "code";
inline constexpr std::string_view TERMINAL = "terminal";

inline constexpr std::size_t MAX_OVERRIDES = 100;
inline constexpr std::size_t MAX_MATCH_CHARS = 120;

// Affinity: value type (the catalogue's entries are immutable).
struct KnownApp {
    std::string name;
    std::string category;
    std::string surface;
    std::vector<std::string> ids;
    bool browser = false;

    bool operator==(const KnownApp&) const = default;
};

// Affinity: value type.
struct KnownSite {
    std::string name;
    std::string category;
    // Other ways a title names the site, compared case-insensitively.
    std::vector<std::string> aliases;

    bool operator==(const KnownSite&) const = default;
};

// Where a dictation goes, as far as styles and prompts care.
// Affinity: value type.
struct AppKind {
    std::string category;
    std::string surface;
    // The site when a browser shows a known one, else the app's name.
    std::string name;

    bool operator==(const AppKind&) const = default;
};

// A user's (match, category) choice.
using Override = std::pair<std::string, std::string>;

// Every function here is thread-safe and never throws for any input.

// The built-in entry for an exe name, bundle id or window class, if any.
const KnownApp* lookup(std::string_view app_id);
const KnownSite* site(std::string_view name);
bool is_browser(std::string_view app_id);
// A readable name for an app the catalogue doesn't know.
std::string pretty_name(std::string_view app_id);
// The catalogue's name for app_id, else fallback, else a tidy id.
std::string app_name(std::string_view app_id, std::string_view fallback = {});
// The known site or product a browser title names, or "".
std::string site_from_title(std::string_view title);
// The site a browser window shows, from its title; "" for other apps.
std::string title_hint(std::string_view app_id, std::string_view title);
// Every name an app-level override or exclusion may use for identity.
std::set<std::string> app_keys(const AppIdentity& identity);
std::string site_key(const AppIdentity& identity);
// Whether a user-entered app or site name refers to identity.
bool matches(const std::optional<AppIdentity>& identity, std::string_view value);
// Valid (match, category) pairs from stored overrides (a JSON list of {match, category}
// objects), first one wins.
std::vector<Override> parse_overrides(const Json& raw);
// Category, surface and name for identity, or nullopt when it is unknown. A site is more
// specific than the browser showing it, so the order is: the user's choice for the site, the
// built-in site, the user's choice for the app, the built-in app, else Other.
std::optional<AppKind> classify(const std::optional<AppIdentity>& identity,
                                const std::vector<Override>& overrides = {});
std::string surface_for(const std::optional<AppIdentity>& identity);
bool is_terminal(const std::optional<AppIdentity>& identity);
// Every app and site name the catalogue knows, sorted case-insensitively, without repeats.
std::vector<std::string> known_names();
// The category a known app or site name has before any user choice.
std::optional<std::string> builtin_category(std::string_view name);

// Shown on the Styles page as each category's examples, in CATEGORIES order.
const std::vector<std::pair<std::string, std::vector<std::string>>>& featured();
// The whole tables, in declaration order.
const std::vector<KnownApp>& apps();
const std::vector<KnownSite>& sites();

} // namespace fv::focus::catalog
