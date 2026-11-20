#include "history/manager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "history/detail/exclusive_file.h"
#include "history/detail/time.h"
#include "history/format_utils.h"
#include "history/retention_settings.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.history_manager");

// Quick Record names every live dictation, with a profile after " · ".
constexpr std::string_view dictation_source = "Quick Record";
// How far back last_dictation looks past uploads and rewrites.
constexpr std::int64_t last_dictation_scan = 50;
// How long a usage count stays good while the folder looks unchanged.
constexpr auto usage_ttl = std::chrono::milliseconds(2000);
constexpr std::size_t copy_chunk = 1024 * 1024;

std::string replace_all(std::string text, std::string_view from) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from)) {
        text.erase(at, from.size());
    }
    return text;
}

// datetime.strptime(stem, "%Y%m%d_%H%M%S").isoformat(), or nullopt for ValueError. The
// pattern is _strptime's for those directives, matched from the start, and refused when text
// is left over, as strptime does.
std::optional<std::string> parse_recording_stamp(std::string_view stem) {
    static const py::Regex pattern(
        R"((?P<Y>\d\d\d\d)(?P<m>1[0-2]|0[1-9]|[1-9])(?P<d>3[0-1]|[1-2]\d|0[1-9]|[1-9]| [1-9])_)"
        R"((?P<H>2[0-3]|[0-1]\d|\d)(?P<M>[0-5]\d|\d)(?P<S>6[0-1]|[0-5]\d|\d))",
        py::IGNORECASE);
    const auto found = pattern.match(stem);
    if (!found || found->end() != py::len(stem)) {
        return std::nullopt;
    }
    const auto number = [&](std::string_view group) {
        return static_cast<int>(py::py_int(py::strip(found->group(group))).value_or(-1));
    };
    py::DateTime moment;
    moment.year = number("Y");
    moment.month = number("m");
    moment.day = number("d");
    moment.hour = number("H");
    moment.minute = number("M");
    moment.second = number("S");
    if (moment.year < 1 || moment.second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{moment.year},
                                           std::chrono::month{static_cast<unsigned>(moment.month)},
                                           std::chrono::day{static_cast<unsigned>(moment.day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return moment.isoformat();
}

// datetime.fromtimestamp(st_mtime).isoformat(): naive local wall time to the microsecond.
std::string modification_stamp(std::filesystem::file_time_type modified, const Clock& clock) {
    const auto utc = std::chrono::clock_cast<std::chrono::system_clock>(modified);
    const auto micros = std::chrono::round<std::chrono::microseconds>(utc);
    return history_detail::naive_from(clock.zone()->to_local(micros)).isoformat();
}

std::string utf8(const std::filesystem::path& path) {
    return path_to_utf8(path);
}

} // namespace

std::string kind_of(const HistoryEntry& entry) {
    if (entry.entry_kind && !entry.entry_kind->empty()) {
        return *entry.entry_kind;
    }
    const std::string source = entry.source_name.value_or("");
    return source.starts_with(dictation_source) ? "dictation" : "file";
}

std::string entry_version(const HistoryEntry& entry) {
    if (!entry.raw_text || entry.raw_text->empty()) {
        return "";
    }
    const std::string& raw = *entry.raw_text;
    if (entry.text != raw) {
        return std::string(AI_VERSION);
    }
    return entry.cleaned_text && *entry.cleaned_text != raw ? std::string(ORIGINAL_VERSION) : "";
}

std::optional<std::string> ai_text(const HistoryEntry& entry) {
    const std::string version = entry_version(entry);
    if (version == AI_VERSION) {
        return entry.text;
    }
    if (version == ORIGINAL_VERSION) {
        return entry.cleaned_text;
    }
    return std::nullopt;
}

bool is_local_entry(const HistoryEntry& entry) {
    return !entry.origin_device_id && (!entry.stored_on || entry.stored_on->empty());
}

std::string describe_retention(std::optional<std::int64_t> max_recordings,
                               std::optional<std::int64_t> max_bytes) {
    std::string limits;
    if (max_recordings) {
        limits = std::to_string(*max_recordings);
    }
    if (max_bytes) {
        limits += (limits.empty() ? "" : ", ") + format_file_size(static_cast<double>(*max_bytes));
    }
    return limits.empty() ? "all" : limits;
}

std::string RecordingInfo::formatted_timestamp(const Clock& clock) const {
    return format_timestamp(std::string_view(timestamp), clock);
}

std::vector<RecordingInfo> recordings_over_limit(const std::vector<RecordingInfo>& recordings,
                                                 std::optional<std::int64_t> max_recordings,
                                                 std::optional<std::int64_t> max_bytes) {
    auto keep = static_cast<std::int64_t>(recordings.size());
    if (max_recordings) {
        // A negative limit slices from the end, as recordings[:keep] does in Python.
        keep = std::min(keep, *max_recordings);
        if (keep < 0) {
            keep = std::max<std::int64_t>(0, static_cast<std::int64_t>(recordings.size()) + keep);
        }
    }
    if (max_bytes) {
        std::int64_t total = 0;
        for (std::int64_t index = 0; index < keep; ++index) {
            total += recordings[static_cast<std::size_t>(index)].size_bytes;
            if (index > 0 && total > *max_bytes) {
                keep = index;
                break;
            }
        }
    }
    return {recordings.begin() + keep, recordings.end()};
}

HistoryManager::HistoryManager(Database& database, std::filesystem::path recordings_folder,
                               const Json& settings, HistoryServices services)
    : HistoryManager(database, std::move(recordings_folder),
                     RecordingRetention{resolve_max_saved_recordings(settings),
                                        resolve_max_saved_recordings_bytes(settings)},
                     std::move(services), true) {}

HistoryManager::HistoryManager(Database& database, std::filesystem::path recordings_folder,
                               RecordingRetention retention, HistoryServices services)
    : HistoryManager(database, std::move(recordings_folder), retention, std::move(services),
                     false) {}

HistoryManager::HistoryManager(Database& database, std::filesystem::path recordings_folder,
                               RecordingRetention retention, HistoryServices services, bool)
    : repository_(database), recordings_folder_(std::move(recordings_folder)),
      services_(std::move(services)), retention_(retention) {
    std::filesystem::create_directories(recordings_folder_);
    logger.info("HistoryManager initialized (recordings: {}, max: {})", utf8(recordings_folder_),
                describe_retention(retention.max_recordings, retention.max_bytes));
}

std::filesystem::path HistoryManager::default_recordings_folder(const Paths& paths) {
    return paths.user_file("recordings");
}

std::optional<std::int64_t> HistoryManager::max_recordings() const {
    std::lock_guard lock(retention_mutex_);
    return retention_.max_recordings;
}

std::optional<std::int64_t> HistoryManager::max_bytes() const {
    std::lock_guard lock(retention_mutex_);
    return retention_.max_bytes;
}

RecordSync& HistoryManager::record_sync() {
    if (services_.record_sync != nullptr) {
        return *services_.record_sync;
    }
    if (services_.find_record_sync) {
        if (RecordSync* found = services_.find_record_sync()) {
            return *found;
        }
    }
    return null_record_sync();
}

const Clock& HistoryManager::clock() const {
    return services_.clock != nullptr ? *services_.clock : system_clock();
}

void HistoryManager::forget_replayable_events() {
    if (services_.forget_replayable_events) {
        services_.forget_replayable_events();
    }
}

void HistoryManager::set_retention(std::optional<std::int64_t> max_recordings,
                                   std::optional<std::int64_t> max_bytes) {
    {
        std::lock_guard lock(retention_mutex_);
        retention_ = {max_recordings, max_bytes};
    }
    logger.info("Recording retention updated (max: {})",
                describe_retention(max_recordings, max_bytes));
    rotate_recordings();
}

std::optional<HistoryEntry> HistoryManager::add_entry(const AddEntry& fields) {
    ContextFields context;
    std::string ignored;
    for (const auto& [key, value] : fields.columns) {
        const bool known =
            std::ranges::find(HISTORY_CONTEXT_COLUMNS, key) != HISTORY_CONTEXT_COLUMNS.end();
        if (!known) {
            ignored += ignored.empty() ? key : ", " + key;
        } else if (value) {
            context.emplace(key, value);
        }
    }
    if (!ignored.empty()) {
        logger.debug("Ignored unknown history fields: {}", ignored);
    }
    if (fields.raw_text && *fields.raw_text != fields.text) {
        // Kept beside raw_text so choosing the original never loses the AI's version.
        context.emplace("cleaned_text", fields.text);
    }

    std::optional<std::string> saved_audio;
    if (fields.source_audio_path && !fields.source_audio_path->empty()) {
        std::error_code error;
        if (std::filesystem::exists(*fields.source_audio_path, error)) {
            saved_audio = save_recording(*fields.source_audio_path);
        }
    }

    NewEntry made;
    made.text = fields.text;
    made.model = fields.model;
    made.audio_file = saved_audio;
    made.transcription_time = fields.transcription_time;
    made.audio_duration = fields.audio_duration;
    made.file_size = fields.file_size;
    made.raw_text = fields.raw_text;
    made.cleanup_provider = fields.cleanup_provider;
    made.cleanup_model = fields.cleanup_model;
    made.source_name = fields.source_name;
    made.context = context;
    const HistoryEntry entry = create_entry(made, clock());
    repository_.add_history_entry(entry, context);

    logger.info("Added history entry: {}...", entry.id.substr(0, 8));
    if (entry.entry_kind == FAILED_RECORDING_KIND) {
        return entry; // Keep recovery audio on the computer that recorded it.
    }
    if (kind_of(entry) == "dictation") {
        // Before record_saved: the move to the host can start right away.
        std::lock_guard lock(newest_mutex_);
        newest_dictation_ = entry;
    }
    record_sync().record_saved("dictation", entry.id);
    return entry;
}

std::optional<HistoryEntry>
HistoryManager::failed_recording_for_path(const std::filesystem::path& audio_path) {
    const std::string filename = utf8(audio_path.filename());
    const auto path = get_recording_path(filename);
    if (path && history_detail::same_normalized_path(*path, audio_path)) {
        return repository_.get_failed_recording(filename);
    }
    return std::nullopt;
}

HistoryEntry HistoryManager::complete_failed_recording(std::string_view entry_id,
                                                       const FailedRecordingRetry& retry) {
    const ContextFields context = history_context(retry.context);
    HistoryColumnValues fields;
    for (const auto& [key, value] : context) {
        if (key != "cleaned_text") {
            fields.emplace_back(key, *value);
        }
    }
    const auto text_or_null = [](const std::optional<std::string>& value) -> sql::Value {
        if (value) {
            return *value;
        }
        return nullptr;
    };
    fields.emplace_back("text", retry.text);
    fields.emplace_back("model", retry.model);
    fields.emplace_back("transcription_time", retry.transcription_time
                                                  ? sql::Value(*retry.transcription_time)
                                                  : sql::Value(nullptr));
    fields.emplace_back("raw_text", text_or_null(retry.raw_text));
    fields.emplace_back("cleanup_provider", text_or_null(retry.cleanup_provider));
    fields.emplace_back("cleanup_model", text_or_null(retry.cleanup_model));
    fields.emplace_back("cleaned_text", retry.raw_text && *retry.raw_text != retry.text
                                            ? sql::Value(retry.text)
                                            : sql::Value(nullptr));
    auto entry = repository_.complete_failed_recording(entry_id, fields);
    if (!entry) {
        throw std::runtime_error("The failed recording entry is no longer in History");
    }
    record_sync().record_saved("dictation", entry->id);
    return *entry;
}

std::optional<std::string>
HistoryManager::save_recording(const std::filesystem::path& source_path) {
    try {
        const py::DateTime now = history_detail::local_now(clock());
        const std::string stem = std::format("recording_{:04}{:02}{:02}_{:02}{:02}{:02}", now.year,
                                             now.month, now.day, now.hour, now.minute, now.second);
        std::string filename;
        std::filesystem::path dest_path;
        std::unique_ptr<history_detail::ExclusiveFile> destination;
        for (int suffix = 1;; ++suffix) {
            filename = suffix == 1 ? stem + ".wav" : std::format("{}-{}.wav", stem, suffix);
            dest_path = recordings_folder_ / path_from_utf8(filename);
            destination = history_detail::ExclusiveFile::create(dest_path);
            if (destination) {
                break;
            }
        }

        try {
            std::ifstream source(source_path, std::ios::binary);
            if (!source) {
                throw std::filesystem::filesystem_error(
                    "Cannot open the recording", source_path,
                    std::make_error_code(std::errc::no_such_file_or_directory));
            }
            std::vector<std::byte> buffer(copy_chunk);
            while (source) {
                source.read(reinterpret_cast<char*>(buffer.data()),
                            static_cast<std::streamsize>(buffer.size()));
                const auto got = static_cast<std::size_t>(source.gcount());
                if (got == 0) {
                    break;
                }
                destination->write(std::span(buffer.data(), got));
            }
            if (source.bad()) {
                throw std::filesystem::filesystem_error("Cannot read the recording", source_path,
                                                        std::make_error_code(std::errc::io_error));
            }
            destination->sync();
            destination->close();
        } catch (...) {
            destination.reset();
            std::error_code ignored;
            std::filesystem::remove(dest_path, ignored);
            throw;
        }
        invalidate_recordings_usage();
        logger.info("Saved recording: {}", filename);

        rotate_recordings();

        return filename;
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to save recording: {}", error.what()));
        return std::nullopt;
    }
}

std::optional<std::string>
HistoryManager::preserve_recording(const std::filesystem::path& source_path) {
    std::error_code error;
    if (source_path.empty() || !std::filesystem::is_regular_file(source_path, error)) {
        return std::nullopt;
    }
    return save_recording(source_path);
}

void HistoryManager::rotate_recordings() {
    const RecordingRetention retention = [&] {
        std::lock_guard lock(retention_mutex_);
        return retention_;
    }();
    if (!retention.max_recordings && !retention.max_bytes) {
        return;
    }
    try {
        const auto expired = recordings_over_limit(retention.max_recordings, retention.max_bytes);
        for (const RecordingInfo& rec : expired) {
            try {
                std::error_code error;
                if (!std::filesystem::remove(rec.file_path, error)) {
                    // os.remove raises for a file already gone as well.
                    throw std::filesystem::filesystem_error(
                        "Cannot remove the recording", rec.file_path,
                        error ? error : std::make_error_code(std::errc::no_such_file_or_directory));
                }
                invalidate_recordings_usage();
                logger.info("Removed old recording: {}", rec.filename);

                repository_.clear_history_audio_file(rec.filename);
            } catch (const std::exception& error) {
                logger.error_with(error, std::format("Failed to remove recording {}: {}",
                                                     rec.filename, error.what()));
            }
        }
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to rotate recordings: {}", error.what()));
    }
}

std::vector<RecordingInfo>
HistoryManager::recordings_over_limit(std::optional<std::int64_t> max_recordings,
                                      std::optional<std::int64_t> max_bytes) {
    return fv::recordings_over_limit(get_recordings(), max_recordings, max_bytes);
}

std::pair<std::int64_t, std::int64_t> HistoryManager::get_recordings_usage() {
    std::lock_guard lock(usage_mutex_);
    // The folder's modification time is the signature: adding or removing a file changes it.
    // (Python adds st_ctime, which on Windows is the creation time and never changes.)
    std::optional<std::filesystem::file_time_type> signature;
    {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(recordings_folder_, error);
        if (!error) {
            signature = modified;
        }
    }
    const auto now = clock().steady();
    if (usage_cache_ && signature == usage_signature_ && now - usage_checked_at_ < usage_ttl) {
        return *usage_cache_;
    }
    std::int64_t count = 0;
    std::int64_t total_bytes = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(recordings_folder_, error), end;
         !error && it != end; it.increment(error)) {
        const std::string name = utf8(it->path().filename());
        if (!name.ends_with(".wav")) {
            continue;
        }
        std::error_code stat_error;
        if (it->is_regular_file(stat_error)) {
            const auto size = std::filesystem::file_size(it->path(), stat_error);
            if (stat_error == std::errc::no_such_file_or_directory) {
                continue; // A concurrent retention pass removed it.
            }
            if (stat_error) {
                throw std::filesystem::filesystem_error("Cannot read the recording", it->path(),
                                                        stat_error);
            }
            ++count;
            total_bytes += static_cast<std::int64_t>(size);
        }
    }
    // A missing folder counts as empty; any other failure to list it reaches the caller (the
    // Settings page reports it), as os.scandir's OSError does, and nothing is cached.
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error("Cannot list the recordings", recordings_folder_,
                                                error);
    }
    usage_cache_ = {count, total_bytes};
    usage_signature_ = signature;
    usage_checked_at_ = now;
    return *usage_cache_;
}

