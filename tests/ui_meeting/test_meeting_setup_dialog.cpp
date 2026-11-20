// Meeting Mode's first-run setup dialog (tests/test_meeting_setup.py's dialog tests): the steps
// answer through the dialog's hooks or run the real audio check over audio::FakeBackend, so no
// device, network or settings file of this computer is touched.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>

#include "audio/testing/fake_backend.h"
#include "meeting/platform_policy.h"
#include "meeting/setup_check.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "tab_support.h"
#include "ui_meeting/meeting_setup_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::MeetingSetupDialog;
using fv::meeting::AudioCheck;
using fv::meeting::ModelSetup;

namespace {

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

// _wait_until_idle: the background step has finished and its result is shown.
void waitUntilIdle(MeetingSetupDialog& dialog) {
    REQUIRE(pumpUntil([&dialog] { return dialog.audioButton()->isEnabled(); }));
}

std::shared_ptr<fv::audio::testing::FakeBackend> fakeDevices() {
    auto fake = std::make_shared<fv::audio::testing::FakeBackend>();
    fake->set_layout({{"MME", 0, -1}}, {{"Microphone", 0, 1, 0, 16000.0}}, 0);
    fake->set_loopbacks({{"speakers", "Speakers", true, 16000, 1}});
    return fake;
}

MeetingSetupDialog::Hooks modelsReady() {
    MeetingSetupDialog::Hooks hooks;
    hooks.inspect = [](const fv::meeting::SetupDeps&) {
        return ModelSetup{"Parakeet", "", "", false};
    };
    return hooks;
}

} // namespace

TEST_CASE("test_setup_completion", "[test_meeting_setup]") {
    CHECK_FALSE(fv::meeting::setup_complete(Json::object()));
    CHECK(fv::meeting::setup_complete({{"meeting_setup_complete", true}}));
}

TEST_CASE("test_dialog_persists_only_completed_setup", "[test_meeting_setup][widget]") {
    auto fake = fakeDevices();
    std::vector<AudioCheck> answers{{"Signal detected", "No signal detected"},
                                    {"Signal detected", "Signal detected"}};
    std::atomic<int> checks{0};
    // Written on the step's thread, read once it has answered.
    std::atomic<bool> allAllowed{true};
    MeetingSetupDialog::Hooks hooks = modelsReady();
    hooks.checkAudio = [&](fv::audio::Backend&, const fv::meeting::SetupDeps&,
                           const fv::CancelToken&, bool allowed) {
        allAllowed = allAllowed && allowed;
        return answers.at(static_cast<std::size_t>(checks++));
    };
    int saves = 0;
    hooks.markComplete = [&saves] {
        ++saves;
    };
    MeetingSetupDialog dialog({}, *fake, true, nullptr, hooks);
    int completed = 0;
    QObject::connect(&dialog, &MeetingSetupDialog::setupCompleted, &dialog,
                     [&completed] { ++completed; });

    waitUntilIdle(dialog);
    CHECK(dialog.modelStatus()->text() ==
          QStringLiteral("Speech engine: Parakeet\nRequired local files are ready."));
    CHECK_FALSE(dialog.prepareButton()->isEnabled());
    CHECK_FALSE(dialog.finishButton()->isEnabled());
    CHECK_FALSE(dialog.skipButton()->isEnabled());
    dialog.audioButton()->click();
    CHECK_FALSE(dialog.audioButton()->isEnabled());
    waitUntilIdle(dialog);
    CHECK(dialog.skipButton()->isEnabled());
    CHECK_FALSE(dialog.finishButton()->isEnabled());
    // On Linux the dialog adds how to get system audio heard, and a verified system audio makes
    // the meeting require it (meeting_setup.py _show_audio, system_audio_policy).
    const bool onLinux = fv::meeting::current_platform().platform.starts_with("linux");
    QString unheard =
        QStringLiteral("Microphone: Signal detected\nSystem audio: No signal detected");
    if (onLinux) {
        unheard += QStringLiteral("\nCheck the default output in Sound settings and make sure "
                                  "PulseAudio or PipeWire-Pulse is running.");
    }
    CHECK(dialog.audioStatus()->text() == unheard);
    CHECK(saves == 0);
    dialog.audioButton()->click();
    waitUntilIdle(dialog);
    CHECK(dialog.finishButton()->isEnabled());
    CHECK(dialog.finishButton()->isActive());
    CHECK_FALSE(dialog.skipButton()->isEnabled());
    dialog.finishButton()->click();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(saves == 1);
    CHECK(completed == 1);
    CHECK(dialog.systemAudioPolicy() ==
          (onLinux ? QStringLiteral("required") : QStringLiteral("auto")));
    CHECK(allAllowed);
}

