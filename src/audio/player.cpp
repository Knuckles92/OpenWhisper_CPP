#include "audio/player.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "audio/detail/portaudio_platform.h"
#include "base/log.h"
#include "base/utf8.h"

namespace fv::audio {

namespace {

const log::Logger kLog("services.audio_player");

constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM, as wave.py compares the extensible sub-format.
constexpr std::array<unsigned char, 16> kSubtypePcm = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// What wave.open(path, "rb") learns from the header.
struct WavHeader {
    int channels = 0;
    int rate = 0;
    int sample_width = 0;
    std::int64_t frames = 0;
    std::uint64_t data_offset = 0;
};

// wave.Error and EOFError: the file is not a WAV wave.py reads.
struct NotWav {};

std::uint32_t u32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t u16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::ifstream open_for_reading(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        throw std::system_error(std::make_error_code(std::errc::permission_denied),
                                "Permission denied: '" + path_to_utf8(path) + "'");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "No such file or directory: '" + path_to_utf8(path) + "'");
    }
    return in;
}

// wave.Wave_read.initfp with chunk.Chunk: the RIFF/WAVE header, fmt and the data chunk's size.
WavHeader read_header(std::istream& in) {
    const auto read = [&](unsigned char* into, std::size_t count) {
        in.read(reinterpret_cast<char*>(into), static_cast<std::streamsize>(count));
        return static_cast<std::size_t>(in.gcount()) == count;
    };
    unsigned char riff[12];
    if (!read(riff, 8) || std::memcmp(riff, "RIFF", 4) != 0) {
        throw NotWav{};
    }
    if (!read(riff + 8, 4) || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        throw NotWav{};
    }
    WavHeader header;
    bool have_format = false;
    std::uint64_t position = 12;
    for (;;) {
        unsigned char chunk[8];
        if (!read(chunk, 8)) {
            break;
        }
        position += 8;
        const std::uint32_t size = u32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<unsigned char> body(std::min<std::uint32_t>(size, 64));
            if (!read(body.data(), body.size()) || body.size() < 16) {
                throw NotWav{};
            }
            const std::uint16_t tag = u16(body.data());
            header.channels = u16(body.data() + 2);
            header.rate = static_cast<int>(u32(body.data() + 4));
            if (tag != kWaveFormatPcm && tag != kWaveFormatExtensible) {
                throw NotWav{}; // "unknown format"
            }
            header.sample_width = (u16(body.data() + 14) + 7) / 8;
            if (tag == kWaveFormatExtensible &&
                (body.size() < 40 ||
                 std::memcmp(body.data() + 24, kSubtypePcm.data(), kSubtypePcm.size()) != 0)) {
                throw NotWav{};
            }
            if (header.sample_width == 0 || header.channels == 0) {
                throw NotWav{}; // "bad sample width", "bad # of channels"
            }
            have_format = true;
            in.seekg(static_cast<std::streamoff>(position + size + (size & 1u)));
            position += size + (size & 1u);
            continue;
        }
        if (std::memcmp(chunk, "data", 4) == 0) {
            if (!have_format) {
                throw NotWav{}; // "data chunk before fmt chunk"
            }
            header.frames =
                static_cast<std::int64_t>(size) / (header.channels * header.sample_width);
            header.data_offset = position;
            return header;
        }
        position += size + (size & 1u);
        in.seekg(static_cast<std::streamoff>(position));
        if (!in) {
            break;
        }
    }
    throw NotWav{}; // "fmt chunk and/or data chunk missing"
}

} // namespace

std::vector<std::int16_t> read_recording_samples(const std::filesystem::path& path,
                                                 std::int64_t frames) {
    std::ifstream in = open_for_reading(path);
    WavHeader header;
    try {
        header = read_header(in);
    } catch (const NotWav&) {
        throw UnsupportedAudio("not a WAV recording");
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(header.data_offset));
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames));
    in.read(reinterpret_cast<char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)));
    // readframes returns what the file still holds.
    samples.resize(static_cast<std::size_t>(in.gcount()) / sizeof(std::int16_t));
    return samples;
}

