#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "recorder/recorder.h"
#include "testkit/testkit.h"
#include "ui_record/microphone_check.h"

namespace fvtest {

// One fake microphone, a data root of its own and a recorder factory that writes only to a
// private path inside it, as the guided setup's check does: the microphone check never records.
// Destroy the widget before the rig (the recorder uses the backend).
struct MicrophoneRig {
    MicrophoneRig()
        : backend(std::make_shared<fv::audio::testing::FakeBackend>()),
          paths(fv::Paths::resolve(fv::identity(), fv::system_env())) {
        backend->set_layout({fv::audio::testing::FakeHostApi{"MME", 0, -1}},
                            {fv::audio::testing::FakeDevice{"Test Microphone", 0, 1, 0, 44100.0}},
                            0);
    }

    // The factory a MicrophoneCheck takes; `recorder` points at the one it made last.
    fv::MicrophoneCheck::RecorderFactory factory() {
        return [this] {
            fv::RecorderOptions options;
            options.output_file = root.path() / "microphone-check.wav";
            auto made = std::make_unique<fv::Recorder>(*backend, paths, std::move(options));
            recorder = made.get();
            return made;
        };
    }
    std::filesystem::path outputFile() const { return root.path() / "microphone-check.wav"; }

    fv::testkit::TempDataRoot root;
    std::shared_ptr<fv::audio::testing::FakeBackend> backend;
    fv::Paths paths;
    fv::Recorder* recorder = nullptr;
};

} // namespace fvtest
