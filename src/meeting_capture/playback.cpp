#include "meeting_capture/playback.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "meeting_capture/detail/files.h"
#include "meeting_capture/detail/wav_file.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.audio_playback");

std::mutex& meeting_lock(const std::string& meeting_id) {
    // One lock per meeting, kept for the process's life as Python's dict of locks is.
    static std::mutex guard;
    static std::map<std::string, std::unique_ptr<std::mutex>, std::less<>> locks;
    const std::lock_guard lock(guard);
    auto& slot = locks[meeting_id];
    if (!slot) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}

// float(row.get(key) or 0.0).
double number_or_zero(const Json& row, const char* key) {
    if (!row.is_object()) {
        return 0.0;
    }
    const auto found = row.find(key);
    if (found == row.end() || !found->is_number()) {
        return 0.0;
    }
    return found->get<double>();
}

// float(row[key]).
double number_at(const Json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end()) {
        throw py::PyError("KeyError", std::string("'") + key + "'");
    }
    if (!found->is_number()) {
        throw py::TypeError("float() argument must be a string or a real number");
    }
    return found->get<double>();
}

std::string text_or_empty(const Json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || !found->is_string()) {
        return "";
    }
    return found->get<std::string>();
}

// os.path.realpath(os.path.abspath(path)).
std::filesystem::path resolved(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = path.empty() ? std::filesystem::current_path(error)
                                                  : std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    std::filesystem::path real = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        real = absolute.lexically_normal();
    }
    // abspath drops a trailing separator ("C:\\spool\\" is "C:\\spool"), which a path's
    // comparison would otherwise count as an empty last element; a root keeps its own.
    if (!real.has_filename() && real.has_relative_path()) {
        real = real.parent_path();
    }
    return real;
}

std::filesystem::path safe_chunk_path(const std::string& path,
                                      const std::filesystem::path& spool_dir) {
    const std::filesystem::path spool = resolved(spool_dir);
    const std::filesystem::path candidate = resolved(path_from_utf8(path));
    if (candidate.parent_path() != spool) {
        throw py::ValueError("audio chunk path is outside its meeting spool");
    }
    return candidate;
}

// os.stat's st_mtime_ns: nanoseconds since the Unix epoch, leap seconds not counted.
std::int64_t mtime_ns(std::filesystem::file_time_type time) {
#ifdef _WIN32
    // MSVC's file_time_type is the FILETIME: 100 ns ticks since 1601-01-01. Python subtracts the
    // 1601 to 1970 span directly; MSVC's clock_cast goes through utc_clock and would also take
    // off the 27 leap seconds since 1972 (and needs the time zone database to do so).
    constexpr std::int64_t kTicksTo1970 = 116444736000000000LL;
    return (static_cast<std::int64_t>(time.time_since_epoch().count()) - kTicksTo1970) * 100;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::file_clock::to_sys(time).time_since_epoch())
        .count();
#endif
}

// [id, st_size, st_mtime_ns] per chunk, (0, 0) for a file that cannot be read.
Json watermark(const std::vector<Json>& chunks) {
    Json out = Json::array();
    for (const Json& chunk : chunks) {
        const std::filesystem::path path = path_from_utf8(text_or_empty(chunk, "file_path"));
        std::error_code error;
        std::int64_t size = 0;
        std::int64_t modified_ns = 0;
        const auto bytes = std::filesystem::file_size(path, error);
        if (!error) {
            const auto written = std::filesystem::last_write_time(path, error);
            if (!error) {
                size = static_cast<std::int64_t>(bytes);
                modified_ns = mtime_ns(written);
            }
        }
        const auto id = chunk.find("id");
        out.push_back(Json::array({id == chunk.end() ? Json() : *id, size, modified_ns}));
    }
    return out;
}

