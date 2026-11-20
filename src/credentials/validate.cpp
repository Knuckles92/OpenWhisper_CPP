#include "credentials/validate.h"

#include <stdexcept>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv {

namespace {

constexpr std::string_view kMask = "••••";

} // namespace

bool keyring_did_not_respond(const CredentialStatus& status) {
    return status.reason == kKeyringNoResponse;
}

std::string validate_credential_name(std::string_view name) {
    std::string cleaned = py::strip(name);
    if (cleaned.empty()) {
        throw std::invalid_argument("Credential name is empty");
    }
    return cleaned;
}

std::string validate_api_key(std::string_view raw) {
    std::string cleaned = py::strip(raw);
    if (cleaned.empty()) {
        throw std::invalid_argument("Paste a key first.");
    }
    if (py::len(cleaned) > kMaxApiKeyLength) {
        throw std::invalid_argument("That is longer than any API key (512 characters max).");
    }
    for (const char32_t cp : py::to_code_points(cleaned)) {
        if (!(cp > 32 && cp < 127)) {
            throw std::invalid_argument(
                "The key contains a space or a character outside plain ASCII. "
                "Copy it again from the provider's dashboard.");
        }
    }
    return cleaned;
}

std::string mask_key(std::string_view key) {
    if (key.empty() || py::len(key) < kMinMaskSuffixLength) {
        return std::string(kMask);
    }
    return std::string(kMask) + py::slice(key, -4);
}

} // namespace fv
