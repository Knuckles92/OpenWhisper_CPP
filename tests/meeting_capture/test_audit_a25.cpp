// Behaviour audit of area a25 (docs/port/map/a25-meeting-capture-asr-diarize.md): the rows whose
// code was right but whose specifics no other test pinned. Each case names its map row.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "asr_test_support.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/spool.h"
#include "meeting_state/testing/fake_meeting_repository.h"

using fv::meeting::CaptureBlock;
using fv::meeting::MAX_IN_MEMORY_CHUNKS;
using fv::meeting::MeetingAsrEngine;
using fv::meeting::MeetingClock;
using fv::meeting::SpoolOptions;
using fv::meeting::SpoolWriter;
using fv::meeting::test::RecordingRepository;
using fv::meeting::test::register_chunk;
using fv::meeting::test::SpeechRig;
using fv::meeting::test::TempDir;
using fv::meeting::test::tone;
using fv::meeting_state::SpooledChunk;
using fv::meeting_state::testing::FakeMeetingRepository;

namespace {

struct SpoolRig {
    SpoolRig() {
        fv::meeting::test::set_steady(fake, 5000.0);
        clock.start();
        t0 = clock.monotonic();
    }

    std::unique_ptr<SpoolWriter> writer(SpoolOptions options = {}) {
        return std::make_unique<SpoolWriter>(
            "m_test", "mic", folder.path(), clock, repository, [](const SpooledChunk&) {},
            std::move(options));
    }

    TempDir folder;
    fv::FakeClock fake;
    MeetingClock clock{fake};
    double t0 = 0.0;
    std::shared_ptr<FakeMeetingRepository> repository =
        std::make_shared<FakeMeetingRepository>(fake);
};

} // namespace

// A25-21
TEST_CASE("audit A25-21: a chunk the meeting cannot register is reported in Python's words",
          "[audit_a25]") {
    SpoolRig rig;
    rig.repository->fail_register_chunk(2); // the attempt and its one retry
    auto writer = rig.writer();
    const auto samples = tone(1.0, 16000);
    for (std::size_t at = 0; at + 1024 <= samples.size(); at += 1024) {
        fv::meeting::test::feed_waiting(
            *writer, fv::meeting::test::block_of(samples, at, 1024, 16000,
                                                 rig.t0 + static_cast<double>(at) / 16000));
    }
    writer->flush();
    REQUIRE(writer->health_error());
    CHECK(*writer->health_error() ==
          "mic audio could not be added to the meeting. Its file was kept for recovery.");
}

// A25-22
TEST_CASE("audit A25-22: a full spool queue drops the block and says so in Python's words",
          "[audit_a25]") {
    SpoolRig rig;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::once_flag once;
    SpoolOptions options;
    options.queue_size = 1;
    options.before_block = [&] {
        std::call_once(once, [&] { entered.set_value(); });
        REQUIRE(released.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    };
    auto writer = rig.writer(options);
    CHECK(fv::meeting::QUEUE_MAX_BLOCKS == 512);
    CHECK(fv::meeting::FLUSH_TIMEOUT_S == 30.0);
    const std::vector<std::int16_t> samples(1600, 2000);
    const CaptureBlock block =
        fv::meeting::test::block_of(samples, 0, samples.size(), 16000, rig.t0);
    writer->feed(block);
    REQUIRE(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    writer->feed(block);
    writer->feed(block);
    REQUIRE(writer->health_error());
    CHECK(*writer->health_error() ==
          "mic audio blocks were dropped because recording storage fell behind. This meeting's "
          "audio is incomplete.");
    release.set_value();
    writer->flush();
}

// A25-47
TEST_CASE("audit A25-47: enqueue keeps one copy, sends later chunks through SQLite once one "
          "waits there, and refuses after stop",
          "[audit_a25]") {
    TempDir folder;
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<RecordingRepository>();
    std::vector<SpooledChunk> chunks;
    for (int index = 1; index <= static_cast<int>(MAX_IN_MEMORY_CHUNKS) + 2; ++index) {
        chunks.push_back(register_chunk(*repository, folder.path(), index, index));
    }
    // Not started: nothing leaves the in-memory queue.
    MeetingAsrEngine engine(speech.service, repository, "m_test", "", fv::system_clock());
    REQUIRE(engine.load_backend({}));
    REQUIRE(engine.enqueue(chunks[0]));
    CHECK(engine.enqueue(chunks[0])); // already queued: true, not queued twice
    CHECK(engine.outstanding() == 1);
    for (std::size_t i = 1; i < MAX_IN_MEMORY_CHUNKS; ++i) {
        REQUIRE(engine.enqueue(chunks[i]));
    }
    CHECK(engine.outstanding() == MAX_IN_MEMORY_CHUNKS);
    CHECK_FALSE(engine.enqueue(chunks[MAX_IN_MEMORY_CHUNKS]));
    CHECK_FALSE(engine.enqueue(chunks[MAX_IN_MEMORY_CHUNKS + 1]));
    CHECK(engine.outstanding() == MAX_IN_MEMORY_CHUNKS);
    engine.stop();
    CHECK_FALSE(engine.enqueue(chunks[0]));
    CHECK(repository->get_pending_chunks("m_test", std::nullopt, {}).size() == chunks.size());
}
