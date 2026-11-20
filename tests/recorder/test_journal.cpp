// Failure injection for durable capture; no microphone or personal data
// (tests/test_recording_journal.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <regex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "recorder/journal.h"
#include "recorder_test_support.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fv::recorder_test {
// Set by main(): this test program, which also serves as the killed child.
extern std::filesystem::path g_self;
} // namespace fv::recorder_test

namespace {

using namespace fv::recorder_test;
using fv::JournalFile;
using fv::JournalOptions;
using fv::RecorderOptions;
using fv::RecordingJournal;
using std::chrono::milliseconds;

const auto kNoError = [](const std::string&) {
};

std::vector<std::byte> repeated(std::initializer_list<std::uint8_t> unit, std::size_t times) {
    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i < times; ++i) {
        for (const std::uint8_t b : unit) {
            bytes.push_back(static_cast<std::byte>(b));
        }
    }
    return bytes;
}

std::vector<std::byte> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}

// The PCM of a recovered WAV (Python's wave header: 44 bytes).
std::vector<std::byte> recovered_pcm(const std::filesystem::path& wav) {
    auto bytes = read_all(wav);
    REQUIRE(bytes.size() >= 44);
    return {bytes.begin() + 44, bytes.end()};
}

// A file that refuses or delays writes, standing in for the journal's file object.
class InjectedFile final : public JournalFile {
public:
    enum class Mode { Pass, Full, Slow, Short, SlowSync, SlowThenFail };

    InjectedFile(std::unique_ptr<JournalFile> inner, Mode mode,
                 std::shared_ptr<Flag> entered = nullptr, std::shared_ptr<Flag> release = nullptr)
        : inner_(std::move(inner)), mode_(mode), entered_(std::move(entered)),
          release_(std::move(release)) {}

    std::size_t write(std::span<const std::byte> data) override {
        switch (mode_) {
        case Mode::Full:
            throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                    "No space left on device");
        case Mode::Slow:
            // The writer thread: no Catch2 assertions here.
            entered_->set();
            release_->wait(milliseconds(3000));
            return inner_->write(data);
        case Mode::SlowThenFail:
            entered_->set();
            release_->wait(milliseconds(10000));
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    "a previous disk write finally failed");
        case Mode::Short:
            return inner_->write(data.first(std::min<std::size_t>(4, data.size())));
        default:
            return inner_->write(data);
        }
    }
    void sync() override {
        if (mode_ == Mode::SlowSync) {
            entered_->set();
            release_->wait(milliseconds(3000));
        }
        inner_->sync();
    }
    void close() override { inner_->close(); }

private:
    std::unique_ptr<JournalFile> inner_;
    Mode mode_;
    std::shared_ptr<Flag> entered_;
    std::shared_ptr<Flag> release_;
};

std::function<std::unique_ptr<JournalFile>(std::unique_ptr<JournalFile>)>
inject(InjectedFile::Mode mode, std::shared_ptr<Flag> entered = nullptr,
       std::shared_ptr<Flag> release = nullptr) {
    return [=](std::unique_ptr<JournalFile> file) -> std::unique_ptr<JournalFile> {
        return std::make_unique<InjectedFile>(std::move(file), mode, entered, release);
    };
}

std::size_t entries(const std::filesystem::path& folder) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(folder)) {
        ++count;
    }
    return count;
}

// A child process of this program; killed on scope exit if still running. `mode` is
// "--journal-child" or "--recorder-child" (main.cpp).
class Child {
public:
    Child(const std::filesystem::path& output, const std::filesystem::path& ready,
          const char* mode = "--journal-child") {
#ifdef _WIN32
        const std::string flag(mode);
        std::wstring command = L"\"" + g_self.wstring() + L"\" " +
                               std::wstring(flag.begin(), flag.end()) + L" \"" + output.wstring() +
                               L"\" \"" + ready.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &info));
        CloseHandle(info.hThread);
        process_ = info.hProcess;
#else
        pid_ = fork();
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            execl(g_self.c_str(), g_self.c_str(), mode, output.c_str(), ready.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
#endif
    }
    ~Child() { kill(); }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void kill() {
#ifdef _WIN32
        if (process_ != nullptr) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
#else
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

} // namespace

