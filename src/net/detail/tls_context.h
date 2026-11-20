#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// First: it sets the Windows version Asio is built for.
#include "net/detail/asio.h"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include "net/types.h"

// OpenSSL contexts of the server's TLS listeners and of the WebSocket client.
namespace fv::net::detail {

namespace ssl = boost::asio::ssl;

using PlainStream = beast::tcp_stream;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

template <class Stream> inline constexpr bool kIsTls = std::is_same_v<Stream, TlsStream>;

// tls.py server_context: TLS 1.2 at least, the identity's certificate and key, and no TLS 1.3
// session tickets when it asks (SSL_CTX_set_num_tickets 0). The error names what OpenSSL refused.
std::expected<std::shared_ptr<ssl::context>, std::string>
make_server_context(const TlsIdentity& identity);

// tls.py client_context: TLS 1.2 at least, no CA and no host name check (the caller pins the
// certificate's fingerprint).
std::shared_ptr<ssl::context> make_client_context();

// The DER of the certificate the peer presented; empty when it presented none.
std::vector<std::byte> peer_certificate_der(SSL* ssl);

// The oldest OpenSSL error queued on this thread, as text, and the queue cleared; `fallback`
// when the queue is empty.
std::string take_openssl_error(std::string fallback);

} // namespace fv::net::detail