TEST_CASE("test_closing_waits_for_audio_devices_to_be_released", "[test_meeting_setup][widget]") {
    auto fake = fakeDevices();
    MeetingSetupDialog::Hooks hooks = modelsReady();
    std::atomic<bool> returned{false};
    std::atomic<bool> closedWhenReturned{false};
    std::atomic<bool> cancelled{false};
    // The real check, long enough that only the dialog's cancel ends it.
    hooks.checkAudio = [&](fv::audio::Backend& backend, const fv::meeting::SetupDeps& deps,
                           const fv::CancelToken& cancel, bool allowed) {
        AudioCheck result =
            fv::meeting::check_audio(backend, deps, cancel, allowed, std::chrono::seconds(30));
        cancelled = cancel.cancelled();
        closedWhenReturned = fake->open_streams() == 0;
        returned = true;
        return result;
    };
    MeetingSetupDialog dialog({}, *fake, true, nullptr, hooks);
    std::vector<int> finished;
    QObject::connect(&dialog, &QDialog::finished, &dialog,
                     [&finished](int result) { finished.push_back(result); });
    dialog.show();
    waitUntilIdle(dialog);
    dialog.audioButton()->click();
    REQUIRE(pumpUntil([&fake] {
        int started = 0;
        for (const auto& stream : fake->streams()) {
            started += stream->started() ? 1 : 0;
        }
        return started == 2;
    }));

    dialog.close();
    CHECK(dialog.isVisible());
    CHECK_FALSE(dialog.isEnabled());
    CHECK(finished.empty());
    CHECK(dialog.audioStatus()->text() == QStringLiteral("Closing the audio check…"));

    REQUIRE(pumpUntil([&dialog] { return !dialog.isVisible(); }));
    CHECK(returned);
    CHECK(cancelled);
    // The devices were released before the dialog finished.
    CHECK(closedWhenReturned);
    CHECK(fake->open_streams() == 0);
    for (const auto& stream : fake->streams()) {
        CHECK(stream->closed());
    }
    CHECK(finished == std::vector<int>{QDialog::Rejected});
}

TEST_CASE("test_setup_fits_narrow_windows", "[test_meeting_setup][widget]") {
    for (const int percent : {100, 130}) {
        DYNAMIC_SECTION("font " << percent) {
            fvtest::AppTheme theme(QStringLiteral("dark"), percent);
            auto fake = fakeDevices();
            MeetingSetupDialog dialog({}, *fake, true, nullptr, modelsReady());
            dialog.show();
            dialog.resize(460, dialog.sizeHint().height());
            waitUntilIdle(dialog);
            // Let the layout follow the model check's text.
            pumpUntil([] { return false; }, 50);
            CHECK(dialog.width() <= 460);
            for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>()) {
                if (!button->isVisible()) {
                    continue;
                }
                INFO(button->text().toStdString());
                CHECK(dialog.rect().contains(button->mapTo(&dialog, button->rect().topLeft())));
                CHECK(dialog.rect().contains(button->mapTo(&dialog, button->rect().bottomRight())));
                CHECK(button->width() >= button->minimumSizeHint().width());
            }
            for (fv::WrappedLabel* label : dialog.findChildren<fv::WrappedLabel*>()) {
                if (label->isVisible()) {
                    INFO(label->text().toStdString());
                    CHECK(label->height() >= label->heightForWidth(label->width()));
                }
            }
            dialog.close();
        }
    }
}

