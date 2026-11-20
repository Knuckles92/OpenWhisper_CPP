#pragma once

#include <optional>
#include <string>
#include <vector>

class QComboBox;

namespace fv {

// settings_models.py's _fill_thinking_combo: lists the offered thinking levels in combo (label
// from llm::thinking_label, level as item data) and selects `saved`, or the nearest offered one.
// With nothing offered the combo holds one disabled "Provider default" item with no data.
// Returns the level shown (nullopt when none is offered). Filling never emits a signal and never
// saves. Affinity: GUI thread.
std::optional<std::string> fillThinkingCombo(QComboBox* combo,
                                             const std::vector<std::string>& offered,
                                             const std::string& saved);

} // namespace fv
