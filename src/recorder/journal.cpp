#include "recorder/journal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/wav_writer.h"
#include "base/ids.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "recorder/detail/platform.h"

namespace fv {

const char* const STORAGE_CANNOT_KEEP_UP = "Recording storage cannot keep up; recording stopped.";

namespace {

const log::Logger kLog("services.recording_journal");

using recorder_detail::OsFile;

// Sample formats a recovered session may claim; anything else is not ours.
constexpr std::int64_t kMinRecoveryRate = 8000;
constexpr std::int64_t kMaxRecoveryRate = 384000;

constexpr const char* kSessionFiles[] = {"audio.pcm", "session.json", "active.lock", "discarded"};

// Another process (or this one, through another handle) holds the session's lease: Python's
// BlockingIOError / PermissionError from msvcrt.locking and fcntl.flock.
class LeaseBusy : public std::runtime_error {
public:
    LeaseBusy() : std::runtime_error("The recording session is in use") {}
};

// _Lease: an OS lock, released even after a hard kill; no stale PID heuristics.
OsFile take_lease(const std::filesystem::path& path) {
    OsFile file = OsFile::open_append(path);
    if (file.size() == 0) {
        static constexpr std::byte kZero[] = {std::byte{'0'}};
        file.write_all(kZero);
    }
    file.seek(0);
    if (!file.try_lock()) {
        throw LeaseBusy();
    }
    return file;
}

bool is_code(const std::error_code& code, std::errc condition) {
    return code == std::make_error_condition(condition);
}

// _remove_session: deletes only the fixed names, never following user paths, then the folder.
void remove_session(const std::filesystem::path& directory) {
    for (const char* name : kSessionFiles) {
        std::error_code error;
        std::filesystem::remove(directory / name, error);
        if (error && !is_code(error, std::errc::no_such_file_or_directory)) {
            throw std::filesystem::filesystem_error("Could not remove a journal file",
                                                    directory / name, error);
        }
    }
    std::error_code error;
    if (!std::filesystem::remove(directory, error)) {
        throw std::filesystem::filesystem_error(
            "Could not remove the journal folder", directory,
            error ? error : std::make_error_code(std::errc::no_such_file_or_directory));
    }
}

// The real audio.pcm: unbuffered, as open(path, "x+b", buffering=0).
class OsJournalFile final : public JournalFile {
public:
    explicit OsJournalFile(OsFile file) : file_(std::move(file)) {}
    std::size_t write(std::span<const std::byte> data) override { return file_.write(data); }
    void sync() override { file_.sync(); }
    void close() override { file_.close(); }

private:
    OsFile file_;
};

std::string read_text(const std::filesystem::path& path) {
    OsFile file = OsFile::open_read(path);
    std::string text(static_cast<std::size_t>(file.size()), '\0');
    const std::size_t got = file.read(std::as_writable_bytes(std::span(text)));
    text.resize(got);
    return text;
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>(value >> 8));
}

void put_tag(std::vector<std::byte>& out, std::string_view tag) {
    for (const char c : tag) {
        out.push_back(static_cast<std::byte>(c));
    }
}

// The header Python's wave module leaves on a finished file of `data_bytes`.
std::vector<std::byte> wave_header(const SessionFormat& format, std::uint64_t data_bytes) {
    const auto length = static_cast<std::uint32_t>(data_bytes);
    std::vector<std::byte> bytes;
    put_tag(bytes, "RIFF");
    put_u32(bytes, 36 + length);
    put_tag(bytes, "WAVE");
    put_tag(bytes, "fmt ");
    put_u32(bytes, 16);
    put_u16(bytes, 1); // WAVE_FORMAT_PCM
    put_u16(bytes, static_cast<std::uint16_t>(format.channels));
    put_u32(bytes, static_cast<std::uint32_t>(format.rate));
    put_u32(bytes, static_cast<std::uint32_t>(format.channels * format.rate * format.sample_width));
    put_u16(bytes, static_cast<std::uint16_t>(format.channels * format.sample_width));
    put_u16(bytes, static_cast<std::uint16_t>(format.sample_width * 8));
    put_tag(bytes, "data");
    put_u32(bytes, length);
    return bytes;
}

// _copy_pcm: `size` bytes in bounded blocks; a short source fails with `short_message`.
template <class Sink>
void copy_pcm(OsFile& source, std::uint64_t size, const char* short_message, Sink&& sink) {
    std::vector<std::byte> block(std::min<std::uint64_t>(JOURNAL_COPY_BYTES, size));
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), remaining));
        const std::size_t got = source.read(std::span(block).first(want));
        if (got == 0) {
            throw std::runtime_error(short_message);
        }
        sink(std::span<const std::byte>(block).first(got));
        remaining -= got;
    }
}

