#include "remote_host/identity.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "net/tls.h"
#include "remote/wire.h"

namespace fv::remote_host {

namespace {

constexpr log::Logger logger("services.remote_asr.tls");

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) {
        return std::nullopt;
    }
    return text;
}

// open(path, encoding="ascii").read(): universal newlines, and a byte past ASCII is a
// UnicodeDecodeError (a ValueError).
std::string ascii_text(const std::string& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if (byte > 0x7F) {
            throw std::invalid_argument("The certificate is not ASCII text");
        }
        if (bytes[i] == '\r') {
            text += '\n';
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++i;
            }
            continue;
        }
        text += bytes[i];
    }
    return text;
}

// The fingerprint of the certificate at `path`; nullopt when it cannot be read (OSError) or is
// no certificate (ValueError).
std::optional<std::string> fingerprint_of(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    try {
        return net::certificate_sha256_from_pem(ascii_text(*bytes));
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

void write_file(const std::filesystem::path& path, const std::string& bytes, bool owner_only) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Could not write " + path_to_utf8(path.filename()));
    }
    if (owner_only) {
        // Before the key goes in: nobody else may read it. Windows keeps the per-user profile's
        // ACL, where this only clears the read-only attribute.
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        throw std::runtime_error("Could not write " + path_to_utf8(path.filename()));
    }
}

void generate(const std::filesystem::path& cert_path, const std::filesystem::path& key_path,
              std::string_view host_name) {
    std::string common_name(identity().product);
    common_name += " host ";
    common_name += host_name;
    const net::TlsIdentity made = net::generate_self_signed_p256(common_name);
    std::filesystem::create_directories(std::filesystem::absolute(cert_path).parent_path());
    write_file(key_path, made.key_pem, true);
    write_file(cert_path, made.cert_pem, false);
    logger.info("Created the remote engine host certificate");
}

} // namespace

HostIdentity ensure_host_identity(const std::filesystem::path& dir, std::string_view host_name) {
    HostIdentity out;
    out.cert_path = dir / remote::CERT_FILENAME;
    out.key_path = dir / remote::KEY_FILENAME;
    std::optional<std::string> fingerprint;
    if (file_exists(out.cert_path) && file_exists(out.key_path)) {
        fingerprint = fingerprint_of(out.cert_path);
        if (!fingerprint) {
            logger.warning("Remote engine certificate is unreadable; creating a new one");
        }
    }
    if (!fingerprint) {
        generate(out.cert_path, out.key_path, host_name);
        fingerprint = fingerprint_of(out.cert_path);
        if (!fingerprint) {
            throw std::runtime_error("The remote engine certificate could not be read back");
        }
    }
    out.fingerprint = *fingerprint;
    out.tls.cert_pem = read_file(out.cert_path).value_or("");
    out.tls.key_pem = read_file(out.key_path).value_or("");
    // A websockets client on Windows stalled on TLS 1.3 session tickets (tls.py server_context).
    out.tls.disable_tickets = true;
    return out;
}

std::string own_fingerprint(const std::filesystem::path& dir) {
    const auto cert_path = dir / remote::CERT_FILENAME;
    if (!file_exists(cert_path)) {
        return "";
    }
    return fingerprint_of(cert_path).value_or("");
}

} // namespace fv::remote_host