// from tests/test_recording_journal.py::test_active_journal_is_skipped_and_recovery_is_idempotent
TEST_CASE("active journal is skipped and recovery is idempotent", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "recorded.wav";
    const auto saved = root.path() / "recordings";
    RecordingJournal journal(output, 16000, 1, 2, kNoError);
    const auto pcm = bytes_of(arange(4096));
    REQUIRE(journal.append(pcm));
    REQUIRE(journal.finish());
    CHECK(fv::recover_recordings(output, saved).empty());
    REQUIRE(journal.close());
    const auto recovered = fv::recover_recordings(output, saved);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered_pcm(recovered[0]) == pcm);
    CHECK(fv::recover_recordings(output, saved).empty());
}

// from tests/test_recording_journal.py::test_recovery_after_process_is_killed
TEST_CASE("recovery after process is killed", "[test_recording_journal][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    const auto ready = root.path() / "ready";
    Child child(output, ready);
    // Readiness with a deadline, so a child that fails to start cannot hang the run.
    REQUIRE(wait_until([&] { return std::filesystem::exists(ready); }, milliseconds(10000)));
    child.kill();
    // Windows releases a killed process's file locks some time after it exits; until then its
    // journal still reads as a live session and is skipped, as it should be. Recovery is made
    // to be retried.
    std::vector<std::filesystem::path> recovered;
    wait_until(
        [&] {
            recovered = fv::recover_recordings(output, root.path() / "saved");
            return !recovered.empty();
        },
        milliseconds(10000));
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].filename().string().starts_with("recording_"));
    CHECK(recovered[0].filename().string().find("-recovered-") != std::string::npos);
    CHECK(recovered_pcm(recovered[0]) == repeated({0x10, 0x00}, 16000));
}

// Acceptance: a dictation killed mid-capture (a Recorder over the fake microphone, not a bare
// journal) leaves a journal that the next start's recovery turns into
// recordings/recording_<stamp>-recovered-<id>.wav.
TEST_CASE("a recording killed mid-capture is recovered into recordings",
          "[test_recording_journal][platform]") {
    const fv::testkit::TempDataRoot root;
    const auto ready = root.path() / "ready";
    Child child(root.path(), ready, "--recorder-child");
    REQUIRE(wait_until([&] { return std::filesystem::exists(ready); }, milliseconds(10000)));
    child.kill();

    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    REQUIRE(paths.data_root() == root.path());
    const auto output = paths.user_file(fv::RECORDED_AUDIO_FILE);
    const auto recordings = paths.user_file("recordings");
    std::vector<std::filesystem::path> recovered;
    wait_until(
        [&] {
            recovered = fv::recover_recordings(output, recordings);
            return !recovered.empty();
        },
        milliseconds(10000));
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].parent_path() == recordings);
    const std::string name = recovered[0].filename().string();
    CHECK(
        std::regex_match(name, std::regex(R"(recording_\d{8}_\d{6}-recovered-[0-9a-f]{32}\.wav)")));
    // Everything the microphone handed over before the kill, without the save's end padding.
    const auto samples = wav_samples(recovered[0]);
    CHECK(samples.size() == 20 * 1024);
    CHECK(std::all_of(samples.begin(), samples.end(), [](std::int16_t s) { return s == 0x10; }));
    // The session is gone: a second recovery finds nothing.
    CHECK(fv::recover_recordings(output, recordings).empty());
    CHECK_FALSE(std::filesystem::exists(output));
}

// from tests/test_recording_journal.py::test_disk_full_stops_recording_and_reports_once
TEST_CASE("disk full stops recording and reports once", "[test_recording_journal]") {
    Rig rig;
    RecorderOptions options{.output_file = rig.file("capture.wav")};
    options.journal.wrap_file = inject(InjectedFile::Mode::Full);
    rig.make(options);
    std::vector<std::string> errors;
    Flag failed;
    std::mutex mutex;
    const auto subscription =
        rig.rec().capture_error.connect(rig.inline_executor, [&](const std::string& message) {
            const std::lock_guard lock(mutex);
            errors.push_back(message);
            failed.set();
        });
    REQUIRE(rig.rec().start_recording());
    rig.backend.opened().back().fake->deliver(tone(1));
    REQUIRE(failed.wait(milliseconds(2000)));
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(2000)));
    CHECK_FALSE(rig.rec().is_recording());
    const std::lock_guard lock(mutex);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("saved") != std::string::npos);
    CHECK_FALSE(rig.rec().last_capture_error().empty());
}

