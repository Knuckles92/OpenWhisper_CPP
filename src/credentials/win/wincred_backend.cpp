#include "credentials/win/wincred_backend.h"

#include <windows.h>

#include <wincred.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "base/log.h"
#include "credentials/detail/errors.h"
#include "credentials/detail/utf8_check.h"
#include "credentials/platform_backend.h"
#include "pyport/unicode.h"

namespace fv {

namespace {

constexpr log::Logger keyring_logger("keyring.backends.Windows");
constexpr wchar_t kComment[] = L"Stored using python-keyring";

// UTF-8 to UTF-16, refusing text that is not UTF-8 (the message names no content).
std::wstring widen(std::string_view text) {
    if (const auto error = credentials_detail::check_utf8(text)) {
        throw std::invalid_argument(credentials_detail::describe(*error));
    }
    std::wstring wide;
    wide.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char32_t cp = py::next_code_point(text, pos);
        if (cp >= 0x10000) {
            const char32_t offset = cp - 0x10000;
            wide.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
            wide.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
        } else {
            wide.push_back(static_cast<wchar_t>(cp));
        }
    }
    return wide;
}

// A service or credential name, which ends up in a NUL-terminated TargetName or UserName. A NUL
// inside would cut the target short and address another credential, outside the service; ctypes
// refuses such a string as keyring passes it (ValueError: embedded null character). A blob is
// passed with its size, so a value may hold one.
std::wstring widen_name(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("embedded null character");
    }
    return widen(text);
}

[[noreturn]] void throw_last_error(const char* function) {
    const DWORD code = GetLastError();
    throw std::system_error(static_cast<int>(code), std::system_category(), function);
}

struct Stored {
    std::optional<std::wstring> user; // NULL UserName stays distinct from ""
    std::string blob;
};

struct CredFreer {
    void operator()(CREDENTIALW* credential) const { CredFree(credential); }
};

// keyring's _read_credential: nullopt for ERROR_NOT_FOUND, a throw for anything else.
std::optional<Stored> read_credential(const std::wstring& target) {
    CREDENTIALW* raw = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        if (GetLastError() == ERROR_NOT_FOUND) {
            return std::nullopt;
        }
        throw_last_error("CredReadW");
    }
    const std::unique_ptr<CREDENTIALW, CredFreer> credential(raw);
    Stored stored;
    if (credential->UserName != nullptr) {
        stored.user = std::wstring(credential->UserName);
    }
    if (credential->CredentialBlob != nullptr && credential->CredentialBlobSize > 0) {
        stored.blob.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                           credential->CredentialBlobSize);
    }
    return stored;
}

void write_credential(const std::wstring& target, const std::optional<std::wstring>& user,
                      const std::wstring& value) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.UserName = user.has_value() ? const_cast<wchar_t*>(user->c_str()) : nullptr;
    credential.Comment = const_cast<wchar_t*>(kComment);
    credential.Persist = CRED_PERSIST_ENTERPRISE;
    // UTF-16LE code units, without a terminator, as ctypes' create_unicode_buffer minus its NUL.
    std::vector<BYTE> blob(value.size() * sizeof(wchar_t));
    if (!blob.empty()) {
        std::memcpy(blob.data(), value.data(), blob.size());
    }
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = blob.empty() ? nullptr : blob.data();
    const BOOL ok = CredWriteW(&credential, 0);
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok) {
        throw_last_error("CredWriteW");
    }
}

void delete_credential(const std::wstring& target) {
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        if (GetLastError() == ERROR_NOT_FOUND) {
            return;
        }
        throw_last_error("CredDeleteW");
    }
}

// keyring's _compound_name: f"{username}@{service}", where a missing UserName prints as None.
std::wstring compound_name(const std::optional<std::wstring>& user, const std::wstring& service) {
    return (user.has_value() ? *user : std::wstring(L"None")) + L"@" + service;
}

