#include "ui_theme/theme.h"

#include "ui_theme/palette.h"

namespace fv::ui {

Theme Theme::fromPalette(const Palette& palette) {
    Theme theme;
#define FV_THEME_ASSIGN(member, token) theme.member = palette.color(token);
    FV_THEME_COLOURS(FV_THEME_ASSIGN)
#undef FV_THEME_ASSIGN
    return theme;
}

const Theme& currentTheme() {
    // Rebuilt only when another palette becomes current, so a paint reads members.
    static const Palette* source = nullptr;
    static Theme theme;
    const Palette& palette = currentPalette();
    if (source != &palette) {
        theme = Theme::fromPalette(palette);
        source = &palette;
    }
    return theme;
}

} // namespace fv::ui
