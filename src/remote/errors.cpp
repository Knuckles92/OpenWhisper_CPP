#include "remote/errors.h"

#include <array>
#include <cmath>
#include <utility>

namespace fv::remote {

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kClientUnavailable = {{
    {"unavailable", "Client history is unavailable. The client may be offline."},
    {"sharing_disabled", "History sharing is disabled on this client."},
    {"not_found", "Record not found on this client."},
    {"invalid_query", "Invalid client history query."},
    {"snapshot_unavailable", "This client's saved insights are unavailable."},
    {"unsupported", "Direct client queries are not available on this server."},
}};

} // namespace

std::optional<double> retry_after_seconds(const Json& retry_after_ms) {
    if (!retry_after_ms.is_number()) {
        return std::nullopt;
    }
    const double value = retry_after_ms.get<double>();
    if (std::isnan(value) || value < 0) {
        return std::nullopt;
    }
    return value / 1000;
}

RemoteRequestError::RemoteRequestError(const std::string& message, const Json& code_,
                                       const Json& retry_after_ms)
    : std::runtime_error(message),
      code(code_.is_string() ? std::optional(code_.get<std::string>()) : std::nullopt),
      retry_after_s(retry_after_seconds(retry_after_ms)) {}

std::string client_unavailable_code(std::string_view code) {
    for (const auto& [known, message] : kClientUnavailable) {
        if (known == code) {
            return std::string(known);
        }
    }
    return "unavailable";
}

std::string_view client_unavailable_message(std::string_view code) {
    for (const auto& [known, message] : kClientUnavailable) {
        if (known == code) {
            return message;
        }
    }
    return kClientUnavailable[0].second;
}

ClientUnavailable::ClientUnavailable(std::string_view code)
    : std::runtime_error(std::string(client_unavailable_message(client_unavailable_code(code)))),
      code_(client_unavailable_code(code)) {}

ClientUnavailable::ClientUnavailable(const Json& code)
    : ClientUnavailable(code.is_string() ? std::string_view(code.get_ref<const std::string&>())
                                         : std::string_view("unavailable")) {}

} // namespace fv::remote
