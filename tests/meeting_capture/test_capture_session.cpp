// MeetingCapture over fake sources (meeting/capture_session.py): start and its notes, the system
// audio policy, the block router, failover, the watchdog's restarts, default-device following,
// status writes and the audio integrity error. The engine-level capture tests of
// test_meeting_engine.py run over this class with the engine (M7.17).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/capture_session.h"
#include "meeting_capture/testing/fake_capture_source.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::audio::LoopbackEndpoint;
using fv::audio::testing::FakeBackend;
using fv::meeting::CaptureOptions;
using fv::meeting::MeetingCapture;
using fv::meeting::test::LogCapture;
using fv::meeting::test::TempDir;
using fv::meeting::testing::FakeCaptureSource;

namespace {

class RecordingSink final : public fv::meeting::CaptureStatusSink {
public:
    Json capture_state() override {
        const std::lock_guard lock(mutex);
        return has_store ? state : Json();
    }
    bool update_capture(const Json& capture) override {
        const std::lock_guard lock(mutex);
        state = capture;
        ++updates;
        return true;
    }
    void emit_status(std::string note) override {
        const std::lock_guard lock(mutex);
        notes.push_back(std::move(note));
    }
    void emit_error(std::string code, std::string message) override {
        const std::lock_guard lock(mutex);
        errors.emplace_back(std::move(code), std::move(message));
    }
    Json current() {
        const std::lock_guard lock(mutex);
        return state;
    }

    std::mutex mutex;
    bool has_store = true;
    Json state = Json::object();
    int updates = 0;
    std::vector<std::string> notes;
    std::vector<std::pair<std::string, std::string>> errors;
};

LoopbackEndpoint endpoint(std::string id, bool is_default = true) {
    return LoopbackEndpoint{.id = id, .name = "Speakers " + id, .is_default = is_default};
}

struct Rig {
    explicit Rig(CaptureOptions options = {}, int mics = 1, bool loopback = true) {
        fv::meeting::test::set_steady(fake, 100.0);
        clock.start();
        std::vector<fv::audio::testing::FakeDevice> devices;
        for (int i = 0; i < mics; ++i) {
            devices.push_back({.name = "Mic " + std::to_string(i),
                               .hostapi = 0,
                               .max_input_channels = 1,
                               .default_samplerate = 16000});
        }
        backend->set_layout({{.name = "MME", .default_input_device = mics > 0 ? 0 : -1}},
                            std::move(devices), mics > 0 ? 0 : -1);
        if (loopback) {
            backend->set_loopbacks({endpoint("{a}")});
        }
        capture = std::make_unique<MeetingCapture>(
            std::move(options), clock, repository, backend, sink,
            [this] { return recording.load(); },
            [this](const fv::meeting_state::SpooledChunk& chunk) {
                const std::lock_guard lock(mutex);
                chunks.push_back(chunk);
            },
            [this](const fv::meeting::CaptureBlock& block) {
                ++previews;
                if (preview_throws.load()) {
                    throw std::runtime_error("preview failed on " + std::string(block.channel));
                }
            });
        fv::meeting::SourceFactory factory;
        factory.mic = [this](const fv::audio::InputDevice& device) {
            auto source = std::make_shared<FakeCaptureSource>("mic", device.index, true);
            configure(*source);
            const std::lock_guard lock(mutex);
            mic_sources.push_back(source);
            return source;
        };
        factory.loopback = [this](const LoopbackEndpoint& found) {
            auto source = std::make_shared<FakeCaptureSource>("loopback", found.id, true);
            configure(*source);
            if (found.channels == fail_loopback_channels) {
                source->fail_start("unsupported format");
            }
            const std::lock_guard lock(mutex);
            loopback_channels.push_back(found.channels);
            loopback_sources.push_back(source);
            return source;
        };
        capture->set_source_factory(factory);
        // The tests run the watchdog's passes themselves.
        capture->set_timing({.watchdog_interval_s = 3600.0, .retry_interval_s = 3.0});
    }
    ~Rig() {
        capture->stop();
        capture.reset();
    }