void write_recovered_wav(const std::filesystem::path& temporary,
                         const std::filesystem::path& source, std::uint64_t size,
                         const SessionFormat& format) {
    OsFile destination = OsFile::create_truncate(temporary);
    destination.write_all(wave_header(format, size));
    OsFile input = OsFile::open_read(source);
    copy_pcm(input, size, "Incomplete recovery audio",
             [&](std::span<const std::byte> block) { destination.write_all(block); });
    destination.sync();
    destination.close();
}

const Json& key(const Json& meta, const char* name) {
    if (!meta.is_object()) {
        throw py::TypeError("Recovery metadata is not a mapping");
    }
    const auto found = meta.find(name);
    if (found == meta.end()) {
        throw std::out_of_range(std::string("Recovery metadata has no ") + name);
    }
    return *found;
}

// `value in choices` for a JSON number: Python's == lets True stand for 1. A float that equals
// a choice passes Python's check but then fails the copy (read() refuses a float size), which
// leaves the session in place exactly as this refusal does.
std::optional<int> small_choice(const Json& value, int low, int high) {
    std::int64_t number = 0;
    if (value.is_boolean()) {
        number = value.get<bool>() ? 1 : 0;
    } else if (value.is_number_integer()) {
        number = value.get<std::int64_t>();
    } else {
        return std::nullopt;
    }
    if (number < low || number > high) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

SessionFormat session_format_of(const Json& meta) {
    const Json& rate = key(meta, "rate");
    const Json& channels = key(meta, "channels");
    const Json& width = key(meta, "sample_width");
    const bool rate_ok = rate.is_number_integer() && !rate.is_boolean() &&
                         rate.get<std::int64_t>() >= kMinRecoveryRate &&
                         rate.get<std::int64_t>() <= kMaxRecoveryRate;
    const std::optional<int> channel_count = small_choice(channels, 1, 2);
    const std::optional<int> sample_width = small_choice(width, 1, 4);
    if (!rate_ok || !channel_count || !sample_width) {
        throw py::ValueError("Invalid recovery audio format");
    }
    return {static_cast<int>(rate.get<std::int64_t>()), *channel_count, *sample_width};
}

std::chrono::local_time<std::chrono::microseconds> local_now(const Clock& clock) {
    const auto utc = std::chrono::floor<std::chrono::microseconds>(clock.utc());
    return clock.zone()->to_local(utc);
}

} // namespace

namespace recorder_detail {

struct JournalState {
    std::filesystem::path directory;
    std::filesystem::path path;
    int frame_bytes = 0;
    std::function<void(const std::string&)> on_error;
    JournalOptions options;

    OsFile lease;
    std::unique_ptr<JournalFile> file;

    mutable std::mutex mutex; // the queue and every flag below
    mutable std::condition_variable changed;
    std::deque<std::vector<std::byte>> queue;
    std::size_t unfinished = 0; // queued or being written: queue.join() waits for zero
    bool stop = false;
    bool discard = false;
    bool done = false;
    bool abandoned = false;
    std::optional<std::string> error;

    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> dropped{0};

    std::mutex release_mutex;
    bool released = false;