void HistoryManager::invalidate_recordings_usage() {
    std::lock_guard lock(usage_mutex_);
    usage_cache_.reset();
}

std::vector<RecordingInfo> HistoryManager::get_recordings() {
    std::vector<RecordingInfo> recordings;
    try {
        if (!std::filesystem::exists(recordings_folder_)) {
            return recordings;
        }
        for (const auto& item : std::filesystem::directory_iterator(recordings_folder_)) {
            const std::string filename = utf8(item.path().filename());
            if (!filename.ends_with(".wav")) {
                continue;
            }
            const std::filesystem::path file_path = recordings_folder_ / item.path().filename();
            const auto status = std::filesystem::status(file_path);
            if (!std::filesystem::exists(status)) {
                throw std::filesystem::filesystem_error(
                    "Cannot read the recording", file_path,
                    std::make_error_code(std::errc::no_such_file_or_directory));
            }
            const std::int64_t size =
                std::filesystem::is_regular_file(status)
                    ? static_cast<std::int64_t>(std::filesystem::file_size(file_path))
                    : 0;
            std::optional<std::string> timestamp =
                parse_recording_stamp(replace_all(replace_all(filename, "recording_"), ".wav"));
            if (!timestamp) {
                timestamp =
                    modification_stamp(std::filesystem::last_write_time(file_path), clock());
            }
            recordings.push_back({filename, *timestamp, file_path, size});
        }
        // sort(reverse=True) is stable: equal stamps keep the listing order.
        std::ranges::stable_sort(recordings, [](const RecordingInfo& a, const RecordingInfo& b) {
            return a.timestamp > b.timestamp;
        });
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to get recordings: {}", error.what()));
    }
    return recordings;
}

