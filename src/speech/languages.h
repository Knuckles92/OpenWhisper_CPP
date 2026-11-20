#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The languages the local engine's runtime takes (services/local_asr/languages.py), for
// Parakeet: the dropped engines' tables (Qwen, Nemotron, Moonshine, MLX, Unified) are not here.
namespace fv::speech {

// LANGUAGE_LABELS, in order: en English, ru Russian, es Spanish, fr French, pt Portuguese,
// zh Mandarin, auto Auto.
std::span<const std::pair<std::string_view, std::string_view>> language_labels();

// The codes `backend` (a selected_model value) accepts, in order: Parakeet's en, ru, es, fr, pt
// and auto; none for any other key.
std::span<const std::string_view> language_choices(std::string_view backend);

// The language a request uses: `stored` when the backend accepts it, else "en" for an
// English-only backend, else "auto".
std::string selected_language(std::string_view backend, std::optional<std::string_view> stored);

} // namespace fv::speech
