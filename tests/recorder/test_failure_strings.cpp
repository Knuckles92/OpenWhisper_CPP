// The capture failures the user reads whose wording no upstream test pins (map row A05-41:
// services/recorder.py _wait_for_stop, _finish_capture_journal, _capture_saveable), and the level
// meter's own failure, which costs a reading and never the recording
// (_calculate_and_report_level).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "recorder/journal.h"
#include "recorder_test_support.h"

namespace {

using namespace fv::recorder_test;
using fv::JournalFile;
using fv::RecorderOptions;
using std::chrono::milliseconds;

// What the recorder told its listener, as AudioRecorder.error_callback collects it.
struct Failures {
    explicit Failures(Rig& rig)
        : link(rig.rec().capture_error.connect(rig.inline_executor,
                                               [this](const std::string& message) {
                                                   {
                                                       const std::lock_guard lock(mutex);
                                                       messages.push_back(message);
                                                   }
                                                   reported.set();
                                               })) {}

    std::vector<std::string> list() {
        const std::lock_guard lock(mutex);
        return messages;
    }

    std::mutex mutex;
    std::vector<std::string> messages;
    Flag reported;
    fv::Subscription link;
};

// audio.pcm on a disk whose final sync does not return until the test lets it.
class StalledSync final : public JournalFile {
public:
    StalledSync(std::unique_ptr<JournalFile> inner, std::shared_ptr<Flag> release)
        : inner_(std::move(inner)), release_(std::move(release)) {}

    std::size_t write(std::span<const std::byte> data) override { return inner_->write(data); }
    void sync() override {
        // The writer thread: no Catch2 assertions here.
        release_->wait(milliseconds(5000));
        inner_->sync();
    }
    void close() override { inner_->close(); }

private:
    std::unique_ptr<JournalFile> inner_;
    std::shared_ptr<Flag> release_;
};

} // namespace

TEST_CASE("blocks lost after storage stopped are named when the recording is saved") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("dictation.wav")});
    Failures failures(rig);
    REQUIRE(rig.rec().start_recording());
    auto& device = *rig.backend.opened().back().fake;
    device.deliver(tone(1));
    REQUIRE(rig.rec().sync_capture());
    // The journal has stopped: the next block has nowhere to go.
    REQUIRE(rig.rec().journal()->finish());
    device.deliver(tone(2));
    REQUIRE(rig.rec().sync_capture());
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    CHECK(failures.list().empty());

    CHECK_FALSE(rig.rec().save_recording());

    const std::string lost = "Audio blocks were lost; the partial recording was kept.";
    CHECK(failures.list() == std::vector<std::string>{lost});
    CHECK(rig.rec().last_capture_error() == lost);
    // Said once: the partial save reports nothing more.
    CHECK(rig.rec().save_recording(std::nullopt, true));
    CHECK(failures.list().size() == 1);
}

TEST_CASE("storage that does not finish after the stop is reported and nothing is saved") {
    Rig rig;
    auto release = std::make_shared<Flag>();
    RecorderOptions options{.output_file = rig.file("dictation.wav")};
    options.timings.post_roll = milliseconds(20);
    options.journal.finish_timeout = milliseconds(100);
    // A recording this short checkpoints only once: the final sync after the stop.
    options.journal.wrap_file = [release](std::unique_ptr<JournalFile> file) {
        return std::unique_ptr<JournalFile>(
            std::make_unique<StalledSync>(std::move(file), release));
    };
    rig.make(options);
    Failures failures(rig);
    REQUIRE(rig.rec().start_recording());
    rig.backend.opened().back().fake->deliver(tone(4));
    REQUIRE(rig.rec().sync_capture());

    REQUIRE(rig.rec().stop_recording());

    REQUIRE(failures.reported.wait(milliseconds(3000)));
    const std::string unfinished = "Recording storage did not finish; audio kept for recovery.";
    CHECK(failures.list() == std::vector<std::string>{unfinished});
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    // Not even as a partial recording while the journal is still writing.
    CHECK_FALSE(rig.rec().save_recording(std::nullopt, true));
    CHECK_FALSE(std::filesystem::exists(rig.file("dictation.wav")));
    CHECK(rig.rec().last_capture_error() == unfinished);
    CHECK(failures.list().size() == 1);
    release->set();
}

TEST_CASE("a watcher that fails ends the recording with what stopped it") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("dictation.wav")});
    Failures failures(rig);
    REQUIRE(rig.rec().start_recording());
    const auto stream = rig.backend.stream(0);
    stream.fake->deliver(tone(6));

    stream.control->broken.store(true);

    REQUIRE(failures.reported.wait(milliseconds(3000)));
    CHECK(failures.list() ==
          std::vector<std::string>{"Audio capture stopped unexpectedly: the driver is gone"});
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    CHECK_FALSE(rig.rec().is_recording());
    CHECK(rig.backend.fake().open_streams() == 0);
}

TEST_CASE("a storage write that fails is reported as the recording not saved") {
    class Refusing final : public JournalFile {
    public:
        explicit Refusing(std::unique_ptr<JournalFile> inner) : inner_(std::move(inner)) {}
        std::size_t write(std::span<const std::byte>) override {
            throw std::runtime_error("the disk went away");
        }
        void sync() override { inner_->sync(); }
        void close() override { inner_->close(); }

    private:
        std::unique_ptr<JournalFile> inner_;
    };
    Rig rig;
    RecorderOptions options{.output_file = rig.file("dictation.wav")};
    options.journal.wrap_file = [](std::unique_ptr<JournalFile> file) {
        return std::unique_ptr<JournalFile>(std::make_unique<Refusing>(std::move(file)));
    };
    rig.make(options);
    Failures failures(rig);
    REQUIRE(rig.rec().start_recording());

    rig.backend.opened().back().fake->deliver(tone(8));

    REQUIRE(failures.reported.wait(milliseconds(3000)));
    CHECK(failures.list() ==
          std::vector<std::string>{"Recording could not be saved: the disk went away"});
}

TEST_CASE("a level listener that throws costs its reading and not the recording") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("dictation.wav")});
    Failures failures(rig);
    int asked = 0;
    const fv::Subscription meter = rig.rec().level.connect(rig.inline_executor, [&](float) {
        ++asked;
        throw std::runtime_error("the meter broke");
    });
    REQUIRE(rig.rec().start_recording());
    auto& device = *rig.backend.opened().back().fake;

    device.deliver(tone(9));
    device.deliver(tone(10));
    REQUIRE(rig.rec().sync_capture());

    CHECK(asked == 2);
    CHECK(rig.rec().is_recording());
    CHECK(rig.rec().snapshot().recorded_frames == 2 * fv::RECORDER_CHUNK);
    CHECK(failures.list().empty());
    CHECK(rig.rec().last_capture_error().empty());
}
