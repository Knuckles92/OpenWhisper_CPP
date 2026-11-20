// Sweep S2 (open-items A29): Home's Cleanup level combo is one of Home's named controls, as
// settings_home.py's _cleanup_level puts it in controls[TRANSCRIPT_CLEANUP_LEVEL], without being
// a projection of the AI cleanup page's control.
#include <catch2/catch_test_macros.hpp>

#include <QComboBox>

#include "cleanup_support.h"
#include "settings/keys.h"

using fv::Json;
using namespace fvtest;

TEST_CASE("Home names its Cleanup level combo under transcript_cleanup_level", "[widget]") {
    CleanupRig rig(
        Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_level", "high"}});
    QComboBox* combo = rig.homeLevelCombo();
    CHECK(rig.home().control(fv::keys::transcript_cleanup_level.key()) == combo);
    CHECK(combo->currentData().toString() == QStringLiteral("high"));
    // Naming it builds nothing: the AI cleanup page is still unbuilt.
    CHECK_FALSE(rig.dialog().isBuilt(fv::destinations::cleanup));
}
