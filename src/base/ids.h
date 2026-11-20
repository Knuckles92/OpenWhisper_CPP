#pragma once

#include <cstddef>
#include <span>
#include <string>

// Random names and tokens from the operating system's CSPRNG (OpenSSL RAND_bytes), as Python's
// secrets and uuid modules make them.
namespace fv {

void random_bytes(std::span<std::byte> out);

// `chars` lowercase hex digits (secrets.token_hex(n) is random_hex(2 * n)).
std::string random_hex(std::size_t chars);

// secrets.token_urlsafe(nbytes): URL-safe base64 of nbytes random bytes, without "=" padding.
std::string token_urlsafe(std::size_t nbytes = 32);

// uuid.uuid4().hex: 32 lowercase hex digits with the version 4 and RFC 4122 variant bits set.
std::string uuid4_hex();

} // namespace fv