// from
// tests/test_recording_journal.py::test_slow_disk_never_blocks_enqueue_and_overflow_is_reported
TEST_CASE("slow disk never blocks enqueue and overflow is reported", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    std::vector<std::string> errors;
    std::mutex mutex;
    auto entered = std::make_shared<Flag>();
    auto release = std::make_shared<Flag>();
    JournalOptions options;
    options.queue_blocks = 2;
    options.wrap_file = inject(InjectedFile::Mode::Slow, entered, release);
    RecordingJournal journal(
        root.path() / "capture.wav", 16000, 1, 2,
        [&](const std::string& message) {
            const std::lock_guard lock(mutex);
            errors.push_back(message);
        },
        options);
    const auto block = repeated({0, 0}, 1024);
    CHECK(journal.append(block));
    REQUIRE(entered->wait(milliseconds(1000)));
    CHECK(journal.append(block));
    CHECK(journal.append(block));
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(journal.append(block));
    CHECK(std::chrono::steady_clock::now() - start < milliseconds(100));
    CHECK(journal.dropped_frames() == 1024);
    release->set();
    CHECK(journal.finish());
    {
        const std::lock_guard lock(mutex);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0] == fv::STORAGE_CANNOT_KEEP_UP);
    }
    journal.close();
}

// from tests/test_recording_journal.py::test_finish_wakes_a_writer_waiting_for_the_next_block
TEST_CASE("finish wakes a writer waiting for the next block", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    // A poll far longer than the wait below: only finish()'s wake can end it.
    JournalOptions options;
    options.poll = milliseconds(30000);
    RecordingJournal journal(root.path() / "capture.wav", 16000, 1, 2, kNoError, options);
    const auto pcm = repeated({0x12, 0x00}, 1024);
    REQUIRE(journal.append(pcm));
    REQUIRE(journal.wait_until_written(milliseconds(2000)));
    CHECK(journal.finish(milliseconds(2000)));
    CHECK(journal.written_bytes() == pcm.size());
    journal.close();
}

// from tests/test_recording_journal.py::test_finish_writes_every_block_queued_before_it
TEST_CASE("finish writes every block queued before it", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    JournalOptions options;
    options.poll = milliseconds(30000);
    RecordingJournal journal(output, 16000, 1, 2, kNoError, options);
    std::vector<std::byte> all;
    for (std::uint8_t index = 0; index < 40; ++index) {
        const std::vector<std::byte> block(2048, static_cast<std::byte>(index));
        REQUIRE(journal.append(block));
        all.insert(all.end(), block.begin(), block.end());
    }
    REQUIRE(journal.finish(milliseconds(2000)));
    REQUIRE(journal.close());
    const auto recovered = fv::recover_recordings(output, root.path() / "saved");
    REQUIRE(recovered.size() == 1);
    CHECK(recovered_pcm(recovered[0]) == all);
}

// from tests/test_recording_journal.py::test_explicit_cancel_removes_journal
TEST_CASE("explicit cancel removes journal", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    RecordingJournal journal(output, 16000, 1, 2, kNoError);
    journal.append(repeated({0, 0}, 2048));
    REQUIRE(journal.close(true));
    CHECK(entries(fv::recovery_directory(output)) == 0);
}

// from
// tests/test_recording_journal.py::test_device_disconnect_without_another_microphone_is_reported_without_a_stop_press
TEST_CASE("device disconnect without another microphone is reported without a stop press",
          "[test_recording_journal]") {
    // The only microphone: nothing to switch to (test_device_fallback.cpp covers the switch).
    auto fake = std::make_shared<fv::audio::testing::FakeBackend>();
    fake->set_layout({{"MME", 0, -1}}, {{"Microphone (USB)", 0, 1, 0, 44100.0}}, 0);
    Rig rig(fake);
    rig.make(RecorderOptions{.output_file = rig.file("capture.wav")});
    Flag error;
    const auto subscription = rig.rec().capture_error.connect(
        rig.inline_executor, [&](const std::string&) { error.set(); });
    REQUIRE(rig.rec().start_recording());
    rig.backend.opened().back().control->inactive.store(true);
    REQUIRE(error.wait(milliseconds(2000)));
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(2000)));
    CHECK_FALSE(rig.rec().is_recording());
    CHECK(rig.rec().last_capture_error() == fv::NO_MICROPHONE_LEFT);
}

