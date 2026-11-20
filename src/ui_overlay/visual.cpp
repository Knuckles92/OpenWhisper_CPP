#include "ui_overlay/visual.h"

#include <array>
#include <cstdint>
#include <unordered_map>

#include "ui_overlay/aurora_look.h"
#include "ui_overlay/bars_look.h"
#include "ui_overlay/dots_look.h"
#include "ui_overlay/mirror_wave_look.h"
#include "ui_overlay/ocean_looks.h"
#include "ui_overlay/pulse_look.h"
#include "ui_overlay/ribbon_look.h"
#include "ui_theme/palette.h"
#include "uimath/color.h"
#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

namespace fv {

Visual::Visual(const LookContext& context) : context_(context) {}

void Visual::beginWork() {
    snapshot_.take(ribbon().history());
}

double Visual::hue() const {
    return context_.style->hue();
}

double Visual::snapAt(double f) const {
    return snapshot_.at(f);
}

QColor hueColor(double hue, double alpha, bool bright) {
    QColor color = ui::currentPalette().isDark()
                       ? QColor::fromHsv(hue_degrees(hue), bright ? 190 : 150, 245)
                       : QColor::fromHsv(hue_degrees(hue), 235, 205);
    color.setAlpha(clamp_alpha(alpha));
    return color;
}

bool darkTheme() {
    return ui::currentPalette().isDark();
}

namespace {

std::uint32_t packRgb(const Rgba& color) {
    return (static_cast<std::uint32_t>(color.r) << 16) |
           (static_cast<std::uint32_t>(color.g) << 8) | static_cast<std::uint32_t>(color.b);
}

// The hue_color families (overlay_look.h), keyed by the 8-bit colour each (hue, saturation,
// value) reads back as. Within a family neighbouring hues are several units apart in some
// channel, so the key finds the one colour; the first family holding a key keeps it.
const std::unordered_map<std::uint32_t, Hsv>& hueFamilies() {
    static const auto table = [] {
        std::unordered_map<std::uint32_t, Hsv> made;
        constexpr std::array<std::array<int, 2>, 3> kFamilies = {
            {{190, 245}, {150, 245}, {235, 205}}};
        for (const auto& [saturation, value] : kFamilies) {
            for (int h = 0; h < 360; ++h) {
                const Hsv hsv{h, saturation, value};
                made.emplace(packRgb(to_rgba(hsv)), hsv);
            }
        }
        return made;
    }();
    return table;
}

} // namespace

QColor toQColor(const Rgba& color) {
    // Qt keeps a colour made from HSV in 16-bit channels and paints with them; the 8-bit read
    // back (Rgba) would blend differently by a unit here and there. A look colour is rebuilt as
    // Python built it, QColor::fromHsv; any other colour is taken as it reads.
    const auto& families = hueFamilies();
    const auto found = families.find(packRgb(color));
    if (found != families.end()) {
        QColor rebuilt = QColor::fromHsv(found->second.h, found->second.s, found->second.v);
        rebuilt.setAlpha(color.a);
        return rebuilt;
    }
    return QColor(color.r, color.g, color.b, color.a);
}

QPointF toQPointF(const PointD& point) {
    return QPointF(point.x, point.y);
}

std::unique_ptr<Visual> makeVisual(std::string_view look, const LookContext& context) {
    const std::string_view normalized = normalize_look(look);
    if (normalized == kLookRibbon) {
        return std::make_unique<RibbonLook>(context);
    }
    if (normalized == kLookBars) {
        return std::make_unique<BarsLook>(context);
    }
    if (normalized == kLookWave) {
        return std::make_unique<MirrorWaveLook>(context);
    }
    if (normalized == kLookAurora) {
        return std::make_unique<AuroraLook>(context);
    }
    if (normalized == kLookPulse) {
        return std::make_unique<PulseLook>(context);
    }
    if (normalized == kLookDots) {
        return std::make_unique<DotsLook>(context);
    }
    if (normalized == kLookBaitBall) {
        return std::make_unique<BaitBallLook>(context);
    }
    if (normalized == kLookFish) {
        return std::make_unique<RealFishLook>(context);
    }
    if (normalized == kLookGlow) {
        return std::make_unique<DeepGlowLook>(context);
    }
    if (normalized == kLookFizz) {
        return std::make_unique<FizzLook>(context);
    }
    return nullptr;
}

} // namespace fv
