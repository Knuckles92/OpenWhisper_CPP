// History playback through a dedicated output stream, with a fake device
// (tests/test_personalize_s9_audio_player.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/player.h"
#include "audio/testing/fake_backend.h"
#include "audio/wav_writer.h"
#include "testkit/testkit.h"

namespace {

using fv::audio::Player;
using fv::audio::testing::FakeBackend;
using namespace std::chrono_literals;

bool wait_for(const std::function<bool()>& until, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (until()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return until();
}

struct WavSpec {
    int rate = 44100;
    int channels = 1;
    int width = 2;
    double seconds = 0.25;
    int value = 1200;
};

// _wav(): samples arange(frames * channels) % 2000 + value, written by Python's wave writer.
std::filesystem::path wav(const std::filesystem::path& path, WavSpec spec = {}) {
    const auto frames = static_cast<std::size_t>(spec.rate * spec.seconds);
    fv::audio::WavWriter writer(
        path, {.rate = spec.rate, .channels = spec.channels, .sample_width = spec.width});
    if (spec.width == 2) {
        std::vector<std::int16_t> samples(frames * static_cast<std::size_t>(spec.channels));
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::int16_t>(static_cast<int>(i % 2000) + spec.value);
        }
        writer.write(std::span<const std::int16_t>(samples));
    } else {
        const std::vector<std::byte> zeros(frames * static_cast<std::size_t>(spec.channels));
        writer.write(std::span<const std::byte>(zeros));
    }
    writer.commit(std::nullopt);
    return path;
}

std::vector<std::int16_t> first_samples(const std::filesystem::path& path, std::size_t count) {
    return fv::audio::read_recording_samples(path, static_cast<std::int64_t>(count));
}

// The fixture of the Python file: a player on a fake device, stopped at the end.
struct PlayerFixture {
    fv::testkit::TempDataRoot root;
    std::shared_ptr<FakeBackend> device = std::make_shared<FakeBackend>();
    std::unique_ptr<Player> player = std::make_unique<Player>(device);
    ~PlayerFixture() { player->stop(); }
};

class Flags {
public:
    void add(bool value = true) {
        const std::lock_guard lock(mutex_);
        values_.push_back(value);
    }
    std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return values_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<bool> values_;
};

} // namespace

// from tests/test_personalize_s9_audio_player.py::test_only_the_recorders_own_wavs_play
TEST_CASE("only the recorders own wavs play", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    const auto text = f.root.path() / "notes.wav";
    std::ofstream(text) << "not audio";
    const std::vector<std::filesystem::path> refused{
        wav(f.root.path() / "16k.wav", {.rate = 16000}),
        wav(f.root.path() / "stereo.wav", {.channels = 2}),
        wav(f.root.path() / "8bit.wav", {.width = 1}),
        wav(f.root.path() / "empty.wav", {.seconds = 0}),
        text,
        f.root.path() / "missing.wav",
    };
    for (const auto& path : refused) {
        INFO(path.string());
        CHECK_FALSE(f.player->play(path));
        CHECK_FALSE(f.player->last_error().empty());
        CHECK_FALSE(f.player->is_playing());
    }
    CHECK(f.device->streams().empty());
}

// from
// tests/test_personalize_s9_audio_player.py::test_play_streams_the_samples_and_stop_ends_it_once
TEST_CASE("play streams the samples and stop ends it once", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    const auto path = wav(f.root.path() / "take.wav");
    Flags finished;

    REQUIRE(f.player->play(path, [&] { finished.add(); }));
    CHECK(f.player->is_playing());
    CHECK(f.player->path() == path);
    CHECK(f.player->session() == 1);
    CHECK(f.player->duration() == Catch::Approx(0.25));
    REQUIRE(wait_for(
        [&] { return !f.device->streams().empty() && f.device->streams()[0]->started(); }));
    const auto stream = f.device->streams()[0];
    CHECK(stream->config().rate == 44100);
    CHECK(stream->config().channels == 1);

    CHECK(stream->pull(1000));
    CHECK(stream->last() == first_samples(path, 1000));
    CHECK(f.player->position() == Catch::Approx(1000.0 / 44100));

    f.player->stop();
    CHECK(stream->aborted());
    CHECK(stream->closed());
    CHECK_FALSE(f.player->is_playing());
    CHECK(f.player->session() == 0);
    CHECK(f.player->position() == 0.0);
    CHECK(finished.size() == 1);
    f.player->stop();
    CHECK(finished.size() == 1);
}

// from
// tests/test_personalize_s9_audio_player.py::test_a_recording_played_to_the_end_finishes_and_closes
TEST_CASE("a recording played to the end finishes and closes",
          "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    const auto path = wav(f.root.path() / "take.wav", {.seconds = 0.05});
    Flags finished;
    REQUIRE(f.player->play(path, [&] { finished.add(); }));
    REQUIRE(wait_for(
        [&] { return !f.device->streams().empty() && f.device->streams()[0]->started(); }));
    const auto stream = f.device->streams()[0];

    while (stream->pull(1024)) {
    }

    const std::size_t total = static_cast<std::size_t>(44100 * 0.05);
    const auto last = stream->last();
    CHECK(std::all_of(last.begin() + static_cast<std::ptrdiff_t>(total % 1024), last.end(),
                      [](std::int16_t s) { return s == 0; }));
    CHECK(finished.size() == 1);
    CHECK_FALSE(f.player->is_playing());
    CHECK(wait_for([&] { return stream->closed(); }));
    CHECK_FALSE(stream->aborted());
}

