// SpeechService's calls on a slot's loaded backend from the caller's thread:
// supports_recognition, prepare_for_transcription and large_file_size_mb.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "base/testing/manual_executor.h"
#include "speech/service.h"
#include "speech_test_support.h"

using fv::speech::BackendConfig;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::test::parakeet_config;
using fv::speech::test::pump_until;

namespace {

struct Calls {
    std::atomic<int> prepared{0};
    std::atomic<int> released{0};
    std::thread::id prepared_on;
};

// A backend that prepares, takes hints and splits large files, as the cloud backend does.
class PreparingBackend final : public fv::speech::Backend {
public:
    PreparingBackend(std::shared_ptr<Calls> calls, std::string id)
        : calls_(std::move(calls)), id_(std::move(id)) {}
    std::string_view id() const override { return id_; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {ReadyState::Ready, "Fake | cloud", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob&) override { return {}; }
    void release() override { ++calls_->released; }

    bool supports_recognition() const override { return true; }
    bool prepares_for_transcription() const override { return true; }
    void prepare_for_transcription() override {
        calls_->prepared_on = std::this_thread::get_id();
        ++calls_->prepared;
    }
    std::optional<double> large_file_size_mb(const std::filesystem::path& path) const override {
        return path.filename() == "big.wav" ? std::optional<double>(42.5) : std::nullopt;
    }

private:
    std::shared_ptr<Calls> calls_;
    std::string id_;
};

class PlainBackend final : public fv::speech::Backend {
public:
    std::string_view id() const override { return "parakeet"; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {ReadyState::Ready, "Fake | cpu", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob&) override { return {}; }
    void release() override {}
};

} // namespace

TEST_CASE("the facade reaches the slot's loaded backend on the caller's thread",
          "[service_facade]") {
    auto calls = std::make_shared<Calls>();
    fv::ManualExecutor notify;
    fv::speech::SpeechService service(
        [calls](const BackendConfig& config) -> std::unique_ptr<fv::speech::Backend> {
            if (config.backend == "api") {
                return std::make_unique<PreparingBackend>(calls, "api");
            }
            return std::make_unique<PlainBackend>();
        },
        notify);

    // Nothing loaded: the Backend base's answers.
    CHECK_FALSE(service.supports_recognition(Slot::Dictation));
    CHECK_FALSE(service.prepares_for_transcription(Slot::Dictation));
    service.prepare_for_transcription(Slot::Dictation);
    CHECK(calls->prepared == 0);
    CHECK_FALSE(service.large_file_size_mb(Slot::Dictation, "big.wav").has_value());

    BackendConfig api;
    api.backend = "api";
    service.select(Slot::Dictation, api);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Ready; }));
    CHECK(service.supports_recognition(Slot::Dictation));
    CHECK(service.prepares_for_transcription(Slot::Dictation));
    service.prepare_for_transcription(Slot::Dictation);
    CHECK(calls->prepared == 1);
    CHECK(calls->prepared_on == std::this_thread::get_id());
    CHECK(service.large_file_size_mb(Slot::Dictation, "big.wav") == std::optional<double>(42.5));
    CHECK_FALSE(service.large_file_size_mb(Slot::Dictation, "small.wav").has_value());
    // The meeting slot uses the dictation slot's engine while nothing is selected for it.
    CHECK(service.supports_recognition(Slot::Meeting));

    // Released for a meeting, the backend is gone; restored, it is back.
    service.release_local(Slot::Dictation);
    REQUIRE(pump_until(notify, [&] {
        return calls->released == 1 && !service.supports_recognition(Slot::Dictation);
    }));
    CHECK_FALSE(service.large_file_size_mb(Slot::Dictation, "big.wav").has_value());
    service.restore_local(Slot::Dictation);
    REQUIRE(pump_until(notify, [&] { return service.supports_recognition(Slot::Dictation); }));

    // A reload to a backend that has nothing to prepare.
    service.select(Slot::Dictation, parakeet_config("cpu"));
    REQUIRE(pump_until(notify, [&] {
        return calls->released == 2 &&
               service.readiness(Slot::Dictation).state == ReadyState::Ready;
    }));
    CHECK_FALSE(service.supports_recognition(Slot::Dictation));
    CHECK_FALSE(service.prepares_for_transcription(Slot::Dictation));
    service.prepare_for_transcription(Slot::Dictation);
    CHECK(calls->prepared == 1);
}

namespace {

// A backend whose capabilities say whether it takes whole files.
class FilesBackend final : public fv::speech::Backend {
public:
    explicit FilesBackend(bool files) : files_(files) {}
    std::string_view id() const override { return files_ ? "api" : "parakeet"; }
    fv::speech::Capabilities capabilities() const override {
        fv::speech::Capabilities capabilities;
        capabilities.accepts_files = files_;
        capabilities.windows = !files_;
        return capabilities;
    }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {ReadyState::Ready, "Fake", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob&) override { return {}; }
    void release() override {}

private:
    bool files_;
};

} // namespace

TEST_CASE("accepts_files answers from the backend loaded in the slot", "[service_facade]") {
    fv::ManualExecutor notify;
    fv::speech::SpeechService service(
        [](const BackendConfig& config) -> std::unique_ptr<fv::speech::Backend> {
            return std::make_unique<FilesBackend>(config.backend == "api");
        },
        notify);

    // Nothing loaded takes nothing.
    CHECK_FALSE(service.accepts_files(Slot::Dictation));

    BackendConfig api;
    api.backend = "api";
    service.select(Slot::Dictation, api);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Ready; }));
    CHECK(service.accepts_files(Slot::Dictation));
    // The meeting slot follows the dictation slot's engine.
    CHECK(service.accepts_files(Slot::Meeting));

    service.select(Slot::Dictation, parakeet_config("cpu"));
    REQUIRE(pump_until(notify, [&] {
        return service.readiness(Slot::Dictation).state == ReadyState::Ready &&
               !service.accepts_files(Slot::Dictation);
    }));

    service.release_local(Slot::Dictation);
    REQUIRE(pump_until(
        notify, [&] { return service.readiness(Slot::Dictation).state == ReadyState::Offline; }));
    CHECK_FALSE(service.accepts_files(Slot::Dictation));
}