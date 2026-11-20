#include "speech/languages.h"

#include <algorithm>
#include <array>

namespace fv::speech {

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kLabels{{
    {"en", "English"},
    {"ru", "Russian"},
    {"es", "Spanish"},
    {"fr", "French"},
    {"pt", "Portuguese"},
    {"zh", "Mandarin"},
    {"auto", "Auto"},
}};

// Parakeet v3 covers 25 European languages; the runtime's prompts name these.
constexpr std::array<std::string_view, 6> kParakeet{"en", "ru", "es", "fr", "pt", "auto"};

} // namespace

std::span<const std::pair<std::string_view, std::string_view>> language_labels() {
    return kLabels;
}

std::span<const std::string_view> language_choices(std::string_view backend) {
    if (backend == "parakeet") {
        return kParakeet;
    }
    return {};
}

std::string selected_language(std::string_view backend, std::optional<std::string_view> stored) {
    const auto choices = language_choices(backend);
    if (stored && std::ranges::find(choices, *stored) != choices.end()) {
        return std::string(*stored);
    }
    if (choices.size() == 1 && choices[0] == "en") {
        return "en";
    }
    return "auto";
}

} // namespace fv::speech