TEST_CASE("Start anyway waits for an audio check and a meeting that is not ready", "[widget]") {
    auto fake = fakeDevices();
    MeetingSetupDialog::Hooks hooks;
    hooks.inspect = [](const fv::meeting::SetupDeps&) {
        return ModelSetup{"Parakeet TDT 0.6B v3", "parakeet-v3", "", true};
    };
    hooks.checkAudio = [](fv::audio::Backend&, const fv::meeting::SetupDeps&,
                          const fv::CancelToken&, bool) {
        return AudioCheck{"No signal detected", "Signal detected"};
    };
    MeetingSetupDialog dialog({}, *fake, true, nullptr, hooks);
    CHECK(dialog.cancelButton()->text() == QStringLiteral("Back"));
    CHECK(dialog.finishButton()->text() == QStringLiteral("Start Meeting"));
    CHECK(dialog.isBusy());
    CHECK_FALSE(dialog.audioButton()->isEnabled());
    waitUntilIdle(dialog);
    CHECK(dialog.modelStatus()->text() ==
          QStringLiteral("Speech engine: Parakeet TDT 0.6B v3\nStill needed: Parakeet TDT 0.6B "
                         "v3, speaker identification model"));
    CHECK(dialog.prepareButton()->isEnabled());
    CHECK_FALSE(dialog.skipButton()->isEnabled());
    dialog.audioButton()->click();
    waitUntilIdle(dialog);
    CHECK(dialog.skipButton()->isEnabled());
    CHECK(dialog.audioStatus()->text() ==
          QStringLiteral("Microphone: No signal detected\nSystem audio: Signal detected\nSpeak "
                         "close to the microphone or turn up its input level, then retry."));
    dialog.skipButton()->click();
    CHECK(dialog.result() == QDialog::Accepted);
}

TEST_CASE("Download missing files shows its progress and what is ready", "[widget]") {
    auto fake = fakeDevices();
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    MeetingSetupDialog::Hooks hooks;
    hooks.inspect = [](const fv::meeting::SetupDeps&) {
        return ModelSetup{"Parakeet TDT 0.6B v3", "parakeet-v3", "", false};
    };
    hooks.prepare = [&](const fv::meeting::SetupDeps&, const fv::CancelToken&,
                        const std::function<void(std::string)>& status) {
        status("Downloading Parakeet TDT 0.6B v3…");
        std::unique_lock lock(mutex);
        released.wait(lock, [&release] { return release; });
        return ModelSetup{"Parakeet TDT 0.6B v3", "", "", false};
    };
    MeetingSetupDialog dialog({}, *fake, false, nullptr, hooks);
    CHECK(dialog.cancelButton()->text() == QStringLiteral("Close"));
    CHECK(dialog.finishButton()->text() == QStringLiteral("Done"));
    CHECK(dialog.skipButton()->isHidden());
    waitUntilIdle(dialog);
    dialog.prepareButton()->click();
    REQUIRE(pumpUntil([&dialog] {
        return dialog.modelStatus()->text() == QStringLiteral("Downloading Parakeet TDT 0.6B v3…");
    }));
    CHECK_FALSE(dialog.audioButton()->isEnabled());
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    released.notify_all();
    waitUntilIdle(dialog);
    CHECK(dialog.modelStatus()->text() ==
          QStringLiteral("Speech engine: Parakeet TDT 0.6B v3\nRequired local files are ready."));
    CHECK_FALSE(dialog.prepareButton()->isEnabled());
}

TEST_CASE("a step that fails says so and leaves the dialog usable", "[widget]") {
    auto fake = fakeDevices();
    MeetingSetupDialog::Hooks hooks = modelsReady();
    hooks.checkAudio = [](fv::audio::Backend&, const fv::meeting::SetupDeps&,
                          const fv::CancelToken&, bool) -> AudioCheck {
        throw std::runtime_error("An audio check is still closing. Try again in a moment.");
    };
    MeetingSetupDialog dialog({}, *fake, true, nullptr, hooks);
    waitUntilIdle(dialog);
    dialog.audioButton()->click();
    waitUntilIdle(dialog);
    CHECK(dialog.audioStatus()->text() ==
          QStringLiteral("Couldn't complete this step. An audio check is still closing. Try "
                         "again in a moment."));
    CHECK_FALSE(dialog.skipButton()->isEnabled());
}

TEST_CASE("Done saves meeting_setup_complete, and a failed save only logs", "[widget]") {
    auto fake = fakeDevices();
    for (const bool fails : {false, true}) {
        DYNAMIC_SECTION("save fails " << fails) {
            fv::MemorySettingsStore store;
            if (fails) {
                store.file().fail_next_replace("Disk full");
            }
            MeetingSetupDialog::Hooks hooks = modelsReady();
            hooks.checkAudio = [](fv::audio::Backend&, const fv::meeting::SetupDeps&,
                                  const fv::CancelToken&, bool) {
                return AudioCheck{"Signal detected", "Signal detected"};
            };
            hooks.settings = &store;
            MeetingSetupDialog dialog({}, *fake, false, nullptr, hooks);
            waitUntilIdle(dialog);
            dialog.audioButton()->click();
            waitUntilIdle(dialog);
            dialog.finishButton()->click();
            CHECK(dialog.result() == QDialog::Accepted);
            CHECK(fv::meeting::setup_complete(*store.snapshot()) == !fails);
        }
    }
}