    void run();
    std::chrono::steady_clock::time_point
    write_block(std::span<const std::byte> payload,
                std::chrono::steady_clock::time_point checkpoint);
    void report_error();
    bool finish(std::chrono::milliseconds timeout);
    void release_resources(bool remove);
};

void JournalState::run() {
    set_thread_name("fv-journal-writer");
    auto checkpoint = std::chrono::steady_clock::now();
    try {
        for (;;) {
            std::vector<std::byte> payload;
            {
                std::unique_lock lock(mutex);
                if (discard) {
                    break;
                }
                if (queue.empty()) {
                    if (stop) {
                        break; // after a stop nothing more is appended: what was queued is written
                    }
                    changed.wait_for(lock, options.poll,
                                     [this] { return !queue.empty() || stop || discard; });
                    continue;
                }
                payload = std::move(queue.front());
                queue.pop_front();
            }
            struct TaskDone {
                JournalState* state;
                ~TaskDone() {
                    {
                        const std::lock_guard lock(state->mutex);
                        --state->unfinished;
                    }
                    state->changed.notify_all();
                }
            } task_done{this};
            checkpoint = write_block(payload, checkpoint);
        }
        bool discarded = false;
        {
            const std::lock_guard lock(mutex);
            discarded = discard;
        }
        if (!discarded) {
            file->sync();
        }
    } catch (const std::exception& exc) {
        bool discarded = false;
        {
            const std::lock_guard lock(mutex);
            discarded = discard;
            error = std::string("Recording could not be saved: ") + exc.what();
            stop = true;
        }
        if (!discarded) {
            kLog.warning("Recording journal writer failed ({})", exc.what());
        }
    }
    try {
        report_error();
    } catch (...) { // NOLINT(bugprone-empty-catch): report_error logs what it can
    }
    bool remove = false;
    bool release = false;
    {
        const std::lock_guard lock(mutex);
        done = true;
        remove = discard;
        release = discard || abandoned;
    }
    changed.notify_all();
    if (release) {
        try {
            release_resources(remove);
        } catch (const std::exception& exc) {
            kLog.warning("Could not release the recording journal ({})", exc.what());
        }
    }
}

std::chrono::steady_clock::time_point
JournalState::write_block(std::span<const std::byte> payload,
                          std::chrono::steady_clock::time_point checkpoint) {
    const std::size_t taken = file->write(payload);
    written.fetch_add(taken);
    if (taken != payload.size()) {
        throw std::runtime_error("Incomplete recording write");
    }
    if (std::chrono::steady_clock::now() - checkpoint >= options.checkpoint) {
        file->sync();
        return std::chrono::steady_clock::now();
    }
    return checkpoint;
}

void JournalState::report_error() {
    std::optional<std::string> message;
    {
        const std::lock_guard lock(mutex);
        if (!error || discard) {
            return;
        }
        message = error;
    }
    try {
        if (on_error) {
            on_error(*message);
        }
    } catch (const std::exception& exc) {
        kLog.error_with(exc, "Could not report recording failure");
    }
}

bool JournalState::finish(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    stop = true;
    changed.notify_all();
    return changed.wait_for(lock, timeout, [this] { return done; });
}

void JournalState::release_resources(bool remove) {
    const std::lock_guard lock(release_mutex);
    if (released) {
        return;
    }
    file->close();
    lease.close();
    released = true;
    // The actual file's size: a failed or short write may still have saved a recoverable prefix.
    std::error_code missing;
    const auto size = std::filesystem::file_size(path, missing);
    if (remove || (!missing && size == 0)) {
        remove_session(directory);
    }
}

} // namespace recorder_detail

std::filesystem::path recovery_directory(const std::filesystem::path& output_file) {
    const std::filesystem::path output =
        std::filesystem::weakly_canonical(std::filesystem::absolute(output_file));
    std::filesystem::path name = ".";
    name += output.stem();
    name += "-recovery";
    return output.parent_path() / name;
}

std::string session_json(int rate, int channels, int sample_width,
                         std::chrono::local_time<std::chrono::microseconds> created) {
    const auto day = std::chrono::floor<std::chrono::days>(created);
    const std::chrono::year_month_day date(std::chrono::sys_days(day.time_since_epoch()));
    const std::chrono::hh_mm_ss time(created - day);
    const py::DateTime stamp{.year = static_cast<int>(date.year()),
                             .month = static_cast<int>(static_cast<unsigned>(date.month())),
                             .day = static_cast<int>(static_cast<unsigned>(date.day())),
                             .hour = static_cast<int>(time.hours().count()),
                             .minute = static_cast<int>(time.minutes().count()),
                             .second = static_cast<int>(time.seconds().count()),
                             .microsecond = static_cast<int>(time.subseconds().count())};
    Json metadata = Json::object();
    metadata["version"] = 1;
    metadata["rate"] = rate;
    metadata["channels"] = channels;
    metadata["sample_width"] = sample_width;
    metadata["created"] = stamp.isoformat();
    return py::dumps(metadata);
}

SessionFormat session_format(const std::string& session_json_text) {
    return session_format_of(py::loads(session_json_text));
}

std::filesystem::path recovered_wav_path(const std::string& created,
                                         const std::filesystem::path& recordings_folder,
                                         const std::filesystem::path& session_directory) {
    const py::DateTime when = py::fromisoformat(created);
    const std::string stamp = std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", when.year, when.month,
                                          when.day, when.hour, when.minute, when.second);
    std::filesystem::create_directories(recordings_folder);
    return recordings_folder / path_from_utf8("recording_" + stamp + "-recovered-" +
                                              path_to_utf8(session_directory.filename()) + ".wav");
}