    void configure(FakeCaptureSource& source) {
        if (fail_next_start > 0) {
            --fail_next_start;
            source.fail_start("device busy");
        }
        if (inactive_next_start > 0) {
            --inactive_next_start;
            source.start_inactive(true);
        }
    }
    void advance(double seconds) {
        fake.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(seconds)));
    }
    std::shared_ptr<FakeCaptureSource> mic() {
        const std::lock_guard lock(mutex);
        return mic_sources.empty() ? nullptr : mic_sources.back();
    }
    std::shared_ptr<FakeCaptureSource> loopback() {
        const std::lock_guard lock(mutex);
        return loopback_sources.empty() ? nullptr : loopback_sources.back();
    }
    // A block of `seconds` of a 16 kHz tone through the channel's current source, stamped now;
    // the clock then moves on by the block's length, as a device's would.
    bool deliver(FakeCaptureSource& source, double seconds = 0.064) {
        const auto samples = fv::meeting::test::tone(seconds, 16000);
        const bool delivered = source.deliver(samples, 16000, clock.monotonic());
        advance(seconds);
        return delivered;
    }

    TempDir folder;
    fv::FakeClock fake;
    fv::meeting::MeetingClock clock{fake};
    std::shared_ptr<fv::meeting_state::testing::FakeMeetingRepository> repository =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(fake);
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    RecordingSink sink;
    std::atomic<bool> recording{true};
    std::atomic<bool> preview_throws{false};
    std::atomic<int> previews{0};
    int fail_next_start = 0;
    int inactive_next_start = 0;
    int fail_loopback_channels = 0;     // loopback sources of that many channels fail to start
    std::vector<int> loopback_channels; // each loopback source's endpoint channels
    std::mutex mutex;
    std::vector<fv::meeting_state::SpooledChunk> chunks;
    std::vector<std::shared_ptr<FakeCaptureSource>> mic_sources;
    std::vector<std::shared_ptr<FakeCaptureSource>> loopback_sources;
    std::unique_ptr<MeetingCapture> capture;
};

} // namespace

TEST_CASE("both channels open: no note, and the capture state says so", "[capture_session]") {
    Rig rig;
    CHECK(rig.capture->start("m1", rig.folder.path()) == std::nullopt);
    const Json state = rig.sink.current();
    CHECK(state.at("mic_available") == true);
    CHECK(state.at("loopback_available") == true);
    CHECK(state.at("message") == "");
    CHECK(state.at("audio_incomplete") == false);
    CHECK(state.at("mic_source_generation") == 1);
    // The keys in Python's order.
    std::vector<std::string> keys;
    for (const auto& [key, value] : state.items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"mic_available", "loopback_available", "mic_receiving",
                                           "loopback_receiving", "mic_signal_windows",
                                           "loopback_signal_windows", "mic_source_generation",
                                           "loopback_source_generation", "message",
                                           "audio_incomplete", "integrity_error"});
    CHECK(rig.capture->spool_dirs() == std::vector<std::filesystem::path>{rig.folder.path()});
}

TEST_CASE("no device at all fails the start with Python's sentence", "[capture_session]") {
    Rig rig({}, 0, false);
    try {
        rig.capture->start("m1", rig.folder.path());
        FAIL("start() should have thrown");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "No audio devices could be opened.");
    }
    CHECK(rig.sink.current().at("message") == "No audio devices could be opened.");
}

TEST_CASE("microphone only, and the required and disabled policies", "[capture_session]") {
    SECTION("auto") {
        Rig rig({}, 1, false);
        CHECK(rig.capture->start("m1", rig.folder.path()) ==
              "System-audio capture is unavailable; recording microphone only.");
    }
    SECTION("required") {
        Rig rig({.system_audio_policy = "Required"}, 1, false);
        try {
            rig.capture->start("m1", rig.folder.path());
            FAIL("start() should have thrown");
        } catch (const std::runtime_error& error) {
            CHECK(std::string(error.what()) ==
                  "LINUX_SYSTEM_AUDIO_REQUIRED: System-audio capture is required but unavailable.");
        }
    }
    SECTION("disabled") {
        Rig rig({.system_audio_policy = "disabled"});
        const char* note = "System audio disabled for this meeting by your microphone-only choice.";
        CHECK(rig.capture->start("m1", rig.folder.path()) == note);
        CHECK(rig.loopback() == nullptr);
        CHECK(rig.sink.current().at("message") == note);
        // A disabled loopback is never restarted.
        rig.capture->run_watchdog_pass();
        CHECK(rig.loopback() == nullptr);
    }
    SECTION("no microphone") {
        Rig rig({}, 0, true);
        CHECK(rig.capture->start("m1", rig.folder.path()) == "Microphone capture is unavailable.");
    }
}

