#pragma once

// The fixture of OpenWhisper's tests/test_walkthrough_runtime.py: a TranscriptionRuntime over the
// fakes whose recorder saves a WAV the test wrote, a WalkthroughSession in the rig's data root,
// the FakeRenderer for the PDF and Word documents, a delivery port that records what it is asked
// (the FakeUI's copy_files_to_clipboard, show_walkthrough_ready, paste_walkthrough and
// dismiss_walkthrough_panel), a capture service that names the app in front, and a write-up
// that answers as the test says. Where Python patches build_documents, the real one runs with
// the FakeRenderer and the test reads the manifest back.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "dictation/walkthrough_delivery.h"
#include "dictation/walkthrough_job.h"
#include "focus/capture_service.h"
#include "runtime_support.h"
#include "speech/testing/fake_backend.h"
#include "walkthrough/document.h"
#include "walkthrough/library.h"
#include "walkthrough/renderer.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_renderer.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace fv::dictation::testing {

inline constexpr int kWalkthroughRate = 16'000;

// int16 samples of a 220 Hz tone at 8000 amplitude (the test's _tone), or of silence.
inline std::vector<std::int16_t> tone(double seconds) {
    const auto count = static_cast<std::size_t>(seconds * kWalkthroughRate);
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / kWalkthroughRate;
        samples[i] =
            static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * 220.0 * t));
    }
    return samples;
}

inline std::vector<std::int16_t> silence(double seconds) {
    return std::vector<std::int16_t>(static_cast<std::size_t>(seconds * kWalkthroughRate));
}

inline void put_le(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>(value >> (8 * i) & 0xFF));
    }
}

// A mono WAV at 16 kHz with `pieces` one after another (the test's _write_recording), as
// 16-bit PCM, or 8-bit unsigned PCM when `bits` is 8.
inline void write_recording(const std::filesystem::path& path,
                            std::initializer_list<std::vector<std::int16_t>> pieces,
                            int bits = 16) {
    std::string data;
    for (const auto& piece : pieces) {
        for (const std::int16_t sample : piece) {
            if (bits == 8) {
                data.push_back(static_cast<char>((sample >> 8) + 128));
            } else {
                put_le(data, static_cast<std::uint16_t>(sample), 2);
            }
        }
    }
    const int width = bits / 8;
    std::string wav = "RIFF";
    put_le(wav, static_cast<std::uint32_t>(36 + data.size()), 4);
    wav += "WAVEfmt ";
    put_le(wav, 16, 4);
    put_le(wav, 1, 2); // PCM
    put_le(wav, 1, 2); // mono
    put_le(wav, static_cast<std::uint32_t>(kWalkthroughRate), 4);
    put_le(wav, static_cast<std::uint32_t>(kWalkthroughRate * width), 4);
    put_le(wav, static_cast<std::uint32_t>(width), 2);
    put_le(wav, static_cast<std::uint32_t>(bits), 2);
    wav += "data";
    put_le(wav, static_cast<std::uint32_t>(data.size()), 4);
    wav += data;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(wav.data(), static_cast<std::streamsize>(wav.size()));
}

// The test's _image: 32 x 24 of #ff4f3a.
inline walkthrough::ShotImagePtr
shot_image(walkthrough::FakeShotImage::Save save = walkthrough::FakeShotImage::Save::Works) {
    return std::make_shared<walkthrough::FakeShotImage>(32, 24, 0xFF4F3A, save);
}

// The FakeUI's walkthrough half. Affinity: thread-safe.
class FakeWalkthroughDelivery final : public WalkthroughDeliveryPort {
public:
    using Copy = std::pair<std::vector<std::filesystem::path>, std::string>;
    using Ready = std::pair<walkthrough::WalkthroughResult, bool>;
    using Paste = std::pair<walkthrough::WalkthroughResult, std::optional<focus::AppIdentity>>;

    bool copy_files_to_clipboard(const std::vector<std::filesystem::path>& paths,
                                 const std::string& text) override {
        std::lock_guard lock(mutex_);
        copied_.emplace_back(paths, text);
        return copies;
    }
    void show_walkthrough_ready(const walkthrough::WalkthroughResult& result,
                                bool copied) override {
        std::lock_guard lock(mutex_);
        ready_.emplace_back(result, copied);
    }
    void paste_walkthrough(const walkthrough::WalkthroughResult& result,
                           std::optional<focus::AppIdentity> target) override {
        std::lock_guard lock(mutex_);
        // The card shows the walkthrough that is pasted.
        pasted_after_its_card_ =
            pasted_after_its_card_ && !ready_.empty() && ready_.back().first == result;
        pasted_.emplace_back(result, std::move(target));
    }
    void dismiss_walkthrough_panel() override {
        std::lock_guard lock(mutex_);
        ++dismissed_;
    }

