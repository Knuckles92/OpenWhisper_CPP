#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "walkthrough/shot_image.h"

// What the live walkthrough column shows, without any painting (ui_qt/widgets/
// walkthrough_panel.py:63-318): its constants, paragraphs of ticks and the screenshots between
// them, and the easing and row maths the panel paints with. Qt-free: the panel keeps the
// picture behind each ShotItem's ShotImage. Affinity: the owner's thread (the GUI thread).
namespace fv::ui_walkthrough {

// The column's width, and its gap from the screen's right edge.
inline constexpr int kWidth = 296;
inline constexpr int kScreenMargin = 22;
// The column's share of the screen's height, and the most it grows to.
inline constexpr double kHeightShare = 0.74;
inline constexpr int kMaxHeight = 820;
// Room around the card for its shadow.
inline constexpr int kShadow = 14;
inline constexpr int kPadding = 22;
inline constexpr int kRadius = 18;

// One tick per this many seconds of speech: about a syllable.
inline constexpr double kTickS = 0.075;
// Loudness under this is the room, not the voice: it writes no tick.
inline constexpr double kSpeechGate = 0.14;
inline constexpr double kTickW = 2.0;
inline constexpr double kTickStep = 4.0;
inline constexpr double kTickMin = 3.0;
inline constexpr double kTickMax = 12.0;
inline constexpr double kRowPitch = 18.0;
// A finished row's line, once its ticks have closed up.
inline constexpr double kLineH = 5.0;
inline constexpr double kGap = 14.0;
inline constexpr double kShotMaxH = 190.0;
// Screenshots are kept this many pixels wide: sharp at 2x.
inline constexpr int kThumbPx = 2 * kWidth;

// The open slot of the section being spoken.
inline constexpr double kSlotPadX = 10.0;
inline constexpr double kSlotPadTop = 8.0;
inline constexpr double kSlotPadBottom = 10.0;
inline constexpr double kSlotBadge = 20.0;
inline constexpr double kSlotTimeW = 34.0;
inline constexpr double kSlotHintH = 16.0;
inline constexpr std::string_view kSlotHint = "Talking… Snap to close this section";
inline constexpr std::string_view kAutoSlotHint = "Auto Mode · captures clicks and drawings";
inline constexpr std::string_view kPausedHint = "Paused · Resume to keep talking";
// The tools that hover over a screenshot, and its number badge.
inline constexpr double kTool = 28.0;
inline constexpr double kToolGap = 4.0;
inline constexpr double kBadgeH = 18.0;
// The footer's buttons.
inline constexpr double kFootH = 40.0;
inline constexpr double kFootGap = 6.0;
// A square footer button, holding only an icon.
inline constexpr double kFootIconW = 40.0;
// The Snap button's part that opens its menu.
inline constexpr double kSnapMenuW = 26.0;
// A note's purple row: its padding, and its "Note for the agent" line.
inline constexpr double kNotePadX = 8.0;
inline constexpr double kNotePadTop = 6.0;
inline constexpr double kNoteHeadH = 15.0;
inline constexpr double kNotePadBottom = 7.0;
inline constexpr std::string_view kNoteHint = "Note for the agent · not in the document";

inline constexpr int kFrameMs = 16;
inline constexpr double kTickGrowS = 0.12;
inline constexpr double kShotInS = 0.5;
inline constexpr double kMorphS = 0.7;
inline constexpr double kFadeS = 0.32;
// How long the finished column says what the walkthrough came to.
inline constexpr double kHoldS = 2.6;
// The document never came (it failed without saying so): stop waiting. Past the slowest real
// job: a long walkthrough transcribed on the CPU, then an AI write-up, which may take up to 5
// minutes.
inline constexpr double kGiveUpS = 600.0;
// One mouse-wheel notch scrolls the column this far.
inline constexpr double kWheelStep = 60.0;

// What each control says when the pointer rests on it (TOOLTIPS), by key: "snap" (with
// "{mode}" in it), "snap-menu", "note", "note-off", "undo", "pause", "resume", "cancel",
// "cancel-writing", "done", "up", "down", "remove". "" for any other key.
std::string_view tooltip_template(std::string_view key);

// What the column says over a control: the template of `key` with {mode} replaced by
// `mode_label` in lower case, then Auto Mode's own words for Snap (unless counting down) and
// its menu. "" when the control says nothing.
std::string tooltip_text(std::string_view key, std::string_view mode_label, bool auto_mode,
                         int countdown);

// What was said between two screenshots: one height (0-1) per tick.
struct Paragraph {
    std::vector<double> ticks;
    // When each tick was written, to grow it in.
    std::vector<double> born;
    // Said while Hold for note was held: for the agent, not the document.
    bool note = false;
    // A note's span in seconds of recording; it ends nullopt while held.
    double started_s = 0.0;
    std::optional<double> ended_s;
};

struct ShotItem {
    // The screenshot's number as kept: it never changes, whatever its place.
    int number = 0;
    walkthrough::ShotImagePtr image;
    double born = 0.0;
    // Seconds into the recording when it was kept.
    double kept_at_s = 0.0;
};

using Item = std::variant<Paragraph, ShotItem>;

// Paragraphs and screenshots. hear() takes microphone levels as they arrive; advance() turns
// the loudest of them into a tick every kTickS while the user speaks. A section is the
// paragraphs up to and including a screenshot; the last one, still being spoken, has none.
class WalkthroughColumn {
public:
    WalkthroughColumn();

    // Every change ends the items on a Paragraph.
    std::vector<Item> items;
    // Hold for note is held: the voice writes a note.
    bool holding = false;
    double speech_s = 0.0;

    int shots() const;

    void hear(double level);
    void advance(double dt, double now);
    void add_shot(int number, walkthrough::ShotImagePtr image, double now, double kept_at_s = 0.0);
    // Hold for note was pressed (on) or let go, at_s into the recording.
    void hold_note(bool on, double at_s);
    // The items in sections; the last is the one being spoken.
    std::vector<std::vector<Item>> sections() const;
    // Screenshot number's place on the board, from 1; 0 once it is gone.
    int place(int number) const;
    // Takes screenshot number out; its words open the next section.
    bool remove_shot(int number);
    // Moves screenshot number and its words step sections (-1 up, 1 down).
    bool move_shot(int number, int step);

    const ShotItem* shot(int number) const;

private:
    void rebuild(std::vector<std::vector<Item>> sections);
    Paragraph& paragraph();

    double peak_ = 0.0;
    double carry_ = 0.0;
};

// Python's min(1, max(0, t)) eased: cubic out, and back out with an overshoot of 1.4.
double ease_out(double t);
double ease_out_back(double t);

// Ticks in one row width pixels wide (_per_row), and the rows a paragraph takes (_rows).
int per_row(double width);
int rows(const Paragraph& paragraph, double width);

} // namespace fv::ui_walkthrough
