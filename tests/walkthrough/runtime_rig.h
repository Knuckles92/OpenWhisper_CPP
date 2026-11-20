#pragma once

// The walkthrough runtime's fakes and rig (test_runtime.cpp's, shared with the runtime tests in
// other files): every port implemented over plain fields, a ManualExecutor for the UI and a
// FakeClock.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "settings/store.h"
#include "support.h"
#include "walkthrough/runtime.h"
#include "walkthrough/runtime_ports.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace fv::walkthrough::test {

class Handle final : public SnapHandle {
public:
    bool active() const override { return open_; }
    void keep() override {
        ++keeps;
        open_ = false;
    }
    void drop() override {
        ++drops;
        open_ = false;
    }
    std::optional<Rect> capture_rect() const override { return Rect{1, 2, 30, 40}; }

    int keeps = 0;
    int drops = 0;

private:
    bool open_ = true;
};

class Snap final : public SnapPort {
public:
    std::pair<bool, std::string> support() const override { return {supported, reason}; }
    void request_access() override { ++access_requests; }
    std::shared_ptr<SnapHandle> start(SnapRequest request, Kept kept, Dropped dropped,
                                      Failed failed) override {
        requests.push_back(std::move(request));
        // Exactly one of them runs, after start() returned: never here.
        kept_ = std::move(kept);
        dropped_ = std::move(dropped);
        failed_ = std::move(failed);
        handle = std::make_shared<Handle>();
        return handle;
    }
    ShotImagePtr render(ShotImagePtr frozen, std::optional<Rect> crop, MarksPtr marks) override {
        render_crop = crop;
        render_marks = std::move(marks);
        return frozen;
    }
    bool window_mode_supported() const override { return false; }

    std::vector<SnapRequest> requests;
    Kept kept_;
    Dropped dropped_;
    Failed failed_;
    std::shared_ptr<Handle> handle;
    bool supported = true;
    std::string reason;
    int access_requests = 0;
    std::optional<Rect> render_crop;
    MarksPtr render_marks;
};

class LiveDrawing final : public LiveDrawingPort {
public:
    void arm() override { armed_ = true; }
    void disarm() override { armed_ = false; }
    void set_enabled(bool enabled) override { enabled_changed.emit(enabled); }
    void set_busy(bool value) override { busy = value; }
    void set_capture_pending(bool value, bool captured, std::optional<ScreenRef> screen) override {
        pending = value;
        last_captured = captured;
        last_screen = std::move(screen);
    }
    void set_capture_countdown(double value, double) override { countdown = value; }
    std::optional<CaptureRegion> capture_region() const override { return region; }
    MarksPtr pinned_at(std::pair<int, int>) const override { return pinned; }
    MarksPtr pinned_on(std::optional<ScreenRef>) const override { return pinned; }
    void handle_mark_shortcut(const std::string& action) override { marked.push_back(action); }
    bool interacting() const override { return interacting_; }
    bool armed() const override { return armed_; }
    void close() override { closed = true; }
    bool interacting_ = false, busy = false, pending = false, last_captured = false, closed = false;
    double countdown = 0;
    std::optional<CaptureRegion> region;
    std::optional<ScreenRef> last_screen;
    std::vector<std::string> marked;
    MarksPtr pinned;

private:
    bool armed_ = false;
};

class Panel final : public PanelPort {
public:
    PanelState state() const override { return PanelState::Hidden; }
    bool paused() const override { return paused_; }
    void begin() override {}
    void hear(double) override {}
    void add_shot(int n, ShotImagePtr, double, std::optional<Rect> source) override {
        added.push_back(n);
        sources.push_back(source);
    }
    void end_capture_animation() override {}
    void remove_shot(int n) override { removed.push_back(n); }
    void move_shot(int n, int step) override { moved.emplace_back(n, step); }
    bool shows_recording() const override { return false; }
    void set_snap_mode(const std::string& mode, std::optional<std::string>) override {
        snap_mode = mode;
    }
    void set_auto_mode(bool) override {}
    void set_countdown(int count) override { countdown = count; }
    void set_paused(bool on) override { paused_ = on; }
    void set_notes_on(bool on) override { notes_on = on; }
    std::vector<std::string> snap_modes() const override { return {"region", "screen"}; }
    void finish() override {}
    void complete(int, double) override {}
    void dismiss() override {}
    std::vector<int> added, removed;
    std::vector<std::optional<Rect>> sources;
    std::vector<std::pair<int, int>> moved;
    std::string snap_mode;
    bool paused_ = false, notes_on = false;
    int countdown = 0;
};

