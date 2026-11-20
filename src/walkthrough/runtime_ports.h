#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/signal.h"
#include "base/subscription.h"
#include "walkthrough/session.h"
#include "walkthrough/shot_image.h"

// What the walkthrough runtime (services/runtime/walkthrough.py) reaches outside this library,
// as ports: the Snap overlay and live drawing (fv_ui_snap), the walkthrough's column
// (fv_ui_walkthrough), the recording (TranscriptionRuntime and its Recorder), the recording
// overlay, the click hook and the mark shortcuts. The runtime is Qt-free; each port's adapter
// lives with the widget it drives.
//
// Affinity: every port is a UI actor. Its methods are called on the UI executor, and it calls
// back and emits its signals there, never inline from inside the method that caused them unless
// a method says so.
namespace fv::walkthrough {

// One monitor, as the adapters name it: QScreen::name() and its place in
// QGuiApplication::screens(). Two refs are the same screen when both match.
struct ScreenRef {
    std::string name;
    int index = 0;

    friend bool operator==(const ScreenRef&, const ScreenRef&) = default;
};

// A rectangle in logical pixels (QRect's x, y, width, height).
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

// Marks drawn on a screenshot or on a live canvas, opaque here: fv_ui_snap's annotations.
// nullptr stands for none.
using MarksPtr = std::shared_ptr<const void>;

// What a snap takes (WalkthroughSnapMode, without "delayed", which the runtime counts down
// itself): a region the user draws, the whole screen, the window under the pointer, or the whole
// screen with the pen in hand.
enum class SnapTake { Region, Screen, Window, Draw };

// One snap (ui_qt.snap.start_snap's options).
struct SnapRequest {
    // What the overlay's hint pill says ("" for none).
    std::string hint;
    SnapTake take = SnapTake::Region;
    // "The window" is the one in front, not the one under the pointer (the request came from
    // our own windows).
    bool front = false;
    // A click's global position: its screen, and for Window the window there, is the one taken.
    std::optional<std::pair<int, int>> click_at;
    // The monitor to take; nullopt: the pointer's.
    std::optional<ScreenRef> screen;
};

// A snap that was started: its overlay while it is open. Python's SnapSession.
class SnapHandle {
public:
    virtual ~SnapHandle() = default;
    // Whether the screen is frozen and the overlay is still open.
    virtual bool active() const = 0;
    // Keep what is selected (Enter); while still capturing, keep it once captured.
    virtual void keep() = 0;
    // Drop it (Esc); nothing more is reported but `dropped`.
    virtual void drop() = 0;
    // The kept or open area in global logical pixels, once known.
    virtual std::optional<Rect> capture_rect() const = 0;
};

// Screenshots: the Snap overlay and what renders a kept one.
class SnapPort {
public:
    using Kept = std::function<void(ShotImagePtr image)>;
    using Dropped = std::function<void()>;
    using Failed = std::function<void(std::string reason)>;

    virtual ~SnapPort() = default;
    // (supported, why not) for screenshots on this desktop (snap_support).
    virtual std::pair<bool, std::string> support() const = 0;
    // Asks for what support() found missing (macOS's Screen Recording grant; nothing on
    // Windows).
    virtual void request_access() = 0;
    // Freezes the screen and opens the overlay. Exactly one of the callbacks runs, always after
    // start() has returned. The handle stays valid after its outcome.
    virtual std::shared_ptr<SnapHandle> start(SnapRequest request, Kept kept, Dropped dropped,
                                              Failed failed) = 0;
    // `frozen` cropped to `crop` (logical pixels of the frozen image; nullopt: all of it) with
    // `marks` painted at device resolution (ui_qt.snap.export.render).
    virtual ShotImagePtr render(ShotImagePtr frozen, std::optional<Rect> crop, MarksPtr marks) = 0;
    // Whether "the window under the pointer" can be found on this desktop.
    virtual bool window_mode_supported() const = 0;
};

// Auto Mode's capture region: where on which screen, in the screen's own logical pixels, and
// the same area in global logical pixels.
struct CaptureRegion {
    ScreenRef screen;
    Rect rect;
    Rect global;