RecordingJournal::RecordingJournal(const std::filesystem::path& output_file, int rate, int channels,
                                   int sample_width,
                                   std::function<void(const std::string&)> on_error,
                                   JournalOptions options)
    : state_(std::make_shared<recorder_detail::JournalState>()) {
    auto& state = *state_;
    state.directory = recovery_directory(output_file) / uuid4_hex();
    state.path = state.directory / "audio.pcm";
    state.frame_bytes = channels * sample_width;
    state.on_error = std::move(on_error);
    state.options = std::move(options);

    std::filesystem::create_directories(state.directory.parent_path());
    if (!std::filesystem::create_directory(state.directory)) {
        throw std::filesystem::filesystem_error("The journal folder already exists",
                                                state.directory,
                                                std::make_error_code(std::errc::file_exists));
    }
#ifndef _WIN32
    std::filesystem::permissions(state.directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
#endif
    state.lease = take_lease(state.directory / "active.lock");
    try {
        const Clock& clock = state.options.clock ? *state.options.clock : system_clock();
        const std::string metadata = session_json(rate, channels, sample_width, local_now(clock));
        OsFile session = OsFile::create_new(state.directory / "session.json");
        session.write_all(std::as_bytes(std::span(metadata)));
        session.sync();
        session.close();
        std::unique_ptr<JournalFile> file =
            std::make_unique<OsJournalFile>(OsFile::create_new(state.path));
        if (state.options.wrap_file) {
            file = state.options.wrap_file(std::move(file));
        }
        state.file = std::move(file);
    } catch (...) {
        state.lease.close();
        throw;
    }
    std::thread([state = state_] { state->run(); }).detach();
}

RecordingJournal::~RecordingJournal() {
    bool already_done = false;
    {
        const std::lock_guard lock(state_->mutex);
        state_->abandoned = true;
        state_->stop = true;
        already_done = state_->done;
    }
    state_->changed.notify_all();
    if (already_done) {
        try {
            state_->release_resources(false);
        } catch (const std::exception& exc) {
            kLog.warning("Could not release the recording journal ({})", exc.what());
        }
    }
}

bool RecordingJournal::append(std::span<const std::byte> payload) {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stop || state_->error) {
            return false;
        }
        if (state_->queue.size() >= state_->options.queue_blocks) {
            state_->dropped.fetch_add(payload.size() /
                                      static_cast<std::size_t>(state_->frame_bytes));
            // The writer reports it, never whoever appends.
            state_->error = STORAGE_CANNOT_KEEP_UP;
            state_->stop = true;
        } else {
            state_->queue.emplace_back(payload.begin(), payload.end());
            ++state_->unfinished;
            state_->changed.notify_all();
            return true;
        }
    }
    state_->changed.notify_all();
    return false;
}

bool RecordingJournal::finish() {
    return state_->finish(state_->options.finish_timeout);
}

bool RecordingJournal::finish(std::chrono::milliseconds timeout) {
    return state_->finish(timeout);
}

std::optional<std::string> RecordingJournal::read_from(std::int64_t offset) const {
    const std::uint64_t size = state_->written.load();
    if (offset < 0 || static_cast<std::uint64_t>(offset) > size) {
        return std::nullopt;
    }
    OsFile file = OsFile::open_read(state_->path);
    file.seek(static_cast<std::uint64_t>(offset));
    std::string bytes(static_cast<std::size_t>(size - static_cast<std::uint64_t>(offset)), '\0');
    bytes.resize(file.read(std::as_writable_bytes(std::span(bytes))));
    return bytes;
}

void RecordingJournal::copy_into(audio::WavWriter& wav) {
    // Stop always drains before a save: never snapshot a writer still changing the file.
    if (!finish()) {
        throw std::runtime_error("Recording storage did not finish; audio kept for recovery");
    }
    const auto frame = static_cast<std::uint64_t>(state_->frame_bytes);
    const std::uint64_t size = state_->written.load() / frame * frame;
    OsFile source = OsFile::open_read(state_->path);
    copy_pcm(source, size, "Recording ended before the saved audio was read",
             [&](std::span<const std::byte> block) { wav.write(block); });
}