// from tests/test_personalize_s9_audio_player.py::test_playing_another_recording_replaces_the_first
TEST_CASE("playing another recording replaces the first", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    Flags first;
    Flags second;
    REQUIRE(f.player->play(wav(f.root.path() / "a.wav"), [&] { first.add(); }));
    REQUIRE(wait_for(
        [&] { return !f.device->streams().empty() && f.device->streams()[0]->started(); }));
    REQUIRE(f.player->play(wav(f.root.path() / "b.wav"), [&] { second.add(); }));

    CHECK(first.size() == 1);
    CHECK(second.size() == 0);
    CHECK(f.device->streams()[0]->closed());
    CHECK(f.player->session() == 2);
    CHECK(wait_for([&] {
        const auto streams = f.device->streams();
        return streams.size() == 2 && streams[1]->started();
    }));
    // The second play's on_finished writes to `second`, which goes before the fixture does.
    f.player->stop();
    CHECK(second.size() == 1);
}

// from tests/test_personalize_s9_audio_player.py::test_a_device_that_wont_open_ends_the_playback
TEST_CASE("a device that wont open ends the playback", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    f.device->fail_playback_open("no output device");
    Flags finished;
    REQUIRE(f.player->play(wav(f.root.path() / "take.wav"), [&] { finished.add(); }));
    CHECK(wait_for([&] { return finished.size() == 1; }));
    CHECK_FALSE(f.player->is_playing());
    CHECK(f.player->last_error().find("no output device") != std::string::npos);
}

// from tests/test_personalize_s9_audio_player.py::test_a_device_that_wont_start_is_closed
TEST_CASE("a device that wont start is closed", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    f.device->fail_playback_start("device busy");
    Flags finished;
    REQUIRE(f.player->play(wav(f.root.path() / "take.wav"), [&] { finished.add(); }));
    CHECK(wait_for([&] { return finished.size() == 1; }));
    REQUIRE(f.device->streams().size() == 1);
    CHECK(f.device->streams()[0]->closed());
    CHECK_FALSE(f.player->is_playing());
}

// from
// tests/test_personalize_s9_audio_player.py::test_stopping_while_the_file_loads_opens_no_device
TEST_CASE("stopping while the file loads opens no device", "[test_personalize_s9_audio_player]") {
    fv::testkit::TempDataRoot root;
    auto device = std::make_shared<FakeBackend>();
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    // monkeypatch.setattr(module, "_read_samples", slow_read)
    const auto slow_read = [&](const std::filesystem::path& path, std::int64_t frames) {
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, 3s, [&] { return released; });
        }
        return fv::audio::read_recording_samples(path, frames);
    };
    {
        Player player(device, slow_read);
        Flags finished;
        REQUIRE(player.play(wav(root.path() / "take.wav"), [&] { finished.add(); }));
        player.stop();
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
        std::this_thread::sleep_for(100ms);
        CHECK(finished.size() == 1);
        CHECK(device->streams().empty());
    }
}

// from tests/test_personalize_s9_audio_player.py::test_a_raising_finished_callback_is_contained
TEST_CASE("a raising finished callback is contained", "[test_personalize_s9_audio_player]") {
    PlayerFixture f;
    REQUIRE(f.player->play(wav(f.root.path() / "take.wav"),
                           [] { throw std::runtime_error("division by zero"); }));
    REQUIRE(wait_for(
        [&] { return !f.device->streams().empty() && f.device->streams()[0]->started(); }));
    f.player->stop();
    CHECK_FALSE(f.player->is_playing());
}

// from tests/test_personalize_s9_audio_player.py::test_stop_playback_never_creates_a_player
TEST_CASE("stop playback never creates a player", "[test_personalize_s9_audio_player]") {
    fv::testkit::TempDataRoot root;
    const auto previous = fv::audio::exchange_player(nullptr);
    fv::audio::stop_playback();
    CHECK(fv::audio::exchange_player(nullptr) == nullptr);

    const auto shared = std::make_shared<Player>(std::make_shared<FakeBackend>());
    fv::audio::exchange_player(shared);
    CHECK(fv::audio::player() == shared);
    CHECK(shared->play(wav(root.path() / "take.wav")));
    std::thread stopper(&fv::audio::stop_playback);
    stopper.join();
    CHECK_FALSE(shared->is_playing());
    fv::audio::exchange_player(previous);
}

TEST_CASE("recording frames names why a file is refused") {
    fv::testkit::TempDataRoot root;
    std::ofstream(root.path() / "notes.wav") << "not audio";
    const auto reason = [](const std::filesystem::path& path) {
        try {
            fv::audio::recording_frames(path);
        } catch (const fv::audio::UnsupportedAudio& error) {
            return std::string(error.what());
        }
        return std::string("accepted");
    };
    CHECK(reason(root.path() / "notes.wav") == "not a WAV recording");
    CHECK(reason(wav(root.path() / "16k.wav", {.rate = 16000})) == "not in the recorder's format");
    CHECK(reason(wav(root.path() / "empty.wav", {.seconds = 0})) == "the recording is empty");
    CHECK(fv::audio::recording_frames(wav(root.path() / "take.wav")) == 11025);
    CHECK_THROWS_AS(fv::audio::recording_frames(root.path() / "missing.wav"), std::system_error);
}
