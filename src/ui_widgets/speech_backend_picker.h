#pragma once

#include <span>
#include <string_view>

#include <QString>

class QComboBox;

namespace fv {

// GUI names and ordering for the speech engines FioriVoce has: Parakeet, the API backend and a
// paired computer ("Remote computer"). The ids are the selected_model values every setting
// stores (config.MODEL_VALUE_MAP); the display names are what signals carry and what pickers
// show. OpenWhisper's Apple-silicon annotations ("Parakeet MLX (recommended)", "Parakeet
// (CPU)") are not here: the engines they describe are dropped.

// The canonical display name of engine id; a value no engine has reads as the default engine's.
// Affinity: any thread.
QString backendDisplayName(std::string_view id);

// Adds one item per known id of ids, alphabetical by display name without regard to case, with
// the id as item data and the display name as the item's tooltip. An id no engine has is left
// out. Does not clear combo. Affinity: GUI thread.
void populateBackendCombo(QComboBox* combo, std::span<const std::string_view> ids);

} // namespace fv
