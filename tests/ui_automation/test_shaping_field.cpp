// ShapingField (ui_qt/widgets/action_fields.py): how an action's words are prepared, with
// Profile and Transform only when there are some, and a deleted choice named as gone.
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>

#include "base/json.h"
#include "ui_automation/action_fields.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
using fv::ShapingField;

const ShapingField::Items kProfiles{{QStringLiteral("email"), QStringLiteral("Email")}};
const ShapingField::Items kTransforms{{QStringLiteral("formal"), QStringLiteral("Formal")},
                                      {QStringLiteral("short"), QStringLiteral("Shorter")}};

} // namespace

TEST_CASE("shaping starts cleaned up with Profile and Transform off until there are some",
          "[widget]") {
    ShapingField field;
    field.setChoices({}, {});
    CHECK(field.value() == QStringLiteral("cleanup"));
    CHECK(field.kind() == QStringLiteral("cleanup"));
    CHECK(field.combo()->isHidden());
    CHECK(field.hint()->text() == QStringLiteral("Your usual AI cleanup, whenever it's on."));
    CHECK(field.bar()->accessibleName() == QStringLiteral("How the words are prepared"));
    CHECK(field.bar()->buttons()[0]->isEnabled());
    CHECK(field.bar()->buttons()[1]->isEnabled());
    CHECK_FALSE(field.bar()->buttons()[2]->isEnabled());
    CHECK_FALSE(field.bar()->buttons()[3]->isEnabled());
    CHECK(field.bar()->buttons()[2]->toolTip() ==
          QStringLiteral("You have no cleanup profiles yet."));
    CHECK(field.bar()->buttons()[3]->toolTip() == QStringLiteral("You have no transforms yet."));

    field.setChoices(kProfiles, kTransforms);
    CHECK(field.bar()->buttons()[2]->isEnabled());
    CHECK(field.bar()->buttons()[3]->isEnabled());
    CHECK(field.bar()->buttons()[2]->toolTip().isEmpty());
}

TEST_CASE("shaping reads and writes every kind", "[widget]") {
    ShapingField field;
    field.setChoices(kProfiles, kTransforms);
    field.setValue(QStringLiteral("raw"));
    CHECK(field.value() == QStringLiteral("raw"));
    CHECK(field.hint()->text() ==
          QStringLiteral("Exactly what you said, after your dictionary and snippets."));
    field.setValue(QStringLiteral("profile:email"));
    CHECK(field.value() == QStringLiteral("profile:email"));
    CHECK_FALSE(field.combo()->isHidden());
    CHECK(field.combo()->currentText() == QStringLiteral("Email"));
    CHECK(field.combo()->accessibleName() == QStringLiteral("Cleanup profile"));
    CHECK(field.hint()->text() == QStringLiteral("Formatted by one of your cleanup profiles."));
    field.setValue(QStringLiteral("transform:short"));
    CHECK(field.value() == QStringLiteral("transform:short"));
    CHECK(field.combo()->count() == 2);
    CHECK(field.combo()->accessibleName() == QStringLiteral("Transform"));
    CHECK(field.hint()->text() ==
          QStringLiteral("Cleaned up, then rewritten by one of your transforms."));
    // Anything else is cleanup.
    field.setValue(QStringLiteral("loud"));
    CHECK(field.value() == QStringLiteral("cleanup"));
    field.setValue(QString());
    CHECK(field.value() == QStringLiteral("cleanup"));
}

TEST_CASE("a deleted profile shows that it no longer exists", "[widget]") {
    ShapingField field;
    field.setChoices(kProfiles, kTransforms);
    field.setValue(QStringLiteral("profile:gone"));
    CHECK(field.combo()->currentText() == QStringLiteral("Profile no longer exists"));
    CHECK(field.combo()->count() == 2);
    // The value keeps the missing id, so saving can ask for a new choice.
    CHECK(field.value() == QStringLiteral("profile:gone"));
    field.setValue(QStringLiteral("transform:gone"));
    CHECK(field.combo()->currentText() == QStringLiteral("Transform no longer exists"));
    // New choices keep the current value.
    field.setChoices(kProfiles, {{QStringLiteral("gone"), QStringLiteral("Back again")}});
    CHECK(field.value() == QStringLiteral("transform:gone"));
    CHECK(field.combo()->currentText() == QStringLiteral("Back again"));
}

TEST_CASE("choosing a kind fills its list and is a change", "[widget]") {
    ShapingField field;
    field.setChoices(kProfiles, kTransforms);
    QSignalSpy changed(&field, &ShapingField::changed);
    field.setValue(QStringLiteral("raw"));
    CHECK(changed.count() == 0);
    field.bar()->buttons()[3]->click();
    CHECK(changed.count() == 1);
    CHECK(field.value() == QStringLiteral("transform:formal"));
    Q_EMIT field.combo()->activated(1);
    CHECK(changed.count() == 2);
}

TEST_CASE("shaping choices come from the saved profiles and transforms", "[widget]") {
    ShapingField field;
    const Json settings{
        {"transcript_cleanup_profiles",
         Json::array({Json{{"id", "email"}, {"name", "Email"}, {"instructions", "Be brief."}}})},
        {"text_transforms",
         Json::array({Json{{"id", "formal"}, {"name", "Formal"}, {"instruction", "Formal."}}})}};
    field.setChoices(settings);
    field.setValue(QStringLiteral("profile:email"));
    CHECK(field.combo()->currentText() == QStringLiteral("Email"));
    field.setValue(QStringLiteral("transform:formal"));
    CHECK(field.combo()->currentText() == QStringLiteral("Formal"));
}