    std::vector<Copy> copied() const {
        std::lock_guard lock(mutex_);
        return copied_;
    }
    std::vector<Ready> ready() const {
        std::lock_guard lock(mutex_);
        return ready_;
    }
    std::vector<Paste> pasted() const {
        std::lock_guard lock(mutex_);
        return pasted_;
    }
    int dismissed() const {
        std::lock_guard lock(mutex_);
        return dismissed_;
    }
    bool pasted_after_its_card() const {
        std::lock_guard lock(mutex_);
        return pasted_after_its_card_;
    }

    bool copies = true;

private:
    mutable std::mutex mutex_;
    std::vector<Copy> copied_;
    std::vector<Ready> ready_;
    std::vector<Paste> pasted_;
    int dismissed_ = 0;
    bool pasted_after_its_card_ = true;
};

// focus_context's service as the test patches it: the app in front, and how often it was asked.
// Affinity: thread-safe.
class FrontAppService final : public focus::CaptureService {
public:
    explicit FrontAppService(std::optional<focus::AppIdentity> front = std::nullopt)
        : front_(std::move(front)) {}

    std::shared_ptr<focus::FocusRequest> request(bool, bool) override {
        std::lock_guard lock(mutex_);
        ++requests_;
        return focus::FocusRequest::resolved({});
    }
    std::optional<focus::AppIdentity> current_identity() override {
        std::lock_guard lock(mutex_);
        ++identities_;
        return front_;
    }
    void reread(const std::optional<focus::AppIdentity>&,
                std::function<void(std::optional<focus::TextContext>)> callback) override {
        callback(std::nullopt);
    }
    void shutdown() override {}

    int requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    int identities() const {
        std::lock_guard lock(mutex_);
        return identities_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<focus::AppIdentity> front_;
    int requests_ = 0;
    int identities_ = 0;
};

// focus_context.AppIdentity("chat", "Chat", pid=42, window="chat-window").
inline focus::AppIdentity chat_window() {
    focus::AppIdentity identity;
    identity.app_id = "chat";
    identity.name = "Chat";
    identity.pid = 42;
    identity.window = "chat-window";
    return identity;
}

// The engine's answer for each section's clip, in the order decoded.
inline speech::FakeScript words(std::vector<std::string> texts = {"first part", "second part",
                                                                  "last part"}) {
    speech::FakeScript script;
    script.texts = std::move(texts);
    return script;
}

// The h fixture. The write-up strand runs at once unless `queue_write_ups` (the test's
// walkthrough_executor that only queues).
struct WalkthroughRig {
    struct Options {
        Json settings = Json::object();
        speech::FakeScript script = words();
        bool queue_write_ups = false;
        std::optional<focus::AppIdentity> front;
        // The data root's captures folder becomes the runtime's captures_root.
        bool captures_root = false;
    };

    // Two constructors, not a `= {}` default argument: GCC does not accept the member
    // initializers of a nested struct in a default argument of its enclosing class.
    WalkthroughRig() : WalkthroughRig(Options()) {}

    explicit WalkthroughRig(Options options)
        : front(options.front), rig(rig_options(options)),
          renderer(std::make_shared<walkthrough::FakeRenderer>()),
          audio(rig.root.path() / "recording.wav") {
        walkthrough::set_document_renderer(renderer);
        rig.runtime->set_walkthrough_delivery(&delivery);
        if (options.queue_write_ups) {
            rig.runtime->set_walkthrough_strand(&queued_strand);
        } else {
            rig.runtime->set_walkthrough_strand(&inline_strand);
        }
        rig.runtime->set_walkthrough_write_up([this](const WalkthroughWriteUpRequest& request) {
            {
                std::lock_guard lock(mutex);
                write_ups.push_back(request);
            }
            if (!write_up) {
                return request.result;
            }
            return write_up(request);
        });
        links.push_back(
            rig.runtime->recording_destination_changed.connect(rig.ui, [this](std::string label) {
                std::lock_guard lock(mutex);
                destinations.push_back(std::move(label));
            }));
        walkthrough::WalkthroughSession::Options session_options;
        session_options.started_at =
            std::chrono::local_days{std::chrono::year{2026} / std::chrono::October / 21} +
            std::chrono::hours(14) + std::chrono::minutes(32);
        session_options.root = captures();
        session = std::make_shared<walkthrough::WalkthroughSession>(std::move(session_options));
    }
    ~WalkthroughRig() {
        links.clear();
        if (rig.runtime) {
            rig.runtime->set_walkthrough_delivery(nullptr);
            rig.runtime->set_walkthrough_strand(nullptr);
        }
        walkthrough::set_document_renderer(nullptr);
    }
    WalkthroughRig(const WalkthroughRig&) = delete;
    WalkthroughRig& operator=(const WalkthroughRig&) = delete;

