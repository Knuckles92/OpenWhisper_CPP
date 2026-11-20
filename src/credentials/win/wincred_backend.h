#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "credentials/store.h"

namespace fv {

// Windows Credential Manager, laid out as Python keyring's WinVaultKeyring does, so keys saved by
// either program read in the other. Generic credentials; the target <service> holds the newest
// key with UserName = its name; saving another name first moves the holder to
// <its UserName>@<service>. Reads take <service> when its UserName matches, else
// <name>@<service>. Blobs are UTF-16LE without BOM or terminator, Persist is
// CRED_PERSIST_ENTERPRISE, Comment "Stored using python-keyring".
// Failures other than "not found" throw std::system_error with the Win32 error.
// Affinity: any thread, one call at a time (CredentialStore serializes).
class WinCredBackend final : public CredentialBackend {
public:
    std::optional<std::string> get(std::string_view service, std::string_view name) override;
    void set(std::string_view service, std::string_view name, std::string_view value) override;
    bool remove(std::string_view service, std::string_view name) override;
    CredentialStatus probe() override;
};

namespace wincred_detail {

// keyring's DecodingCredential.value: the blob as UTF-16 (a BOM picks the byte order, little
// endian without one), else as UTF-8 (logged as a warning); throws when it is neither.
std::string decode_blob(std::string_view bytes);

} // namespace wincred_detail

} // namespace fv
