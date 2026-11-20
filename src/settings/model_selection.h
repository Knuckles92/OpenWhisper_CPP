#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>

#include "settings/store.h"

namespace fv {

// config.MODEL_VALUE_MAP's values that FioriVoce has, in OpenWhisper's order: "api",
// "parakeet", "remote" (architecture 7.2; the other engines are dropped).
std::span<const std::string_view> model_values();

// The saved backend when it is one of model_values(), else the default ("parakeet"). A saved
// pre-2.0 API name (api_whisper, api_gpt4o, api_gpt4o_mini) is rewritten to selected_model
// "api" with the model it meant in api_transcription_model, and reads "api". `today` is the
// local date the OpenAI retirement of 2027-02-26 is judged by (the overload without it uses the
// system clock's local date). Never throws: a failure is logged and reads as the default.
std::string load_model_selection(SettingsStore&);
std::string load_model_selection(SettingsStore&, std::chrono::year_month_day today);

// Saves `model`; std::invalid_argument "model_value must be a non-empty string" or
// "Invalid model '<x>'. Valid models: ['api', 'parakeet', 'remote']" first.
void save_model_selection(SettingsStore&, std::string_view model);

} // namespace fv
