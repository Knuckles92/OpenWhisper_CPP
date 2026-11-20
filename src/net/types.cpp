#include "net/types.h"

#include "net/detail/wire.h"
#include "net/http_server.h"

namespace fv::net {

std::optional<std::string_view> header(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (detail::iequals(key, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

void WsConnection::send_text(std::shared_ptr<const std::string> text, std::chrono::milliseconds,
                             std::function<void(bool timed_out)> done) {
    send_text(std::move(text));
    if (done) {
        done(false);
    }
}

const HttpRequest& WsConnection::request() const {
    static const HttpRequest empty{};
    return empty;
}

std::uint64_t WsConnection::id() const {
    return 0;
}

} // namespace fv::net
