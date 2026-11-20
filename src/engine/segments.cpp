#include "engine/transcript.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace fv {

namespace {

// Python's str.isspace() code points: the runtime's word texts are tested after rstrip(), and a
// text without timings after strip(), which remove every one of them.
bool is_python_space(std::uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

// The code point that starts at text[at] and its length in bytes; malformed bytes are one
// code point each, which no whitespace test matches.
std::pair<std::uint32_t, std::size_t> decode_at(std::string_view text, std::size_t at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 1;
    std::uint32_t c = lead;
    if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        c = lead & 0x07u;
    } else if (lead >= 0xE0) {
        length = 3;
        c = lead & 0x0Fu;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        c = lead & 0x1Fu;
    } else {
        return {lead >= 0x80 ? 0xFFFDu : lead, 1};
    }
    if (at + length > text.size()) {
        return {0xFFFDu, 1};
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0u) != 0x80u) {
            return {0xFFFDu, 1};
        }
        c = (c << 6) | (next & 0x3Fu);
    }
    return {c, length};
}

// The last code point of `text` that is not Python whitespace; nullopt when there is none.
std::optional<std::uint32_t> last_non_space(std::string_view text) {
    std::optional<std::uint32_t> last;
    for (std::size_t at = 0; at < text.size();) {
        const auto [c, length] = decode_at(text, at);
        if (!is_python_space(c)) {
            last = c;
        }
        at += length;
    }
    return last;
}

bool ends_sentence(std::string_view word) {
    const std::optional<std::uint32_t> last = last_non_space(word);
    return last && (*last == '.' || *last == '?' || *last == '!');
}

Segment make_segment(std::span<const Word> words) {
    Segment segment;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            segment.text += ' ';
        }
        segment.text += words[i].text;
    }
    segment.start = words.front().start;
    segment.end = words.back().end;
    return segment;
}

} // namespace

std::vector<Segment> segments_from_words(std::string_view text, std::span<const Word> words,
                                         double duration) {
    std::vector<Segment> segments;
    if (words.empty()) {
        if (last_non_space(text)) {
            segments.push_back(Segment{std::string(text), 0.0, duration});
        }
        return segments;
    }
    std::size_t first = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::size_t count = i - first + 1;
        if (ends_sentence(words[i].text) || count >= kSegmentMaxWords) {
            segments.push_back(make_segment(words.subspan(first, count)));
            first = i + 1;
        }
    }
    if (first < words.size()) {
        segments.push_back(make_segment(words.subspan(first)));
    }
    return segments;
}

} // namespace fv