std::vector<HistoryEntry> HistoryManager::get_history(std::optional<std::int64_t> limit) {
    return repository_.get_history_entries(limit);
}

std::vector<HistoryEntry> HistoryManager::search(std::string_view query,
                                                 std::optional<std::int64_t> limit) {
    return repository_.search_history_entries(query, limit);
}

std::optional<HistoryEntry> HistoryManager::get_entry(std::string_view entry_id) {
    return repository_.get_history_entry_by_id(entry_id);
}

std::optional<HistoryEntry> HistoryManager::last_dictation() {
    std::optional<HistoryEntry> newest;
    {
        std::lock_guard lock(newest_mutex_);
        newest = newest_dictation_;
    }
    if (newest) {
        if (auto stored = repository_.get_history_entry_by_id(newest->id)) {
            return stored;
        }
        newest->stored_on = "the host";
        return newest;
    }
    bool move_to_host = false;
    if (services_.dictations_move_to_host) {
        try {
            move_to_host = services_.dictations_move_to_host();
        } catch (const std::exception& error) {
            logger.warning("Could not read where records are kept ({})", error.what());
        }
    }
    if (move_to_host) {
        return std::nullopt;
    }
    for (HistoryEntry& entry :
         repository_.get_history_entries(last_dictation_scan, HistoryOrigin::own())) {
        if (kind_of(entry) == "dictation") {
            return std::move(entry);
        }
    }
    return std::nullopt;
}

