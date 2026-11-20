#include "history/detail/rows.h"

#include <format>
#include <variant>

#include "db/schema.h"

namespace fv::history_detail {

const std::string& select_columns() {
    static const std::string text = [] {
        std::string columns;
        for (const auto column : schema::transcription_history::columns) {
            columns += columns.empty() ? "" : ", ";
            columns += "transcription_history." + std::string(column);
        }
        return "SELECT " + columns + " FROM transcription_history";
    }();
    return text;
}

std::optional<std::string> optional_text(const sql::Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return std::format("{}", *real);
    }
    if (const auto* blob = std::get_if<sql::Blob>(&value)) {
        return std::string(reinterpret_cast<const char*>(blob->data()), blob->size());
    }
    return std::nullopt;
}

std::optional<double> optional_real(const sql::Value& value) {
    if (const auto* real = std::get_if<double>(&value)) {
        return *real;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

std::optional<std::int64_t> optional_integer(const sql::Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer;
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(*real);
    }
    return std::nullopt;
}

HistoryEntry read_entry(const sql::Statement& statement) {
    int column = 0;
    const auto text = [&] {
        return optional_text(statement.column(column++));
    };
    const auto real = [&] {
        return optional_real(statement.column(column++));
    };
    const auto integer = [&] {
        return optional_integer(statement.column(column++));
    };
    HistoryEntry entry;
    entry.id = text().value_or("");
    entry.text = text().value_or("");
    entry.raw_text = text();
    entry.timestamp = text().value_or("");
    entry.model = text().value_or("");
    entry.audio_file = text();
    entry.transcription_time = real();
    entry.audio_duration = real();
    entry.file_size = integer();
    entry.cleanup_provider = text();
    entry.cleanup_model = text();
    entry.source_name = text();
    entry.title = text();
    entry.origin_device_id = text();
    entry.origin_device_name = text();
    entry.app_id = text();
    entry.app_name = text();
    entry.app_category = text();
    entry.cleanup_level = text();
    entry.entry_kind = text();
    entry.cleaned_text = text();
    entry.language = text();
    return entry;
}

} // namespace fv::history_detail
