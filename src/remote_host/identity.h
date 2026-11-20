#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "net/types.h"

// The host's TLS identity on disk (services/remote_asr/tls.py ensure_host_identity and
// own_fingerprint). The host keeps one self-signed certificate for good; a client records its
// SHA-256 fingerprint when it pairs and then refuses any other, so making a new one orphans
// every pairing.
// Affinity: any thread; reads and writes files, so never the GUI thread or the reactor.
namespace fv::remote_host {

struct HostIdentity {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;
    std::string fingerprint; // uppercase hex SHA-256 of the certificate's DER
    // The certificate and key as the TLS listener takes them, without session tickets.
    net::TlsIdentity tls;
};

// remote_engine_host_cert.pem and remote_engine_host_key.pem in `dir`, reused when both exist
// and the certificate reads; otherwise (missing, or unreadable: logged) a new P-256 certificate
// whose common name is "<identity().product> host <host_name>", the key written first (owner
// only on POSIX), then the certificate. Throws std::filesystem::filesystem_error or
// std::runtime_error when the files cannot be written.
HostIdentity ensure_host_identity(const std::filesystem::path& dir, std::string_view host_name);

// This computer's host fingerprint, or "" when it has never shared (no certificate, or one that
// cannot be read).
std::string own_fingerprint(const std::filesystem::path& dir);

} // namespace fv::remote_host
