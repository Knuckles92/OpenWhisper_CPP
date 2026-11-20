#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// Saved webhooks, programs and text filters, Settings > Hooks (services/automation/models.py).
// Only Settings creates or changes them; the live API and MCP never do. Each list keeps fixed
// field names (the backup scrub drops keys that look secret) and its secrets live in the
// credential store. Validation errors are py::ValueError with the sentence shown to the user;
// the mutators edit the settings mapping they are given, for SettingsStore::mutate, and change
// nothing when they throw. Every function is thread-safe and pure over its arguments.
namespace fv::automation::hooks {

inline constexpr std::size_t MAX_WEBHOOKS = 20;
inline constexpr std::size_t MAX_PROGRAMS = 20;
inline constexpr std::size_t MAX_FILTERS = 10;
// Lengths in code points.
inline constexpr std::size_t NAME_MAX_CHARS = 80;
inline constexpr std::size_t MAX_ARGS = 32;
inline constexpr std::size_t ARG_MAX_CHARS = 1000;
inline constexpr std::size_t URL_MAX_CHARS = 2000;
// Event types a loaded hook keeps at most, before unknown ones are dropped.
inline constexpr std::size_t MAX_EVENT_NAMES = 64;

inline constexpr std::string_view BEFORE_CLEANUP = "before_cleanup";
inline constexpr std::string_view AFTER_CLEANUP = "after_cleanup";
inline constexpr std::string_view STAGES[] = {BEFORE_CLEANUP, AFTER_CLEANUP};
inline constexpr std::string_view FILTER_HTTP = "http";
inline constexpr std::string_view FILTER_PROGRAM = "program";
inline constexpr std::string_view FILTER_KINDS[] = {FILTER_HTTP, FILTER_PROGRAM};

inline constexpr std::int64_t PROGRAM_TIMEOUT_DEFAULT_S = 30;
inline constexpr std::int64_t PROGRAM_TIMEOUT_MAX_S = 300;
inline constexpr std::int64_t FILTER_TIMEOUT_DEFAULT_MS = 1500;
inline constexpr std::int64_t FILTER_TIMEOUT_MIN_MS = 100;
inline constexpr std::int64_t FILTER_TIMEOUT_MAX_MS = 5000;

// secrets.token_hex(6).
std::string new_id();
// The credential name of a webhook's signing secret: "<env_prefix>WEBHOOK_SECRET_<id>".
std::string webhook_secret_name(std::string_view hook_id);

// Affinity: value types.
struct Webhook {
    std::string id;
    std::string name;
    std::string url;
    std::vector<std::string> events;
    bool enabled = true;

    // {id, name, url, events, enabled}.
    Json to_dict() const;
    friend bool operator==(const Webhook&, const Webhook&) = default;
};

struct Program {
    std::string id;
    std::string name;
    std::string program;
    std::vector<std::string> args;
    std::vector<std::string> events;
    bool enabled = true;
    std::int64_t timeout_s = PROGRAM_TIMEOUT_DEFAULT_S;

    // {id, name, program, args, events, enabled, timeout_s}.
    Json to_dict() const;
    friend bool operator==(const Program&, const Program&) = default;
};

struct TextFilter {
    std::string id;
    std::string name;
    std::string kind;
    std::string stage;
    std::string url;
    std::string program;
    std::vector<std::string> args;
    std::int64_t timeout_ms = FILTER_TIMEOUT_DEFAULT_MS;
    bool enabled = true;

    // {id, name, kind, stage, url, program, args, timeout_ms, enabled}.
    Json to_dict() const;
    friend bool operator==(const TextFilter&, const TextFilter&) = default;
};

// The saved lists, in order. Rows without a non-blank id and name (and url, program, or a known
// kind and stage), repeated ids and rows past the limit are skipped; text is stripped, events
// cut to 64 names and then to known public types, a non-bool `enabled` reads true, and an
// integer out of range is clamped into it (any other value reads as the default).
std::vector<Webhook> load_webhooks(const Json& settings);
std::vector<Program> load_programs(const Json& settings);
std::vector<TextFilter> load_filters(const Json& settings);

// Throws py::ValueError unless `url` is an http(s) URL with a host, no whitespace and at most
// 2000 code points.
void validate_url(std::string_view url);
// True for loopback, link-local and private hosts (no encryption warning needed): localhost and
// names under .localhost, .local, .lan and .home.arpa, and literal private addresses.
bool url_is_private(std::string_view url);

// Throw py::ValueError with the reason the item cannot be saved: its name (blank, too long, or
// another item's, case-folded), its address or program, its events, its time limit, and the
// limit on how many there may be (for a new id only).
void validate_webhook(const Webhook& hook, const Json& settings);
void validate_program(const Program& program, const Json& settings);
void validate_filter(const TextFilter& text_filter, const Json& settings);

// Add the item (stripped, events filtered to known types) at the end, or replace the one with
// its id in place; throw "This <noun> has no id." or as the validators.
void save_webhook(Json& settings, const Webhook& hook);
void save_program(Json& settings, const Program& program);
void save_filter(Json& settings, const TextFilter& text_filter);
// Remove one; the saved list is rewritten from its loader.
void delete_webhook(Json& settings, std::string_view hook_id);
void delete_program(Json& settings, std::string_view program_id);
void delete_filter(Json& settings, std::string_view filter_id);

std::optional<Webhook> find_webhook(const Json& settings, std::string_view hook_id);
std::optional<Program> find_program(const Json& settings, std::string_view program_id);
std::optional<TextFilter> find_filter(const Json& settings, std::string_view filter_id);

} // namespace fv::automation::hooks
