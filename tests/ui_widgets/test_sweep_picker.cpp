// Sweep H2: open item "ui_widgets/text_model_picker.cpp: ... setCredentialStatus matches an
// answer by URL alone, so two endpoints at one address with different key variables can take
// each other's late answer (a provider-id overload would close it)".

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <QLabel>
#include <QString>

#include "base/json.h"
#include "llm/profiles.h"
#include "ui_widgets/text_model_picker.h"

TEST_CASE("a credential answer for another endpoint at the same address is stale",
          "[sweep_picker][widget]") {
    fv::Json settings = fv::Json::object();
    const auto first =
        fv::llm::upsert_custom_profile(settings, "Lab A", "http://lab.local:8000/v1", "LAB_A_KEY");
    const auto second =
        fv::llm::upsert_custom_profile(settings, "Lab B", "http://lab.local:8000/v1", "LAB_B_KEY");
    REQUIRE(first.id != second.id);
    REQUIRE(first.base_url == second.base_url);
    const QString url = QString::fromStdString(*first.base_url);
    const QString firstId = QString::fromStdString(first.id);
    const QString secondId = QString::fromStdString(second.id);

    fv::TextModelPicker picker;
    picker.setProfiles(fv::llm::list_profiles(settings));
    picker.setProvider(firstId);
    REQUIRE(picker.provider() == firstId);
    // The store was being read for Lab A when the user moved on to Lab B.
    picker.setProvider(secondId);
    picker.setCredentialStatus(QStringLiteral("LAB_A_KEY found"), true, url, firstId);
    CHECK(picker.requirementLabel()->text().isEmpty());

    picker.setCredentialStatus(QStringLiteral("Requires LAB_B_KEY"), false, url, secondId);
    CHECK(picker.requirementLabel()->toolTip() == QStringLiteral("Requires LAB_B_KEY"));
    // A late answer for Lab A, with the same address, leaves Lab B's alone.
    picker.setCredentialStatus(QStringLiteral("LAB_A_KEY found"), true, url, firstId);
    CHECK(picker.requirementLabel()->toolTip() == QStringLiteral("Requires LAB_B_KEY"));

    // Still for the shown address only.
    picker.setCredentialStatus(QStringLiteral("LAB_B_KEY found"), true,
                               QStringLiteral("http://elsewhere:9000/v1"), secondId);
    CHECK(picker.requirementLabel()->toolTip() == QStringLiteral("Requires LAB_B_KEY"));
    picker.setCredentialStatus(QStringLiteral("LAB_B_KEY found"), true, url, secondId);
    CHECK(picker.requirementLabel()->text() == QStringLiteral("LAB_B_KEY found"));
}
