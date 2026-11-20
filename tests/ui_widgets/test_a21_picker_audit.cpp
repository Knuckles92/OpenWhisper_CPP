// Checks the M10 sweep (auditor AU7) added for the agent picker rows of
// docs/port/map/a21-actions-automation-ui.md (A21-59, A21-63, A21-81): the header's words and
// the model row's sizes.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <QLabel>
#include <QLineEdit>

#include "agents/installed_agents.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"

namespace {

bool hasLabel(const QWidget& parent, const QString& text) {
    for (const QLabel* label : parent.findChildren<QLabel*>()) {
        if (label->text() == text) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a21 audit: the agent picker's header names its purpose", "[widget]") {
    fv::agent_picker::resetScanSeam();
    fv::agents::set_installed_agents(nullptr);
    {
        fv::AgentPicker insights;
        CHECK(insights.purpose() == QStringLiteral("AI insights"));
        CHECK(hasLabel(insights, QStringLiteral("Who runs AI insights")));
        CHECK(hasLabel(insights, QStringLiteral("Coding agents you already use can run AI "
                                                "insights with their own sign-in.")));
        CHECK(insights.lookAgainButton()->text() == QStringLiteral("Look again"));
        CHECK(insights.lookAgainButton()->toolTip() ==
              QStringLiteral("Check this computer again, for example after installing an agent"));
        CHECK(insights.shortModelCombo()->minimumHeight() == 40);
        CHECK(insights.longModelCombo()->minimumHeight() == 40);
        CHECK(insights.longModelCombo()->lineEdit()->placeholderText() ==
              QStringLiteral("Type to filter models"));
        fv::AgentPicker cleanup(QStringLiteral("AI cleanup"));
        CHECK(hasLabel(cleanup, QStringLiteral("Who runs AI cleanup")));
    }
    CHECK(fv::agent_picker::waitForWorkers(std::chrono::seconds(5)));
}
