#include "agents/agent_config.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>

#include "agents/detail/home.h"
#include "agents/detail/pyvalue.h"
#include "agents/installed_agents.h"
#include "base/env.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::agents {

namespace {

// ---- the part of TOML that holds Codex's top-level model ----

// Thrown where tomllib would raise TOMLDecodeError.
struct TomlError {};

const py::Regex& scalar_pattern() {
    // Booleans, integers (decimal, hex, octal, binary), floats, inf and nan, dates and times.
    static const py::Regex pattern(
        "(?:true|false"
        "|[+-]?(?:inf|nan)"
        "|0x[0-9A-Fa-f](?:_?[0-9A-Fa-f])*|0o[0-7](?:_?[0-7])*|0b[01](?:_?[01])*"
        "|[+-]?(?:0|[1-9](?:_?[0-9])*)(?:\\.[0-9](?:_?[0-9])*)?(?:[eE][+-]?[0-9](?:_?[0-9])*)?"
        "|[0-9]{4}-[0-9]{2}-[0-9]{2}"
        "(?:[Tt ][0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]+)?(?:[Zz]|[+-][0-9]{2}:[0-9]{2})?)?"
        "|[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]+)?)");
    return pattern;
}

class TomlReader {
public:
    explicit TomlReader(std::string_view text) : text_(text) {}

    // The top-level model: a string, or nullopt when absent or anything else.
    std::optional<std::string> model() {
        parse();
        return model_is_string_ ? model_ : std::nullopt;
    }

    bool supports_isolated_cleanup() {
        parse();
        return isolated_cleanup_;
    }

private:
    void parse() {
        std::vector<std::string> table;
        while (!at_end()) {
            skip_ws();
            if (at_end()) {
                break;
            }
            if (peek() == '[') {
                table = header();
                check_cleanup_destination(table, std::nullopt);
                if (!table.empty() && table.front() == "model") {
                    model_is_string_ = false;
                }
            } else if (peek() != '#' && !at_newline()) {
                keyval(table);
            }
            end_of_line();
        }
    }

    void check_cleanup_destination(const std::vector<std::string>& path,
                                   const std::optional<std::string>& value) {
        constexpr std::array<std::string_view, 11> routing_keys{"openai_base_url",
                                                                "chatgpt_base_url",
                                                                "forced_login_method",
                                                                "forced_chatgpt_workspace_id",
                                                                "cli_auth_credentials_store",
                                                                "profile",
                                                                "oss_provider",
                                                                "model_catalog_json",
                                                                "experimental_realtime_ws_base_url",
                                                                "chatgpt_auth_api_base_url",
                                                                "fallback_model_provider"};
        // Definitions and profiles alone do not select a provider. A selected profile can
        // alter the provider or identity, so it cannot be silently discarded.
        const auto& top = path.front();
        if ((top == "model_provider" && (path.size() != 1 || value != "openai")) ||
            std::ranges::find(routing_keys, top) != routing_keys.end()) {
            isolated_cleanup_ = false;
        }
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }
    bool at_newline() const { return peek() == '\n' || (peek() == '\r' && peek(1) == '\n'); }
    bool starts_with(std::string_view what) const {
        return pos_ <= text_.size() && text_.substr(pos_).starts_with(what);
    }

    void skip_ws() {
        while (peek() == ' ' || peek() == '\t') {
            ++pos_;
        }
    }

    static bool is_control(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7F; }

    void comment() {
        // '#' already seen.
        ++pos_;
        while (!at_end() && !at_newline()) {
            if (is_control(static_cast<unsigned char>(peek()))) {
                throw TomlError{};
            }
            ++pos_;
        }
    }

    void newline() {
        if (peek() == '\n') {
            ++pos_;
        } else if (peek() == '\r' && peek(1) == '\n') {
            pos_ += 2;
        } else {
            throw TomlError{};
        }
    }

    void end_of_line() {
        skip_ws();
        if (peek() == '#') {
            comment();
        }
        if (!at_end()) {
            newline();
        }
    }

    // Blank lines and comments inside an array.
    void skip_ws_comments_newlines() {
        for (;;) {
            skip_ws();
            if (peek() == '#') {
                comment();
            } else if (at_newline()) {
                newline();
            } else {
                return;
            }
        }
    }

