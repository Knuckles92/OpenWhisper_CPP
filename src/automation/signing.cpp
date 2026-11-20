#include "automation/signing.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>

#include "base/base64.h"
#include "base/hmac.h"
#include "base/ids.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::automation::signing {

namespace {

std::vector<std::byte> key_of(std::string_view secret) {
    if (!secret.starts_with(PREFIX)) {
        throw py::ValueError("Webhook secrets start with whsec_.");
    }
    auto key = b64decode(secret.substr(PREFIX.size()));
    if (!key) {
        throw py::ValueError("The webhook secret is not valid base64.");
    }
    if (key->empty()) {
        throw py::ValueError("The webhook secret is empty.");
    }
    return std::move(*key);
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_ascii(std::string_view text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string new_secret() {
    std::array<std::byte, 24> bytes{};
    random_bytes(bytes);
    return std::string(PREFIX) + b64encode(bytes);
}

std::string signature(std::string_view secret, std::string_view msg_id, std::int64_t timestamp,
                      std::string_view body) {
    const std::vector<std::byte> key = key_of(secret);
    std::string signed_text = std::format("{}.{}.", msg_id, timestamp);
    signed_text.append(body);
    const auto digest = hmac_sha256(key, signed_text);
    return "v1," + b64encode(std::as_bytes(std::span(digest)));
}

std::vector<std::pair<std::string, std::string>> headers(std::string_view secret,
                                                         std::string_view msg_id,
                                                         std::string_view body,
                                                         std::optional<std::int64_t> timestamp) {
    const std::int64_t ts = timestamp.value_or(now_seconds());
    std::string signed_value = signature(secret, msg_id, ts, body);
    return {
        {"webhook-id", std::string(msg_id)},
        {"webhook-timestamp", std::to_string(ts)},
        {"webhook-signature", std::move(signed_value)},
    };
}

bool verify(std::string_view secret, const Received& received, std::string_view body,
            std::int64_t tolerance_s, std::optional<std::int64_t> now) {
    // A dict of lowered names: a later header of the same name wins.
    std::string msg_id;
    std::optional<std::string> timestamp_text;
    std::string signatures;
    for (const auto& [name, value] : received) {
        const std::string lowered = py::lower(name);
        if (lowered == "webhook-id") {
            msg_id = value;
        } else if (lowered == "webhook-timestamp") {
            timestamp_text = value;
        } else if (lowered == "webhook-signature") {
            signatures = value;
        }
    }
    const auto ts = py::py_int(timestamp_text.value_or(""));
    if (!ts) {
        return false;
    }
    const std::int64_t current = now.value_or(now_seconds());
    // Python's ints do not overflow: a difference beyond int64 is far outside any tolerance.
    const bool overflows =
        (*ts > 0 && current < INT64_MIN + *ts) || (*ts < 0 && current > INT64_MAX + *ts);
    if (overflows) {
        return false;
    }
    const std::int64_t difference = current - *ts;
    if (difference == INT64_MIN || std::llabs(difference) > tolerance_s || msg_id.empty()) {
        return false;
    }
    std::string expected;
    try {
        expected = signature(secret, msg_id, *ts, body).substr(3);
    } catch (const py::ValueError&) {
        return false;
    }
    for (const std::string& part : py::split(signatures)) {
        const auto comma = part.find(',');
        const std::string_view version =
            std::string_view(part).substr(0, comma == std::string::npos ? part.size() : comma);
        const std::string_view value = comma == std::string::npos
                                           ? std::string_view()
                                           : std::string_view(part).substr(comma + 1);
        if (version != "v1") {
            continue;
        }
        if (!is_ascii(value)) {
            throw py::TypeError("comparing strings with non-ASCII characters is not supported");
        }
        if (constant_time_equal(value, expected)) {
            return true;
        }
    }
    return false;
}

} // namespace fv::automation::signing
