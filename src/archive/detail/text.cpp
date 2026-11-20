#include "archive/detail/text.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "base/utf8.h"

namespace fv::archive::detail {

namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string component(const std::filesystem::path& part) {
    std::string text = path_to_utf8(part);
    if (kWindows) {
        std::transform(text.begin(), text.end(), text.begin(), ascii_lower);
    }
    return text;
}

} // namespace

std::string python_repr(std::string_view text) {
    const bool has_single = text.find('\'') != std::string_view::npos;
    const bool has_double = text.find('"') != std::string_view::npos;
    const char quote = (has_single && !has_double) ? '"' : '\'';
    std::string out(1, quote);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == quote || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (byte < 0x20 || byte == 0x7F) {
            out += std::format("\\x{:02x}", byte);
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, at - start));
        start = at + 1;
    }
}

std::string join(const std::vector<std::string>& parts, char separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

std::string normpath(std::string_view name) {
    std::string text(name);
    if (kWindows) {
        std::replace(text.begin(), text.end(), '\\', '/');
    }
    const bool absolute = text.starts_with('/');
    std::vector<std::string> kept;
    for (auto& part : split(text, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!kept.empty() && kept.back() != "..") {
                kept.pop_back();
            } else if (!absolute) {
                kept.push_back(part);
            }
            continue;
        }
        kept.push_back(std::move(part));
    }
    std::string out = (absolute ? "/" : "") + join(kept, '/');
    return out.empty() ? "." : out;
}

std::filesystem::path realpath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    if (kWindows) {
        absolute = absolute.lexically_normal();
    }
    auto resolved = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        resolved = absolute.lexically_normal();
    }
    // Python drops the \\?\ prefix GetFinalPathNameByHandle adds.
    const auto native = resolved.native();
    if (kWindows && native.size() > 4 && native[0] == '\\' && native[1] == '\\' &&
        native[2] == '?' && native[3] == '\\') {
        resolved = std::filesystem::path(native.substr(4));
    }
    // weakly_canonical keeps a trailing separator as an empty last element.
    if (!resolved.has_filename() && resolved.has_relative_path()) {
        resolved = resolved.parent_path();
    }
    return resolved;
}

bool is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto it = path.begin();
    for (const auto& part : root) {
        if (part.empty()) {
            continue;
        }
        while (it != path.end() && it->empty()) {
            ++it;
        }
        if (it == path.end() || component(*it) != component(part)) {
            return false;
        }
        ++it;
    }
    return true;
}

} // namespace fv::archive::detail
