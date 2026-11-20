#include "net/detail/tls_context.h"

#include <cstring>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace fv::net::detail {

namespace {

// Options Python's ssl.SSLContext sets on every context it creates.
constexpr auto kOptions = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                          ssl::context::no_sslv3 | ssl::context::no_compression |
                          ssl::context::single_dh_use;

} // namespace

std::string take_openssl_error(std::string fallback) {
    const unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (code == 0) {
        return fallback;
    }
    char text[256];
    ERR_error_string_n(code, text, sizeof(text));
    return text;
}

std::expected<std::shared_ptr<ssl::context>, std::string>
make_server_context(const TlsIdentity& identity) {
    auto context = std::make_shared<ssl::context>(ssl::context::tls_server);
    context->set_options(kOptions);
    SSL_CTX* native = context->native_handle();
    ERR_clear_error();
    if (SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION) != 1) {
        return std::unexpected(take_openssl_error("TLS 1.2 could not be required"));
    }
    if (identity.disable_tickets && SSL_CTX_set_num_tickets(native, 0) != 1) {
        return std::unexpected(take_openssl_error("session tickets could not be turned off"));
    }
    boost::system::error_code error;
    context->use_certificate_chain(asio::buffer(identity.cert_pem.data(), identity.cert_pem.size()),
                                   error);
    if (error) {
        ERR_clear_error();
        return std::unexpected("the TLS certificate is unusable (" + error.message() + ")");
    }
    context->use_private_key(asio::buffer(identity.key_pem.data(), identity.key_pem.size()),
                             ssl::context::pem, error);
    if (error) {
        ERR_clear_error();
        return std::unexpected("the TLS private key is unusable (" + error.message() + ")");
    }
    if (SSL_CTX_check_private_key(native) != 1) {
        return std::unexpected(take_openssl_error("the TLS key does not match its certificate"));
    }
    return context;
}

std::shared_ptr<ssl::context> make_client_context() {
    auto context = std::make_shared<ssl::context>(ssl::context::tls_client);
    context->set_options(kOptions);
    SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION);
    context->set_verify_mode(ssl::verify_none);
    return context;
}

std::vector<std::byte> peer_certificate_der(SSL* ssl) {
    X509* certificate = SSL_get1_peer_certificate(ssl);
    if (certificate == nullptr) {
        return {};
    }
    unsigned char* der = nullptr;
    const int size = i2d_X509(certificate, &der);
    std::vector<std::byte> bytes;
    if (size > 0 && der != nullptr) {
        bytes.resize(static_cast<std::size_t>(size));
        std::memcpy(bytes.data(), der, bytes.size());
    }
    OPENSSL_free(der);
    X509_free(certificate);
    return bytes;
}

} // namespace fv::net::detail