TEST_CASE("a source that starts inactive is stopped and the next microphone tried",
          "[capture_session]") {
    Rig rig({}, 2);
    rig.inactive_next_start = 1;
    const LogCapture logs;
    CHECK(rig.capture->start("m1", rig.folder.path()) == std::nullopt);
    REQUIRE(rig.mic_sources.size() == 2);
    CHECK(rig.mic_sources[0]->stop_count() == 1);
    CHECK(std::get<int>(rig.capture->source("mic")->device_id()) == 1);
    CHECK(rig.capture->mic_excluded() == std::vector<fv::audio::DeviceKey>{{"Mic 0", "MME"}});
    CHECK(logs.levels("mic capture reported inactive right after starting") ==
          std::vector{fv::log::Level::Error});
}

TEST_CASE("blocks reach the spool before the preview, which may throw", "[capture_session]") {
    Rig rig;
    rig.capture->start("m1", rig.folder.path());
    rig.preview_throws = true;
    const LogCapture logs;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(rig.deliver(*rig.mic()));
    }
    CHECK(rig.previews.load() == 20);
    // Paused: neither recorded nor previewed, but counted as delivered.
    rig.clock.pause();
    REQUIRE(rig.deliver(*rig.mic()));
    CHECK(rig.previews.load() == 20);
    rig.clock.resume();
    rig.capture->check_spool_health();
    rig.capture->check_spool_health();
    CHECK(logs.levels("Spool feed failed on channel mic (suppressing repeats)") ==
          std::vector{fv::log::Level::Error});
    rig.capture->stop();
    rig.capture->flush();
    std::lock_guard lock(rig.mutex);
    REQUIRE_FALSE(rig.chunks.empty());
    CHECK(rig.chunks.front().channel == "mic");
    CHECK(rig.capture->integrity_error() == std::nullopt);
}

TEST_CASE("flush stops the preview first and hands the final chunk on again", "[capture_session]") {
    Rig rig;
    std::vector<std::string> order;
    rig.capture->set_stop_preview([&] { order.emplace_back("stop_preview"); });
    rig.capture->start("m1", rig.folder.path());
    for (int i = 0; i < 10; ++i) {
        rig.deliver(*rig.mic());
    }
    rig.capture->stop();
    rig.capture->flush();
    CHECK(order == std::vector<std::string>{"stop_preview"});
    std::lock_guard lock(rig.mutex);
    // The spool's own on_chunk, then flush() hands the same final chunk on.
    REQUIRE(rig.chunks.size() == 2);
    CHECK(rig.chunks[0] == rig.chunks[1]);
}

TEST_CASE("a lost microphone is excluded before the channel reopens", "[capture_session]") {
    Rig rig({}, 2);
    rig.capture->start("m1", rig.folder.path());
    REQUIRE(rig.mic_sources.size() == 1);
    rig.mic()->kill();
    rig.capture->run_watchdog_pass();
    REQUIRE(rig.mic_sources.size() == 2);
    CHECK(std::get<int>(rig.capture->source("mic")->device_id()) == 1);
    CHECK(rig.capture->mic_excluded() == std::vector<fv::audio::DeviceKey>{{"Mic 0", "MME"}});
    // The spool is kept across the restart.
    CHECK(rig.capture->spool("mic") != nullptr);
    CHECK(rig.sink.current().at("mic_source_generation") == 2);
}

TEST_CASE("restarts wait the retry interval", "[capture_session]") {
    Rig rig({}, 1);
    rig.capture->start("m1", rig.folder.path());
    rig.fail_next_start = 100;
    rig.mic()->kill();
    rig.capture->run_watchdog_pass();
    const std::size_t after_first = rig.mic_sources.size();
    rig.advance(1.0);
    rig.capture->run_watchdog_pass();
    CHECK(rig.mic_sources.size() == after_first);
    rig.advance(2.0);
    rig.capture->run_watchdog_pass();
    CHECK(rig.mic_sources.size() > after_first);
    CHECK(rig.sink.current().at("mic_available") == false);
    CHECK(rig.sink.current().at("message") == "Microphone unavailable");
}

TEST_CASE("a stalled stream is restarted and the stall is reported", "[capture_session]") {
    Rig rig({}, 1);
    rig.capture->start("m1", rig.folder.path());
    REQUIRE(rig.deliver(*rig.loopback()));
    rig.advance(fv::meeting::FIRST_BLOCK_GRACE_S);
    REQUIRE(rig.deliver(*rig.loopback()));
    const LogCapture logs;
    rig.capture->run_watchdog_pass();
    CHECK(logs.levels("mic capture stream stalled despite an open device") ==
          std::vector{fv::log::Level::Warning});
    REQUIRE(rig.mic_sources.size() == 2);
    CHECK(rig.capture->stall_channels() == std::set<std::string>{"mic"});
    CHECK(rig.sink.current().at("message") ==
          "Microphone is connected but no audio blocks are arriving; check the selected input "
          "device");
    // Blocks arrive again: the stall clears on the next pass.
    REQUIRE(rig.deliver(*rig.mic()));
    REQUIRE(rig.deliver(*rig.loopback()));
    rig.capture->run_watchdog_pass();
    CHECK(rig.capture->stall_channels().empty());
    CHECK(rig.sink.current().at("message") == "");
}

