#pragma once

#include <array>
#include <string_view>
#include <utility>

#include "settings/store.h"

namespace fv {

// services.app_styles.NEW_INSTALL_TONES: {category: tone} seeded once on a brand-new install.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
    kNewInstallAppStyleTones{{
        {"email", "formal"},
        {"work", "casual"},
        {"personal", "casual"},
        {"other", "formal"},
    }};

// Writes the defaults a brand-new install starts with (app_style_tones =
// kNewInstallAppStyleTones), once; true when written. An existing install keeps no
// app_style_tones key, which reads as every category Formal. A failed write is logged and
// reads false.
bool seed_new_install_settings(SettingsStore&);

// Saves the engine of a new install that never chose one ("parakeet"), so a later change of
// the default never switches it; true when written. Writes only when selected_model is absent.
// OpenWhisper's branch for an existing Linux install (keep Local Whisper) is dropped with that
// engine. A failed write is logged and reads false.
bool keep_unchosen_engine(SettingsStore&);

} // namespace fv