// Python's "utf-16" codec: an optional BOM picks the byte order (little endian without one);
// odd lengths and unpaired surrogates fail.
std::optional<std::string> decode_utf16(std::string_view bytes) {
    if (bytes.size() % 2 != 0) {
        return std::nullopt;
    }
    bool big_endian = false;
    std::size_t pos = 0;
    if (bytes.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(bytes[0]);
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            pos = 2;
        } else if (b0 == 0xFE && b1 == 0xFF) {
            big_endian = true;
            pos = 2;
        }
    }
    const auto unit = [&](std::size_t at) -> char16_t {
        const auto lo = static_cast<unsigned char>(bytes[at + (big_endian ? 1 : 0)]);
        const auto hi = static_cast<unsigned char>(bytes[at + (big_endian ? 0 : 1)]);
        return static_cast<char16_t>((hi << 8) | lo);
    };
    std::string out;
    out.reserve(bytes.size());
    while (pos < bytes.size()) {
        const char16_t first = unit(pos);
        pos += 2;
        if (first >= 0xDC00 && first <= 0xDFFF) {
            return std::nullopt;
        }
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (pos >= bytes.size()) {
                return std::nullopt;
            }
            const char16_t second = unit(pos);
            if (second < 0xDC00 || second > 0xDFFF) {
                return std::nullopt;
            }
            pos += 2;
            const char32_t cp = 0x10000 + ((static_cast<char32_t>(first) - 0xD800) << 10) +
                                (static_cast<char32_t>(second) - 0xDC00);
            py::append_utf8(out, cp);
        } else {
            py::append_utf8(out, first);
        }
    }
    return out;
}

} // namespace

namespace wincred_detail {

std::string decode_blob(std::string_view bytes) {
    if (auto text = decode_utf16(bytes)) {
        return std::move(*text);
    }
    if (const auto error = credentials_detail::check_utf8(bytes)) {
        throw credentials_detail::BlobDecodeError(credentials_detail::describe(*error));
    }
    keyring_logger.warning("Retrieved a UTF-8 encoded credential. Please be aware that this "
                           "library only writes credentials in UTF-16.");
    return std::string(bytes);
}

} // namespace wincred_detail

std::optional<std::string> WinCredBackend::get(std::string_view service, std::string_view name) {
    const std::wstring wide_service = widen_name(service);
    const std::wstring wide_name = widen_name(name);
    auto stored = read_credential(wide_service);
    if (!stored.has_value() || (!wide_name.empty() && stored->user != wide_name)) {
        stored = read_credential(compound_name(wide_name, wide_service));
    }
    if (!stored.has_value()) {
        return std::nullopt;
    }
    return wincred_detail::decode_blob(stored->blob);
}

void WinCredBackend::set(std::string_view service, std::string_view name, std::string_view value) {
    const std::wstring wide_service = widen_name(service);
    const std::wstring wide_name = widen_name(name);
    const std::wstring wide_value = widen(value);
    if (const auto existing = read_credential(wide_service)) {
        // Another key, or an older copy of this one, holds the plain target: keep it under its
        // compound name so a reader looking for that name still finds it.
        write_credential(compound_name(existing->user, wide_service), existing->user,
                         widen(wincred_detail::decode_blob(existing->blob)));
    }
    write_credential(wide_service, wide_name, wide_value);
}

bool WinCredBackend::remove(std::string_view service, std::string_view name) {
    const std::wstring wide_service = widen_name(service);
    const std::wstring wide_name = widen_name(name);
    bool deleted = false;
    for (const std::wstring& target : {wide_service, compound_name(wide_name, wide_service)}) {
        const auto existing = read_credential(target);
        if (existing.has_value() && existing->user == wide_name) {
            deleted = true;
            delete_credential(target);
        }
    }
    return deleted;
}

CredentialStatus WinCredBackend::probe() {
    // advapi32 is always there; keyring's own probe only checks that its bindings import.
    return {true, std::string(platform_backend_name()), ""};
}

std::shared_ptr<CredentialBackend> make_platform_backend() {
    return std::make_shared<WinCredBackend>();
}

} // namespace fv