TEST_CASE("system audio restored is announced on the rising edge only", "[capture_session]") {
    Rig rig({}, 1, false);
    rig.capture->start("m1", rig.folder.path());
    REQUIRE(rig.deliver(*rig.mic()));
    rig.backend->set_loopbacks({endpoint("{a}")});
    rig.capture->run_watchdog_pass();
    CHECK(rig.sink.current().at("message") == "System audio restored.");
    CHECK(rig.sink.notes.back() == "System audio restored.");
    REQUIRE(rig.deliver(*rig.mic()));
    REQUIRE(rig.deliver(*rig.loopback()));
    rig.capture->run_watchdog_pass();
    CHECK(rig.sink.current().at("message") == "");
}

TEST_CASE("the loopback follows the default render endpoint and keeps its spool",
          "[capture_session]") {
    Rig rig;
    rig.capture->start("m1", rig.folder.path());
    const auto spool = rig.capture->spool("loopback");
    REQUIRE(rig.deliver(*rig.loopback(), 1.0));
    rig.backend->set_loopbacks({endpoint("{a}", false), endpoint("{b}", true)});
    rig.capture->run_watchdog_pass();
    REQUIRE(rig.loopback_sources.size() == 2);
    CHECK(std::get<std::string>(rig.capture->source("loopback")->device_id()) == "{b}");
    CHECK(rig.loopback_sources[0]->stop_count() == 1);
    CHECK(rig.capture->spool("loopback") == spool);
}

TEST_CASE("a ranked microphone stays while the system default changes", "[capture_session]") {
    SECTION("system default follows") {
        Rig rig({}, 2);
        rig.capture->start("m1", rig.folder.path());
        rig.backend->set_layout({{.name = "MME", .default_input_device = 1}},
                                rig.backend->devices(), 1);
        REQUIRE(rig.deliver(*rig.mic()));
        rig.capture->run_watchdog_pass();
        CHECK(std::get<int>(rig.capture->source("mic")->device_id()) == 1);
    }
    SECTION("ranked stays") {
        Rig rig({.mic_priority = {{"Mic 0", "MME"}}}, 2);
        rig.capture->start("m1", rig.folder.path());
        rig.backend->set_layout({{.name = "MME", .default_input_device = 1}},
                                rig.backend->devices(), 1);
        REQUIRE(rig.deliver(*rig.mic()));
        rig.capture->run_watchdog_pass();
        CHECK(std::get<int>(rig.capture->source("mic")->device_id()) == 0);
    }
}

TEST_CASE("status is written only when it changed", "[capture_session]") {
    Rig rig;
    rig.capture->start("m1", rig.folder.path());
    const int updates = rig.sink.updates;
    rig.capture->update_status();
    rig.capture->update_status();
    CHECK(rig.sink.updates == updates);
    rig.sink.has_store = false;
    rig.capture->update_status("anything");
    CHECK(rig.sink.updates == updates);
}

TEST_CASE("a spool failure becomes the meeting's audio integrity error once", "[capture_session]") {
    Rig rig;
    // A folder where the first mic chunk goes, so writing it fails.
    std::filesystem::create_directories(rig.folder.path() / "mic_00000.wav");
    rig.capture->start("m1", rig.folder.path());
    for (int i = 0; i < 10; ++i) {
        rig.deliver(*rig.mic());
    }
    rig.capture->stop();
    rig.capture->flush();
    rig.capture->check_spool_health();
    const auto error = rig.capture->integrity_error();
    REQUIRE(error);
    CHECK(*error == "mic audio chunk could not be written. Partial meeting audio was kept for "
                    "recovery.");
    REQUIRE(rig.sink.errors.size() == 1);
    CHECK(rig.sink.errors[0].first == "capture_incomplete");
    CHECK(rig.sink.errors[0].second ==
          *error + " Check available audio in Meeting History and free storage or reduce system "
                   "load before recording again.");
    const Json state = rig.sink.current();
    CHECK(state.at("audio_incomplete") == true);
    CHECK(state.at("integrity_error") == *error);
    CHECK(state.at("message").get<std::string>().starts_with(*error));
}

