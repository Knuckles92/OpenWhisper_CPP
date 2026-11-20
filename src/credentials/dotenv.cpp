#include "credentials/dotenv.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <system_error>

#include "base/log.h"
#include "credentials/detail/utf8_check.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.credentials");
constexpr log::Logger dotenv_logger("dotenv.main");

using Values = std::vector<std::pair<std::string, std::optional<std::string>>>;

// python-dotenv 1.2.3's parser.py patterns, verbatim.
struct Patterns {
    py::Regex multiline_whitespace{R"(\s*)", py::MULTILINE};
    py::Regex whitespace{R"([^\S\r\n]*)"};
    py::Regex export_prefix{R"((?:export[^\S\r\n]+)?)"};
    py::Regex single_quoted_key{R"('([^']+)')"};
    py::Regex unquoted_key{R"(([^=\#\s]+))"};
    py::Regex equal_sign{R"((=[^\S\r\n]*))"};
    py::Regex single_quoted_value{R"('((?:\\.|[^'\\])*)')", py::DOTALL};
    py::Regex double_quoted_value{R"x("((?:\\.|[^"\\])*)")x", py::DOTALL};
    py::Regex unquoted_value{R"(([^\r\n]*))"};
    py::Regex comment{R"((?:[^\S\r\n]*#[^\r\n]*)?)"};
    py::Regex end_of_line{R"([^\S\r\n]*(?:\r\n|\n|\r|$))"};
    py::Regex rest_of_line{R"([^\r\n]*(?:\r|\n|\r\n)?)"};
    py::Regex double_quote_escapes{R"(\\[\\'"abfnrtv])"};
    py::Regex single_quote_escapes{R"(\\[\\'])"};
    py::Regex inline_comment{R"(\s+#.*)"};
    // variables.py's _posix_variable, without the VERBOSE layout.
    py::Regex posix_variable{R"(\$\{(?P<name>[^\}:]*)(?::-(?P<default>[^\}]*))?\})"};
};

const Patterns& patterns() {
    static const Patterns compiled;
    return compiled;
}

// parser.py's Error: a statement that does not parse.
struct ParseError {};

// parser.py's Reader, over code point positions as Python's are, with the byte offset kept
// alongside.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool has_next() const { return byte_ < text_.size(); }
    void set_mark() { mark_line_ = line_; }
    int marked_line() const { return mark_line_; }

    // The next code point as a string, "" at the end.
    std::string_view peek() const {
        if (!has_next()) {
            return {};
        }
        std::size_t next = byte_;
        py::next_code_point(text_, next);
        return text_.substr(byte_, next - byte_);
    }

    std::vector<std::optional<std::string>> read_regex(const py::Regex& regex) {
        auto match = regex.match(text_, chars_);
        if (!match.has_value()) {
            throw ParseError{};
        }
        const std::size_t end = match->byte_end(0);
        // Position.advance: re.findall(r"(\r\n|\n|\r)") over the text consumed, so "\r\n" inside
        // it is one line end and a lone "\r" (text not read through universal newlines) is one.
        for (std::size_t i = byte_; i < end; ++i) {
            if (text_[i] == '\n' || (text_[i] == '\r' && (i + 1 == end || text_[i + 1] != '\n'))) {
                ++line_;
            }
        }
        chars_ = match->end(0);
        byte_ = end;
        std::vector<std::optional<std::string>> groups;
        for (std::size_t i = 1; i <= regex.group_count(); ++i) {
            groups.push_back(match->group_if(i));
        }
        return groups;
    }

private:
    std::string_view text_;
    std::size_t chars_ = 0;
    std::size_t byte_ = 0;
    int line_ = 1;
    int mark_line_ = 1;
};

std::string decode_escapes(const py::Regex& regex, std::string_view value) {
    return regex.sub(
        [](const py::Match& match) -> std::string {
            // codecs.decode(..., "unicode-escape") of the two characters matched.
            switch (match.group(0)[1]) {
            case 'a':
                return "\a";
            case 'b':
                return "\b";
            case 'f':
                return "\f";
            case 'n':
                return "\n";
            case 'r':
                return "\r";
            case 't':
                return "\t";
            case 'v':
                return "\v";
            default:
                return match.group(0).substr(1);
            }
        },
        value);
}

std::optional<std::string> parse_key(Reader& reader) {
    const std::string_view next = reader.peek();
    if (next == "#") {
        return std::nullopt;
    }
    const auto& p = patterns();
    return *reader.read_regex(next == "'" ? p.single_quoted_key : p.unquoted_key).at(0);
}

std::string parse_value(Reader& reader) {
    const auto& p = patterns();
    const std::string_view next = reader.peek();
    if (next == "'") {
        return decode_escapes(p.single_quote_escapes,
                              *reader.read_regex(p.single_quoted_value).at(0));
    }
    if (next == "\"") {
        return decode_escapes(p.double_quote_escapes,
                              *reader.read_regex(p.double_quoted_value).at(0));
    }
    if (next.empty() || next == "\n" || next == "\r") {
        return "";
    }
    const std::string part = *reader.read_regex(p.unquoted_value).at(0);
    return py::rstrip(p.inline_comment.sub("", part));
}

struct Binding {
    std::optional<std::string> key;
    std::optional<std::string> value;
};

// parser.py's parse_binding; nullopt for a statement that does not parse.
std::optional<Binding> parse_binding(Reader& reader) {
    const auto& p = patterns();
    reader.set_mark();
    try {
        reader.read_regex(p.multiline_whitespace);
        if (!reader.has_next()) {
            return Binding{};
        }
        reader.read_regex(p.export_prefix);
        Binding binding;
        binding.key = parse_key(reader);
        reader.read_regex(p.whitespace);
        if (reader.peek() == "=") {
            reader.read_regex(p.equal_sign);
            binding.value = parse_value(reader);
        }
        reader.read_regex(p.comment);
        reader.read_regex(p.end_of_line);
        return binding;
    } catch (const ParseError&) {
        reader.read_regex(p.rest_of_line);
        return std::nullopt;
    }
}

// The value a ${name} reads from the process environment. Python's os.environ is a dict whose
// keys Windows has upper-cased, so there only an upper-case name finds a variable.
std::optional<std::string> environment_value(const EnvReader& env, const std::string& name) {
    if (name.empty() || name.find('=') != std::string::npos ||
        name.find('\0') != std::string::npos) {
        return std::nullopt;
    }
#if defined(_WIN32)
    if (py::upper(name) != name) {
        return std::nullopt;
    }
#endif
    return env.get(name);
}

// main.py's resolve_variables with override=True: names above in the file win over the
// environment; a name without a value reads as "".
Values resolve_variables(const Values& raw, const EnvReader& env) {
    Values resolved;
    std::map<std::string, std::size_t, std::less<>> index;
    for (const auto& [name, value] : raw) {
        std::optional<std::string> result;
        if (value.has_value()) {
            std::string text;
            std::size_t cursor = 0; // bytes
            for (const py::Match& match : patterns().posix_variable.finditer(*value)) {
                text.append(*value, cursor, match.byte_start(0) - cursor);
                const std::string variable = match.group("name");
                const std::optional<std::string> fallback = match.group_if(2);
                if (const auto known = index.find(variable); known != index.end()) {
                    text += resolved[known->second].second.value_or("");
                } else if (auto from_env = environment_value(env, variable)) {
                    text += *from_env;
                } else {
                    text += fallback.value_or("");
                }
                cursor = match.byte_end(0);
            }
            text.append(*value, cursor);
            result = std::move(text);
        }
        if (const auto known = index.find(name); known != index.end()) {
            resolved[known->second].second = std::move(result);
        } else {
            index.emplace(name, resolved.size());
            resolved.emplace_back(name, std::move(result));
        }
    }
    return resolved;
}

// Python's universal newlines: "\r\n" and "\r" read as "\n".
std::string translate_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace

Values parse_dotenv(std::string_view text, const EnvReader& interpolation_env) {
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    if (text.starts_with(bom)) {
        text.remove_prefix(bom.size());
    }
    Reader reader(text);
    Values raw;
    while (reader.has_next()) {
        const auto binding = parse_binding(reader);
        if (!binding.has_value()) {
            dotenv_logger.warning("python-dotenv could not parse statement starting at line {}",
                                  reader.marked_line());
            continue;
        }
        if (binding->key.has_value()) {
            raw.emplace_back(*binding->key, binding->value);
        }
    }
    return resolve_variables(raw, interpolation_env);
}

struct DotEnv::State {
    std::filesystem::path file;
    const EnvReader* env = nullptr;
    std::mutex mutex;
    bool cached = false;
    std::filesystem::file_time_type time{};
    std::uintmax_t size = 0;
    Values values;
};

DotEnv::DotEnv(std::filesystem::path file) : DotEnv(std::move(file), system_env()) {}

DotEnv::DotEnv(std::filesystem::path file, const EnvReader& interpolation_env)
    : state_(std::make_shared<State>()) {
    state_->file = std::move(file);
    state_->env = &interpolation_env;
}

const std::filesystem::path& DotEnv::file() const {
    return state_->file;
}

Values DotEnv::values() const {
    State& state = *state_;
    std::error_code error;
    const auto status = std::filesystem::status(state.file, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return {};
    }
    const auto time = std::filesystem::last_write_time(state.file, error);
    if (error) {
        return {};
    }
    const auto size = std::filesystem::file_size(state.file, error);
    if (error) {
        return {};
    }
    std::lock_guard lock(state.mutex);
    if (state.cached && state.time == time && state.size == size) {
        return state.values;
    }
    std::ifstream stream(state.file, std::ios::binary);
    if (!stream.is_open()) {
        logger.warning("Failed to read .env file: the file could not be opened");
        return {};
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) {
        logger.warning("Failed to read .env file: the file could not be read");
        return {};
    }
    if (const auto invalid = credentials_detail::check_utf8(bytes)) {
        logger.warning("Failed to read .env file: {}", credentials_detail::describe(*invalid));
        return {};
    }
    try {
        state.values = parse_dotenv(translate_newlines(bytes), *state.env);
    } catch (const std::exception& failure) {
        logger.warning("Failed to read .env file: {}", failure.what());
        return {};
    }
    state.cached = true;
    state.time = time;
    state.size = size;
    return state.values;
}

std::optional<std::string> DotEnv::get(std::string_view name) const {
    for (auto& [key, value] : values()) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace fv
