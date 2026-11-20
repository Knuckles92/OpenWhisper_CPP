#include "net/detail/server_log.h"

#include "base/log.h"

namespace fv::net::detail {

namespace {

constexpr log::Logger kQuietLog("services.asyncio_utils");
constexpr log::Logger kLoopLog("asyncio");
constexpr log::Logger kServerLog("uvicorn.error");

} // namespace

void log_connection_error(ErrorSite site, bool peer_reset, std::string_view message) {
    if (site == ErrorSite::Closing && peer_reset) {
        kQuietLog.debug("Peer reset the connection while closing: {}", message);
        return;
    }
    if (site == ErrorSite::Closing) {
        kLoopLog.error("Exception while closing a connection: {}", message);
    } else {
        kLoopLog.error("Exception on a connection: {}", message);
    }
}

void log_handler_exception(const std::exception* error) {
    if (error != nullptr) {
        kServerLog.error_with(*error, "Exception in ASGI application");
    } else {
        kServerLog.error("Exception in ASGI application");
    }
}

void log_no_response() {
    kServerLog.error("ASGI callable returned without starting response.");
}

void log_second_response() {
    kServerLog.warning("A response was already sent; the second one is dropped.");
}

void log_invalid_request() {
    kServerLog.warning("Invalid HTTP request received.");
}

} // namespace fv::net::detail
