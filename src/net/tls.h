#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "net/types.h"

namespace fv::net {

// A self-signed P-256 certificate for the remote-engine host, as OpenWhisper's (tls.py
// _generate): subject and issuer one CommonName, `common_name` cut to 64 code points; a random
// positive serial below 2^159; valid from a day ago for 7,300 days; X.509 v3 with no
// extensions; signed with ECDSA SHA-256. The key is PKCS#8 PEM, unencrypted. disable_tickets is
// set, as the host serves it.
TlsIdentity generate_self_signed_p256(std::string_view common_name);

// protocol.certificate_fingerprint: the uppercase hex SHA-256 of a certificate's DER.
std::string certificate_sha256_from_der(std::span<const std::byte> der);

// The same of a PEM certificate, read as ssl.PEM_cert_to_DER_cert reads it: the text must start
// with "-----BEGIN CERTIFICATE-----" (no whitespace before it), end with "-----END
// CERTIFICATE-----" once stripped, and what lies between is base64. Throws std::invalid_argument
// otherwise.
std::string certificate_sha256_from_pem(std::string_view pem);

} // namespace fv::net
