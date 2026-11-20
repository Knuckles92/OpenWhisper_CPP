#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "focus/synthetic_keys.h"

// How fv_focus builds and injects INPUTs on Windows, with SendInput replaceable so tests see the
// INPUTs without injecting anything.
namespace fv::focus {

// SendInput, or a recorder in tests: takes the batch and returns how many events the OS took.
using SendInputFn = std::function<UINT(std::span<INPUT> inputs)>;

// Marks a keyboard or mouse INPUT as FioriVoce's own (kSyntheticInputTag in dwExtraInfo), so the
// hotkey hook neither matches nor records it. A hardware INPUT has no extra info.
void tag_input(INPUT& input);
// One keyboard INPUT per stroke, tagged.
std::vector<INPUT> keyboard_inputs(std::span<const KeyStroke> strokes);
// Tags every input, then hands the batch to `send`. Every INPUT fv_focus injects goes through
// here, so none leaves untagged.
UINT send_tagged(std::span<INPUT> inputs, const SendInputFn& send);

// The OS keyboard, injecting through `send_input` instead of SendInput.
std::shared_ptr<KeyboardPort> make_keyboard_port(SendInputFn send_input);

} // namespace fv::focus
