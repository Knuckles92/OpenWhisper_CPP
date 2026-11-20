// test_strict_switch_reads.py::test_a_malformed_consent_is_not_consent: a saved consent that is
// not a real true (a string, say) reads as no consent. OpenWhisper_CPP authorizes meeting insights
// by per-destination grants (MeetingPresenter::askIntelligenceConsent); the saved switch it still
// writes is read strictly as well.
#include <catch2/catch_test_macros.hpp>

#include "base/json.h"
#include "settings/keys.h"

TEST_CASE("test_a_malformed_consent_is_not_consent", "[unit]") {
    using fv::Json;
    for (const Json& saved : {Json("no"), Json("yes"), Json(1), Json("true"), Json(nullptr)}) {
        INFO(saved.dump());
        CHECK_FALSE(fv::keys::meeting_cloud_consent_given.read(
            Json{{"meeting_cloud_consent_given", saved}}));
    }
    CHECK(fv::keys::meeting_cloud_consent_given.read(Json{{"meeting_cloud_consent_given", true}}));
}
