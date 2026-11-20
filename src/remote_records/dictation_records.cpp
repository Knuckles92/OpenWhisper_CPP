#include "remote_records/dictation_records.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <utility>

#include "base/atomic_file.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "history/manager.h"
#include "history/repository.h"
#include "pyport/errors.h"

namespace fv::remote_records {
namespace {

namespace fs = std::filesystem;

using OptionalText = std::optional<std::string> HistoryEntry::*;
constexpr std::array<std::pair<std::string_view, OptionalText>, 5> kText{{
    {"raw_text", &HistoryEntry::raw_text},
    {"cleanup_provider", &HistoryEntry::cleanup_provider},
    {"cleanup_model", &HistoryEntry::cleanup_model},
    {"source_name", &HistoryEntry::source_name},
    {"title", &HistoryEntry::title},
}};
constexpr std::array<std::pair<std::string_view, OptionalText>, 4> kExtended{{
    {"entry_kind", &HistoryEntry::entry_kind},
    {"cleanup_level", &HistoryEntry::cleanup_level},
    {"cleaned_text", &HistoryEntry::cleaned_text},
    {"language", &HistoryEntry::language},
}};

const Json& get(const Json& object, std::string_view key) {
    static const Json missing;
    const auto at = object.find(std::string(key));
    return at != object.end() ? *at : missing;
}

std::optional<std::string> optional_text(const Json& object, std::string_view key) {
    const auto& value = get(object, key);
    if (value.is_null()) {
        return std::nullopt;
    }
    const auto cap = key == "text" || key == "raw_text" || key == "cleaned_text" ? 2U << 20 : 4096U;
    if (!value.is_string() || value.get_ref<const std::string&>().size() > cap) {
        throw py::ValueError("Invalid entry." + std::string(key) + ".");
    }
    return value.get<std::string>();
}

std::string required_text(const Json& object, std::string_view key) {
    auto result = optional_text(object, key);
    if (!result) {
        throw py::ValueError("entry." + std::string(key) + " is required.");
    }
    return std::move(*result);
}

std::optional<double> number(const Json& object, std::string_view key) {
    const auto& value = get(object, key);
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        throw py::ValueError("Invalid entry." + std::string(key) + ".");
    }
    return value.get<double>();
}

std::optional<std::int64_t> byte_count(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) {
        throw py::ValueError("Invalid entry.file_size.");
    }
    return value.get<std::int64_t>();
}

HistoryEntry parse_entry(std::string_view id, const Json& record,
                         const remote::PairedDevice& owner) {
    const auto& data = get(record, "entry");
    if (!data.is_object() || required_text(data, "id") != id) {
        throw py::ValueError("The entry's id doesn't match the record.");
    }
    HistoryEntry entry;
    entry.id = std::string(id);
    entry.text = required_text(data, "text");
    entry.timestamp = required_text(data, "timestamp");
    entry.model = required_text(data, "model");
    entry.transcription_time = number(data, "transcription_time");
    entry.audio_duration = number(data, "audio_duration");
    entry.file_size = byte_count(get(data, "file_size"));
    if (!owner.id.empty()) {
        entry.origin_device_id = owner.id;
        entry.origin_device_name = owner.name;
    }
    for (const auto& [name, field] : kText) {
        entry.*field = optional_text(data, name);
    }
    const auto& extended = get(record, "entry_ext");
    for (const auto& [name, field] : kExtended) {
        try {
            entry.*field = optional_text(extended, name);
        } catch (const py::ValueError&) {
            // Newer advisory fields never prevent importing the core entry.
        }
    }
    return entry;
}

template <class T> Json optional_json(const std::optional<T>& value) {
    return value ? Json(*value) : Json();
}

Json export_entry(const HistoryEntry& entry) {
    Json value{{"id", entry.id},
               {"text", entry.text},
               {"timestamp", entry.timestamp},
               {"model", entry.model}};
    value["transcription_time"] = optional_json(entry.transcription_time);
    value["audio_duration"] = optional_json(entry.audio_duration);
    value["file_size"] = optional_json(entry.file_size);
    for (const auto& [name, field] : kText) {
        value[std::string(name)] = optional_json(entry.*field);
    }
    return value;
}

Json export_extended(const HistoryEntry& entry) {
    Json value = Json::object();
    for (const auto& [name, field] : kExtended) {
        value[std::string(name)] = optional_json(entry.*field);
    }
    return value;
}