void HistoryManager::forget_dictation(std::string_view entry_id) {
    std::lock_guard lock(newest_mutex_);
    if (newest_dictation_ && newest_dictation_->id == entry_id) {
        newest_dictation_.reset();
    }
}

std::optional<HistoryEntry> HistoryManager::use_version(std::string_view entry_id,
                                                        std::string_view version) {
    if (version != ORIGINAL_VERSION && version != AI_VERSION) {
        throw py::ValueError("Unknown history entry version: " + py::repr(version));
    }
    const auto entry = repository_.get_history_entry_by_id(entry_id);
    if (!entry || !is_local_entry(*entry)) {
        return std::nullopt;
    }
    const std::string current = entry_version(*entry);
    if (current.empty()) {
        return std::nullopt;
    }
    if (current == version) {
        return entry;
    }
    HistoryEdits changes;
    if (version == ORIGINAL_VERSION) {
        changes.emplace("text", entry->raw_text);
        if (!entry->cleaned_text) {
            changes.emplace("cleaned_text", entry->text);
        }
    } else {
        changes.emplace("text", entry->cleaned_text);
    }
    auto updated = repository_.update_history_entry(entry_id, changes);
    if (!updated) {
        return std::nullopt;
    }
    logger.info("History entry {}... now shows its {} text", entry_id.substr(0, 8), version);
    record_sync().record_edited("dictation", entry_id);
    return updated;
}

