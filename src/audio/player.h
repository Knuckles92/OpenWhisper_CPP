#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/backend.h"

// Playback of saved dictation audio from History (services/audio_player.py). Plays only the
// recorder's own WAV (mono 16-bit PCM at 44100 Hz): every recording History keeps is one, so
// there is no decoder here. Uses its own output stream.
namespace fv::audio {

namespace detail {
struct PlayerImpl;
}

inline constexpr int kRecorderRate = 44100;
inline constexpr int kRecorderChannels = 1;

// The file is not a recording this player plays (Python's UnsupportedAudio, a ValueError).
// what() is "not a WAV recording", "not in the recorder's format" or "the recording is empty".
class UnsupportedAudio : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// How many frames the recording at `path` holds; reads only its header. Throws
// UnsupportedAudio, or std::system_error when the file cannot be read.
std::int64_t recording_frames(const std::filesystem::path& path);
// The first `frames` samples of the recording (wave's readframes: fewer when the file holds
// fewer). Throws as recording_frames does.
std::vector<std::int16_t> read_recording_samples(const std::filesystem::path& path,
                                                 std::int64_t frames);

// How a Player loads a recording's samples on its loading thread; read_recording_samples unless
// a test slows it down.
using SampleLoader =
    std::function<std::vector<std::int16_t>(const std::filesystem::path&, std::int64_t frames)>;

// Plays one file at a time. on_finished runs once per play(), however it ends (played out,
// stopped, replaced, or failed to start), on whichever thread ended it, so it must not touch
// widgets. The samples load and the device opens on a "history-playback" thread; the stream is
// closed on a "history-playback-close" thread, since it cannot be closed from its own finished
// callback.
// Affinity: thread-safe.
class Player {
public:
    explicit Player(std::shared_ptr<Backend> backend);
    Player(std::shared_ptr<Backend> backend, SampleLoader load_samples);
    ~Player(); // stops playback
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Starts playing `path`; false when playback could not start (last_error() says why).
    bool play(const std::filesystem::path& path, std::function<void()> on_finished = {});
    void stop();
    bool is_playing() const;            // from play() until the recording ends or is stopped
    std::uint64_t session() const;      // which play() call is current, 0 when nothing plays
    std::filesystem::path path() const; // the recording playing now, or empty
    double position() const;            // seconds played of the current recording, or 0
    double duration() const;            // seconds in the current recording, or 0
    std::string last_error() const;

private:
    std::shared_ptr<detail::PlayerImpl> impl_;
};

// The process-wide player on make_portaudio_backend(), created on first use.
std::shared_ptr<Player> player();
// Stops any playback without creating a player; safe from any thread.
void stop_playback();
// Replaces the process-wide player (null: none yet) and returns the previous one. For tests.
std::shared_ptr<Player> exchange_player(std::shared_ptr<Player> replacement);

} // namespace fv::audio
