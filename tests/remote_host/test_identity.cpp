// The host's certificate on disk (services/remote_asr/tls.py ensure_host_identity and
// own_fingerprint).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "base/hash.h"
#include "base/identity.h"
#include "net/tls.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/identity.h"
#include "testkit/testkit.h"

namespace {

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

// The DER inside a PEM certificate, as ssl.PEM_cert_to_DER_cert reads it.
std::vector<std::byte> der_of(const std::string& pem) {
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    const std::string end = "-----END CERTIFICATE-----";
    std::string body = pem.substr(begin.size(), pem.find(end) - begin.size());
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::byte> out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : body) {
        const auto at = alphabet.find(c);
        if (at == std::string::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

TEST_CASE("test_host_identity_is_created_once_and_reused") {
    fv::testkit::TempDataRoot root;
    const auto first = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    const auto second = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    CHECK(first.fingerprint == second.fingerprint);
    CHECK(first.fingerprint.size() == 64);
}

TEST_CASE("the identity files are the key first, then the certificate, beside each other") {
    fv::testkit::TempDataRoot root;
    const auto dir = root.path() / "nested" / "host";
    const auto made = fv::remote_host::ensure_host_identity(dir, "devbox");
    CHECK(made.cert_path == dir / fv::remote::CERT_FILENAME);
    CHECK(made.key_path == dir / fv::remote::KEY_FILENAME);
    const std::string key = read(made.key_path);
    const std::string cert = read(made.cert_path);
    CHECK(key.starts_with("-----BEGIN PRIVATE KEY-----"));
    CHECK(cert.starts_with("-----BEGIN CERTIFICATE-----"));
    CHECK(made.tls.cert_pem == cert);
    CHECK(made.tls.key_pem == key);
    // No TLS 1.3 session tickets: a websockets client on Windows stalled on them.
    CHECK(made.tls.disable_tickets);
#ifndef _WIN32
    const auto perms = std::filesystem::status(made.key_path).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
#endif
}

TEST_CASE("the fingerprint is the certificate's SHA-256 by both helpers") {
    fv::testkit::TempDataRoot root;
    const auto made = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    const std::string cert = read(made.cert_path);
    CHECK(made.fingerprint == fv::net::certificate_sha256_from_pem(cert));
    CHECK(made.fingerprint == fv::remote::certificate_fingerprint(der_of(cert)));
    CHECK(fv::remote_host::own_fingerprint(root.path()) == made.fingerprint);
}

TEST_CASE("the certificate names this product's host") {
    fv::testkit::TempDataRoot root;
    const auto made = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    const auto der = der_of(read(made.cert_path));
    const std::string expected = std::string(fv::identity().product) + " host devbox";
    const std::string bytes(reinterpret_cast<const char*>(der.data()), der.size());
    CHECK(bytes.find(expected) != std::string::npos);
}

TEST_CASE("an unreadable certificate is made again, which changes the fingerprint") {
    fv::testkit::TempDataRoot root;
    const auto first = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    write(first.cert_path, "not a certificate");
    CHECK(fv::remote_host::own_fingerprint(root.path()).empty());
    const auto second = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    CHECK(second.fingerprint.size() == 64);
    CHECK(second.fingerprint != first.fingerprint);
    CHECK(fv::remote_host::own_fingerprint(root.path()) == second.fingerprint);
}

TEST_CASE("a missing key makes both files again") {
    fv::testkit::TempDataRoot root;
    const auto first = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    std::filesystem::remove(first.key_path);
    const auto second = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    CHECK(std::filesystem::exists(second.key_path));
    CHECK(second.fingerprint != first.fingerprint);
}

TEST_CASE("a computer that never shared has no fingerprint") {
    fv::testkit::TempDataRoot root;
    CHECK(fv::remote_host::own_fingerprint(root.path()).empty());
}

TEST_CASE("a certificate written with Windows line endings still reads") {
    fv::testkit::TempDataRoot root;
    const auto made = fv::remote_host::ensure_host_identity(root.path(), "devbox");
    std::string cert = read(made.cert_path);
    std::string crlf;
    for (const char c : cert) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    write(made.cert_path, crlf);
    CHECK(fv::remote_host::own_fingerprint(root.path()) == made.fingerprint);
    CHECK(fv::remote_host::ensure_host_identity(root.path(), "devbox").fingerprint ==
          made.fingerprint);
}
