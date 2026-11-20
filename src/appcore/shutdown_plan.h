#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "base/clock.h"

namespace fv {

// The steps of Quit, in the order they run (OpenWhisper's ApplicationController.cleanup).
// Backends and connections are interrupted before the strands are joined, because a strand may be
// inside a model load or a network request; the persistence strand is drained, never canceled,
// so accepted transcripts survive Quit; the database closes last.
enum class ShutdownStep {
    BackupCoordinator,   // backup coordinator shutdown
    StartupWork,         // the startup strand: wait, cancel what is queued
    CancelRoot,          // the root cancel source every wait listens to
    ModelBatchDownload,  // "cancel model batch download"
    AutoMode,            // "Error stopping Auto Mode"
    TranscriptionBegin,  // transcription runtime begin_shutdown
    WalkthroughLibrary,  // the UI's walkthrough library edit
    CommandRuntime,      // "Error during command runtime cleanup"
    Automation,          // "Error stopping automation"
    PlaybackAndFocus,    // "Error stopping playback or focus capture"
    CancelTranscription, // "Error canceling transcription"
    Timers,              // "Error stopping watchdog timers" (the reload timer among them)
    InterruptBackends,   // cancel and close every speech backend
    MeetingRuntime,      // "Error during meeting runtime cleanup"
    HotkeyRuntime,       // "Error during hotkey runtime cleanup"
    HotkeyManager,       // "Error during hotkey cleanup"
    Recorder,            // "Error during recorder cleanup"
    Streaming,           // "Error during streaming cleanup"
    ComponentInstalls,   // "Error cancelling component installs"
    RecordSync,          // "Error stopping the record sync"
    RemoteEngine,        // "Error stopping the remote engine host"
    Strands,             // jobs, components, walkthrough-library: cancel queued, wait
    Persistence,         // drain, never cancel
    Backends,            // "Error during transcription backends cleanup"
    UiController,        // "Error during UI controller cleanup"
    Database,            // "Error closing database"
};

// Quit as one ordered list that each feature adds its steps to. run() runs them by step, and
// within a step in the order they were added. A step with a failure label is attempted: an
// exception is logged as "{failure}: {exception}" and the next step runs. A step without one
// is not, as in OpenWhisper: its exception ends run() and reaches the caller.
// Afterwards the shutdown_s metric is recorded.
// Affinity: UI actor.
class ShutdownPlan {
public:
    explicit ShutdownPlan(Clock& clock = system_clock());
    ShutdownPlan(const ShutdownPlan&) = delete;
    ShutdownPlan& operator=(const ShutdownPlan&) = delete;

    void add(ShutdownStep step, std::string failure_label, std::function<void()> fn);
    // Runs once; a second call does nothing.
    void run();
    // True from the moment run() starts (OpenWhisper's _shutting_down).
    bool started() const;
    std::size_t size() const;

private:
    struct Entry {
        ShutdownStep step;
        std::string failure_label;
        std::function<void()> fn;
    };

    Clock& clock_;
    std::vector<Entry> entries_;
    bool started_ = false;
};

} // namespace fv