// from
// tests/test_recording_journal.py::test_start_keeps_previous_output_until_new_recording_is_saved
TEST_CASE("start keeps previous output until new recording is saved", "[test_recording_journal]") {
    Rig rig;
    const auto output = rig.file("capture.wav");
    {
        std::ofstream out(output, std::ios::binary);
        out << "previous unsaved audio";
    }
    rig.make(RecorderOptions{.output_file = output});
    REQUIRE(rig.rec().start_recording());
    const auto bytes = read_all(output);
    CHECK(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) ==
          "previous unsaved audio");
}

// from tests/test_recording_journal.py::test_partial_write_keeps_recoverable_prefix
TEST_CASE("partial write keeps recoverable prefix", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    JournalOptions options;
    options.wrap_file = inject(InjectedFile::Mode::Short);
    RecordingJournal journal(output, 16000, 1, 2, kNoError, options);
    journal.append(repeated({0x12, 0x00}, 8));
    REQUIRE(journal.finish());
    CHECK(journal.written_bytes() == 4);
    CHECK(journal.error() == "Recording could not be saved: Incomplete recording write");
    REQUIRE(journal.close());
    const auto recovered = fv::recover_recordings(output, root.path() / "saved");
    REQUIRE(recovered.size() == 1);
    CHECK(recovered_pcm(recovered[0]) == repeated({0x12, 0x00}, 2));
}

// from
// tests/test_recording_journal.py::test_canceled_timed_out_writer_removes_audio_when_it_unblocks
TEST_CASE("canceled timed out writer removes audio when it unblocks", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    auto entered = std::make_shared<Flag>();
    auto release = std::make_shared<Flag>();
    JournalOptions options;
    options.finish_timeout = milliseconds(10);
    options.wrap_file = inject(InjectedFile::Mode::Slow, entered, release);
    RecordingJournal journal(output, 16000, 1, 2, kNoError, options);
    journal.append(repeated({0x12, 0x00}, 8));
    REQUIRE(entered->wait(milliseconds(1000)));
    CHECK_FALSE(journal.close(true));
    CHECK(std::filesystem::exists(journal.directory() / "discarded"));
    release->set();
    CHECK(wait_until([&] { return !std::filesystem::exists(journal.directory()); },
                     milliseconds(2000)));
    CHECK(fv::recover_recordings(output, root.path() / "saved").empty());
}

// from tests/test_recording_journal.py::test_recovery_never_restores_canceled_session
TEST_CASE("recovery never restores canceled session", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "capture.wav";
    RecordingJournal journal(output, 16000, 1, 2, kNoError);
    journal.append(repeated({0x12, 0x00}, 8));
    REQUIRE(journal.close());
    { std::ofstream touch(journal.directory() / "discarded"); }
    CHECK(fv::recover_recordings(output, root.path() / "saved").empty());
    CHECK_FALSE(std::filesystem::exists(journal.directory()));
}

// from tests/test_recording_journal.py::test_old_writer_error_cannot_stop_next_recording
TEST_CASE("old writer error cannot stop next recording", "[test_recording_journal]") {
    // The first recording's writer is stuck on a write that fails only after the next
    // recording has started; its late error must not reach the new one.
    Rig rig;
    auto entered = std::make_shared<Flag>();
    auto release = std::make_shared<Flag>();
    auto journals = std::make_shared<std::atomic<int>>(0);
    RecorderOptions options{.output_file = rig.file("capture.wav")};
    options.timings.post_roll = milliseconds(50);
    options.journal.finish_timeout = milliseconds(50);
    options.journal.wrap_file =
        [=](std::unique_ptr<JournalFile> file) -> std::unique_ptr<JournalFile> {
        const bool stuck = journals->fetch_add(1) == 0;
        return std::make_unique<InjectedFile>(
            std::move(file), stuck ? InjectedFile::Mode::SlowThenFail : InjectedFile::Mode::Pass,
            entered, release);
    };
    rig.make(options);
    REQUIRE(rig.rec().start_recording());
    const auto old = rig.rec().journal();
    rig.backend.opened().back().fake->deliver(tone(3));
    REQUIRE(entered->wait(milliseconds(2000)));
    REQUIRE(rig.rec().stop_recording());
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    rig.rec().clear_recording_data(false, false);

    REQUIRE(rig.rec().start_recording());
    release->set();
    REQUIRE(wait_until([&] { return old->error().has_value(); }));
    std::this_thread::sleep_for(milliseconds(200)); // the old writer reports, then exits
    CHECK(rig.rec().last_capture_error().empty());
    CHECK(rig.rec().is_recording());
}

