#include "walkthrough/runtime.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "base/platform_info.h"
#include "base/utf8.h"
#include "settings/keys.h"
#include "walkthrough/storage.h"

namespace fv::walkthrough {
using namespace std::chrono_literals;

std::string shots_label(int count) {
    if (count == 0)
        return "Walkthrough";
    return "Walkthrough · " + std::to_string(count) + (count == 1 ? " shot" : " shots");
}

WalkthroughRuntime::WalkthroughRuntime(RuntimeServices services) : services_(std::move(services)) {}
WalkthroughRuntime::~WalkthroughRuntime() {
    cleanup();
    alive_.reset();
}
std::shared_ptr<WalkthroughSession> WalkthroughRuntime::session() const {
    return services_.recording.recording_walkthrough();
}
void WalkthroughRuntime::tell(std::string text, bool done) {
    if (services_.tell)
        services_.tell(std::move(text), done);
}
void WalkthroughRuntime::attach_panel(PanelPort* panel) {
    if (panel == panel_)
        return;
    links_.clear();
    panel_ = panel;
    if (!panel)
        return;
    auto& ui = services_.ui;
    links_.push_back(panel->snap_requested.connect(ui, [this] {
        if (finish_after_snap_)
            return;
        if (drawing_ && !countdown_ && !snap_)
            drawing_->arm();
        else
            snap("ui");
    }));
    links_.push_back(panel->snap_mode_chosen.connect(
        ui, [this](std::string mode) { choose_snap_mode(std::move(mode)); }));
    links_.push_back(panel->finish_requested.connect(ui, [this] { finish(); }));
    links_.push_back(panel->cancel_requested.connect(ui, [this] { cancel(); }));
    links_.push_back(panel->undo_requested.connect(ui, [this] { undo_last_shot(); }));
    links_.push_back(panel->remove_requested.connect(ui, [this](int n) { remove_shot(n); }));
    links_.push_back(panel->move_requested.connect(ui, [this](int n, int s) { move_shot(n, s); }));
    links_.push_back(panel->pause_requested.connect(ui, [this](bool on) { set_paused(on); }));
    links_.push_back(panel->note_held.connect(ui, [this](bool on) { hold_note(on); }));
}
void WalkthroughRuntime::toggle(std::string source) {
    if (session()) {
        finish();
        return;
    }
    auto& recording = services_.recording;
    if (recording.is_recording()) {
        tell("Finish the recording in progress first");
        return;
    }
    if (recording.refuse_during_meeting())
        return;
    if (recording.writing_up()) {
        tell("Wait for the last walkthrough's write-up, or cancel it");
        return;
    }
    if (auto reason = recording.busy_message(); !reason.empty()) {
        tell(reason);
        return;
    }
    WalkthroughSession::Options options;
    options.root = captures_root(services_.paths);
    options.clock = &services_.clock;
    auto next = std::make_shared<WalkthroughSession>(std::move(options));
    save_link_ = next->save_failed.connect(services_.ui, [this](int number) {
        services_.recording.report_status("Screenshot " + std::to_string(number) +
                                          " couldn't be saved — the walkthrough keeps recording");
    });
    if (!recording.start_walkthrough(next, std::move(source)))
        return;
    active_ = true;
    walkthrough_active.emit(true);
    if (panel_) {
        panel_->set_snap_mode(services_.settings.get(keys::walkthrough_snap_mode));
        panel_->set_notes_on(services_.settings.get(keys::walkthrough_write_up));
    }
    excluded_ = services_.overlay.keep_out_of_screenshots(true);
    if (services_.settings.get(keys::walkthrough_auto_mode) &&
        (services_.platform == "win32" || services_.platform == "darwin"))
        start_auto(next);
}
void WalkthroughRuntime::cancel() {
    if (!services_.recording.cancel_write_up())
        services_.recording.cancel();
}
void WalkthroughRuntime::finish() {
    if (!session())
        return;
    if (snap_ && snap_->active()) {
        finish_after_snap_ = true;
        auto handle = snap_;
        handle->keep();
        return;
    }
    if (click_ || drawing_pending_ || auto_snap_) {
        finish_after_snap_ = true;
        if (drawing_ && !drawing_->interacting()) {
            drawing_->set_busy(true);
            if (drawing_pending_)
                capture_auto();
            else if (click_) {
                auto_timer_.cancel();
                std::weak_ptr<int> alive = alive_;
                auto_timer_ = services_.ui.post_after(350ms, [this, alive] {
                    if (alive.lock())
                        capture_auto();
                });
            }
        }
        return;
    }
    services_.recording.stop_recording();
}
void WalkthroughRuntime::snap(std::string source) {
    if (finish_after_snap_)
        return;
    if (auto_snap_ && auto_drawing_) {
        snap_after_drawing_ = std::move(source);
        return;
    }
    discard_auto();
    if (drawing_)
        drawing_->set_enabled(false);
    if (snap_ && snap_->active()) {
        auto handle = snap_;
        handle->keep();
        return;
    }
    if (countdown_) {
        stop_countdown();
        snap_now(SnapTake::Region);
        return;
    }
    source_ = std::move(source);
    if (source_ == "tray") {
        std::weak_ptr<int> alive = alive_;
        timer_ = services_.ui.post_after(300ms, [this, alive] {
            if (alive.lock())
                snap_picked();
        });
    } else
        snap_picked();
}
void WalkthroughRuntime::choose_snap_mode(std::string mode) {
    if (mode != "region" && mode != "window" && mode != "screen" && mode != "delayed")
        return;
    services_.settings.save_setting("walkthrough_snap_mode", mode);
    if (panel_)
        panel_->set_snap_mode(mode);
    stop_countdown();
    snap("ui");
}
void WalkthroughRuntime::snap_picked() {
    const auto mode = session() ? services_.settings.get(keys::walkthrough_snap_mode) : "region";
    if (mode == "delayed") {
        countdown_ = 3;
        countdown_tick();
        return;
    }
    snap_now(mode == "window"   ? SnapTake::Window
             : mode == "screen" ? SnapTake::Screen
                                : SnapTake::Region);
}
void WalkthroughRuntime::countdown_tick() {
    if (!countdown_) {
        snap_now(SnapTake::Region);
        return;
    }
    if (panel_)
        panel_->set_countdown(countdown_);
    services_.overlay.show_caption("Snapping in " + std::to_string(countdown_) + "…");
    std::weak_ptr<int> alive = alive_;
    timer_ = services_.ui.post_after(1s, [this, alive] {
        if (alive.lock()) {
            --countdown_;
            countdown_tick();
        }
    });
}
void WalkthroughRuntime::stop_countdown() {
    timer_.cancel();
    countdown_ = 0;
    if (panel_)
        panel_->set_countdown(0);
}
void WalkthroughRuntime::snap_now(SnapTake take) {
    stop_countdown();
    auto [supported, reason] = services_.snap.support();
    if (!supported) {
        tell(reason);
        services_.snap.request_access();
        return;
    }
    snap_for_ = session();
    SnapRequest request;
    request.hint = snap_for_ ? "Recording continues — talk while you draw" : "";
    request.take = take;
    request.front = source_ != "hotkey";
    std::weak_ptr<int> alive = alive_;
    const auto generation = ++snap_generation_;
    Executor* ui = &services_.ui;
    snap_ = services_.snap.start(
        std::move(request),
        [this, alive, ui, generation](ShotImagePtr image) {
            ui->post([this, alive, generation, image = std::move(image)] {
                if (alive.lock() && generation == snap_generation_)
                    on_kept(image);
            });
        },
        [this, alive, ui, generation] {
            ui->post([this, alive, generation] {
                if (alive.lock() && generation == snap_generation_)
                    on_dropped();
            });
        },
        [this, alive, ui, generation](std::string reason) {
            ui->post([this, alive, generation, reason = std::move(reason)] {
                if (alive.lock() && generation == snap_generation_)
                    on_failed(reason);
            });
        });
}
void WalkthroughRuntime::on_kept(ShotImagePtr image) {
    ++snap_generation_;
    auto owner = std::move(snap_for_);
    auto rect = snap_ ? snap_->capture_rect() : std::nullopt;
    snap_.reset();
    bool kept = false;
    if (owner) {
        try {
            auto shot = owner->add_shot(image, services_.recording.recording_duration());
            if (shot) {
                kept = true;
                const auto count = owner->shot_count();
                services_.recording.set_destination_label(shots_label(count));
                if (panel_)
                    panel_->add_shot(shot->number, image, shot->kept_at_s, rect);
                services_.overlay.show_caption("Screenshot " + std::to_string(count) + " added");
            }
        } catch (const ScreenshotNotSaved& error) {
            tell("Couldn't save the screenshot (" + std::string(error.what()) +
                 ") — the walkthrough keeps recording");
            kept = true;
        }
    }
    if (!kept) {
        std::optional<std::filesystem::path> path;
        try {
            auto local = std::chrono::floor<std::chrono::seconds>(
                services_.clock.zone()->to_local(services_.clock.utc()));
            path = new_screenshot_path(captures_root(services_.paths), local);
            image->save_png(*path);
        } catch (...) {
            path.reset();
        }
        const bool copied =
            services_.copy_image &&
            services_.copy_image(image, path, path ? base_utf8::path_to_utf8(*path) : "");
        const std::string unsaved = path ? "" : " (its file couldn't be saved)";
        tell(copied ? "Screenshot copied" + unsaved +
                          (owner ? " — the walkthrough had already stopped" : "")
                    : "Couldn't copy the screenshot" + unsaved,
             copied && path.has_value());
    }
    finish_if_asked();
    if (drawing_ && !finish_after_snap_)
        drawing_->set_enabled(!services_.recording.paused());
}
void WalkthroughRuntime::on_dropped() {
    ++snap_generation_;
    snap_.reset();
    snap_for_.reset();
    finish_if_asked();
    if (drawing_ && !finish_after_snap_)
        drawing_->set_enabled(!services_.recording.paused());
}
void WalkthroughRuntime::on_failed(std::string reason) {
    ++snap_generation_;
    snap_.reset();
    snap_for_.reset();
    tell(reason.empty() ? "Couldn't take a screenshot" : std::move(reason));
    finish_if_asked();
    if (drawing_ && !finish_after_snap_)
        drawing_->set_enabled(!services_.recording.paused());
}
void WalkthroughRuntime::finish_if_asked() {
    if (std::exchange(finish_after_snap_, false) && session())
        services_.recording.stop_recording();
}
void WalkthroughRuntime::shot_removed(int number) {
    services_.recording.set_destination_label(shots_label(session()->shot_count()));
    if (panel_)
        panel_->remove_shot(number);
}
void WalkthroughRuntime::undo_last_shot() {
    if (auto s = session())
        if (auto n = s->undo_last())
            shot_removed(*n);
}
void WalkthroughRuntime::remove_shot(int n) {
    if (auto s = session(); s && s->remove_shot(n))
        shot_removed(n);
}
void WalkthroughRuntime::move_shot(int n, int step) {
    if (auto s = session(); s && s->move_shot(n, step) && panel_)
        panel_->move_shot(n, step);
}
void WalkthroughRuntime::set_paused(bool paused) {
    if (!session() || finish_after_snap_)
        return;
    if (paused && !services_.recording.pause())
        return;
    if (!paused)
        services_.recording.resume();
    discard_auto();
    if (drawing_)
        drawing_->set_enabled(!paused);
    if (panel_)
        panel_->set_paused(paused);
    services_.recording.report_status(paused ? "Walkthrough · Paused"
                                             : "Walkthrough · Recording...");
}
void WalkthroughRuntime::hold_note(bool on) {
    if (auto s = session()) {
        auto t = services_.recording.recording_duration();
        if (on)
            s->hold_note(t);
        else
            s->release_note(t);
    }
}
void WalkthroughRuntime::on_recording_state_changed(bool recording) {
    if (recording || !active_)
        return;
    active_ = false;
    stop_auto();
    finish_after_snap_ = false;
    stop_countdown();
    walkthrough_active.emit(false);
    if (std::exchange(excluded_, false))
        services_.overlay.keep_out_of_screenshots(false);
    if (snap_ && snap_->active() && snap_for_) {
        auto handle = snap_;
        if (services_.recording.canceling_recording() || services_.recording.capture_canceled())
            handle->drop();
        else
            handle->keep();
    }
}
void WalkthroughRuntime::start_auto(const std::shared_ptr<WalkthroughSession>& owner) {
    auto [supported, reason] = services_.snap.support();
    if (!supported) {
        tell(reason);
        services_.snap.request_access();
        return;
    }
    if (!services_.clicks || !services_.live) {
        tell("Auto Mode couldn't watch for clicks. You can still use Snap.");
        return;
    }
    std::weak_ptr<int> alive = alive_;
    std::weak_ptr<WalkthroughSession> weak = owner;
    Executor* ui = &services_.ui;
    try {
        auto_hook_ = services_.clicks([this, alive, weak, ui](int x, int y, double at) {
            ui->post([this, alive, weak, x, y, at] {
                if (!alive.lock() || weak.lock() != session() || finish_after_snap_ ||
                    at < auto_after_ || !auto_can_capture())
                    return;
                const bool pending = click_.has_value();
                click_ = std::pair{x, y};
                if (!pending)
                    schedule_auto();
            });
        });
        if (!auto_hook_)
            throw std::runtime_error("click hook unavailable");
        std::map<std::string, std::string> shortcuts;
        for (const auto& [action, key] :
             std::map<std::string, std::string>{{"keep_marks", "f8"}, {"clear", "f9"}}) {
            if (services_.marks &&
                (!services_.hotkey_conflict || services_.hotkey_conflict(key).empty()))
                shortcuts[action] = key;
        }
        std::map<std::string, std::string> labels;
        for (const auto& [action, key] : shortcuts)
            labels[action] = key == "f8" ? "F8" : "F9";
        drawing_ = services_.live(std::move(labels));
        if (!drawing_)
            throw std::runtime_error("drawing unavailable");
        drawing_links_.push_back(drawing_->arming.connect(*ui, [this] { discard_auto(); }));
        drawing_links_.push_back(drawing_->capture_changed.connect(*ui, [this] {
            discard_auto();
            finish_if_asked();
        }));
        drawing_links_.push_back(drawing_->capture_cancelled.connect(*ui, [this] {
            discard_auto();
            finish_if_asked();
        }));
        drawing_links_.push_back(drawing_->capture_delay_changed.connect(*ui, [this](int) {
            if (click_ || drawing_pending_)
                schedule_auto();
        }));
        drawing_links_.push_back(drawing_->interaction_changed.connect(*ui, [this](bool active) {
            if (!click_ && !drawing_pending_)
                return;
            if (active) {
                auto_timer_.cancel();
                auto_progress_timer_.cancel();
                if (drawing_)
                    drawing_->set_capture_countdown(0);
            } else if (finish_after_snap_ && drawing_pending_)
                capture_auto();
            else
                schedule_auto();
        }));
        drawing_links_.push_back(drawing_->drawn.connect(*ui, [this](ScreenRef screen,
                                                                     MarksPtr marks) {
            if (!auto_can_capture(true))
                return;
            if (drawing_pending_ && drawing_pending_->first != screen) {
                drawing_next_ = std::pair{screen, marks};
                capture_auto();
                return;
            }
            click_.reset();
            drawing_pending_ = std::pair{screen, marks};
            drawing_->set_capture_pending(true);
            if (finish_after_snap_ || services_.settings.get(keys::walkthrough_capture_delay) == 0)
                capture_auto();
            else
                schedule_auto();
        }));
        if (services_.marks && !shortcuts.empty()) {
            auto register_marks = [this, alive, weak, ui, shortcuts](bool enabled) {
                services_.marks(enabled ? shortcuts : std::map<std::string, std::string>{},
                                [this, alive, weak, ui](std::string action) {
                                    ui->post([this, alive, weak, action = std::move(action)] {
                                        if (alive.lock() && weak.lock() == session() && drawing_)
                                            drawing_->handle_mark_shortcut(action);
                                    });
                                });
            };
            drawing_links_.push_back(drawing_->enabled_changed.connect(*ui, register_marks));
            register_marks(true);
        }
        if (panel_)
            panel_->set_auto_mode(true);
    } catch (...) {
        stop_auto();
        tell("Auto Mode couldn't watch for clicks. You can still use Snap.");
    }
}
bool WalkthroughRuntime::auto_can_capture(bool drawing) const {
    return auto_hook_ && drawing_ && session() && !services_.recording.paused() && !snap_ &&
           !auto_snap_ && !countdown_ && !timer_.active() && (drawing || !drawing_->armed());
}
void WalkthroughRuntime::schedule_auto() {
    if (!drawing_)
        return;
    auto_timer_.cancel();
    const auto delay = services_.settings.get(keys::walkthrough_capture_delay);
    auto_delay_ = static_cast<int>(delay);
    auto_deadline_ =
        services_.clock.steady() + (delay ? std::chrono::milliseconds(delay * 1000) : 350ms);
    auto_progress_timer_.cancel();
    drawing_->set_capture_countdown(static_cast<double>(delay), static_cast<double>(delay));
    if (drawing_->interacting())
        return;
    std::weak_ptr<int> alive = alive_;
    auto_timer_ = services_.ui.post_after(delay ? std::chrono::milliseconds(delay * 1000) : 350ms,
                                          [this, alive] {
                                              if (alive.lock())
                                                  capture_auto();
                                          });
    if (delay)
        auto_progress();
}
void WalkthroughRuntime::auto_progress() {
    if (!drawing_ || !auto_timer_.active())
        return;
    const double left = std::max(
        0.01, std::chrono::duration<double>(auto_deadline_ - services_.clock.steady()).count());
    drawing_->set_capture_countdown(left, static_cast<double>(auto_delay_));
    std::weak_ptr<int> alive = alive_;
    auto_progress_timer_ = services_.ui.post_after(33ms, [this, alive] {
        if (alive.lock())
            auto_progress();
    });
}
void WalkthroughRuntime::capture_auto() {
    if (!drawing_ || drawing_->interacting()) {
        auto_timer_.cancel();
        auto_progress_timer_.cancel();
        return;
    }
    auto_timer_.cancel();
    auto_progress_timer_.cancel();
    drawing_->set_capture_countdown(0);
    auto pending = std::exchange(drawing_pending_, std::nullopt);
    auto position = std::exchange(click_, std::nullopt);
    if ((!pending && !position) || !auto_can_capture(pending.has_value())) {
        finish_if_asked();
        return;
    }
    auto owner = session();
    auto region = drawing_->capture_region();
    std::optional<ScreenRef> screen;
    MarksPtr marks;
    std::optional<double> at;
    auto_drawing_ = pending.has_value();
    if (pending) {
        screen = pending->first;
        marks = pending->second;
        at = services_.recording.recording_duration();
        drawing_->set_busy(true);
    } else if (region) {
        screen = region->screen;
        marks = drawing_->pinned_on(screen);
    } else
        marks = drawing_->pinned_at(*position);
    std::optional<Rect> crop, source;
    if (region && screen && *screen == region->screen) {
        crop = region->rect;
        source = region->global;
    }
    SnapRequest request;
    request.take = SnapTake::Screen;
    request.screen = screen;
    if (!screen)
        request.click_at = position;
    const auto generation = ++auto_generation_;
    std::weak_ptr<int> alive = alive_;
    std::weak_ptr<WalkthroughSession> weak = owner;
    Executor* ui = &services_.ui;
    auto callback = [this, alive, weak, ui, generation, marks, at, crop, source,
                     screen](ShotImagePtr image, std::string reason) {
        ui->post([this, alive, weak, generation, image, marks, at, crop, source, screen,
                  reason = std::move(reason)] {
            if (alive.lock())
                auto_outcome(generation, weak, image, marks, at, crop, source, screen, reason);
        });
    };
    auto_snap_ = services_.snap.start(
        std::move(request), [callback](ShotImagePtr image) { callback(std::move(image), ""); },
        [callback] { callback(nullptr, ""); },
        [callback](std::string reason) { callback(nullptr, std::move(reason)); });
}
void WalkthroughRuntime::auto_outcome(std::uint64_t generation,
                                      std::weak_ptr<WalkthroughSession> weak, ShotImagePtr image,
                                      MarksPtr marks, std::optional<double> at,
                                      std::optional<Rect> crop, std::optional<Rect> source,
                                      std::optional<ScreenRef> screen, std::string reason) {
    if (generation != auto_generation_ || !auto_snap_)
        return;
    auto handle = std::move(auto_snap_);
    auto_drawing_ = false;
    ++auto_generation_;
    auto owner = weak.lock();
    bool captured = false;
    if (owner && owner == session() && drawing_) {
        if (image) {
            try {
                if (marks || crop)
                    image = services_.snap.render(image, crop, marks);
                auto shot =
                    owner->add_shot(image, at.value_or(services_.recording.recording_duration()));
                if (shot) {
                    captured = true;
                    services_.recording.set_destination_label(shots_label(owner->shot_count()));
                    if (panel_)
                        panel_->add_shot(shot->number, image, shot->kept_at_s,
                                         source ? source : handle->capture_rect());
                }
            } catch (const ScreenshotNotSaved& error) {
                tell("Couldn't save the screenshot (" + std::string(error.what()) +
                     ") — the walkthrough keeps recording");
            }
        } else if (!reason.empty())
            tell(reason);
    }
    auto next = std::exchange(drawing_next_, std::nullopt);
    auto next_snap = std::exchange(snap_after_drawing_, std::nullopt);
    if (drawing_) {
        drawing_->set_capture_pending(false, captured, screen);
        drawing_->set_busy(false);
    }
    if (next && owner == session() && drawing_) {
        drawing_pending_ = std::move(next);
        drawing_->set_capture_pending(true);
        if (finish_after_snap_)
            capture_auto();
        else
            schedule_auto();
    } else {
        finish_if_asked();
        if (next_snap)
            snap(*next_snap);
    }
}
void WalkthroughRuntime::discard_auto() {
    auto_after_ =
        std::chrono::duration<double>(services_.clock.steady().time_since_epoch()).count();
    auto_timer_.cancel();
    auto_progress_timer_.cancel();
    click_.reset();
    drawing_pending_.reset();
    drawing_next_.reset();
    snap_after_drawing_.reset();
    ++auto_generation_;
    auto handle = std::move(auto_snap_);
    auto_drawing_ = false;
    if (handle && handle->active())
        handle->drop();
    if (drawing_) {
        drawing_->set_capture_countdown(0);
        drawing_->set_capture_pending(false);
        drawing_->set_busy(false);
    }
}
void WalkthroughRuntime::stop_auto() {
    discard_auto();
    drawing_links_.clear();
    if (services_.marks)
        services_.marks({}, [](std::string) {});
    if (drawing_) {
        auto drawing = std::move(drawing_);
        drawing->close();
    }
    if (auto_hook_) {
        auto_hook_.reset();
        if (panel_)
            panel_->set_auto_mode(false);
    }
}
void WalkthroughRuntime::cleanup() {
    stop_auto();
    timer_.cancel();
    links_.clear();
    save_link_.disconnect();
    ++snap_generation_;
    auto handle = std::move(snap_);
    snap_for_.reset();
    if (handle && handle->active())
        handle->drop();
    if (std::exchange(excluded_, false))
        services_.overlay.keep_out_of_screenshots(false);
}
} // namespace fv::walkthrough
