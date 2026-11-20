#include "agentapi/history_cursor.h"

#include <cmath>
#include <optional>
#include <span>

#include "agentapi/history_store.h"
#include "base/base64.h"
#include "base/hash.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::agentapi {

namespace {

constexpr std::string_view kInvalidCursor = "Invalid cursor or cursor used with different filters.";

[[noreturn]] void invalid() {
    throw InvalidQuery(std::string(kInvalidCursor));
}

// Strict UTF-8 (Python's bytes.decode("utf-8")): no overlong forms, no surrogates, nothing above
// U+10FFFF.
bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t cp = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
            cp = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2;
            cp = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3;
            cp = lead & 0x07;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
            (extra == 3 && (cp < 0x10000 || cp > 0x10ffff))) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// json.loads of bytes: a UTF-8 byte order mark is skipped (detect_encoding gives "utf-8-sig").
// Input whose first bytes hold a NUL would be read as UTF-16 or UTF-32; no cursor this API
// writes looks like that, so it is refused.
Json loads_bytes(std::string_view bytes) {
    if (bytes.starts_with("\xef\xbb\xbf")) {
        bytes.remove_prefix(3);
    } else if ((!bytes.empty() && bytes[0] == '\0') || (bytes.size() > 1 && bytes[1] == '\0')) {
        invalid();
    }
    if (!valid_utf8(bytes)) {
        invalid();
    }
    try {
        return py::loads(bytes);
    } catch (const py::PyError&) {
        invalid();
    }
}

} // namespace

std::string fingerprint(const Json& scope) {
    py::DumpOptions options;
    options.sort_keys = true;
    return sha256_hex(py::dumps(scope, options)).substr(0, FINGERPRINT_CHARS);
}

std::string encode_cursor(std::string_view fingerprint, const Json& keys) {
    Json payload = Json::object();
    payload["scope"] = std::string(fingerprint);
    payload["keys"] = keys;
    return b64encode(py::dumps(payload), /*urlsafe=*/true);
}

Json cursor_keys(std::string_view cursor, const std::vector<std::string>& valid_fingerprints,
                 std::size_t key_count) {
    if (py::len(cursor) > CURSOR_MAX_CHARS) {
        invalid();
    }
    const std::optional<std::vector<std::byte>> bytes = b64decode(cursor, /*urlsafe=*/true);
    if (!bytes) {
        invalid();
    }
    const Json decoded =
        loads_bytes(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!decoded.is_object() || !decoded.contains("keys") || !decoded.contains("scope")) {
        invalid();
    }
    const Json& scope = decoded["scope"];
    const Json& values = decoded["keys"];
    bool known = false;
    if (scope.is_string()) {
        for (const auto& valid : valid_fingerprints) {
            known = known || scope.get<std::string>() == valid;
        }
    }
    if (!known || !values.is_array() || values.size() != key_count || values.empty()) {
        invalid();
    }
    const Json& first = values[0];
    if (first.is_boolean() || !first.is_number() || !std::isfinite(first.get<double>())) {
        invalid();
    }
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (!values[i].is_string()) {
            invalid();
        }
    }
    return values;
}

} // namespace fv::agentapi