// from
// tests/test_recording_journal.py::test_cancel_returns_while_cancellation_checkpoint_is_stalled
TEST_CASE("cancel returns while cancellation checkpoint is stalled", "[test_recording_journal]") {
    // The journal's disposal stalls on a write stuck on slow storage; Qt's cancel handler must
    // not wait on it.
    Rig rig;
    auto entered = std::make_shared<Flag>();
    auto release = std::make_shared<Flag>();
    RecorderOptions options{.output_file = rig.file("capture.wav")};
    options.journal.wrap_file = inject(InjectedFile::Mode::Slow, entered, release);
    rig.make(options);
    std::filesystem::path directory;
    {
        REQUIRE(rig.rec().start_recording());
        directory = rig.rec().journal()->directory();
        rig.backend.opened().back().fake->deliver(tone(3));
        REQUIRE(entered->wait(milliseconds(1000)));
        const auto started = std::chrono::steady_clock::now();
        rig.rec().cancel_recording();
        CHECK(std::chrono::steady_clock::now() - started < milliseconds(500));
        CHECK(rig.rec().capture_canceled());
        CHECK_FALSE(rig.rec().has_recording_data());
    }
    release->set();
    rig.rec().cleanup();
    CHECK(wait_until([&] { return !std::filesystem::exists(directory); }, milliseconds(3000)));
}

// from tests/test_recording_journal.py::test_stop_wait_allows_final_storage_checkpoint
TEST_CASE("stop wait allows final storage checkpoint", "[test_recording_journal]") {
    const bool quiet_tail = GENERATE(false, true);
    Rig rig;
    auto entered = std::make_shared<Flag>();
    auto release = std::make_shared<Flag>();
    RecorderOptions options{.output_file = rig.file("capture.wav")};
    // The quiet tail ends the post-roll itself, so its cap only has to outlast the feeding.
    options.timings.post_roll = milliseconds(quiet_tail ? 1200 : 50);
    options.timings.finalize_grace = milliseconds(50);
    // A recording this short checkpoints only once: the final sync after the stop.
    options.journal.wrap_file = inject(InjectedFile::Mode::SlowSync, entered, release);
    rig.make(options);
    REQUIRE(rig.rec().start_recording());
    const auto audio = arange(1024);
    auto& device = *rig.backend.opened().back().fake;
    device.deliver(audio);
    if (quiet_tail) {
        // Enough of a recording for a noise floor, then a quiet tail after the stop.
        for (int i = 0; i < 30; ++i) {
            device.deliver(noise_block(i < 10 ? -60.0 : -25.0, static_cast<std::uint64_t>(i)));
        }
    }
    REQUIRE(rig.rec().sync_capture());
    REQUIRE(rig.rec().stop_recording());
    if (quiet_tail) {
        for (int i = 0; i < 20 && rig.rec().snapshot().post_roll_end_reason.empty(); ++i) {
            device.deliver(noise_block(-60.0, 100 + static_cast<std::uint64_t>(i)));
            rig.rec().sync_capture(milliseconds(10));
        }
        CHECK(rig.rec().snapshot().post_roll_end_reason == "quiet");
    }
    auto pending =
        std::async(std::launch::async, [&] { return rig.rec().wait_for_stop_completion(); });
    REQUIRE(entered->wait(milliseconds(1000)));
    // Storage is still within its own deadline after the capture deadline has elapsed; it must
    // not turn valid audio into a failure.
    std::this_thread::sleep_for(milliseconds(150));
    CHECK(pending.wait_for(milliseconds(0)) != std::future_status::ready);
    CHECK_FALSE(rig.rec().wait_for_stop_completion(milliseconds(10)));
    release->set();
    REQUIRE(pending.wait_for(milliseconds(1000)) == std::future_status::ready);
    CHECK(pending.get());
    CHECK(rig.rec().last_capture_error().empty());
    CHECK_FALSE(rig.rec().is_recording());
    REQUIRE(rig.rec().save_recording());
    const auto samples = wav_samples(rig.rec().output_file());
    CHECK(std::vector<std::int16_t>(samples.begin(), samples.begin() + 1024) == audio);
}