fs::path checked_folder(const fs::path& root, std::string_view owner) {
    const auto path = path_from_utf8(safe_name(Json("devices/" + std::string(owner))));
    const auto parent = fs::weakly_canonical(fs::absolute(root));
    const auto folder = fs::weakly_canonical(parent / path);
    if (folder.lexically_relative(parent) != path) {
        throw py::ValueError("A device's recordings folder is unavailable.");
    }
    return folder;
}

HistoryOrigin origin(std::string_view device) {
    return device.empty() ? HistoryOrigin::own() : HistoryOrigin::device(std::string(device));
}

bool is_wav(const fs::path& path) {
    std::array<char, 12> header{};
    std::ifstream source(path, std::ios::binary);
    source.read(header.data(), static_cast<std::streamsize>(header.size()));
    return source && std::string_view(header.data(), 4) == "RIFF" &&
           std::string_view(header.data() + 8, 4) == "WAVE";
}

class DictationRecords final : public RecordKind {
public:
    DictationRecords(fs::path root, Database& db) : root_(std::move(root)), db_(db), entries_(db) {}

    Json import_record(std::string_view id, const fs::path& files, const Json& record,
                       const remote::PairedDevice& owner) override {
        auto entry = parse_entry(id, record, owner);
        const auto& audio_name = get(record, "audio");
        fs::path staged;
        if (!audio_name.is_null()) {
            const auto name = safe_name(audio_name);
            if (!name.starts_with("audio/") || name.find('/', 6) != std::string::npos ||
                path_from_utf8(name).extension() != ".wav") {
                throw py::ValueError("The entry's audio must be one WAV file.");
            }
            const auto source = files / path_from_utf8(name);
            if (!is_wav(source)) {
                throw py::ValueError("The entry's audio isn't a WAV file.");
            }
            const auto folder = owner.id.empty() ? fs::weakly_canonical(fs::absolute(root_))
                                                 : checked_folder(root_, owner.id);
            fs::create_directories(folder);
            staged = folder / (uuid4_hex() + ".wav");
            fs::copy_file(source, staged);
            auto filename = path_to_utf8(staged.filename());
            entry.audio_file = owner.id.empty() ? filename : "devices/" + owner.id + "/" + filename;
        }
        std::optional<fs::path> old_audio;
        try {
            db_.write([&](sql::Connection&) {
                const auto existing = entries_.get_history_entry_by_id(id);
                if (existing && existing->origin_device_id != entry.origin_device_id) {
                    throw py::ValueError("A different history entry already has this id.");
                }
                if (existing) {
                    if (!owner.id.empty() &&
                        (export_entry(*existing) != export_entry(entry) ||
                         export_extended(*existing) != export_extended(entry))) {
                        throw std::runtime_error(
                            "The host already keeps a different version of this dictation. "
                            "Both copies were kept.");
                    }
                    if (staged.empty()) {
                        entry.audio_file = existing->audio_file;
                    } else if (const auto saved_audio = audio(*existing)) {
                        if (!owner.id.empty()) {
                            if (download::sha256_file(*saved_audio, true) !=
                                download::sha256_file(staged, true)) {
                                throw std::runtime_error(
                                    "The host already keeps a different recording for this "
                                    "dictation. Both copies were kept.");
                            }
                            fs::remove(staged);
                            staged.clear();
                            entry.audio_file = existing->audio_file;
                        } else {
                            old_audio = saved_audio;
                        }
                    }
                }
                entries_.put_history_entry(entry);
            });
        } catch (...) {
            if (!staged.empty()) {
                std::error_code error;
                fs::remove(staged, error);
            }
            throw;
        }
        if (old_audio) {
            std::error_code error;
            fs::remove(*old_audio, error);
        }
        if (!owner.id.empty()) {
            apply_retention(owner.id, get(record, "retention"));
        }
        return {{"audio", entry.audio_file.has_value()}};
    }

