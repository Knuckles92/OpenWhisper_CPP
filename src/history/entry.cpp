#include "history/entry.h"

#include <algorithm>
#include <vector>

#include "base/ids.h"
#include "history/detail/time.h"
#include "history/format_utils.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv {

namespace {

bool is_context_column(std::string_view name) {
    return std::ranges::find(HISTORY_CONTEXT_COLUMNS, name) != HISTORY_CONTEXT_COLUMNS.end();
}

template <class Entry> auto* context_member(Entry& entry, std::string_view name) {
    using Member = decltype(&entry.app_id);
    Member member = nullptr;
    if (name == "app_id") {
        member = &entry.app_id;
    } else if (name == "app_name") {
        member = &entry.app_name;
    } else if (name == "app_category") {
        member = &entry.app_category;
    } else if (name == "cleanup_level") {
        member = &entry.cleanup_level;
    } else if (name == "entry_kind") {
        member = &entry.entry_kind;
    } else if (name == "cleaned_text") {
        member = &entry.cleaned_text;
    } else if (name == "language") {
        member = &entry.language;
    }
    return member;
}

} // namespace

std::string HistoryEntry::formatted_timestamp(const Clock& clock) const {
    return format_timestamp(std::string_view(timestamp), clock);
}

std::string HistoryEntry::preview_text() const {
    return fv::preview_text(text);
}

std::optional<std::string>* HistoryEntry::context_column(std::string_view name) {
    return context_member(*this, name);
}

const std::optional<std::string>* HistoryEntry::context_column(std::string_view name) const {
    return context_member(*this, name);
}

std::string preview_text(std::string_view text) {
    const std::string body = py::strip(text);
    if (body.empty()) {
        return "Empty transcript";
    }
    constexpr std::size_t max_len = 100;
    if (py::len(body) <= max_len) {
        return body;
    }
    std::string head = py::slice(body, 0, static_cast<std::int64_t>(max_len));
    // rsplit(' ', 1)[0]: everything before the last space, or all of it without one.
    if (const auto space = head.rfind(' '); space != std::string::npos) {
        head.resize(space);
    }
    return head + "...";
}

ContextFields history_context(const ContextFields& values) {
    std::vector<std::string> unknown;
    for (const auto& [key, value] : values) {
        if (!is_context_column(key)) {
            unknown.push_back(key);
        }
    }
    if (!unknown.empty()) {
        std::string names;
        for (const auto& name : unknown) { // the map keeps them sorted, as sorted() does
            names += names.empty() ? name : ", " + name;
        }
        throw py::TypeError("Not history context columns: " + names);
    }
    ContextFields kept;
    for (const auto& [key, value] : values) {
        if (value) {
            kept.emplace(key, value);
        }
    }
    return kept;
}

std::string uuid4_string() {
    const std::string hex = uuid4_hex();
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

HistoryEntry create_entry(const NewEntry& fields, const Clock& clock) {
    const ContextFields context = history_context(fields.context);
    HistoryEntry entry;
    entry.id = uuid4_string();
    entry.text = fields.text;
    entry.raw_text = fields.raw_text;
    entry.timestamp = history_detail::utc_now(clock).isoformat();
    entry.model = fields.model;
    entry.audio_file = fields.audio_file;
    entry.transcription_time = fields.transcription_time;
    entry.audio_duration = fields.audio_duration;
    entry.file_size = fields.file_size;
    entry.cleanup_provider = fields.cleanup_provider;
    entry.cleanup_model = fields.cleanup_model;
    entry.source_name = fields.source_name;
    for (const auto& [key, value] : context) {
        *entry.context_column(key) = value;
    }
    return entry;
}

} // namespace fv
