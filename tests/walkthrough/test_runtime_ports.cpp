// The runtime ports are declared here (M6.6) and implemented in later waves: M6.10 (Snap, live
// drawing), M6.11 (recording) and M6.16 (panel, overlay, clicks, mark shortcuts). This test is
// the header's first translation unit: it includes it on its own and implements every port, so
// a port that does not compile, or a pure virtual that can't be overridden, shows up now.
#include "walkthrough/runtime_ports.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/testing/manual_executor.h"

using namespace fv::walkthrough;

namespace {

class Handle final : public SnapHandle {
public:
    bool active() const override { return open_; }
    void keep() override { open_ = false; }
    void drop() override { open_ = false; }
    std::optional<Rect> capture_rect() const override { return Rect{1, 2, 30, 40}; }

private:
    bool open_ = true;
};

class Snap final : public SnapPort {
public:
    std::pair<bool, std::string> support() const override { return {true, ""}; }
    void request_access() override {}
    std::shared_ptr<SnapHandle> start(SnapRequest request, Kept kept, Dropped dropped,
                                      Failed failed) override {
        requests.push_back(std::move(request));
        // Exactly one of them runs, after start() returned: never here.
        (void)kept;
        (void)dropped;
        (void)failed;
        return std::make_shared<Handle>();
    }
    ShotImagePtr render(ShotImagePtr frozen, std::optional<Rect>, MarksPtr) override {
        return frozen;
    }
    bool window_mode_supported() const override { return false; }

    std::vector<SnapRequest> requests;
};

class LiveDrawing final : public LiveDrawingPort {
public:
    void arm() override { armed_ = true; }
    void disarm() override { armed_ = false; }
    void set_enabled(bool enabled) override { enabled_changed.emit(enabled); }
    void set_busy(bool) override {}
    void set_capture_pending(bool, bool, std::optional<ScreenRef>) override {}
    void set_capture_countdown(double, double) override {}
    std::optional<CaptureRegion> capture_region() const override { return std::nullopt; }
    MarksPtr pinned_at(std::pair<int, int>) const override { return nullptr; }
    MarksPtr pinned_on(std::optional<ScreenRef>) const override { return nullptr; }
    void handle_mark_shortcut(const std::string&) override {}
    bool interacting() const override { return false; }
    bool armed() const override { return armed_; }
    void close() override {}

private:
    bool armed_ = false;
};

class Panel final : public PanelPort {
public:
    PanelState state() const override { return PanelState::Hidden; }
    bool paused() const override { return false; }
    void begin() override {}
    void hear(double) override {}
    void add_shot(int, ShotImagePtr, double, std::optional<Rect>) override {}
    void end_capture_animation() override {}
    void remove_shot(int) override {}
    void move_shot(int, int) override {}
    bool shows_recording() const override { return false; }
    void set_snap_mode(const std::string&, std::optional<std::string>) override {}
    void set_auto_mode(bool) override {}
    void set_countdown(int) override {}
    void set_paused(bool) override {}
    void set_notes_on(bool) override {}
    std::vector<std::string> snap_modes() const override { return {"region", "screen"}; }
    void finish() override {}
    void complete(int, double) override {}
    void dismiss() override {}
};

class Recording final : public RecordingPort {
public:
    bool start_walkthrough(std::shared_ptr<WalkthroughSession> session,
                           std::string source) override {
        session_ = std::move(session);
        source_ = std::move(source);
        return true;
    }
    void stop_recording() override {}
    void cancel() override {}
    std::shared_ptr<WalkthroughSession> recording_walkthrough() const override { return session_; }
    bool writing_up() const override { return false; }
    bool cancel_write_up() override { return false; }
    bool canceling_recording() const override { return false; }
    bool capture_canceled() const override { return false; }
    bool is_recording() const override { return session_ != nullptr; }
    bool paused() const override { return false; }
    bool pause() override { return true; }
    void resume() override {}
    double recording_duration() const override { return 0.0; }
    bool refuse_during_meeting() override { return false; }
    std::string busy_message() const override { return ""; }
    void report_status(const std::string&) override {}
    void set_destination_label(const std::string&) override {}

    std::string source_;

private:
    std::shared_ptr<WalkthroughSession> session_;
};

class Overlay final : public OverlayPort {
public:
    void show_caption(const std::string&) override {}
    bool keep_out_of_screenshots(bool) override { return true; }
};

} // namespace

TEST_CASE("every runtime port can be implemented and driven", "[runtime_ports]") {
    fv::ManualExecutor executor;

    Snap snap;
    const auto handle = snap.start(
        {.hint = "Drag to snap", .take = SnapTake::Window, .front = true}, [](ShotImagePtr) {},
        [] {}, [](std::string) {});
    CHECK(handle->active());
    CHECK(handle->capture_rect() == Rect{1, 2, 30, 40});
    handle->keep();
    CHECK_FALSE(handle->active());
    REQUIRE(snap.requests.size() == 1);
    CHECK(snap.requests[0].take == SnapTake::Window);
    CHECK_FALSE(snap.requests[0].screen);

    LiveDrawing drawing;
    std::vector<bool> enabled;
    std::vector<std::pair<ScreenRef, MarksPtr>> drawn;
    const auto enabled_link =
        drawing.enabled_changed.connect(executor, [&](bool on) { enabled.push_back(on); });
    const auto drawn_link =
        drawing.drawn.connect(executor, [&](const ScreenRef& screen, const MarksPtr& marks) {
            drawn.emplace_back(screen, marks);
        });
    drawing.set_enabled(true);
    drawing.drawn.emit(ScreenRef{"DISPLAY2", 1}, std::make_shared<int>(3));
    executor.run_all();
    CHECK(enabled == std::vector<bool>{true});
    REQUIRE(drawn.size() == 1);
    CHECK(drawn[0].first == ScreenRef{"DISPLAY2", 1});
    CHECK(drawn[0].second != nullptr);

    Panel panel;
    int moves = 0;
    const auto move_link = panel.move_requested.connect(
        executor, [&](int number, int step) { moves += number * step; });
    panel.move_requested.emit(2, -1);
    executor.run_all();
    CHECK(moves == -2);
    CHECK(panel.state() == PanelState::Hidden);

    Recording recording;
    CHECK(recording.start_walkthrough(nullptr, "hotkey"));
    CHECK(recording.source_ == "hotkey");

    Overlay overlay;
    CHECK(overlay.keep_out_of_screenshots(true));

    std::vector<std::tuple<int, int, double>> clicks;
    const ClickWatcher watch = [](std::function<void(int, int, double)> on_click) {
        on_click(10, 20, 1.5);
        return std::make_unique<fv::Subscription>();
    };
    const auto watching = watch([&](int x, int y, double at) { clicks.emplace_back(x, y, at); });
    CHECK(clicks == std::vector<std::tuple<int, int, double>>{{10, 20, 1.5}});

    std::map<std::string, std::string> registered;
    const MarkShortcuts shortcuts = [&](std::map<std::string, std::string> keys,
                                        std::function<void(std::string)> on_shortcut) {
        registered = std::move(keys);
        on_shortcut("clear");
    };
    std::string pressed;
    shortcuts({{"clear", "ctrl+shift+x"}},
              [&](std::string action) { pressed = std::move(action); });
    CHECK(registered == std::map<std::string, std::string>{{"clear", "ctrl+shift+x"}});
    CHECK(pressed == "clear");
}