    RigOptions rig_options(const Options& options) {
        RigOptions made;
        made.settings = paste_settings();
        for (const auto& [key, value] : options.settings.items()) {
            made.settings[key] = value;
        }
        made.script = options.script;
        made.recorder = [this](const std::filesystem::path& root) -> RecorderPort& {
            // A save hands the runtime the recording the test wrote.
            recorder = std::make_unique<FakeRecorder>(root / "recorded_audio.wav", rig.trace,
                                                      root / "recording.wav");
            return *recorder;
        };
        const bool with_root = options.captures_root;
        made.services = [this, with_root](TranscriptionServices& services) {
            services.capture = &front;
            if (with_root) {
                services.captures_root =
                    services.history->recordings_folder().parent_path() / "captures";
            }
        };
        return made;
    }

    TranscriptionRuntime& runtime() { return *rig.runtime; }
    std::filesystem::path captures() const { return rig.root.path() / "captures"; }
    std::filesystem::path folder() const { return session->folder(); }

    // _record: starts the walkthrough, keeps a screenshot at each time, makes `edit` to the
    // board and stops it; the stop's post-roll, save and transcription wait on the jobs strand.
    void record(const std::vector<double>& shots_at, double length_s = 6.0,
                const std::function<void(walkthrough::WalkthroughSession&)>& edit = {}) {
        REQUIRE(runtime().start_walkthrough(session));
        for (const double kept_at : shots_at) {
            recorder->duration = kept_at;
            REQUIRE(session->add_shot(shot_image(), kept_at));
        }
        if (edit) {
            edit(*session);
        }
        recorder->duration = length_s;
        runtime().stop_recording();
    }

    // The decodes the engine was asked for, in order.
    std::vector<speech::FakeCall> decodes() const {
        std::vector<speech::FakeCall> found;
        for (auto& call : rig.engine->calls()) {
            if (call.op == "decode") {
                found.push_back(call);
            }
        }
        return found;
    }

    // The walkthrough the documents were built from (build_documents' arguments, read back).
    walkthrough::Walkthrough built() const { return walkthrough::load_document(folder()); }
    std::vector<std::string> built_texts() const {
        std::vector<std::string> texts;
        for (const auto& section : built().sections) {
            texts.push_back(section.text);
        }
        return texts;
    }

    std::vector<WalkthroughWriteUpRequest> write_up_calls() const {
        std::lock_guard lock(mutex);
        return write_ups;
    }
    std::vector<std::string> destination_labels() const {
        std::lock_guard lock(mutex);
        return destinations;
    }
    bool status_seen(const std::string& status) const {
        for (const auto& seen : rig.statuses()) {
            if (seen == status) {
                return true;
            }
        }
        return false;
    }
    bool status_containing(const std::string& part) const {
        for (const auto& seen : rig.statuses()) {
            if (seen.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    FrontAppService front;
    // Made by the rig, before the runtime.
    std::unique_ptr<FakeRecorder> recorder;
    RuntimeRig rig;
    std::shared_ptr<walkthrough::FakeRenderer> renderer;
    FakeWalkthroughDelivery delivery;
    InlineExecutor inline_strand;
    ManualExecutor queued_strand;
    std::filesystem::path audio;
    std::shared_ptr<walkthrough::WalkthroughSession> session;
    // What the write-up does with its request; empty hands back the walkthrough as spoken.
    std::function<walkthrough::WalkthroughResult(const WalkthroughWriteUpRequest&)> write_up;
    mutable std::mutex mutex;
    std::vector<WalkthroughWriteUpRequest> write_ups;
    std::vector<std::string> destinations;
    std::vector<Subscription> links;
};

} // namespace fv::dictation::testing
