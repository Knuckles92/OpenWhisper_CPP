#pragma once

#include "ui_widgets/setting_tile.h"

namespace fv {

class Button;

// The "New in OpenWhisper_CPP" note at the top of Settings > Home (settings_home.py _build_intro):
// an intro tile (kind "intro", tileId "whatsNew") with the current appcore::whats_new() text and a
// "Got it" button. Home shows it until the note's `updated` date is saved as whats_new_seen.
// Affinity: GUI thread.
class WhatsNewTile : public InfoTile {
    Q_OBJECT
public:
    // The button's narrowest width, at 100 % text.
    static constexpr int kDismissWidth = 88;

    explicit WhatsNewTile(QWidget* parent = nullptr);

    Button* dismissButton() const { return dismiss_; }

Q_SIGNALS:
    // "Got it" was clicked; Home saves the note as seen and hides the tile once saved.
    void dismissRequested();

private:
    Button* dismiss_;
};

} // namespace fv