class Recording final : public RecordingPort {
public:
    bool start_walkthrough(std::shared_ptr<WalkthroughSession> session,
                           std::string source) override {
        session_ = std::move(session);
        source_ = std::move(source);
        return true;
    }
    void stop_recording() override { ++stops; }
    // The recording has ended: the controller no longer has a walkthrough.
    void end_session() { session_.reset(); }
    void cancel() override { ++cancels; }
    std::shared_ptr<WalkthroughSession> recording_walkthrough() const override { return session_; }
    bool writing_up() const override { return writing; }
    bool cancel_write_up() override {
        if (!writing)
            return false;
        writing = false;
        return true;
    }
    bool canceling_recording() const override { return canceling; }
    bool capture_canceled() const override { return canceled; }
    bool is_recording() const override { return other_recording || session_ != nullptr; }
    bool paused() const override { return paused_; }
    bool pause() override {
        if (!can_pause)
            return false;
        paused_ = true;
        return true;
    }
    void resume() override { paused_ = false; }
    double recording_duration() const override { return duration; }
    bool refuse_during_meeting() override { return meeting; }
    std::string busy_message() const override { return unavailable; }
    void report_status(const std::string& text) override { status = text; }
    void set_destination_label(const std::string& text) override { label = text; }

    std::string source_;
    int stops = 0;
    int cancels = 0;
    bool writing = false;
    bool other_recording = false;
    bool meeting = false;
    bool canceling = false, canceled = false, paused_ = false, can_pause = true;
    double duration = 0;
    std::string status, label;
    std::string unavailable;

private:
    std::shared_ptr<WalkthroughSession> session_;
};

class Overlay final : public OverlayPort {
public:
    void show_caption(const std::string& value) override { caption = value; }
    bool keep_out_of_screenshots(bool on) override {
        excluded = on;
        return true;
    }
    std::string caption;
    bool excluded = false;
};

struct RuntimeRig {
    fv::walkthrough::test::TempFolder folder;
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(folder.path())}};
    fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    fv::SettingsStore settings{folder / "settings.json"};
    fv::FakeClock clock;
    fv::ManualExecutor ui{clock};
    Recording recording;
    Snap snap;
    Overlay overlay;
    Panel panel;
    std::string notice;
    std::vector<std::filesystem::path> copied;
    std::string copied_text;
    ShotImagePtr copied_image;
    std::shared_ptr<LiveDrawing> drawing = std::make_shared<LiveDrawing>();
    std::function<void(int, int, double)> click;
    std::function<void(std::string)> mark;
    std::map<std::string, std::string> registered;
    std::string conflict_key;
    std::unique_ptr<WalkthroughRuntime> runtime;
    // Whether the clipboard takes a copy, and whether the click hook starts.
    bool copy_works = true;
    bool clicks_work = true;

    explicit RuntimeRig(bool automatic = false) {
        if (automatic)
            settings.save_setting("walkthrough_auto_mode", true);
        RuntimeServices services{ui, clock, settings, paths, recording, snap, overlay};
        services.platform = "win32";
        services.live = [this](auto) {
            return drawing;
        };
        services.clicks = [this](auto callback) -> std::unique_ptr<fv::Subscription> {
            if (!clicks_work)
                return nullptr;
            click = std::move(callback);
            return std::make_unique<fv::Subscription>();
        };
        services.marks = [this](auto bindings, auto callback) {
            registered = std::move(bindings);
            mark = std::move(callback);
        };
        services.hotkey_conflict = [this](const std::string& key) {
            return key == conflict_key ? std::string("already used") : std::string();
        };
        services.tell = [this](std::string text, bool) {
            notice = std::move(text);
        };
        services.copy_image = [this](ShotImagePtr image, std::optional<std::filesystem::path> path,
                                     std::string text) {
            copied_image = std::move(image);
            if (path)
                copied.push_back(*path);
            copied_text = std::move(text);
            return copy_works;
        };
        runtime = std::make_unique<WalkthroughRuntime>(std::move(services));
        runtime->attach_panel(&panel);
    }
    void keep() {
        snap.kept_(std::make_shared<FakeShotImage>(2, 2, 0x123456));
        ui.run_all();
    }
};

} // namespace fv::walkthrough::test