std::int64_t recording_frames(const std::filesystem::path& path) {
    std::ifstream in = open_for_reading(path);
    WavHeader header;
    try {
        header = read_header(in);
    } catch (const NotWav&) {
        throw UnsupportedAudio("not a WAV recording");
    }
    if (header.channels != kRecorderChannels || header.sample_width != 2 ||
        header.rate != kRecorderRate) {
        throw UnsupportedAudio("not in the recorder's format");
    }
    if (header.frames <= 0) {
        throw UnsupportedAudio("the recording is empty");
    }
    return header.frames;
}

namespace detail {

struct PlayerImpl;

// One play() call, from loading its file to its end.
struct Playback final : PlaybackSource, std::enable_shared_from_this<Playback> {
    std::uint64_t session = 0;
    std::filesystem::path path;
    std::int64_t frames = 0;
    std::function<void()> on_finished;
    std::vector<std::int16_t> samples;
    // Frames handed to the device so far; written only by its callback.
    std::atomic<std::int64_t> position{0};
    std::unique_ptr<Stream> stream; // guarded by PlayerImpl::stream_mutex
    bool done = false;              // guarded by PlayerImpl::mutex
    std::weak_ptr<PlayerImpl> owner;

    // PortAudio's callback: the next block, then silence and a stop.
    std::size_t read(std::span<std::int16_t> out) override {
        const std::int64_t start = position.load(std::memory_order_relaxed);
        const auto available = static_cast<std::int64_t>(samples.size()) - start;
        const auto count = static_cast<std::size_t>(
            std::clamp<std::int64_t>(available, 0, static_cast<std::int64_t>(out.size())));
        std::copy_n(samples.data() + start, count, out.data());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(count), out.end(), std::int16_t{0});
        position.store(start + static_cast<std::int64_t>(count), std::memory_order_relaxed);
        return count;
    }

    void finished() override;
};

struct PlayerImpl : std::enable_shared_from_this<PlayerImpl> {
    PlayerImpl(std::shared_ptr<Backend> device, SampleLoader loader)
        : backend(std::move(device)), load_samples(std::move(loader)) {}

    std::shared_ptr<Backend> backend;
    SampleLoader load_samples;
    mutable std::mutex mutex;
    // Separate from mutex: PortAudio calls the finished callback, which takes mutex, while
    // abort() runs under this one.
    std::mutex stream_mutex;
    std::shared_ptr<Playback> current;
    std::uint64_t sessions = 0;
    std::string last_error;

    void set_error(std::string text) {
        const std::lock_guard lock(mutex);
        last_error = std::move(text);
    }

    bool is_current(const std::shared_ptr<Playback>& playback) const {
        const std::lock_guard lock(mutex);
        return current == playback;
    }

    void finish(const std::shared_ptr<Playback>& playback) {
        {
            const std::lock_guard lock(mutex);
            if (playback->done) {
                return;
            }
            playback->done = true;
            if (current == playback) {
                current.reset();
            }
        }
        if (playback->on_finished) {
            try {
                playback->on_finished();
            } catch (...) {
                kLog.warning("A playback finished callback raised");
            }
        }
    }

    void start(const std::shared_ptr<Playback>& playback) {
        try {
            playback->samples = load_samples(playback->path, playback->frames);
        } catch (const std::exception& error) {
            set_error(error.what());
            kLog.warning("History playback couldn't start: {}", std::string(error.what()));
            finish(playback);
            return;
        }
        if (!is_current(playback)) {
            return;
        }
        bool failed = false;
        {
            const std::lock_guard lock(stream_mutex);
            if (!is_current(playback)) {
                return;
            }
            StreamConfig config{.device_index = -1};
            config.rate = kRecorderRate;
            config.channels = kRecorderChannels;
            auto opened = backend->open_playback(config, *playback);
            if (opened) {
                playback->stream = std::move(*opened);
            } else {
                set_error(opened.error());
                kLog.warning("History playback couldn't start: {}", opened.error());
                failed = true;
            }
        }
        if (failed) {
            finish(playback);
        }
    }

