#pragma once

#include <memory>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/executor.h"
#include "base/paths.h"
#include "base/platform_info.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "settings/store.h"
#include "walkthrough/runtime_ports.h"

namespace fv::walkthrough {

std::string shots_label(int count);

struct RuntimeServices {
    Executor& ui;
    Clock& clock;
    SettingsStore& settings;
    const Paths& paths;
    RecordingPort& recording;
    SnapPort& snap;
    OverlayPort& overlay;
    std::function<std::shared_ptr<LiveDrawingPort>(std::map<std::string, std::string>)> live;
    ClickWatcher clicks;
    MarkShortcuts marks;
    std::function<std::string(const std::string&)> hotkey_conflict;
    std::function<void(std::string, bool)> tell;
    std::function<bool(ShotImagePtr, std::optional<std::filesystem::path>, std::string)> copy_image;
    std::string_view platform = python_platform();
};

// Manual walkthrough and screenshot controls. All methods run on the UI executor.
class WalkthroughRuntime {
public:
    explicit WalkthroughRuntime(RuntimeServices services);
    ~WalkthroughRuntime();
    void attach_panel(PanelPort* panel);
    void toggle(std::string source = "hotkey");
    void cancel();
    void finish();
    void snap(std::string source = "hotkey");
    void choose_snap_mode(std::string mode);
    void undo_last_shot();
    void remove_shot(int number);
    void move_shot(int number, int step);
    void set_paused(bool paused);
    void hold_note(bool on);
    void on_recording_state_changed(bool recording);
    void cleanup();
    Signal<bool> walkthrough_active;

private:
    std::shared_ptr<WalkthroughSession> session() const;
    void tell(std::string text, bool done = false);
    void snap_picked();
    void snap_now(SnapTake take);
    void countdown_tick();
    void stop_countdown();
    void finish_if_asked();
    void on_kept(ShotImagePtr image);
    void on_dropped();
    void on_failed(std::string reason);
    void shot_removed(int number);
    void start_auto(const std::shared_ptr<WalkthroughSession>& session);
    void stop_auto();
    void discard_auto();
    bool auto_can_capture(bool drawing = false) const;
    void schedule_auto();
    void auto_progress();
    void capture_auto();
    void auto_outcome(std::uint64_t generation, std::weak_ptr<WalkthroughSession> session,
                      ShotImagePtr image, MarksPtr marks, std::optional<double> at,
                      std::optional<Rect> crop, std::optional<Rect> source,
                      std::optional<ScreenRef> screen, std::string reason);
    RuntimeServices services_;
    PanelPort* panel_ = nullptr;
    std::vector<Subscription> links_;
    Subscription save_link_;
    std::shared_ptr<WalkthroughSession> snap_for_;
    std::shared_ptr<SnapHandle> snap_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    TimerHandle timer_;
    std::string source_ = "hotkey";
    int countdown_ = 0;
    bool active_ = false;
    bool excluded_ = false;
    bool finish_after_snap_ = false;
    std::uint64_t snap_generation_ = 0;
    std::unique_ptr<Subscription> auto_hook_;
    std::shared_ptr<LiveDrawingPort> drawing_;
    std::vector<Subscription> drawing_links_;
    TimerHandle auto_timer_;
    TimerHandle auto_progress_timer_;
    std::chrono::steady_clock::time_point auto_deadline_{};
    int auto_delay_ = 0;
    std::shared_ptr<SnapHandle> auto_snap_;
    bool auto_drawing_ = false;
    std::uint64_t auto_generation_ = 0;
    std::optional<std::pair<int, int>> click_;
    std::optional<std::pair<ScreenRef, MarksPtr>> drawing_pending_;
    std::optional<std::pair<ScreenRef, MarksPtr>> drawing_next_;
    std::optional<std::string> snap_after_drawing_;
    double auto_after_ = 0;
};
} // namespace fv::walkthrough