    static bool is_bare(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    }

    std::vector<std::string> key() {
        std::vector<std::string> parts;
        for (;;) {
            skip_ws();
            if (peek() == '"') {
                if (starts_with("\"\"\"")) {
                    throw TomlError{};
                }
                parts.push_back(basic_string());
            } else if (peek() == '\'') {
                if (starts_with("'''")) {
                    throw TomlError{};
                }
                parts.push_back(literal_string());
            } else {
                const auto start = pos_;
                while (is_bare(peek())) {
                    ++pos_;
                }
                if (start == pos_) {
                    throw TomlError{};
                }
                parts.emplace_back(text_.substr(start, pos_ - start));
            }
            skip_ws();
            if (peek() != '.') {
                return parts;
            }
            ++pos_;
        }
    }

    static std::string joined(const std::vector<std::string>& path) {
        std::string out;
        for (const auto& part : path) {
            out += part;
            out += '\x1f';
        }
        return out;
    }

    std::vector<std::string> header() {
        const bool array = starts_with("[[");
        pos_ += array ? 2 : 1;
        auto path = key();
        if (array ? !starts_with("]]") : peek() != ']') {
            throw TomlError{};
        }
        pos_ += array ? 2 : 1;
        const auto name = joined(path);
        if (values_.contains(name) || (!array && tables_.contains(name))) {
            throw TomlError{};
        }
        if (array) {
            // Each [[name]] starts a new element: what the previous one defined is not taken.
            const auto nested = [&](const std::string& key) {
                return key.size() > name.size() && key.starts_with(name);
            };
            std::erase_if(values_, nested);
            std::erase_if(tables_, nested);
        }
        tables_.insert(name);
        return path;
    }

    void keyval(const std::vector<std::string>& table) {
        auto path = key();
        if (peek() != '=') {
            throw TomlError{};
        }
        ++pos_;
        skip_ws();
        const bool top_model = table.empty() && path.size() == 1 && path.front() == "model";
        if (table.empty() && path.size() > 1 && path.front() == "model") {
            model_is_string_ = false;
        }
        auto full = table;
        full.insert(full.end(), path.begin(), path.end());
        std::vector<std::string> prefix;
        for (std::size_t i = 0; i + 1 < full.size(); ++i) {
            prefix.push_back(full[i]);
            if (values_.contains(joined(prefix))) {
                throw TomlError{};
            }
        }
        const auto name = joined(full);
        if (values_.contains(name) || tables_.contains(name)) {
            throw TomlError{};
        }
        values_.insert(name);
        auto parsed = value();
        check_cleanup_destination(full, parsed);
        if (top_model) {
            model_ = std::move(parsed);
            model_is_string_ = model_is_string_ && model_.has_value();
        }
    }

    // A value; its text when it is a string.
    std::optional<std::string> value() {
        if (starts_with("\"\"\"")) {
            return multiline_string('"');
        }
        if (starts_with("'''")) {
            return multiline_string('\'');
        }
        if (peek() == '"') {
            return basic_string();
        }
        if (peek() == '\'') {
            return literal_string();
        }
        if (peek() == '[') {
            array();
            return std::nullopt;
        }
        if (peek() == '{') {
            inline_table();
            return std::nullopt;
        }
        scalar();
        return std::nullopt;
    }

    void array() {
        ++pos_;
        for (;;) {
            skip_ws_comments_newlines();
            if (peek() == ']') {
                ++pos_;
                return;
            }
            value();
            skip_ws_comments_newlines();
            if (peek() == ',') {
                ++pos_;
            } else if (peek() == ']') {
                ++pos_;
                return;
            } else {
                throw TomlError{};
            }
        }
    }

