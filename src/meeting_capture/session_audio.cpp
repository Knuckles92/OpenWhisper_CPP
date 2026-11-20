#include "meeting_capture/session_audio.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "meeting_capture/detail/files.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/spool.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.capture.spool");
constexpr log::Logger offline_logger("meeting.asr.offline");

std::filesystem::path with_suffix(const std::filesystem::path& path, std::string_view suffix) {
    std::filesystem::path out = path;
    out += std::filesystem::path(std::string(suffix));
    return out;
}

std::filesystem::path spool_file(const std::filesystem::path& spool_dir, std::string_view channel,
                                 std::string_view suffix) {
    return spool_dir / path_from_utf8(std::string(channel) + std::string(suffix));
}

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

// `value or fallback`.
const Json& or_value(const Json& value, const Json& fallback) {
    return truthy(value) ? value : fallback;
}

const Json& get(const Json& object, std::string_view key) {
    static const Json null_value;
    if (!object.is_object()) {
        return null_value;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? null_value : *found;
}

// str(value) of a JSON scalar; containers come out as JSON text, which Python spells nearly so.
std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    return py::dumps(value);
}

// int(value) and float(value), raising what Python raises.
std::int64_t to_int(const Json& value) {
    const auto converted = py::py_int_checked(value);
    if (converted) {
        return *converted;
    }
    switch (converted.error()) {
    case py::IntError::Type:
        throw py::TypeError(
            "int() argument must be a string, a bytes-like object or a real number");
    case py::IntError::Overflow:
        throw py::OverflowError("cannot convert float infinity to integer");
    default:
        throw py::ValueError("invalid literal for int()");
    }
}

