// services/dictation_language.py: the paired host's languages once it reports them
// (tests/test_personalize_s8_language.py).

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "speech/dictation_language.h"

namespace dl = fv::speech;
using fv::Json;
using Codes = std::vector<std::string>;

namespace {

constexpr const char* kFingerprint = "synthetic-reporting-host";

struct Forget {
    ~Forget() { dl::forget_remote_runtime(kFingerprint); }
};

// What RemoteBackend announces for a host's runtime report (note_remote_runtime's arguments).
dl::RemoteVoiceCapabilities report(const std::string& family,
                                   std::optional<std::vector<std::string>> languages,
                                   const std::string& selected = "") {
    return {.host = "devbox",
            .engine_label = family,
            .family = family,
            .languages = std::move(languages),
            .own_language = selected};
}

} // namespace

TEST_CASE("remote host languages once known", "[test_personalize_s8_language]") {
    // from
    // tests/test_personalize_s8_language.py::TestChoices::test_remote_host_languages_once_known
    const Forget forget;
    const Json settings{{"selected_model", "remote"},
                        {"remote_engine_client", {{"fingerprint", kFingerprint}}},
                        {"dictation_languages", Json::array({"en", "de", "es"})},
                        {"dictation_active_language", ""}};

    dl::note_remote_runtime(kFingerprint,
                            report("parakeet", Codes{"en", "ru", "es", "auto"}, "es"));
    CHECK(dl::language_choices(settings) == Codes{"en", "es"});
    CHECK(dl::engine_language(settings) == "es");

    // An OpenWhisper host's Whisper lists no languages: it takes every one.
    dl::note_remote_runtime(kFingerprint, report("local_whisper", Codes{}));
    CHECK(dl::language_choices(settings) == Codes{"en", "de", "es"});

    dl::note_remote_runtime(kFingerprint, report("moonshine", Codes{"en"}));
    CHECK(dl::language_choices(settings) == Codes{"en"});
    CHECK(dl::single_language_reason(settings) == "The other computer's engine uses one language.");

    dl::forget_remote_runtime(kFingerprint);
    CHECK(dl::language_choices(settings) == Codes{"en", "de", "es"});
}