    void inline_table() {
        ++pos_;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return;
        }
        std::set<std::string> keys;
        for (;;) {
            auto path = key();
            if (!keys.insert(joined(path)).second || peek() != '=') {
                throw TomlError{};
            }
            ++pos_;
            skip_ws();
            value();
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                skip_ws();
            } else if (peek() == '}') {
                ++pos_;
                return;
            } else {
                throw TomlError{};
            }
        }
    }

    void scalar() {
        const auto start = pos_;
        const auto token_char = [](char c) {
            return is_bare(c) || c == '+' || c == '.' || c == ':';
        };
        while (token_char(peek())) {
            ++pos_;
        }
        // A date and a time may be separated by one space.
        if (peek() == ' ' && peek(1) >= '0' && peek(1) <= '9' && pos_ - start == 10) {
            ++pos_;
            while (token_char(peek())) {
                ++pos_;
            }
        }
        const auto token = text_.substr(start, pos_ - start);
        if (token.empty() || !scalar_pattern().fullmatch(token)) {
            throw TomlError{};
        }
    }

    void append_escape(std::string& out) {
        // '\\' already seen.
        ++pos_;
        if (at_end()) {
            throw TomlError{};
        }
        const char c = peek();
        ++pos_;
        switch (c) {
        case 'b':
            out += '\b';
            return;
        case 't':
            out += '\t';
            return;
        case 'n':
            out += '\n';
            return;
        case 'f':
            out += '\f';
            return;
        case 'r':
            out += '\r';
            return;
        case '"':
            out += '"';
            return;
        case '\\':
            out += '\\';
            return;
        case 'u':
        case 'U': {
            const std::size_t digits = c == 'u' ? 4 : 8;
            if (pos_ + digits > text_.size()) {
                throw TomlError{};
            }
            char32_t cp = 0;
            for (std::size_t i = 0; i < digits; ++i) {
                const char h = text_[pos_ + i];
                int v = 0;
                if (h >= '0' && h <= '9') {
                    v = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                    v = h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                    v = h - 'A' + 10;
                } else {
                    throw TomlError{};
                }
                cp = cp * 16 + static_cast<char32_t>(v);
            }
            pos_ += digits;
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                throw TomlError{};
            }
            py::append_utf8(out, cp);
            return;
        }
        default:
            throw TomlError{};
        }
    }

    std::string basic_string() {
        ++pos_;
        std::string out;
        for (;;) {
            if (at_end()) {
                throw TomlError{};
            }
            const char c = peek();
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                append_escape(out);
                continue;
            }
            if (is_control(static_cast<unsigned char>(c))) {
                throw TomlError{};
            }
            out += c;
            ++pos_;
        }
    }

    std::string literal_string() {
        ++pos_;
        const auto start = pos_;
        for (;;) {
            if (at_end()) {
                throw TomlError{};
            }
            const char c = peek();
            if (c == '\'') {
                std::string out(text_.substr(start, pos_ - start));
                ++pos_;
                return out;
            }
            if (is_control(static_cast<unsigned char>(c))) {
                throw TomlError{};
            }
            ++pos_;
        }
    }

    std::string multiline_string(char quote) {
        pos_ += 3;
        if (at_newline()) {
            newline();
        }
        std::string out;
        for (;;) {
            if (at_end()) {
                throw TomlError{};
            }
            const char c = peek();
            if (c == quote && peek(1) == quote && peek(2) == quote) {
                std::size_t run = 3;
                while (peek(run) == quote) {
                    ++run;
                }
                if (run > 5) {
                    throw TomlError{};
                }
                out.append(run - 3, quote);
                pos_ += run;
                return out;
            }
            if (at_newline()) {
                newline();
                out += '\n';
                continue;
            }
            if (quote == '"' && c == '\\') {
                // A line-ending backslash drops the newline and the whitespace after it.
                std::size_t ahead = 1;
                while (peek(ahead) == ' ' || peek(ahead) == '\t') {
                    ++ahead;
                }
                if (peek(ahead) == '\n' || (peek(ahead) == '\r' && peek(ahead + 1) == '\n')) {
                    pos_ += ahead;
                    for (;;) {
                        skip_ws();
                        if (!at_newline()) {
                            break;
                        }
                        newline();
                    }
                    continue;
                }
                append_escape(out);
                continue;
            }
            if (is_control(static_cast<unsigned char>(c))) {
                throw TomlError{};
            }
            out += c;
            ++pos_;
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::set<std::string> values_;
    std::set<std::string> tables_;
    std::optional<std::string> model_;
    bool model_is_string_ = true;
    bool isolated_cleanup_ = true;
};

std::optional<std::string> read_file(const std::filesystem::path& file) {
    // A folder from an empty home is relative; it names nothing of the user's.
    if (!file.has_parent_path() || file.parent_path().is_relative()) {
        return std::nullopt;
    }
    std::error_code error;
    if (std::filesystem::is_directory(file, error)) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace

std::string strip_jsonc(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    bool in_string = false;
    while (i < n) {
        const char ch = text[i];
        if (in_string) {
            out += ch;
            if (ch == '\\' && i + 1 < n) {
                out += text[i + 1];
                i += 2;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            out += ch;
            ++i;
        } else if (text.substr(i).starts_with("//")) {
            const auto end = text.find('\n', i);
            i = end == std::string_view::npos ? n : end;
        } else if (text.substr(i).starts_with("/*")) {
            const auto end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? n : end + 2;
        } else {
            out += ch;
            ++i;
        }
    }
    static const py::Regex trailing_comma(",(\\s*[}\\]])");
    return trailing_comma.sub("\\1", out);
}

std::optional<std::string> codex_config_model(std::string_view toml) {
    if (!detail::is_valid_utf8(toml)) {
        return std::nullopt;
    }
    try {
        return TomlReader(toml).model();
    } catch (const TomlError&) {
        return std::nullopt;
    }
}

std::optional<Json> read_json_object(const std::filesystem::path& file, bool jsonc) {
    const auto bytes = read_file(file);
    if (!bytes || !detail::is_valid_utf8(*bytes)) {
        return std::nullopt;
    }
    const auto text = detail::universal_newlines(*bytes);
    try {
        auto data = py::loads(jsonc ? strip_jsonc(text) : text);
        if (!data.is_object()) {
            return std::nullopt;
        }
        return data;
    } catch (const py::PyError&) {
        return std::nullopt;
    }
}

std::vector<AgentModel> codex_models_from(const Json& cache) {
    std::vector<AgentModel> models;
    for (const auto& entry : detail::iterate(detail::get(cache, "models"))) {
        if (!entry.is_object()) {
            continue;
        }
        const auto& visibility = detail::get(entry, "visibility");
        if (!visibility.is_null() && visibility != Json("list")) {
            continue;
        }
        const auto& slug = detail::get(entry, "slug");
        if (slug.is_string() && !slug.get_ref<const std::string&>().empty()) {
            const auto& display = detail::get(entry, "display_name");
            models.push_back({slug.get<std::string>(), detail::truthy(display)
                                                           ? detail::py_str(display)
                                                           : slug.get<std::string>()});
        }
    }
    return models;
}

ModelEfforts codex_model_efforts_from(const Json& cache) {
    ModelEfforts efforts;
    for (const auto& entry : detail::iterate(detail::get(cache, "models"))) {
        if (!entry.is_object() || !detail::get(entry, "slug").is_string()) {
            continue;
        }
        std::vector<std::string> levels;
        for (const auto& level :
             detail::iterate(detail::get(entry, "supported_reasoning_levels"))) {
            if (level.is_object() && detail::truthy(detail::get(level, "effort"))) {
                levels.push_back(detail::py_str(detail::get(level, "effort")));
            }
        }
        if (levels.empty()) {
            continue;
        }
        const auto slug = detail::get(entry, "slug").get<std::string>();
        auto it = std::find_if(efforts.begin(), efforts.end(),
                               [&](const auto& item) { return item.first == slug; });
        if (it == efforts.end()) {
            efforts.emplace_back(slug, std::move(levels));
        } else {
            it->second = std::move(levels);
        }
    }
    return efforts;
}

AgentConfig::AgentConfig(EnvLookup env, std::filesystem::path home)
    : env_(std::move(env)), home_(std::move(home)) {}

AgentConfig AgentConfig::system() {
    return AgentConfig([](std::string_view name) { return system_env().get(name); },
                       path_from_utf8(detail::home_folder(system_env())));
}

std::optional<std::string> AgentConfig::env(std::string_view name) const {
    return env_ ? env_(name) : std::nullopt;
}

std::filesystem::path AgentConfig::claude_config_dir() const {
    if (auto dir = env("CLAUDE_CONFIG_DIR"); dir && !dir->empty()) {
        return path_from_utf8(*dir);
    }
    return home_ / ".claude";
}

std::filesystem::path AgentConfig::codex_home() const {
    if (auto dir = env("CODEX_HOME"); dir && !dir->empty()) {
        return path_from_utf8(*dir);
    }
    return home_ / ".codex";
}

std::filesystem::path AgentConfig::opencode_config_dir() const {
    std::filesystem::path base = home_ / ".config";
    if (auto dir = env("XDG_CONFIG_HOME"); dir && !dir->empty()) {
        base = path_from_utf8(*dir);
    }
    return base / "opencode";
}

std::string AgentConfig::configured_default_model(std::string_view agent_id) const {
    std::optional<std::string> model;
    if (agent_id == CLAUDE_CODE) {
        const auto data = read_json_object(claude_config_dir() / "settings.json");
        if (data) {
            const auto& value = detail::get(*data, "model");
            if (value.is_string()) {
                model = value.get<std::string>();
            }
        }
    } else if (agent_id == CODEX) {
        if (const auto text = read_file(codex_home() / "config.toml")) {
            model = codex_config_model(*text);
        }
    } else if (agent_id == OPENCODE) {
        for (const char* name : {"opencode.jsonc", "opencode.json", "config.json"}) {
            const auto data = read_json_object(opencode_config_dir() / name, true);
            if (data && detail::get(*data, "model").is_string()) {
                model = detail::get(*data, "model").get<std::string>();
                break;
            }
        }
    }
    return model ? py::strip(*model) : std::string();
}

bool AgentConfig::supports_isolated_cleanup(std::string_view agent_id) const {
    const bool codex = agent_id == CODEX;
    if (!codex && agent_id != CLAUDE_CODE) {
        return false;
    }
    if (codex && !env("CODEX_PROFILE").value_or("").empty()) {
        return false;
    }
    const auto path = codex ? codex_home() / "config.toml" : claude_config_dir() / "settings.json";
    if (!path.is_absolute()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return !error;
    }
    if (codex) {
        const auto text = read_file(path);
        if (!text || !detail::is_valid_utf8(*text)) {
            return false;
        }
        try {
            return TomlReader(*text).supports_isolated_cleanup();
        } catch (const TomlError&) {
            return false;
        }
    }
    const auto settings = read_json_object(path);
    if (!settings) {
        return false;
    }
    constexpr std::array<std::string_view, 7> settings_overrides{
        "apiKeyHelper",     "awsAuthRefresh",    "awsCredentialExport", "gcpAuthRefresh",
        "forceLoginMethod", "forceLoginOrgUUID", "modelOverrides"};
    for (const auto name : settings_overrides) {
        const auto field = settings->find(name);
        if (field != settings->end() && !field->empty()) {
            return false;
        }
    }
    const auto environment = settings->find("env");
    if (environment == settings->end() || environment->is_null()) {
        return true;
    }
    if (!environment->is_object()) {
        return false;
    }
    // These variables can change the API destination, account, transport, or model alias.
    for (const auto& [name, value] : environment->items()) {
        const auto key = py::upper(name);
        if (key.starts_with("ANTHROPIC_") || key.starts_with("CLAUDE_CODE_USE_") ||
            key.starts_with("CLAUDE_CODE_OAUTH_") || key.starts_with("AWS_") ||
            key.starts_with("GOOGLE_") || key.starts_with("CLOUD_ML_") || key.ends_with("_PROXY") ||
            key == "CLAUDE_CODE_PROVIDER_MANAGED_BY_HOST") {
            if (!value.is_null()) {
                return false;
            }
        }
    }
    return true;
}

std::vector<AgentModel> AgentConfig::codex_models() const {
    const auto data = read_json_object(codex_home() / "models_cache.json");
    return codex_models_from(data.value_or(Json::object()));
}

ModelEfforts AgentConfig::codex_model_efforts() const {
    const auto data = read_json_object(codex_home() / "models_cache.json");
    return codex_model_efforts_from(data.value_or(Json::object()));
}

std::string AgentConfig::default_model_label(std::string_view agent_id) const {
    const auto configured = configured_default_model(agent_id);
    const auto& name = agent_spec(agent_id).name;
    return configured.empty() ? std::string(name) + " default"
                              : std::string(name) + " default (" + configured + ")";
}

} // namespace fv::agents
