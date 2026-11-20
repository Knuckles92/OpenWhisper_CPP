#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

namespace fv::text::detail {

// data/cleanup_prompts.json, parsed once (tools/gen/cleanup_prompts.py writes it from
// OpenWhisper's config and services, so no prompt sentence is retyped here).
const Json& prompt_data();

// A string under prompt_data(); throws std::out_of_range when the generated file lacks it.
const std::string& prompt_text(std::string_view section, std::string_view name);

// A settings mapping as the readers expect one: `settings` when it is an object, else {} (Python
// passes `settings or {}`).
const Json& or_empty(const Json& settings);

} // namespace fv::text::detail