bool HistoryManager::delete_entry(std::string_view entry_id, bool delete_audio_file,
                                  bool kept_on_host) {
    if (!kept_on_host) {
        forget_dictation(entry_id);
    }
    const auto entry =
        delete_audio_file ? repository_.get_history_entry_by_id(entry_id) : std::nullopt;
    const bool result = repository_.delete_history_entry(entry_id);
    if (result) {
        if (!kept_on_host) {
            forget_replayable_events();
        }
        logger.info("Deleted history entry: {}...", entry_id.substr(0, 8));
        if (entry && entry->audio_file && !entry->audio_file->empty()) {
            delete_recording_file(*entry->audio_file);
        }
        record_sync().record_deleted("dictation", entry_id);
    }
    return result;
}

bool HistoryManager::delete_recording_file(std::string_view filename) {
    const auto audio_path = get_recording_path(filename);
    if (!audio_path) {
        repository_.clear_history_audio_file(filename);
        logger.info("Saved recording already absent: {}", filename);
        return true;
    }
    std::error_code error;
    if (!std::filesystem::remove(*audio_path, error)) {
        if (!error) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        logger.error("Failed to delete saved recording {}: {}", filename, error.message());
        return false;
    }
    invalidate_recordings_usage();

    repository_.clear_history_audio_file(filename);
    logger.info("Deleted saved recording: {}", filename);
    return true;
}

void HistoryManager::forget_all_dictations() {
    std::lock_guard lock(newest_mutex_);
    newest_dictation_.reset();
}

void HistoryManager::clear_history() {
    forget_all_dictations();
    repository_.clear_history();
    forget_replayable_events();
    record_sync().cleared("dictation");
    logger.info("History cleared");
}

void HistoryManager::clear_history_and_recordings() {
    for (const RecordingInfo& rec : get_recordings()) {
        std::error_code error;
        if (!std::filesystem::remove(rec.file_path, error)) {
            if (!error) {
                error = std::make_error_code(std::errc::no_such_file_or_directory);
            }
            logger.error("Failed to remove recording {}: {}", rec.filename, error.message());
        }
    }
    forget_all_dictations();
    repository_.clear_history();
    forget_replayable_events();
    record_sync().cleared("dictation");
    logger.info("History and recordings cleared");
}

std::optional<std::filesystem::path> HistoryManager::get_recording_path(std::string_view filename) {
    if (filename.empty()) {
        return std::nullopt;
    }
    std::filesystem::path file_path = recordings_folder_ / path_from_utf8(filename);
    std::error_code error;
    if (std::filesystem::exists(file_path, error)) {
        return file_path;
    }
    return std::nullopt;
}

} // namespace fv
