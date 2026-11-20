#include "ui_walkthrough/walkthrough_column.h"

#include <array>
#include <cctype>
#include <cmath>
#include <utility>

#include "uimath/easing.h"
#include "uimath/loudness.h"

namespace fv::ui_walkthrough {
namespace {

struct Tooltip {
    std::string_view key;
    std::string_view text;
};

constexpr std::array<Tooltip, 13> kTooltips{{
    {"snap", "Take a screenshot: {mode}"},
    {"snap-menu", "Choose what Snap takes"},
    {"note", "Hold while you talk: it goes to the AI write-up as an instruction, not into the "
             "document"},
    {"note-off", "Turn on AI write-up on the Walkthrough tab to leave notes for it"},
    {"undo", "Undo the last screenshot; its words stay"},
    {"pause", "Pause: nothing is recorded until you resume"},
    {"resume", "Resume recording"},
    {"cancel", "Cancel the walkthrough: nothing is kept"},
    {"cancel-writing", "Cancel: nothing is kept, unless the AI is already writing it up; then "
                       "it's kept as spoken"},
    {"done", "Finish the walkthrough"},
    {"up", "Move up, with its words"},
    {"down", "Move down, with its words"},
    {"remove", "Remove the screenshot; its words join the next one's"},
}};

// Python's max(a, b) and min(a, b): the first argument unless the second compares past it.
double py_max(double a, double b) {
    return b > a ? b : a;
}
double py_min(double a, double b) {
    return b < a ? b : a;
}

double clamp01(double t) {
    return py_min(1.0, py_max(0.0, t));
}

// Python's float floor division (float_floor_div in floatobject.c), so a row holds exactly as
// many ticks as OpenWhisper's.
double floor_div(double vx, double wx) {
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) {
            div -= 1.0;
        }
    }
    double floordiv = 0.0;
    if (div != 0.0) {
        floordiv = std::floor(div);
        if (div - floordiv > 0.5) {
            floordiv += 1.0;
        }
    } else {
        floordiv = std::copysign(0.0, vx / wx);
    }
    return floordiv;
}

bool is_paragraph(const Item& item) {
    return std::holds_alternative<Paragraph>(item);
}

} // namespace

std::string_view tooltip_template(std::string_view key) {
    for (const Tooltip& tip : kTooltips) {
        if (tip.key == key) {
            return tip.text;
        }
    }
    return {};
}

std::string tooltip_text(std::string_view key, std::string_view mode_label, bool auto_mode,
                         int countdown) {
    std::string text(tooltip_template(key));
    // str.format(mode=...): only "snap" has a field, and no template has braces otherwise.
    if (const auto at = text.find("{mode}"); at != std::string::npos) {
        std::string lower(mode_label);
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        text.replace(at, 6, lower);
    }
    if (auto_mode) {
        if (key == "snap" && countdown == 0) {
            text = "Draw on the live screen with the pen. Release to save a screenshot.";
        } else if (key == "snap-menu") {
            text = "Take a screenshot manually";
        }
    }
    return text;
}

WalkthroughColumn::WalkthroughColumn() {
    items.emplace_back(Paragraph{});
}

int WalkthroughColumn::shots() const {
    int count = 0;
    for (const Item& item : items) {
        count += std::holds_alternative<ShotItem>(item) ? 1 : 0;
    }
    return count;
}

void WalkthroughColumn::hear(double level) {
    peak_ = py_max(peak_, level_to_height(level));
}

void WalkthroughColumn::advance(double dt, double now) {
    carry_ += dt;
    while (carry_ >= kTickS) {
        carry_ -= kTickS;
        const double peak = std::exchange(peak_, 0.0);
        if (peak >= kSpeechGate) {
            // Heights spread over the voice's range, not the room's.
            paragraph().ticks.push_back((peak - kSpeechGate) / (1.0 - kSpeechGate));
            paragraph().born.push_back(now);
            speech_s += kTickS;
        }
    }
}

void WalkthroughColumn::add_shot(int number, walkthrough::ShotImagePtr image, double now,
                                 double kept_at_s) {
    if (auto* last = std::get_if<Paragraph>(&items.back())) {
        if (last->ticks.empty()) {
            // Nothing was said since the last screenshot: no empty paragraph.
            items.pop_back();
        } else if (last->note && holding) {
            last->ended_s = kept_at_s;
        }
    }
    items.emplace_back(ShotItem{number, std::move(image), now, kept_at_s});
    // A note still held goes on after the screenshot.
    Paragraph next;
    next.note = holding;
    next.started_s = kept_at_s;
    items.emplace_back(std::move(next));
}