    void release(const std::shared_ptr<Playback>& playback) {
        const std::lock_guard lock(stream_mutex);
        std::unique_ptr<Stream> stream = std::move(playback->stream);
        stream.reset();
    }

    void stream_finished(const std::shared_ptr<Playback>& playback) {
        finish(playback);
        // The stream cannot be closed from inside its own finished callback.
        std::thread([self = shared_from_this(), playback] {
            name_this_thread("history-playback-close");
            self->release(playback);
        }).detach();
    }

    void stop() {
        std::shared_ptr<Playback> playback;
        {
            const std::lock_guard lock(mutex);
            playback = std::exchange(current, nullptr);
        }
        if (!playback) {
            return;
        }
        {
            const std::lock_guard lock(stream_mutex);
            std::unique_ptr<Stream> stream = std::move(playback->stream);
            if (stream) {
                try {
                    stream->abort();
                } catch (const std::exception&) {
                    kLog.debug("Could not abort history playback");
                }
                stream.reset();
            }
        }
        finish(playback);
    }
};

void Playback::finished() {
    if (const std::shared_ptr<PlayerImpl> player = owner.lock()) {
        player->stream_finished(shared_from_this());
    }
}

} // namespace detail

Player::Player(std::shared_ptr<Backend> backend)
    : Player(std::move(backend), &read_recording_samples) {}

Player::Player(std::shared_ptr<Backend> backend, SampleLoader load_samples)
    : impl_(std::make_shared<detail::PlayerImpl>(std::move(backend), std::move(load_samples))) {}

Player::~Player() {
    impl_->stop();
}

bool Player::play(const std::filesystem::path& path, std::function<void()> on_finished) {
    std::int64_t frames = 0;
    try {
        frames = recording_frames(path);
    } catch (const std::exception& error) {
        std::string reason = error.what();
        if (reason.empty()) {
            reason = "OSError";
        }
        impl_->set_error(reason);
        kLog.info("A history recording can't be played: {}", reason);
        return false;
    }
    stop();
    auto playback = std::make_shared<detail::Playback>();
    {
        const std::lock_guard lock(impl_->mutex);
        playback->session = ++impl_->sessions;
        playback->path = path;
        playback->frames = frames;
        playback->on_finished = std::move(on_finished);
        playback->owner = impl_;
        impl_->current = playback;
        impl_->last_error.clear();
    }
    std::thread([impl = impl_, playback] {
        detail::name_this_thread("history-playback");
        impl->start(playback);
    }).detach();
    return true;
}

void Player::stop() {
    impl_->stop();
}

bool Player::is_playing() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->current != nullptr;
}

std::uint64_t Player::session() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->current ? impl_->current->session : 0;
}

std::filesystem::path Player::path() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->current ? impl_->current->path : std::filesystem::path();
}

double Player::position() const {
    std::shared_ptr<detail::Playback> current;
    {
        const std::lock_guard lock(impl_->mutex);
        current = impl_->current;
    }
    return current ? static_cast<double>(current->position.load()) / kRecorderRate : 0.0;
}

double Player::duration() const {
    std::shared_ptr<detail::Playback> current;
    {
        const std::lock_guard lock(impl_->mutex);
        current = impl_->current;
    }
    return current ? static_cast<double>(current->frames) / kRecorderRate : 0.0;
}

std::string Player::last_error() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->last_error;
}

namespace {

std::mutex& player_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<Player>& player_slot() {
    static std::shared_ptr<Player> slot;
    return slot;
}

} // namespace

std::shared_ptr<Player> player() {
    const std::lock_guard lock(player_mutex());
    if (!player_slot()) {
        player_slot() = std::make_shared<Player>(make_portaudio_backend());
    }
    return player_slot();
}

void stop_playback() {
    std::shared_ptr<Player> current;
    {
        const std::lock_guard lock(player_mutex());
        current = player_slot();
    }
    if (current) {
        current->stop();
    }
}

std::shared_ptr<Player> exchange_player(std::shared_ptr<Player> replacement) {
    const std::lock_guard lock(player_mutex());
    return std::exchange(player_slot(), std::move(replacement));
}

} // namespace fv::audio
