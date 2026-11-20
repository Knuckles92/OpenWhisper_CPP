// services/local_asr/languages.py: speech/languages.h against its golden vectors and
// tests/test_local_asr_languages.py.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "golden/golden.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "speech/languages.h"

using fv::Json;

TEST_CASE("golden languages", "[golden]") {
    fv::golden::for_each("services.local_asr.languages", "LANGUAGE_LABELS",
                         [](const fv::golden::Case& c) {
                             Json labels = Json::array();
                             for (const auto& [code, text] : fv::speech::language_labels()) {
                                 labels.push_back(Json::array({code, text}));
                             }
                             CHECK(fv::golden::equal(c.result<Json>(), labels));
                         });
    fv::golden::for_each(
        "services.local_asr.languages", "language_choices", [](const fv::golden::Case& c) {
            std::vector<std::string> choices;
            for (const auto code :
                 fv::speech::language_choices(std::string_view(c.arg<std::string>(0)))) {
                choices.emplace_back(code);
            }
            CHECK(choices == c.result<std::vector<std::string>>());
        });
    fv::golden::for_each("services.local_asr.languages", "selected_language",
                         [](const fv::golden::Case& c) {
                             const auto stored = c.arg<std::optional<std::string>>(1);
                             const std::optional<std::string_view> view =
                                 stored ? std::optional<std::string_view>(*stored) : std::nullopt;
                             CHECK(fv::speech::selected_language(c.arg<std::string>(0), view) ==
                                   c.result<std::string>());
                         });
}

// from
// tests/test_local_asr_languages.py::test_switching_mandarin_to_parakeet_selects_auto_without_saving
// The engine field is M2's widget; what it shows comes from selected_language, which reads the
// saved Mandarin as Auto for Parakeet and writes nothing.
TEST_CASE("switching mandarin to parakeet selects auto without saving",
          "[test_local_asr_languages]") {
    fv::MemorySettingsStore store;
    store.update(Json{{"local_asr_language", "zh"}});
    const auto writes = store.file().writes();
    const std::string saved = store.get(fv::keys::local_asr_language);
    CHECK(fv::speech::selected_language("parakeet", saved) == "auto");
    CHECK(store.get(fv::keys::local_asr_language) == "zh");
    CHECK(store.file().writes() == writes);
}

TEST_CASE("parakeet takes five languages and auto") {
    std::vector<std::string> choices;
    for (const auto code : fv::speech::language_choices(std::string_view("parakeet"))) {
        choices.emplace_back(code);
    }
    CHECK(choices == std::vector<std::string>{"en", "ru", "es", "fr", "pt", "auto"});
    CHECK(fv::speech::language_choices(std::string_view("api")).empty());
    CHECK(fv::speech::selected_language("parakeet", std::nullopt) == "auto");
}
