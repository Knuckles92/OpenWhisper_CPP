#include "automation/detail/sse_parser.h"

#include <utility>

namespace fv::automation {

SseParser::SseParser(std::string last_id) : last_id_(std::move(last_id)) {}

void SseParser::reset() {
    type_.clear();
    data_.clear();
    id_.reset();
}

std::optional<SseEvent> SseParser::feed(std::string_view line) {
    if (line.empty()) {
        if (id_) {
            last_id_ = *id_;
        }
        std::optional<SseEvent> event;
        if (!data_.empty()) {
            std::string data;
            for (std::size_t i = 0; i < data_.size(); ++i) {
                if (i > 0) {
                    data += '\n';
                }
                data += data_[i];
            }
            event = SseEvent{type_.empty() ? "message" : type_, std::move(data), last_id_};
        }
        reset();
        return event;
    }
    if (line.front() == ':') {
        return std::nullopt;
    }
    const std::size_t colon = line.find(':');
    const std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? "" : line.substr(colon + 1);
    if (value.starts_with(' ')) {
        value.remove_prefix(1);
    }
    if (field == "event") {
        type_ = std::string(value);
    } else if (field == "data") {
        data_.emplace_back(value);
    } else if (field == "id" && value.find('\0') == std::string_view::npos) {
        id_ = std::string(value);
    }
    return std::nullopt;
}

namespace detail {

std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    const auto at = [&](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            low = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            high = 0x9F;
        } else if (lead == 0xF0) {
            length = 4;
            low = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            high = 0x8F;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t valid = 1;
        while (valid < length && i + valid < bytes.size()) {
            const unsigned char next = at(i + valid);
            if (next < (valid == 1 ? low : 0x80) || next > (valid == 1 ? high : 0xBF)) {
                break;
            }
            ++valid;
        }
        if (valid == length) {
            out.append(bytes.substr(i, length));
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += valid;
    }
    return out;
}

std::string sse_line(std::string_view raw) {
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) {
        raw.remove_suffix(1);
    }
    return decode_utf8_replace(raw);
}

} // namespace detail

} // namespace fv::automation