TEST_CASE("the watchdog thread runs passes while recording", "[capture_session]") {
    Rig rig({}, 2);
    rig.capture->set_timing({.watchdog_interval_s = 0.01, .retry_interval_s = 3.0});
    rig.capture->start("m1", rig.folder.path());
    rig.mic()->kill();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rig.capture->mic_excluded().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(rig.capture->mic_excluded() == std::vector<fv::audio::DeviceKey>{{"Mic 0", "MME"}});
    rig.capture->stop();
}

TEST_CASE("the block router allocates nothing on the device thread", "[capture_session]") {
    Rig rig;
    rig.capture->start("m1", rig.folder.path());
    const auto samples = fv::meeting::test::tone(0.064, 16000);
    const auto mic = rig.mic();
    const auto loopback = rig.loopback();
    REQUIRE(mic);
    REQUIRE(loopback);
    bool mic_delivered = false;
    bool loopback_delivered = false;
    {
        // Monitor, spool queue and preview, as a device callback runs them.
        const fv::testkit::RealtimeGuard guard;
        mic_delivered = mic->deliver(samples, 16000, rig.clock.monotonic());
        loopback_delivered = loopback->deliver(samples, 16000, rig.clock.monotonic());
    }
    CHECK(mic_delivered);
    CHECK(loopback_delivered);
    CHECK(rig.previews.load() == 2);
}

TEST_CASE("a fake source's stop waits for the block in flight", "[capture_session]") {
    // The capture's sources promise that no block arrives after stop(): a restart then starts
    // the replacement as the spool's only producer.
    FakeCaptureSource source("mic", 0);
    std::promise<void> entered;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<int> blocks{0};
    source.start([&](const fv::meeting::CaptureBlock&) {
        if (blocks.fetch_add(1) == 0) {
            entered.set_value();
        }
        released.wait();
    });
    const std::vector<std::int16_t> samples(160, 1);
    std::atomic<bool> first_delivered{false};
    std::thread device([&] { first_delivered = source.deliver(samples, 16000, 0.0); });
    REQUIRE(entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { source.stop(); });
    CHECK(stopped.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
    release.set_value();
    stopped.wait();
    device.join();
    CHECK(first_delivered.load());
    CHECK_FALSE(source.deliver(samples, 16000, 0.0));
    CHECK(blocks.load() == 1);
    CHECK(source.stop_count() == 1);
}

TEST_CASE("a surround loopback that does not open is opened again as stereo", "[capture_session]") {
    // capture_session.py start_fallback_loopback: SoundCard records the default speaker at
    // min(2, channels) channels.
    Rig rig;
    rig.backend->set_loopbacks({LoopbackEndpoint{
        .id = "{a}", .name = "Speakers 5.1", .is_default = true, .rate = 48000, .channels = 6}});
    rig.fail_loopback_channels = 6;
    CHECK_FALSE(rig.capture->start("m1", rig.folder.path()).has_value());
    CHECK(rig.loopback_channels == std::vector<int>{6, 2});
    REQUIRE(rig.capture->source("loopback"));
    CHECK(std::get<std::string>(rig.capture->source("loopback")->device_id()) == "{a}");
}

TEST_CASE("a stereo loopback that does not open has no second path", "[capture_session]") {
    Rig rig;
    rig.backend->set_loopbacks({LoopbackEndpoint{
        .id = "{a}", .name = "Speakers", .is_default = true, .rate = 48000, .channels = 2}});
    rig.fail_loopback_channels = 2;
    const auto note = rig.capture->start("m1", rig.folder.path());
    CHECK(rig.loopback_channels == std::vector<int>{2});
    CHECK(note == "System-audio capture is unavailable; recording microphone only.");
}

TEST_CASE("the watchdog asks only for the default endpoint while the loopback records it",
          "[capture_session]") {
    Rig rig;
    rig.capture->start("m1", rig.folder.path());
    REQUIRE(rig.deliver(*rig.loopback(), 1.0));
    REQUIRE(rig.deliver(*rig.mic()));
    const int listings = rig.backend->loopback_listings();
    const int queries = rig.backend->default_loopback_queries();
    rig.capture->run_watchdog_pass();
    CHECK(rig.backend->loopback_listings() == listings);
    CHECK(rig.backend->default_loopback_queries() == queries + 1);
    CHECK(rig.loopback_sources.size() == 1);
    // A new default is found by listing, and followed.
    rig.backend->set_loopbacks({endpoint("{a}", false), endpoint("{b}", true)});
    rig.capture->run_watchdog_pass();
    CHECK(rig.backend->loopback_listings() > listings);
    CHECK(std::get<std::string>(rig.capture->source("loopback")->device_id()) == "{b}");
}