void WalkthroughColumn::hold_note(bool on, double at_s) {
    if (on == holding) {
        return;
    }
    holding = on;
    if (on) {
        if (const auto* last = std::get_if<Paragraph>(&items.back()); last && last->ticks.empty()) {
            items.pop_back();
        }
        Paragraph note;
        note.note = true;
        note.started_s = at_s;
        items.emplace_back(std::move(note));
        return;
    }
    // Every change ends the items on a paragraph, so the last item is the held note.
    Paragraph& last = std::get<Paragraph>(items.back());
    last.ended_s = at_s;
    if (last.ticks.empty()) {
        // Held in silence: nothing to show.
        items.pop_back();
    }
    items.emplace_back(Paragraph{});
}

std::vector<std::vector<Item>> WalkthroughColumn::sections() const {
    std::vector<std::vector<Item>> result(1);
    for (const Item& item : items) {
        result.back().push_back(item);
        if (std::holds_alternative<ShotItem>(item)) {
            result.emplace_back();
        }
    }
    return result;
}

int WalkthroughColumn::place(int number) const {
    int index = 0;
    for (const Item& item : items) {
        if (const auto* shot = std::get_if<ShotItem>(&item)) {
            ++index;
            if (shot->number == number) {
                return index;
            }
        }
    }
    return 0;
}

const ShotItem* WalkthroughColumn::shot(int number) const {
    for (const Item& item : items) {
        if (const auto* shot = std::get_if<ShotItem>(&item); shot && shot->number == number) {
            return shot;
        }
    }
    return nullptr;
}

bool WalkthroughColumn::remove_shot(int number) {
    auto split = sections();
    const int index = place(number) - 1;
    if (index < 0) {
        return false;
    }
    const auto at = static_cast<std::size_t>(index);
    std::vector<Item> removed = std::move(split[at]);
    split.erase(split.begin() + index);
    removed.pop_back(); // the screenshot itself
    split[at].insert(split[at].begin(), std::make_move_iterator(removed.begin()),
                     std::make_move_iterator(removed.end()));
    rebuild(std::move(split));
    return true;
}

bool WalkthroughColumn::move_shot(int number, int step) {
    auto split = sections();
    const int index = place(number) - 1;
    const int to = index + step;
    // The section being spoken stays last.
    if (index < 0 || !(0 <= to && to < static_cast<int>(split.size()) - 1)) {
        return false;
    }
    std::swap(split[static_cast<std::size_t>(index)], split[static_cast<std::size_t>(to)]);
    rebuild(std::move(split));
    return true;
}

void WalkthroughColumn::rebuild(std::vector<std::vector<Item>> split) {
    std::vector<Item> rebuilt;
    for (auto& section : split) {
        for (auto& item : section) {
            auto* incoming = std::get_if<Paragraph>(&item);
            auto* previous = rebuilt.empty() ? nullptr : std::get_if<Paragraph>(&rebuilt.back());
            if (incoming && previous && !incoming->note && !previous->note) {
                // Words that now meet run on as one paragraph; notes stay apart.
                previous->ticks.insert(previous->ticks.end(), incoming->ticks.begin(),
                                       incoming->ticks.end());
                previous->born.insert(previous->born.end(), incoming->born.begin(),
                                      incoming->born.end());
            } else {
                rebuilt.push_back(std::move(item));
            }
        }
    }
    if (rebuilt.empty() || !is_paragraph(rebuilt.back())) {
        rebuilt.emplace_back(Paragraph{});
    }
    items = std::move(rebuilt);
}

Paragraph& WalkthroughColumn::paragraph() {
    return std::get<Paragraph>(items.back());
}

double ease_out(double t) {
    t = clamp01(t);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

double ease_out_back(double t) {
    // The column's own overshoot: its screenshots settle gently.
    return fv::ease_out_back(clamp01(t), 1.4);
}

int per_row(double width) {
    const double fit = floor_div(width + kTickStep - kTickW, kTickStep);
    return fit > 1.0 ? static_cast<int>(fit) : 1;
}

int rows(const Paragraph& paragraph, double width) {
    const int per = per_row(width);
    const auto count = static_cast<int>(paragraph.ticks.size());
    return (count + per - 1) / per;
}

} // namespace fv::ui_walkthrough
