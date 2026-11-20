#include "remote/detail/pyvalue.h"

#include <cstdint>
#include <string>

#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::remote::detail {

namespace {

bool continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// The length of the well-formed UTF-8 sequence at `pos`, or 0, with the length of its longest
// valid prefix (at least 1) in `prefix` for a replacement.
std::size_t sequence_at(std::string_view bytes, std::size_t pos, bool surrogatepass,
                        std::size_t& prefix) {
    const auto at = [&](std::size_t i) {
        return static_cast<unsigned char>(bytes[pos + i]);
    };
    const std::size_t left = bytes.size() - pos;
    const unsigned char lead = at(0);
    prefix = 1;
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
            low = 0xA0;
        } else if (lead == 0xED && !surrogatepass) {
            high = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
            low = 0x90;
        } else if (lead == 0xF4) {
            high = 0x8F;
        }
    } else {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (i >= left) {
            return 0;
        }
        const unsigned char byte = at(i);
        const bool ok = i == 1 ? (byte >= low && byte <= high) : continuation(byte);
        if (!ok) {
            return 0;
        }
        prefix = i + 1;
    }
    return length;
}

void append_unit(std::string& out, char32_t cp) {
    py::append_utf8(out, cp);
}

bool decode_units16(std::span<const std::byte> data, bool big_endian, std::string& out) {
    if (data.size() % 2 != 0) {
        return false;
    }
    const auto unit = [&](std::size_t i) {
        const auto a = std::to_integer<std::uint16_t>(data[2 * i]);
        const auto b = std::to_integer<std::uint16_t>(data[2 * i + 1]);
        return static_cast<char16_t>(big_endian ? (a << 8) | b : (b << 8) | a);
    };
    const std::size_t count = data.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
        const char16_t first = unit(i);
        if (first >= 0xD800 && first <= 0xDBFF && i + 1 < count) {
            const char16_t second = unit(i + 1);
            if (second >= 0xDC00 && second <= 0xDFFF) {
                append_unit(out, 0x10000 + ((static_cast<char32_t>(first) - 0xD800) << 10) +
                                     (static_cast<char32_t>(second) - 0xDC00));
                ++i;
                continue;
            }
        }
        // A lone surrogate passes ("surrogatepass") in its three-byte form.
        append_unit(out, first);
    }
    return true;
}

bool decode_units32(std::span<const std::byte> data, bool big_endian, std::string& out) {
    if (data.size() % 4 != 0) {
        return false;
    }
    for (std::size_t i = 0; i < data.size(); i += 4) {
        char32_t cp = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::size_t index = big_endian ? i + k : i + 3 - k;
            cp = (cp << 8) | std::to_integer<char32_t>(data[index]);
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        append_unit(out, cp);
    }
    return true;
}

void repr_into(std::string& out, const Json& value) {
    if (value.is_string()) {
        out += py::repr(value.get_ref<const std::string&>());
    } else {
        out += py_str(value);
    }
}

} // namespace

bool truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return false;
    case Json::value_t::boolean:
        return value.get<bool>();
    case Json::value_t::number_integer:
        return value.get<std::int64_t>() != 0;
    case Json::value_t::number_unsigned:
        return value.get<std::uint64_t>() != 0;
    case Json::value_t::number_float:
        return value.get<double>() != 0.0;
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
    case Json::value_t::binary:
        return !value.empty();
    }
    return false;
}

std::string py_str(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return "None";
    case Json::value_t::boolean:
        return value.get<bool>() ? "True" : "False";
    case Json::value_t::number_integer:
        return std::to_string(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
        return std::to_string(value.get<std::uint64_t>());
    case Json::value_t::number_float:
        return py::float_repr(value.get<double>());
    case Json::value_t::string:
        return value.get<std::string>();
    case Json::value_t::array: {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            repr_into(out, item);
        }
        return out + "]";
    }
    case Json::value_t::object: {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += py::repr(key);
            out += ": ";
            repr_into(out, item);
        }
        return out + "}";
    }
    case Json::value_t::binary:
        return "b''";
    }
    return "";
}

std::string py_str_or_empty(const Json& value) {
    return truthy(value) ? py_str(value) : std::string();
}

bool is_int(const Json& value) {
    return value.is_number_integer();
}

std::optional<std::string> decode_utf8(std::string_view bytes, bool surrogatepass) {
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t prefix = 0;
        const std::size_t length = sequence_at(bytes, pos, surrogatepass, prefix);
        if (length == 0) {
            return std::nullopt;
        }
        pos += length;
    }
    return std::string(bytes);
}

std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t prefix = 0;
        const std::size_t length = sequence_at(bytes, pos, false, prefix);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            pos += prefix;
        } else {
            out.append(bytes.substr(pos, length));
            pos += length;
        }
    }
    return out;
}

std::optional<std::string> json_bytes_to_text(std::span<const std::byte> data) {
    const auto byte = [&](std::size_t i) {
        return std::to_integer<unsigned>(data[i]);
    };
    const auto starts = [&](std::initializer_list<unsigned> prefix) {
        if (data.size() < prefix.size()) {
            return false;
        }
        std::size_t i = 0;
        for (const unsigned value : prefix) {
            if (byte(i++) != value) {
                return false;
            }
        }
        return true;
    };
    std::string out;
    if (starts({0x00, 0x00, 0xFE, 0xFF})) {
        return decode_units32(data.subspan(4), true, out) ? std::optional(out) : std::nullopt;
    }
    if (starts({0xFF, 0xFE, 0x00, 0x00})) {
        return decode_units32(data.subspan(4), false, out) ? std::optional(out) : std::nullopt;
    }
    if (starts({0xFE, 0xFF})) {
        return decode_units16(data.subspan(2), true, out) ? std::optional(out) : std::nullopt;
    }
    if (starts({0xFF, 0xFE})) {
        return decode_units16(data.subspan(2), false, out) ? std::optional(out) : std::nullopt;
    }
    if (starts({0xEF, 0xBB, 0xBF})) {
        return decode_utf8(as_chars(data.subspan(3)), true);
    }
    if (data.size() >= 4) {
        if (byte(0) == 0) {
            const bool utf16 = byte(1) != 0;
            const bool ok =
                utf16 ? decode_units16(data, true, out) : decode_units32(data, true, out);
            return ok ? std::optional(out) : std::nullopt;
        }
        if (byte(1) == 0) {
            const bool utf16 = byte(2) != 0 || byte(3) != 0;
            const bool ok =
                utf16 ? decode_units16(data, false, out) : decode_units32(data, false, out);
            return ok ? std::optional(out) : std::nullopt;
        }
    } else if (data.size() == 2) {
        if (byte(0) == 0) {
            return decode_units16(data, true, out) ? std::optional(out) : std::nullopt;
        }
        if (byte(1) == 0) {
            return decode_units16(data, false, out) ? std::optional(out) : std::nullopt;
        }
    }
    return decode_utf8(as_chars(data), true);
}

std::string_view as_chars(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

} // namespace fv::remote::detail