double to_float(const Json& value) {
    const auto converted = py::py_float(value);
    if (converted) {
        return *converted;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float");
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

std::string compact_json(const Json& value) {
    py::DumpOptions options;
    options.item_sep = ",";
    options.key_sep = ":";
    return py::dumps(value, options);
}

// open(tmp, "w"), json.dump, flush, fsync, os.replace.
void write_json_atomic(const std::filesystem::path& path, const Json& payload) {
    const std::filesystem::path tmp = with_suffix(path, ".tmp");
    {
        detail::BinaryFile file = detail::BinaryFile::open(tmp, "wb");
        file.write(compact_json(payload));
        file.sync();
        file.close();
    }
    detail::replace_file(tmp, path);
}

std::optional<Json> load_json_object(const std::filesystem::path& path) {
    std::string text;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    try {
        Json data = py::loads(text);
        if (!data.is_object()) {
            return std::nullopt;
        }
        return data;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void make_parent(const std::filesystem::path& path) {
    const std::filesystem::path parent = std::filesystem::absolute(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

// np.frombuffer(raw, "<i2"), which refuses an odd number of bytes.
std::vector<std::int16_t> as_samples(std::span<const std::byte> raw) {
    if (raw.size() % 2 != 0) {
        throw py::ValueError("buffer size must be a multiple of element size");
    }
    std::vector<std::int16_t> samples(raw.size() / 2);
    if (!raw.empty()) {
        // memcpy with the null data() of an empty vector is undefined even for zero bytes.
        std::memcpy(samples.data(), raw.data(), raw.size());
    }
    return samples;
}

bool holds_audio(const std::filesystem::path& wav_path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(wav_path, error)) {
        return false;
    }
    const auto size = std::filesystem::file_size(wav_path, error);
    return !error && size > 44;
}

} // namespace

std::filesystem::path session_pcm_path(const std::filesystem::path& spool_dir,
                                       std::string_view channel) {
    return spool_file(spool_dir, channel, "_session.pcm");
}

std::filesystem::path session_16k_pcm_path(const std::filesystem::path& spool_dir,
                                           std::string_view channel) {
    return spool_file(spool_dir, channel, "_session.16k.pcm");
}

std::filesystem::path session_meta_path(const std::filesystem::path& spool_dir,
                                        std::string_view channel) {
    return spool_file(spool_dir, channel, "_session.json");
}

std::filesystem::path session_wav_path(const std::filesystem::path& spool_dir,
                                       std::string_view channel) {
    return spool_file(spool_dir, channel, "_session.wav");
}

std::filesystem::path chunk_recovery_meta_path(const std::filesystem::path& file_path) {
    std::string canonical = path_to_utf8(file_path);
    constexpr std::string_view orphan = ".orphan";
    if (canonical.ends_with(orphan)) {
        canonical.resize(canonical.size() - orphan.size());
    }
    return path_from_utf8(canonical + ".recovery.json");
}

void write_chunk_recovery_meta(const std::filesystem::path& file_path, std::string_view meeting_id,
                               std::string_view channel, std::int64_t seq, double start_s,
                               double duration_s, int sample_rate) {
    Json payload = Json::object();
    payload["version"] = 1;
    payload["meeting_id"] = std::string(meeting_id);
    payload["channel"] = std::string(channel);
    payload["seq"] = seq;
    payload["start_s"] = start_s;
    payload["duration_s"] = duration_s;
    payload["sample_rate"] = sample_rate;
    write_json_atomic(chunk_recovery_meta_path(file_path), payload);
}

std::optional<ChunkRecoveryMeta> load_chunk_recovery_meta(const std::filesystem::path& file_path) {
    const std::optional<Json> data = load_json_object(chunk_recovery_meta_path(file_path));
    if (!data) {
        return std::nullopt;
    }
    static const Json empty_text = "";
    try {
        if (!data->contains("seq") || !data->contains("start_s") || !data->contains("duration_s")) {
            return std::nullopt; // KeyError
        }
        ChunkRecoveryMeta meta;
        meta.meeting_id = py_str(or_value(get(*data, "meeting_id"), empty_text));
        meta.channel = py_str(or_value(get(*data, "channel"), empty_text));
        meta.seq = to_int(get(*data, "seq"));
        meta.start_s = to_float(get(*data, "start_s"));
        meta.duration_s = to_float(get(*data, "duration_s"));
        static const Json default_rate = TARGET_RATE;
        meta.sample_rate =
            static_cast<int>(to_int(or_value(get(*data, "sample_rate"), default_rate)));
        return meta;
    } catch (const py::PyError& error) {
        if (error.type_name() == "OverflowError") {
            throw;
        }
        return std::nullopt;
    }
}

void remove_chunk_recovery_meta(const std::filesystem::path& file_path) {
    std::error_code error;
    const std::filesystem::path sidecar = chunk_recovery_meta_path(file_path);
    if (!std::filesystem::remove(sidecar, error) && error &&
        error != std::errc::no_such_file_or_directory) {
        logger.debug("Could not remove recovered chunk sidecar for {}", path_to_utf8(file_path));
    }
}

void write_session_meta(const std::filesystem::path& path, int sample_rate,
                        std::int64_t sample_count, double origin_s,
                        std::optional<double> pcm_origin_s, std::int64_t prefix_16k_samples,
                        bool pcm_active) {
    Json payload = Json::object();
    payload["sample_rate"] = sample_rate;
    payload["sample_count"] = sample_count;
    payload["origin_s"] = origin_s;
    // The native PCM is overwritten after a rate change while the 16 kHz file keeps the converted
    // prefix; these fields place the current native file on its own after a hard kill.
    payload["pcm_origin_s"] = pcm_origin_s.value_or(origin_s);
    payload["prefix_16k_samples"] = std::max<std::int64_t>(0, prefix_16k_samples);
    payload["pcm_active"] = pcm_active;
    write_json_atomic(path, payload);
}

std::optional<SessionMeta> load_session_meta(const std::filesystem::path& path) {
    const std::optional<Json> data = load_json_object(path);
    if (!data) {
        return std::nullopt;
    }
    static const Json zero = 0;
    static const Json zero_float = 0.0;
    try {
        SessionMeta meta;
        meta.sample_rate = static_cast<int>(to_int(or_value(get(*data, "sample_rate"), zero)));
        meta.sample_count = to_int(or_value(get(*data, "sample_count"), zero));
        meta.origin_s = to_float(or_value(get(*data, "origin_s"), zero_float));
        if (data->contains("pcm_origin_s")) {
            meta.pcm_origin_s = to_float(get(*data, "pcm_origin_s"));
        }
        if (data->contains("prefix_16k_samples")) {
            meta.prefix_16k_samples =
                std::max<std::int64_t>(0, to_int(get(*data, "prefix_16k_samples")));
        }
        if (data->contains("pcm_active")) {
            meta.pcm_active = truthy(get(*data, "pcm_active"));
        }
        return meta;
    } catch (const py::PyError& error) {
        if (error.type_name() == "OverflowError") {
            throw;
        }
        return std::nullopt;
    }
}

std::int64_t resample_pcm_file_to_16k(const std::filesystem::path& src_pcm, int src_rate,
                                      const std::filesystem::path& dest_pcm, bool append,
                                      double window_s) {
    const int rate = std::max(1, src_rate);
    const auto window_n =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(py::round_half_even(window_s * rate)));
    std::int64_t written = 0;
    const char* mode = append && std::filesystem::is_regular_file(dest_pcm) ? "ab" : "wb";
    make_parent(dest_pcm);
    detail::BinaryFile src = detail::BinaryFile::open(src_pcm, "rb");
    detail::BinaryFile dest = detail::BinaryFile::open(dest_pcm, mode);
    std::vector<std::byte> raw(static_cast<std::size_t>(window_n) * 2);
    while (true) {
        const std::size_t got = src.read(raw);
        if (got == 0) {
            break;
        }
        const std::vector<std::int16_t> frames = as_samples(std::span(raw.data(), got));
        if (frames.empty()) {
            break;
        }
        const std::vector<std::int16_t> out = resample_to_16k(frames, rate);
        dest.write(std::as_bytes(std::span(out)));
        written += static_cast<std::int64_t>(out.size());
    }
    dest.sync();
    dest.close();
    return written;
}

std::int64_t pcm_slice_to_wav(const std::filesystem::path& src_pcm, int src_rate,
                              std::int64_t start_sample, std::int64_t end_sample,
                              const std::filesystem::path& wav_path, double window_s) {
    const int rate = std::max(1, src_rate);
    const std::int64_t start = std::max<std::int64_t>(0, start_sample);
    const std::int64_t end = std::max(start, end_sample);
    std::int64_t remaining = end - start;
    if (remaining <= 0) {
        return 0;
    }
    const auto window_n =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(py::round_half_even(window_s * rate)));
    const std::filesystem::path tmp = with_suffix(wav_path, ".tmp");
    make_parent(wav_path);
    std::int64_t written = 0;
    try {
        {
            detail::BinaryFile src = detail::BinaryFile::open(src_pcm, "rb");
            detail::WavOut wav(tmp, TARGET_RATE);
            src.seek(static_cast<std::uint64_t>(start) * 2);
            std::vector<std::byte> raw;
            while (remaining > 0) {
                raw.resize(static_cast<std::size_t>(std::min(remaining, window_n)) * 2);
                const std::size_t got = src.read(raw);
                const std::vector<std::int16_t> frames = as_samples(std::span(raw.data(), got));
                if (frames.empty()) {
                    break;
                }
                const std::vector<std::int16_t> out = resample_to_16k(frames, rate);
                wav.write(std::span<const std::int16_t>(out));
                written += static_cast<std::int64_t>(out.size());
                remaining -= static_cast<std::int64_t>(frames.size());
            }
            wav.close();
        }
        {
            // Windows' _commit rejects a read-only descriptor where POSIX fsync accepts one.
            detail::BinaryFile handle = detail::BinaryFile::open(tmp, "r+b");
            handle.sync();
            handle.close();
        }
        detail::replace_file(tmp, wav_path);
        return written;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}

void pcm16k_to_wav(const std::filesystem::path& pcm_path, const std::filesystem::path& wav_path) {
    const std::filesystem::path tmp = with_suffix(wav_path, ".tmp");
    make_parent(wav_path);
    {
        detail::WavOut wav(tmp, TARGET_RATE);
        detail::BinaryFile src = detail::BinaryFile::open(pcm_path, "rb");
        std::vector<std::byte> raw(static_cast<std::size_t>(TARGET_RATE) * 2 * 10);
        while (true) {
            const std::size_t got = src.read(raw);
            if (got == 0) {
                break;
            }
            wav.write(std::span<const std::byte>(raw.data(), got));
        }
        wav.close();
    }
    detail::replace_file(tmp, wav_path);
}

std::optional<double> concat_channel_chunks_to_wav(const std::vector<Json>& chunks,
                                                   std::string_view channel,
                                                   const std::filesystem::path& output_path) {
    static const Json empty_text = "";
    static const Json zero = 0;
    static const Json zero_float = 0.0;
    struct Selected {
        double start_s;
        std::int64_t seq;
        const Json* row;
    };
    std::vector<Selected> selected;
    for (const Json& chunk : chunks) {
        if (py_str(or_value(get(chunk, "channel"), empty_text)) == channel &&
            truthy(get(chunk, "file_path"))) {
            selected.push_back({to_float(or_value(get(chunk, "start_s"), zero_float)),
                                to_int(or_value(get(chunk, "seq"), zero)), &chunk});
        }
    }
    std::stable_sort(selected.begin(), selected.end(), [](const Selected& a, const Selected& b) {
        return a.start_s != b.start_s ? a.start_s < b.start_s : a.seq < b.seq;
    });
    if (selected.empty()) {
        return std::nullopt;
    }
    const double origin_s = selected.front().start_s;
    const std::filesystem::path tmp = with_suffix(output_path, ".tmp");
    make_parent(output_path);
    double cursor_s = origin_s;
    {
        detail::WavOut wav(tmp, TARGET_RATE);
        for (const Selected& chunk : selected) {
            const double gap_s = chunk.start_s - cursor_s;
            if (gap_s > 1.0 / TARGET_RATE) {
                const std::vector<std::int16_t> zeros(
                    static_cast<std::size_t>(py::round_half_even(gap_s * TARGET_RATE)));
                wav.write(std::span<const std::int16_t>(zeros));
                cursor_s += static_cast<double>(zeros.size()) / TARGET_RATE;
            }
            const std::string id = py_str(get(*chunk.row, "id"));
            detail::WavContents source;
            try {
                source = detail::read_wav(path_from_utf8(py_str(get(*chunk.row, "file_path"))));
            } catch (const std::system_error& error) {
                logger.error_with(error,
                                  std::format("Failed to load chunk {} for session fallback", id));
                continue;
            } catch (const detail::WaveError& error) {
                logger.error_with(error,
                                  std::format("Failed to load chunk {} for session fallback", id));
                continue;
            }
            if (source.channels != 1 || source.sample_width != 2 || source.rate != TARGET_RATE) {
                logger.warning("Skipping incompatible session-fallback chunk {}", id);
                continue;
            }
            wav.write(std::span<const std::byte>(source.frames));
            cursor_s += static_cast<double>(source.frames.size() / 2) / TARGET_RATE;
        }
        wav.close();
    }
    detail::replace_file(tmp, output_path);
    return origin_s;
}

std::optional<std::filesystem::path> resolve_session_wav(const std::filesystem::path& spool_dir,
                                                         std::string_view channel,
                                                         const std::vector<Json>& chunks) {
    const std::filesystem::path wav_path = session_wav_path(spool_dir, channel);
    if (holds_audio(wav_path)) {
        return wav_path;
    }
    if (chunks.empty()) {
        return std::nullopt;
    }
    std::optional<double> origin;
    try {
        origin = concat_channel_chunks_to_wav(chunks, channel, wav_path);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Session-WAV chunk fallback failed for {}", channel));
        return std::nullopt;
    }
    if (!origin) {
        return std::nullopt;
    }
    if (holds_audio(wav_path)) {
        return wav_path;
    }
    return std::nullopt;
}

double session_origin_s(const std::filesystem::path& spool_dir, std::string_view channel) {
    const std::optional<SessionMeta> meta =
        load_session_meta(session_meta_path(spool_dir, channel));
    return meta ? meta->origin_s : 0.0;
}

std::tuple<std::optional<std::vector<std::int16_t>>, int, double>
load_channel_session(const std::filesystem::path& spool_dir, std::string_view channel,
                     const std::vector<Json>& chunks) {
    const std::optional<std::filesystem::path> wav_path =
        resolve_session_wav(spool_dir, channel, chunks);
    if (!wav_path) {
        return {std::nullopt, TARGET_RATE, 0.0};
    }
    std::pair<std::vector<std::int16_t>, int> loaded;
    try {
        loaded = detail::load_wav_int16(*wav_path);
    } catch (const std::exception& error) {
        offline_logger.error_with(
            error, std::format("Failed to load session WAV {}", path_to_utf8(*wav_path)));
        return {std::nullopt, TARGET_RATE, 0.0};
    }
    double origin = session_origin_s(spool_dir, channel);
    if (origin == 0.0 && !chunks.empty()) {
        static const Json empty_text = "";
        static const Json zero_float = 0.0;
        std::optional<double> earliest;
        for (const Json& chunk : chunks) {
            if (py_str(or_value(get(chunk, "channel"), empty_text)) != channel) {
                continue;
            }
            const double start = to_float(or_value(get(chunk, "start_s"), zero_float));
            if (!earliest || start < *earliest) {
                earliest = start;
            }
        }
        if (earliest) {
            origin = *earliest;
        }
    }
    return {std::move(loaded.first), loaded.second, origin};
}

} // namespace fv::meeting
