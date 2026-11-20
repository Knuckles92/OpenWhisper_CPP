#include "meeting_web/auth.h"

#include "base/ids.h"
#include "meeting_web/detail/py_value.h"

namespace fv::meeting_web {

namespace {

std::optional<std::string> stored_token(const Json& value) {
    if (!detail::truthy(value)) {
        return std::nullopt;
    }
    return detail::py_str(value);
}

} // namespace

std::string generate_token() {
    return token_urlsafe(kTokenBytes);
}

std::pair<std::string, std::string> generate_token_pair() {
    std::string host = generate_token();
    std::string guest = generate_token();
    return {std::move(host), std::move(guest)};
}

bool tokens_match(std::string_view candidate, std::string_view expected) {
    if (expected.empty()) {
        return false;
    }
    // compare_digest: the loop runs over the expected token whatever the candidate holds, so
    // the time taken says nothing about where the first difference is.
    unsigned int difference = candidate.size() == expected.size() ? 0U : 1U;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const unsigned char left =
            i < candidate.size() ? static_cast<unsigned char>(candidate[i]) : 0U;
        difference |= static_cast<unsigned int>(left ^ static_cast<unsigned char>(expected[i]));
    }
    return difference == 0;
}

std::optional<std::string> resolve_role(std::optional<std::string_view> token,
                                        std::optional<std::string_view> host_token,
                                        std::optional<std::string_view> guest_token) {
    if (!token || token->empty()) {
        return std::nullopt;
    }
    if (host_token && tokens_match(*token, *host_token)) {
        return std::string(kRoleHost);
    }
    if (guest_token && tokens_match(*token, *guest_token)) {
        return std::string(kRoleGuest);
    }
    return std::nullopt;
}

std::optional<std::string> resolve_stored_role(std::optional<std::string_view> token,
                                               const Json& host_token, const Json& guest_token) {
    const auto host = stored_token(host_token);
    const auto guest = stored_token(guest_token);
    return resolve_role(token, host ? std::optional<std::string_view>(*host) : std::nullopt,
                        guest ? std::optional<std::string_view>(*guest) : std::nullopt);
}

} // namespace fv::meeting_web