    friend bool operator==(const CaptureRegion&, const CaptureRegion&) = default;
};

// Auto Mode's live drawing (ui_qt/snap/live.py LiveAnnotations): a floating toolbar and a
// transparent canvas per screen to draw on while recording. The runtime makes one per
// walkthrough (RuntimeServices' factory) and close()s it at the end.
class LiveDrawingPort {
public:
    virtual ~LiveDrawingPort() = default;

    // Takes the pen (the canvases take input).
    virtual void arm() = 0;
    // Puts the tool down.
    virtual void disarm() = 0;
    // Shows (true) or hides the toolbar and canvases; hiding disarms.
    virtual void set_enabled(bool enabled) = 0;
    // A screenshot is being taken: no drawing meanwhile.
    virtual void set_busy(bool busy) = 0;
    // A delayed screenshot is coming (`pending`), or came (`captured`) or was dropped. `screen`
    // is the monitor it was of: marks drawn on another one stay; nullopt: every monitor.
    virtual void set_capture_pending(bool pending, bool captured = false,
                                     std::optional<ScreenRef> screen = std::nullopt) = 0;
    // The delay chip's countdown: seconds left of `total`; 0 when none is coming.
    virtual void set_capture_countdown(double remaining, double total = 0.0) = 0;
    // The capture region the user dragged, or nullopt for the whole screen.
    virtual std::optional<CaptureRegion> capture_region() const = 0;
    // The pinned marks of the canvas under a click's global `position`, and of `screen`'s
    // canvas (nullopt: none); nullptr when there are none.
    virtual MarksPtr pinned_at(std::pair<int, int> position) const = 0;
    virtual MarksPtr pinned_on(std::optional<ScreenRef> screen) const = 0;
    // A mark shortcut ("keep_marks", "clear") was pressed.
    virtual void handle_mark_shortcut(const std::string& action) = 0;
    // Whether a stroke is being drawn or a label typed.
    virtual bool interacting() const = 0;
    // Whether a tool is in hand or a region is being dragged.
    virtual bool armed() const = 0;
    // Closes the toolbar and canvases for good.
    virtual void close() = 0;

    // A tool is about to be taken up.
    Signal<> arming;
    // The capture region changed or went.
    Signal<> capture_changed;
    // A mark was completed on `screen`: every mark of that canvas.
    Signal<ScreenRef, MarksPtr> drawn;
    // The delay chip picked a new delay, in seconds.
    Signal<int> capture_delay_changed;
    // Clear or Esc: a pending delayed screenshot is cancelled.
    Signal<> capture_cancelled;
    // A stroke or a label started (true) or ended.
    Signal<bool> interaction_changed;
    // set_enabled changed the state.
    Signal<bool> enabled_changed;
};

// What the column shows (WalkthroughPanel's RECORDING, WRITING, DONE and HIDDEN).
enum class PanelState { Recording, Writing, Done, Hidden };

// The walkthrough's column (ui_qt/widgets/walkthrough_panel.py, fv::WalkthroughPanel). It is made
// once and kept for every walkthrough.
class PanelPort {
public:
    virtual ~PanelPort() = default;

    virtual PanelState state() const = 0;
    // Whether the recording it shows is paused.
    virtual bool paused() const = 0;

    // A walkthrough started recording: an empty column on the pointer's screen.
    virtual void begin() = 0;
    // The microphone's level, 0 to 1.
    virtual void hear(double level) = 0;
    // Screenshot `number` (as kept) joins the column, kept_at_s into the recording; with
    // source_rect (global logical pixels) the picture flies there from it first.
    virtual void add_shot(int number, ShotImagePtr image, double kept_at_s,
                          std::optional<Rect> source_rect = std::nullopt) = 0;
    // Lands a flying screenshot.
    virtual void end_capture_animation() = 0;
    // Screenshot `number` (as kept) left the walkthrough; its words stay.
    virtual void remove_shot(int number) = 0;
    // Screenshot `number` (as kept) moved `step` sections, with its words.
    virtual void move_shot(int number, int step) = 0;
    // Whether the column shows the walkthrough, recording or being written.
    virtual bool shows_recording() const = 0;
    // What Snap takes (a WalkthroughSnapMode value) and, when given, the Screenshot shortcut's
    // keys as shown.
    virtual void set_snap_mode(const std::string& mode,
                               std::optional<std::string> shortcut = std::nullopt) = 0;
    virtual void set_auto_mode(bool enabled) = 0;
    // Seconds until an "In 3 seconds" snap; 0 when none is coming.
    virtual void set_countdown(int seconds) = 0;
    virtual void set_paused(bool paused) = 0;
    // Whether Hold for note works: only an AI write-up reads notes.
    virtual void set_notes_on(bool on) = 0;
    // The kinds of screenshot this desktop can take.
    virtual std::vector<std::string> snap_modes() const = 0;
    // The recording stopped: the column writes it.
    virtual void finish() = 0;
    // The document is ready: `shots` screenshots, `duration_s` of speech.
    virtual void complete(int shots, double duration_s) = 0;
    // Canceled or failed: fade at once.
    virtual void dismiss() = 0;

