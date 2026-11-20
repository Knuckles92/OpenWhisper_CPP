#include "llm/sse.h"

#include <algorithm>
#include <climits>

namespace fv::llm {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

std::optional<int> digits_value(std::string_view text) {
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    long long value = 0;
    for (const char c : text) {
        value = std::min<long long>(value * 10 + (c - '0'), INT_MAX);
    }
    return static_cast<int>(value);
}

} // namespace

void SseParser::feed(std::string_view bytes, const SseHandler& on_event) {
    if (!started_) {
        // The first bytes are held back until they are known to be the BOM or not.
        std::size_t taken = 0;
        while (taken < bytes.size() && pending_.size() < kBom.size() &&
               bytes[taken] == kBom[pending_.size()]) {
            pending_.push_back(bytes[taken++]);
        }
        bytes.remove_prefix(taken);
        if (pending_.size() == kBom.size()) {
            pending_.clear();
            started_ = true;
        } else if (!bytes.empty()) {
            started_ = true; // what was held back is the line's first bytes
        } else {
            return;
        }
    }
    while (!bytes.empty()) {
        if (after_cr_) {
            after_cr_ = false;
            if (bytes.front() == '\n') {
                bytes.remove_prefix(1);
                continue;
            }
        }
        const auto end = bytes.find_first_of("\r\n");
        if (end == std::string_view::npos) {
            pending_.append(bytes);
            return;
        }
        pending_.append(bytes.substr(0, end));
        after_cr_ = bytes[end] == '\r';
        bytes.remove_prefix(end + 1);
        const std::string text = std::move(pending_);
        pending_.clear();
        line(text, on_event);
    }
}

void SseParser::finish(const SseHandler& on_event) {
    if (!pending_.empty()) {
        const std::string text = std::move(pending_);
        pending_.clear();
        line(text, on_event);
    }
    dispatch(on_event);
    *this = SseParser();
}

void SseParser::line(std::string_view text, const SseHandler& on_event) {
    if (text.empty()) {
        dispatch(on_event);
        return;
    }
    if (text.front() == ':') {
        return;
    }
    std::string_view field = text;
    std::string_view value;
    if (const auto colon = text.find(':'); colon != std::string_view::npos) {
        field = text.substr(0, colon);
        value = text.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }
    if (field == "event") {
        event_ = value;
    } else if (field == "data") {
        data_.append(value);
        data_.push_back('\n');
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            last_id_ = value;
        }
    } else if (field == "retry") {
        if (const auto ms = digits_value(value)) {
            retry_ = ms;
        }
    }
}

void SseParser::dispatch(const SseHandler& on_event) {
    if (data_.empty()) {
        event_.clear();
        return;
    }
    data_.pop_back();
    SseEvent event{event_.empty() ? std::string("message") : std::move(event_), std::move(data_),
                   last_id_, retry_};
    event_.clear();
    data_.clear();
    retry_.reset();
    on_event(event);
}

} // namespace fv::llm
