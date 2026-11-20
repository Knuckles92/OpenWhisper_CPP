#pragma once

#include <exception>
#include <string_view>

// How the server logs what goes wrong on a connection, as OpenWhisper's uvicorn servers did
// with services/asyncio_utils.quiet_connection_lost installed. No Boost here.
namespace fv::net::detail {

enum class ErrorSite {
    Closing, // shutting the socket down while closing the connection
    Other,   // anything else the server itself does (accepting, ...)
};

// A peer that reset the connection while it was being closed is DEBUG on
// "services.asyncio_utils" ("Peer reset the connection while closing: ..."): the socket is
// closed either way. Everything else is ERROR on "asyncio".
void log_connection_error(ErrorSite site, bool peer_reset, std::string_view message);

// A handler that threw: ERROR on "uvicorn.error", "Exception in ASGI application", with the
// exception's type (null for a non-std exception); its message may hold user text.
void log_handler_exception(const std::exception* error);

// A handler dropped its Responder without answering: ERROR on "uvicorn.error".
void log_no_response();

// A Responder answered twice: WARNING on "uvicorn.error"; the second answer is dropped.
void log_second_response();

// A request the parser refused: WARNING on "uvicorn.error", "Invalid HTTP request received.".
void log_invalid_request();

} // namespace fv::net::detail
