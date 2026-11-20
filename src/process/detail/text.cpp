#include "process/detail/text.h"

#include <memory>
#include <utility>

#include "base/utf8.h"

namespace fv::process_detail {

namespace {

void append_replacement(std::string& out) {
    out += "\xEF\xBF\xBD";
}

} // namespace

std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    const auto at = [&](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
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
            append_replacement(out);
            ++i;
            continue;
        }
        std::size_t valid = 1;
        while (valid < length && i + valid < bytes.size()) {
            const unsigned char next = at(i + valid);
            const unsigned char min = valid == 1 ? low : 0x80;
            const unsigned char max = valid == 1 ? high : 0xBF;
            if (next < min || next > max) {
                break;
            }
            ++valid;
        }
        if (valid == length) {
            out.append(bytes.substr(i, length));
        } else {
            append_replacement(out);
        }
        i += valid;
    }
    return out;
}

ByteSink line_splitter(LineSink sink) {
    auto buffer = std::make_shared<std::string>();
    return [buffer, sink = std::move(sink)](std::span<const char> bytes) {
        if (bytes.empty()) {
            if (!buffer->empty() && sink) {
                sink(decode_utf8_replace(*buffer));
            }
            buffer->clear();
            return;
        }
        buffer->append(bytes.data(), bytes.size());
        std::size_t start = 0;
        for (;;) {
            const auto end = buffer->find('\n', start);
            if (end == std::string::npos) {
                break;
            }
            std::string_view line(buffer->data() + start, end - start);
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            if (sink) {
                sink(decode_utf8_replace(line));
            }
            start = end + 1;
        }
        buffer->erase(0, start);
    };
}

std::string home_folder() {
#ifdef _WIN32
    if (auto profile = env_utf8("USERPROFILE")) {
        return *profile;
    }
    const auto drive = env_utf8("HOMEDRIVE");
    const auto path = env_utf8("HOMEPATH");
    if (path) {
        return drive.value_or("") + *path;
    }
    return {};
#else
    return env_utf8("HOME").value_or("");
#endif
}

} // namespace fv::process_detail