    Signal<> snap_requested;
    Signal<std::string> snap_mode_chosen;
    // Hold for note pressed (true) or let go.
    Signal<bool> note_held;
    Signal<bool> pause_requested;
    Signal<> undo_requested;
    Signal<> cancel_requested;
    Signal<> finish_requested;
    // (number, step)
    Signal<int, int> move_requested;
    Signal<int> remove_requested;
};

// The recording a walkthrough is (TranscriptionRuntime and its Recorder, through the
// controller).
class RecordingPort {
public:
    virtual ~RecordingPort() = default;

    // Starts recording `session` as a walkthrough; false when it could not start (the reason is
    // already shown). `source` is who asked ("hotkey", "ui", "tray").
    virtual bool start_walkthrough(std::shared_ptr<WalkthroughSession> session,
                                   std::string source) = 0;
    // Stops the recording; the walkthrough is then transcribed and written.
    virtual void stop_recording() = 0;
    // The controller's Cancel: the recording, or the job in progress.
    virtual void cancel() = 0;
    // The walkthrough being recorded, or nullptr.
    virtual std::shared_ptr<WalkthroughSession> recording_walkthrough() const = 0;
    // Whether the last walkthrough's AI write-up still runs.
    virtual bool writing_up() const = 0;
    // Cancels that write-up; false when none runs.
    virtual bool cancel_write_up() = 0;
    // Whether the recording that just stopped was canceled: the runtime's own flag, and the
    // recorder's capture_canceled.
    virtual bool canceling_recording() const = 0;
    virtual bool capture_canceled() const = 0;
    // Whether the recorder records (a dictation or a walkthrough).
    virtual bool is_recording() const = 0;
    // Whether the recording is paused.
    virtual bool paused() const = 0;
    // Pauses; false when the recorder could not.
    virtual bool pause() = 0;
    virtual void resume() = 0;
    // Seconds recorded so far, without the paused time.
    virtual double recording_duration() const = 0;
    // True, after saying why on the status line, when a meeting, a backup or the meeting setup
    // holds the recorder (_refuse_dictation_during_meeting).
    virtual bool refuse_during_meeting() = 0;
    // Why capture must not start yet (the engine is not ready), or "".
    virtual std::string busy_message() const = 0;
    // The status line (the controller's status_update).
    virtual void report_status(const std::string& text) = 0;
    // What the recording overlay says is being recorded ("Walkthrough · 2 shots").
    virtual void set_destination_label(const std::string& label) = 0;
};

// The recording overlay while a walkthrough records.
class OverlayPort {
public:
    virtual ~OverlayPort() = default;
    // A short caption on the overlay ("Screenshot 2 added", "Snapping in 3…").
    virtual void show_caption(const std::string& text) = 0;
    // Leaves the overlay out of every screen capture, or stops; true where the desktop can.
    virtual bool keep_out_of_screenshots(bool enabled) = 0;
};

// Watches for clicks anywhere on the desktop: on_click(x, y, at) with the global position and
// the steady time in seconds, on the hook's own thread. nullptr when the hook could not be
// installed; destroying the subscription stops watching.
using ClickWatcher = std::function<std::unique_ptr<Subscription>(
    std::function<void(int x, int y, double at)> on_click)>;

// Registers the mark shortcuts (action to hotkey, family "walkthrough_marks"; an empty map
// unregisters them) and what a press calls with its action, on the hook's thread.
using MarkShortcuts = std::function<void(std::map<std::string, std::string> shortcuts,
                                         std::function<void(std::string action)> on_shortcut)>;

} // namespace fv::walkthrough
