#include "remote/protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>

#include "base/hash.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "remote/detail/pyvalue.h"

namespace fv::remote {

namespace {

constexpr std::size_t kHeaderSize = 4;

std::vector<std::byte> frame_with(const Json& header, std::size_t payload_size) {
    const std::string body = dumps_compact(header);
    if (body.size() > MAX_HEADER_BYTES) {
        throw ProtocolError("Request header is too large");
    }
    std::vector<std::byte> frame;
    frame.reserve(kHeaderSize + body.size() + payload_size);
    const auto length = static_cast<std::uint32_t>(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
    }
    const auto bytes = detail::as_bytes(body);
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    return frame;
}

std::int16_t pcm_sample(float sample) {
    // numpy: float32 * 32768.0 stays float32, rint rounds half to even (the default rounding
    // mode), clip, then astype("<i2").
    const float scaled = sample * 32768.0f;
    if (std::isnan(scaled)) {
        return 0;
    }
    const float rounded = std::nearbyint(scaled);
    return static_cast<std::int16_t>(std::clamp(rounded, -32768.0f, 32767.0f));
}

std::string upper_hex(std::string_view hex) {
    std::string out(hex);
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'f') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

int parse_port(std::string_view text) {
    const auto port = py::py_int_checked(text);
    if (!port) {
        if (port.error() == py::IntError::Overflow) {
            throw py::ValueError("The port must be between 1 and 65535");
        }
        throw py::ValueError("The port must be a number");
    }
    if (*port < 1 || *port > 65535) {
        throw py::ValueError("The port must be between 1 and 65535");
    }
    return static_cast<int>(*port);
}

bool has_space(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (py::is_space(py::next_code_point(text, pos))) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string dumps_compact(const Json& value) {
    py::DumpOptions options;
    options.item_sep = ",";
    options.key_sep = ":";
    return py::dumps(value, options);
}

std::string dumps_default(const Json& value) {
    return py::dumps(value);
}

std::vector<std::byte> encode_audio(std::span<const float> samples) {
    std::vector<std::byte> out;
    out.reserve(samples.size() * 2);
    for (const float sample : samples) {
        const auto value = static_cast<std::uint16_t>(pcm_sample(sample));
        out.push_back(static_cast<std::byte>(value & 0xFF));
        out.push_back(static_cast<std::byte>(value >> 8));
    }
    return out;
}

std::vector<float> decode_audio(std::span<const std::byte> data) {
    if (data.size() % 2 != 0) {
        throw ProtocolError("Audio payload has an odd byte count");
    }
    std::vector<float> out;
    out.reserve(data.size() / 2);
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const auto value =
            static_cast<std::int16_t>(std::to_integer<std::uint16_t>(data[i]) |
                                      (std::to_integer<std::uint16_t>(data[i + 1]) << 8));
        out.push_back(static_cast<float>(value) / 32768.0f);
    }
    return out;
}

std::vector<std::byte> pack_request(const Json& header) {
    return frame_with(header, 0);
}

std::vector<std::byte> pack_request(const Json& header, std::span<const float> audio) {
    auto frame = frame_with(header, audio.size() * 2);
    const auto pcm = encode_audio(audio);
    frame.insert(frame.end(), pcm.begin(), pcm.end());
    return frame;
}

std::vector<std::byte> pack_request(const Json& header, std::span<const std::byte> payload) {
    auto frame = frame_with(header, payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

Frame unpack_frame(std::span<const std::byte> frame, bool binary) {
    if (!binary) {
        throw ProtocolError("Requests must be binary frames");
    }
    if (frame.size() < kHeaderSize) {
        throw ProtocolError("Request frame is truncated");
    }
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < kHeaderSize; ++i) {
        length = (length << 8) | std::to_integer<std::uint64_t>(frame[i]);
    }
    if (length > MAX_HEADER_BYTES || kHeaderSize + length > frame.size()) {
        throw ProtocolError("Request header length is invalid");
    }
    const auto body = frame.subspan(kHeaderSize, static_cast<std::size_t>(length));
    Json header;
    try {
        const auto text = detail::json_bytes_to_text(body);
        if (!text) {
            throw ProtocolError("Request header is not JSON");
        }
        header = py::loads(*text);
    } catch (const py::ValueError&) {
        throw ProtocolError("Request header is not JSON");
    }
    if (!header.is_object()) {
        throw ProtocolError("Request header must be an object");
    }
    const auto payload = frame.subspan(kHeaderSize + static_cast<std::size_t>(length));
    return Frame{std::move(header), std::vector<std::byte>(payload.begin(), payload.end())};
}

std::vector<float> payload_audio(std::span<const std::byte> payload) {
    if (payload.size() > MAX_AUDIO_BYTES) {
        throw ProtocolError("Request audio is too long");
    }
    return decode_audio(payload);
}

std::pair<Json, std::vector<float>> unpack_request(std::span<const std::byte> frame, bool binary) {
    Frame unpacked = unpack_frame(frame, binary);
    auto audio = payload_audio(unpacked.payload);
    return {std::move(unpacked.header), std::move(audio)};
}

std::vector<std::string> header_phrases(const Json& header) {
    std::vector<std::string> phrases;
    if (!header.is_object()) {
        return phrases;
    }
    const auto found = header.find("phrases");
    if (found == header.end() || !found->is_array()) {
        return phrases;
    }
    for (const auto& value : *found) {
        if (!value.is_string()) {
            continue;
        }
        std::string phrase;
        for (const auto& word : py::split(value.get_ref<const std::string&>())) {
            if (!phrase.empty()) {
                phrase += ' ';
            }
            phrase += word;
        }
        if (!phrase.empty() && py::len(phrase) <= MAX_HINT_CHARS) {
            phrases.push_back(std::move(phrase));
        }
        if (phrases.size() >= MAX_HINT_PHRASES) {
            break;
        }
    }
    return phrases;
}

std::string certificate_fingerprint(std::span<const std::byte> der) {
    Sha256 hash;
    hash.update(der);
    return upper_hex(hash.hexdigest());
}

std::string short_fingerprint(std::string_view fingerprint, int groups) {
    std::u32string clean;
    for (const char32_t cp : py::to_code_points(py::upper(fingerprint))) {
        if (py::is_alnum(cp)) {
            clean.push_back(cp);
        }
    }
    const std::int64_t end =
        std::min<std::int64_t>(static_cast<std::int64_t>(clean.size()), std::int64_t{groups} * 4);
    std::string out;
    for (std::int64_t i = 0; i < end; i += 4) {
        if (!out.empty()) {
            out += '-';
        }
        out += py::to_utf8(std::u32string_view(clean).substr(static_cast<std::size_t>(i), 4));
    }
    return out;
}

std::string token_digest(std::string_view token) {
    return sha256_hex(token);
}

std::string clean_name(std::string_view name) {
    std::string printable;
    std::size_t pos = 0;
    while (pos < name.size()) {
        const char32_t cp = py::next_code_point(name, pos);
        if (py::is_printable(cp)) {
            py::append_utf8(printable, cp);
        }
    }
    return py::slice(py::strip(printable), 0, static_cast<std::int64_t>(MAX_NAME));
}

std::string clean_name(const Json& name) {
    return clean_name(detail::py_str_or_empty(name));
}

std::string pairing_commitment(std::span<const std::uint8_t> nonce) {
    Sha256 hash;
    hash.update(COMMIT_DOMAIN);
    hash.update(std::as_bytes(nonce));
    return hash.hexdigest();
}

std::string pairing_sas(std::span<const std::uint8_t> host_nonce,
                        std::span<const std::uint8_t> client_nonce, std::string_view fingerprint) {
    // encode("ascii", "replace"): one "?" per code point outside ASCII.
    std::string ascii;
    for (const char32_t cp : py::to_code_points(py::upper(fingerprint))) {
        ascii += cp < 0x80 ? static_cast<char>(cp) : '?';
    }
    Sha256 hash;
    hash.update(SAS_DOMAIN);
    hash.update(std::as_bytes(host_nonce));
    hash.update(std::as_bytes(client_nonce));
    hash.update(ascii);
    const auto digest = hash.digest();
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        number = (number << 8) | digest[i];
    }
    return std::format("{:06}", number % 1'000'000);
}

std::string format_sas(std::string_view sas) {
    if (py::len(sas) != static_cast<std::size_t>(SAS_DIGITS)) {
        return std::string(sas);
    }
    return py::slice(sas, 0, 3) + " " + py::slice(sas, 3);
}

std::pair<std::string, int> parse_address(std::string_view text, int default_port) {
    std::string raw = py::strip(text);
    static constexpr std::array<std::string_view, 4> kSchemes = {"https://", "http://", "wss://",
                                                                 "ws://"};
    const std::string lowered = py::lower(raw);
    for (const std::string_view scheme : kSchemes) {
        if (lowered.starts_with(scheme)) {
            raw = py::slice(raw, static_cast<std::int64_t>(scheme.size()));
            break;
        }
    }
    raw = py::strip(std::string_view(raw).substr(0, raw.find('/')));
    if (raw.empty()) {
        throw py::ValueError("Enter the host's address, such as 192.168.1.20 or devbox.local");
    }
    std::string host;
    std::optional<int> port;
    if (raw.starts_with('[')) {
        const auto end = raw.find(']');
        if (end == std::string::npos) {
            throw py::ValueError("The IPv6 address is missing its closing bracket");
        }
        host = raw.substr(1, end - 1);
        const std::string_view rest = std::string_view(raw).substr(end + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) {
                throw py::ValueError("Put the port after the bracket, as in [fe80::1]:47821");
            }
            port = parse_port(rest.substr(1));
        }
    } else if (std::count(raw.begin(), raw.end(), ':') == 1) {
        const auto colon = raw.find(':');
        host = raw.substr(0, colon);
        port = parse_port(std::string_view(raw).substr(colon + 1));
    } else {
        // No colon, or an unbracketed IPv6 address with no port.
        host = raw;
    }
    host = py::strip(host);
    if (host.empty() || has_space(host)) {
        throw py::ValueError("The host's address is not valid");
    }
    return {host, port.value_or(default_port)};
}

std::string format_address(std::string_view host, int port) {
    if (host.find(':') != std::string_view::npos) {
        return std::format("[{}]:{}", host, port);
    }
    return std::format("{}:{}", host, port);
}

} // namespace fv::remote