bool RecordingJournal::close(bool discard) {
    bool discarding = false;
    {
        const std::lock_guard release_lock(state_->release_mutex);
        if (discard && !state_->released) {
            {
                const std::lock_guard lock(state_->mutex);
                state_->discard = true;
            }
            state_->changed.notify_all();
            // Recovery must honour the cancel even if a disk write outlives the timeout or the
            // app is killed before the writer can clean up.
            try {
                OsFile marker = OsFile::open_append(state_->directory / "discarded");
                marker.sync();
                marker.close();
            } catch (const std::system_error& exc) {
                // Full or broken storage must not prevent an in-process cancel; the writer's
                // deferred cleanup still removes the captured PCM.
                kLog.warning("Could not checkpoint recording cancellation ({})", exc.what());
            }
        }
    }
    if (!finish()) {
        return false; // keep the lease while the disk operation is in flight
    }
    {
        const std::lock_guard lock(state_->mutex);
        discarding = state_->discard;
    }
    state_->release_resources(discarding);
    return true;
}

void RecordingJournal::request_discard() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->discard = true;
    }
    state_->changed.notify_all();
}

void RecordingJournal::report_error(const std::string& message, bool stop) {
    {
        const std::lock_guard lock(state_->mutex);
        state_->error = message;
        if (stop) {
            state_->stop = true;
        }
    }
    state_->changed.notify_all();
}

bool RecordingJournal::wait_until_written(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout, [this] { return state_->unfinished == 0; });
}

std::optional<std::string> RecordingJournal::error() const {
    const std::lock_guard lock(state_->mutex);
    return state_->error;
}

std::uint64_t RecordingJournal::written_bytes() const {
    return state_->written.load();
}

std::uint64_t RecordingJournal::dropped_frames() const {
    return state_->dropped.load();
}

int RecordingJournal::frame_bytes() const {
    return state_->frame_bytes;
}

const std::filesystem::path& RecordingJournal::directory() const {
    return state_->directory;
}

const std::filesystem::path& RecordingJournal::path() const {
    return state_->path;
}

namespace {

void recover_session(const std::filesystem::path& directory,
                     const std::filesystem::path& recordings_folder,
                     std::vector<std::filesystem::path>& recovered) {
    OsFile lease;
    std::optional<std::filesystem::path> temporary;
    try {
        try {
            lease = take_lease(directory / "active.lock");
            if (std::filesystem::exists(directory / "discarded")) {
                lease.close();
                remove_session(directory);
                return;
            }
            const Json meta = py::loads(read_text(directory / "session.json"));
            const SessionFormat format = session_format_of(meta);
            const auto frame = static_cast<std::uint64_t>(format.channels * format.sample_width);
            const std::filesystem::path source = directory / "audio.pcm";
            const std::uint64_t size = std::filesystem::file_size(source) / frame * frame;
            if (size > 0) {
                const Json& created = key(meta, "created");
                if (!created.is_string()) {
                    throw py::TypeError("fromisoformat: argument must be str");
                }
                const std::filesystem::path target =
                    recovered_wav_path(created.get<std::string>(), recordings_folder, directory);
                temporary = target;
                temporary->replace_extension(".wav.tmp");
                if (!std::filesystem::exists(target)) {
                    write_recovered_wav(*temporary, source, size, format);
                    recorder_detail::replace_file(*temporary, target);
                }
                recovered.push_back(target);
            }
            lease.close();
            remove_session(directory);
        } catch (const LeaseBusy&) {
            // An active session, including another process's.
        } catch (const std::system_error& error) {
            if (is_code(error.code(), std::errc::permission_denied)) {
                // Python's PermissionError: an active session.
            } else if (is_code(error.code(), std::errc::no_such_file_or_directory)) {
                // The app died after making the folder and before writing into it: nothing was
                // recorded, and leaving it would warn at every start.
                lease = OsFile();
                try {
                    remove_session(directory);
                } catch (const std::exception& exc) {
                    kLog.debug("Could not remove an empty dictation journal ({})", exc.what());
                }
            } else {
                throw;
            }
        }
    } catch (const std::exception& exc) {
        kLog.warning("Could not recover interrupted dictation ({})", exc.what());
    }
    lease = OsFile();
    if (temporary) {
        std::error_code ignored;
        std::filesystem::remove(*temporary, ignored);
    }
}

} // namespace

std::vector<std::filesystem::path>
recover_recordings(const std::filesystem::path& output_file,
                   const std::filesystem::path& recordings_folder) {
    const std::filesystem::path root = recovery_directory(output_file);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return {};
    }
    // Listed first: recovering removes entries.
    std::vector<std::filesystem::path> sessions;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        std::error_code entry_error;
        if (entry.is_symlink(entry_error) || !entry.is_directory(entry_error)) {
            continue;
        }
        sessions.push_back(entry.path());
    }
    std::vector<std::filesystem::path> recovered;
    for (const auto& directory : sessions) {
        recover_session(directory, recordings_folder, recovered);
    }
    return recovered;
}

} // namespace fv