    // kinds.py _apply_retention: the device's own recording limits, applied to what it keeps
    // here. Entries keep their text; only their oldest recordings go, newest first by the
    // entry's time.
    void apply_retention(std::string_view device, const Json& retention) {
        if (!retention.is_object()) {
            return;
        }
        const auto limit = [&](std::string_view key) -> std::optional<std::int64_t> {
            const auto& value = get(retention, key);
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) {
                return std::nullopt;
            }
            const auto count = value.get<std::int64_t>();
            return count > 0 ? std::make_optional(count) : std::nullopt;
        };
        const auto max_count = limit("max_recordings");
        const auto max_bytes = limit("max_bytes");
        if (!max_count && !max_bytes) {
            return;
        }
        std::vector<RecordingInfo> recordings;
        for (const auto& entry : entries_.get_history_entries({}, origin(device))) {
            if (const auto file = audio(entry)) {
                std::error_code error;
                const auto size = fs::file_size(*file, error);
                recordings.push_back({*entry.audio_file, entry.timestamp, *file,
                                      error ? 0 : static_cast<std::int64_t>(size)});
            }
        }
        std::ranges::stable_sort(recordings, std::greater{}, &RecordingInfo::timestamp);
        for (const auto& recording : recordings_over_limit(recordings, max_count, max_bytes)) {
            std::error_code error;
            fs::remove(recording.file_path, error);
            if (error) {
                continue;
            }
            entries_.clear_history_audio_file(recording.filename);
        }
    }

    Json list(std::string_view device, std::string_view query, int limit) override {
        const auto rows = entries_.search_history_entries(query, limit, origin(device));
        Json out = Json::array();
        for (const auto& entry : rows) {
            auto data = export_entry(entry);
            data.update(export_extended(entry));
            const auto file = audio(entry);
            data["has_audio"] = file.has_value();
            data["audio_bytes"] = file ? fs::file_size(*file) : 0;
            out.push_back(std::move(data));
        }
        return out;
    }

    Files export_record(std::string_view device, std::string_view id,
                        const fs::path& dest) override {
        const auto entry = entries_.get_history_entry_by_id(id, origin(device));
        if (!entry) {
            throw RecordNotFound("That history entry isn't here anymore.");
        }
        Json record{{"kind", "dictation"},
                    {"format", 1},
                    {"entry", export_entry(*entry)},
                    {"entry_ext", export_extended(*entry)},
                    {"audio", nullptr}};
        Files files;
        if (const auto file = audio(*entry)) {
            files["audio/recording.wav"] = *file;
            record["audio"] = "audio/recording.wav";
        }
        write_file_atomic(dest / "record.json", record.dump());
        return files;
    }

    Json stored_files(std::string_view, std::string_view) override { return Json::object(); }

    bool remove(std::string_view device, std::string_view id) override {
        std::optional<fs::path> file;
        const bool found = db_.write([&](sql::Connection&) {
            const auto entry = entries_.get_history_entry_by_id(id, origin(device));
            if (!entry) {
                return false;
            }
            file = audio(*entry);
            return entries_.delete_history_entry(id);
        });
        if (found && file) {
            std::error_code error;
            fs::remove(*file, error);
        }
        return found;
    }

    int delete_all(std::string_view device) override {
        int count = 0;
        for (const auto& entry : entries_.get_history_entries({}, origin(device))) {
            count += remove(device, entry.id) ? 1 : 0;
        }
        return count;
    }

    int rename_owner(std::string_view device, std::string_view name) override {
        return entries_.rename_history_origin(device, name);
    }

    Json summary(std::string_view device) override {
        const auto rows = entries_.get_history_entries({}, origin(device));
        std::uint64_t bytes = 0;
        for (const auto& entry : rows) {
            if (const auto file = audio(entry)) {
                bytes += fs::file_size(*file);
            }
        }
        return {{"count", rows.size()}, {"bytes", bytes}};
    }

    std::vector<remote::RecordOwnerGroup> owners() override {
        std::vector<remote::RecordOwnerGroup> out;
        for (const auto& entry : entries_.history_origin_counts()) {
            out.push_back({entry.device_id,
                           entry.name.empty() ? "Unnamed computer" : entry.name,
                           {{"dictation", entry.count}}});
        }
        return out;
    }

    bool owns(std::string_view device, std::string_view id) override {
        return entries_.get_history_entry_by_id(id, origin(device)).has_value();
    }

private:
    std::optional<fs::path> audio(const HistoryEntry& entry) const {
        if (!entry.audio_file) {
            return std::nullopt;
        }
        const auto path =
            fs::weakly_canonical(fs::absolute(root_) / path_from_utf8(*entry.audio_file));
        const auto folder = entry.origin_device_id ? checked_folder(root_, *entry.origin_device_id)
                                                   : fs::weakly_canonical(fs::absolute(root_));
        if (path.parent_path() == folder && fs::is_regular_file(path)) {
            return path;
        }
        return std::nullopt;
    }

    fs::path root_;
    Database& db_;
    HistoryRepository entries_;
};

} // namespace

std::shared_ptr<RecordKind> dictation_records(fs::path recordings, Database& db) {
    return std::make_shared<DictationRecords>(std::move(recordings), db);
}

} // namespace fv::remote_records