// The cached playback.json, or nullopt when it cannot be read or parsed (Python passes on
// OSError, ValueError and TypeError).
std::optional<Json> read_cache(const std::filesystem::path& metadata) {
    std::ifstream in(metadata, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        return py::loads(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void write_metadata(const std::filesystem::path& path, const Json& marks) {
    Json record = Json::object();
    record["watermark"] = marks;
    py::DumpOptions options;
    options.item_sep = ",";
    options.key_sep = ":";
    detail::BinaryFile file = detail::BinaryFile::open(path, "wb");
    file.write(py::dumps(record, options));
    file.sync();
    file.close();
}

std::string python_type(const Json& value) {
    if (value.is_array()) {
        return "list";
    }
    if (value.is_string()) {
        return "str";
    }
    if (value.is_boolean()) {
        return "bool";
    }
    if (value.is_number_float()) {
        return "float";
    }
    if (value.is_number()) {
        return "int";
    }
    return "NoneType";
}

std::string id_text(const Json& chunk) {
    const auto id = chunk.find("id");
    if (id == chunk.end() || id->is_null()) {
        return "None";
    }
    return id->dump();
}

} // namespace

void render_mono(const std::vector<Json>& chunks, const std::filesystem::path& output_path) {
    double end_s = 0.0;
    bool first = true;
    for (const Json& chunk : chunks) {
        const double end = number_or_zero(chunk, "start_s") + number_or_zero(chunk, "duration_s");
        end_s = first ? end : std::max(end_s, end);
        first = false;
    }
    if (first) {
        throw py::ValueError("max() arg is an empty sequence");
    }
    const std::int64_t total_samples = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(py::round_half_even(end_s * PLAYBACK_RATE)));
    detail::WavOut target(output_path, PLAYBACK_RATE);
    std::vector<std::int32_t> mixed;
    std::vector<std::int16_t> contributors;
    std::vector<std::int16_t> rendered;
    for (std::int64_t block_start = 0; block_start < total_samples; block_start += BLOCK_SAMPLES) {
        const std::int64_t block_end = std::min(total_samples, block_start + BLOCK_SAMPLES);
        const auto length = static_cast<std::size_t>(block_end - block_start);
        mixed.assign(length, 0);
        contributors.assign(length, 0);
        for (const Json& chunk : chunks) {
            const auto chunk_start = static_cast<std::int64_t>(
                py::round_half_even(number_at(chunk, "start_s") * PLAYBACK_RATE));
            const auto chunk_frames = static_cast<std::int64_t>(
                py::round_half_even(number_at(chunk, "duration_s") * PLAYBACK_RATE));
            const std::int64_t chunk_end = chunk_start + chunk_frames;
            const std::int64_t overlap_start = std::max(block_start, chunk_start);
            const std::int64_t overlap_end = std::min(block_end, chunk_end);
            if (overlap_end <= overlap_start) {
                continue;
            }
            detail::WavReader source(path_from_utf8(text_or_empty(chunk, "file_path")));
            if (source.channels() != 1 || source.sample_width() != 2 ||
                source.rate() != PLAYBACK_RATE) {
                logger.warning("Skipping incompatible meeting chunk {}", id_text(chunk));
                continue;
            }
            const std::vector<std::byte> raw =
                source.read_frames(static_cast<std::uint64_t>(overlap_start - chunk_start),
                                   static_cast<std::uint64_t>(overlap_end - overlap_start));
            const std::size_t available = raw.size() / 2;
            const std::size_t size =
                std::min(available, static_cast<std::size_t>(overlap_end - overlap_start));
            if (size == 0) {
                continue;
            }
            const auto destination = static_cast<std::size_t>(overlap_start - block_start);
            for (std::size_t i = 0; i < size; ++i) {
                std::int16_t sample = 0;
                std::memcpy(&sample, raw.data() + 2 * i, 2); // little-endian, as "<i2"
                mixed[destination + i] += sample;
                contributors[destination + i] =
                    static_cast<std::int16_t>(contributors[destination + i] + 1);
            }
        }
        rendered.resize(length);
        for (std::size_t i = 0; i < length; ++i) {
            std::int32_t value = mixed[i];
            if (contributors[i] > 1) {
                // numpy's //= floors.
                const std::int32_t count = contributors[i];
                std::int32_t quotient = value / count;
                if (value % count != 0 && value < 0) {
                    --quotient;
                }
                value = quotient;
            }
            rendered[i] = static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
        }
        target.write(std::span<const std::int16_t>(rendered));
    }
    target.close();
}

std::filesystem::path build_playback(meeting_state::MeetingRepository& repository,
                                     const std::string& meeting_id) {
    const std::optional<Json> meeting = repository.get_meeting(meeting_id);
    if (!meeting) {
        throw py::ValueError("unknown meeting");
    }
    const std::filesystem::path spool_dir = path_from_utf8(text_or_empty(*meeting, "spool_dir"));
    std::error_code error;
    if (spool_dir.empty() || !std::filesystem::is_directory(spool_dir, error)) {
        throw py::PyError("FileNotFoundError", "meeting audio is unavailable");
    }
    std::vector<Json> chunks = repository.get_audio_chunks(meeting_id);
    if (chunks.empty()) {
        throw py::PyError("FileNotFoundError", "meeting audio is unavailable");
    }
    for (Json& chunk : chunks) {
        chunk["file_path"] =
            path_to_utf8(safe_chunk_path(text_or_empty(chunk, "file_path"), spool_dir));
    }

    const std::filesystem::path output = spool_dir / "playback.wav";
    const std::filesystem::path metadata = spool_dir / "playback.json";
    const Json marks = watermark(chunks);
    const std::lock_guard lock(meeting_lock(meeting_id));
    if (const std::optional<Json> cached = read_cache(metadata)) {
        if (!cached->is_object()) {
            // cached.get(...) on a list or a number: Python's AttributeError is not caught.
            throw py::PyError("AttributeError",
                              "'" + python_type(*cached) + "' object has no attribute 'get'");
        }
        const auto stored = cached->find("watermark");
        if (stored != cached->end() && py::py_equal(*stored, marks) &&
            std::filesystem::is_regular_file(output, error)) {
            return output;
        }
    }

    std::filesystem::path temp_output = output;
    temp_output += ".tmp";
    std::filesystem::path temp_metadata = metadata;
    temp_metadata += ".tmp";
    struct RemoveTemporaries {
        const std::filesystem::path& output;
        const std::filesystem::path& metadata;
        ~RemoveTemporaries() {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
            std::filesystem::remove(metadata, ignored);
        }
    } cleanup{temp_output, temp_metadata};
    render_mono(chunks, temp_output);
    write_metadata(temp_metadata, marks);
    detail::replace_file(temp_output, output);
    detail::replace_file(temp_metadata, metadata);
    return output;
}

} // namespace fv::meeting
