#pragma once

#include <QObject>

#include "walkthrough/runtime_ports.h"

namespace fv {
class WalkthroughPanel;

class WalkthroughPanelPort final : public QObject, public walkthrough::PanelPort {
public:
    explicit WalkthroughPanelPort(WalkthroughPanel& panel);
    walkthrough::PanelState state() const override;
    bool paused() const override;
    void begin() override;
    void hear(double level) override;
    void add_shot(int number, walkthrough::ShotImagePtr image, double at,
                  std::optional<walkthrough::Rect> source) override;
    void end_capture_animation() override;
    void remove_shot(int number) override;
    void move_shot(int number, int step) override;
    bool shows_recording() const override;
    void set_snap_mode(const std::string& mode, std::optional<std::string> shortcut) override;
    void set_auto_mode(bool enabled) override;
    void set_countdown(int seconds) override;
    void set_paused(bool paused) override;
    void set_notes_on(bool on) override;
    std::vector<std::string> snap_modes() const override;
    void finish() override;
    void complete(int shots, double duration) override;
    void dismiss() override;

private:
    WalkthroughPanel& panel_;
};
} // namespace fv
