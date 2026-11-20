#include "net/detail/router.h"

#include <stdexcept>

namespace fv::net::detail {

namespace {

bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

bool is_identifier(std::string_view text) {
    if (text.empty() || !is_name_start(text.front())) {
        return false;
    }
    for (const char c : text) {
        if (!is_name_char(c)) {
            return false;
        }
    }
    return true;
}

} // namespace

RoutePattern::RoutePattern(std::string_view pattern) {
    std::string literal;
    std::vector<std::string> names;
    std::size_t i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c != '{') {
            literal += c;
            ++i;
            continue;
        }
        const auto close = pattern.find('}', i);
        const auto inner = close == std::string_view::npos ? std::string_view()
                                                           : pattern.substr(i + 1, close - i - 1);
        const auto colon = inner.find(':');
        const auto name = inner.substr(0, colon);
        const auto converter =
            colon == std::string_view::npos ? std::string_view("str") : inner.substr(colon + 1);
        // Starlette's PARAM_REGEX: anything else in braces is literal text.
        if (close == std::string_view::npos || !is_identifier(name) ||
            (colon != std::string_view::npos && !is_identifier(converter))) {
            literal += c;
            ++i;
            continue;
        }
        Token::Kind kind;
        if (converter == "str") {
            kind = Token::Kind::Segment;
        } else if (converter == "int") {
            kind = Token::Kind::Digits;
        } else if (converter == "path") {
            kind = Token::Kind::Path;
        } else {
            throw std::invalid_argument("Unknown path convertor '" + std::string(converter) + "'");
        }
        for (const auto& seen : names) {
            if (seen == name) {
                throw std::invalid_argument("Duplicated param name " + std::string(name) +
                                            " at path " + std::string(pattern));
            }
        }
        names.emplace_back(name);
        if (!literal.empty()) {
            tokens_.push_back({std::move(literal), Token::Kind::Literal});
            literal.clear();
        }
        tokens_.push_back({std::string(name), kind});
        i = close + 1;
    }
    if (!literal.empty()) {
        tokens_.push_back({std::move(literal), Token::Kind::Literal});
    }
}

std::optional<Params> RoutePattern::match(std::string_view path) const {
    Params params;
    if (!match_from(0, path, 0, params)) {
        return std::nullopt;
    }
    return params;
}

bool RoutePattern::match_from(std::size_t token, std::string_view path, std::size_t position,
                              Params& params) const {
    if (token == tokens_.size()) {
        return position == path.size();
    }
    const Token& current = tokens_[token];
    if (current.kind == Token::Kind::Literal) {
        if (path.substr(position, current.text.size()) != current.text) {
            return false;
        }
        return match_from(token + 1, path, position + current.text.size(), params);
    }
    // The longest stretch this parameter may take, then shorter ones (a greedy regex group).
    std::size_t longest = path.size();
    std::size_t shortest = position + 1;
    if (current.kind == Token::Kind::Segment) {
        longest = std::min(path.find('/', position), path.size());
    } else if (current.kind == Token::Kind::Digits) {
        longest = position;
        while (longest < path.size() && path[longest] >= '0' && path[longest] <= '9') {
            ++longest;
        }
    } else {
        shortest = position;
    }
    for (std::size_t end = longest + 1; end-- > shortest;) {
        params.emplace_back(current.text, std::string(path.substr(position, end - position)));
        if (match_from(token + 1, path, end, params)) {
            return true;
        }
        params.pop_back();
    }
    return false;
}

void Router::add(Route route) {
    routes_.push_back(std::move(route));
}

RouteMatch Router::match(std::string_view method, std::string_view path) const {
    RouteMatch result;
    const Route* partial = nullptr;
    for (const Route& route : routes_) {
        auto params = route.pattern.match(path);
        if (!params) {
            continue;
        }
        if (route.method == method || (method == "HEAD" && route.method == "GET")) {
            result.route = &route;
            result.params = std::move(*params);
            return result;
        }
        if (partial == nullptr) {
            partial = &route;
        }
    }
    if (partial != nullptr) {
        result.path_known = true;
        result.allowed.push_back(partial->method);
        if (partial->method == "GET") {
            result.allowed.emplace_back("HEAD");
        }
        return result;
    }
    if (path != "/") {
        std::string toggled(path);
        if (toggled.ends_with('/')) {
            toggled.pop_back();
        } else {
            toggled += '/';
        }
        for (const Route& route : routes_) {
            if (route.pattern.match(toggled)) {
                result.redirect_path = std::move(toggled);
                break;
            }
        }
    }
    return result;
}

} // namespace fv::net::detail