TEST_CASE("the journal's files and session", "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "recorded_audio.wav";
    RecordingJournal journal(output, 44100, 1, 2, kNoError);
    CHECK(journal.directory().parent_path() == fv::recovery_directory(output));
    CHECK(fv::recovery_directory(output).filename() == ".recorded_audio-recovery");
    CHECK(journal.directory().filename().string().size() == 32);
    CHECK(std::filesystem::exists(journal.directory() / "active.lock"));
    const auto format = fv::session_format(std::string(
        reinterpret_cast<const char*>(read_all(journal.directory() / "session.json").data()),
        read_all(journal.directory() / "session.json").size()));
    CHECK(format.rate == 44100);
    CHECK(format.channels == 1);
    CHECK(format.sample_width == 2);
    REQUIRE(journal.append(bytes_of({1, 2, 3})));
    REQUIRE(journal.wait_until_written(milliseconds(2000)));
    CHECK(journal.read_from(2) == string_of({2, 3}));
    CHECK(journal.read_from(6) == std::string());
    CHECK_FALSE(journal.read_from(7).has_value());
    CHECK_FALSE(journal.read_from(-1).has_value());
    // An empty session is removed when released.
    RecordingJournal empty(output, 44100, 1, 2, kNoError);
    const auto empty_directory = empty.directory();
    REQUIRE(empty.close());
    CHECK_FALSE(std::filesystem::exists(empty_directory));
    REQUIRE(journal.close(true));
}

TEST_CASE("a session without its files is removed and a broken one is kept",
          "[test_recording_journal]") {
    const fv::testkit::TempDataRoot root;
    const auto output = root.path() / "recorded_audio.wav";
    const auto recovery = fv::recovery_directory(output);
    // The app died after making the folder and before writing into it.
    std::filesystem::create_directories(recovery / "aaaa");
    // A session whose format is not one this app records.
    std::filesystem::create_directories(recovery / "bbbb");
    {
        std::ofstream(recovery / "bbbb" / "session.json")
            << R"({"version": 1, "rate": 7999, "channels": 1, "sample_width": 2, "created": "2026-11-14T09:30:00"})";
        std::ofstream(recovery / "bbbb" / "audio.pcm", std::ios::binary) << "\x01\x02";
    }
    LogCapture logs("services.recording_journal");
    CHECK(fv::recover_recordings(output, root.path() / "saved").empty());
    CHECK_FALSE(std::filesystem::exists(recovery / "aaaa"));
    CHECK(std::filesystem::exists(recovery / "bbbb" / "audio.pcm"));
    CHECK(logs.starting_with("Could not recover interrupted dictation").size() == 1);
}

// The killed child of "recovery after process is killed": journal one second of audio, say it
// is ready, and wait to be killed.
namespace fv::recorder_test {
int journal_child(const std::filesystem::path& output, const std::filesystem::path& ready) {
    RecordingJournal journal(output, 16000, 1, 2, [](const std::string&) {});
    const auto pcm = repeated({0x10, 0x00}, 16000);
    if (!journal.append(pcm) || !journal.wait_until_written(milliseconds(10000))) {
        return 2;
    }
    { std::ofstream(ready) << "ready"; }
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}

// The killed child of "a recording killed mid-capture is recovered into recordings": a
// dictation recorder in data root `root` on a fake microphone, 20 blocks captured and written,
// then it waits to be killed with the stream still open.
int recorder_child(const std::filesystem::path& root, const std::filesystem::path& ready) {
    fv::MapEnvReader env{
        {std::string(fv::identity().env_prefix) + "DATA_DIR", fv::path_to_utf8(root)}};
    fv::pin_per_user_folders(env, root / "per-user");
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    auto fake = std::make_shared<fv::audio::testing::FakeBackend>();
    fake->set_layout({{"MME", 0, -1}}, {{"Microphone (USB)", 0, 1, 0, 44100.0}}, 0);
    fv::Recorder recorder(*fake, paths);
    if (!recorder.start_recording()) {
        return 2;
    }
    const auto stream = fake->streams().back();
    for (int i = 0; i < 20; ++i) {
        stream->deliver(tone(0x10));
    }
    if (!recorder.sync_capture(milliseconds(10000)) || !recorder.is_recording()) {
        return 3;
    }
    { std::ofstream(ready) << "ready"; }
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
} // namespace fv::recorder_test
