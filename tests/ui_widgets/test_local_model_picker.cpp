// The compact local-model assignment of Settings: rebuilt with signals blocked, and announcing
// only a person's real change.
#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include "ui_widgets/local_model_picker.h"

namespace {

std::vector<fv::LocalModelPicker::Model> two() {
    return {{QStringLiteral("parakeet-v3"), QStringLiteral("Parakeet TDT 0.6B v3")},
            {QStringLiteral("orukeet-v0.1"), QStringLiteral("Orukeet TDT 0.6B")}};
}

} // namespace

TEST_CASE("the picker rebuilds its combo without announcing it", "[widget]") {
    fv::LocalModelPicker picker;
    QSignalSpy changed(&picker, &fv::LocalModelPicker::modelChanged);
    CHECK(picker.objectName() == QStringLiteral("localModelPicker"));
    CHECK(picker.modelCombo()->objectName() == QStringLiteral("localModelPickerCombo"));
    CHECK(picker.modelCombo()->minimumHeight() == 40);
    CHECK(picker.manageButton()->objectName() == QStringLiteral("localModelPickerManage"));
    CHECK(picker.manageButton()->text() == QStringLiteral("Manage downloads"));
    CHECK(picker.manageButton()->toolTip() ==
          QStringLiteral("Open Downloads to add or delete speech models"));
    CHECK(picker.captionLabel()->objectName() == QStringLiteral("localModelPickerCaption"));

    picker.setModels(two(), QStringLiteral("orukeet-v0.1"));
    CHECK(picker.modelCombo()->count() == 2);
    CHECK(picker.modelCombo()->itemText(0) == QStringLiteral("Parakeet TDT 0.6B v3"));
    // Each item's tooltip is its key.
    CHECK(picker.modelCombo()->itemData(1, Qt::ToolTipRole).toString() ==
          QStringLiteral("orukeet-v0.1"));
    CHECK(picker.currentModel() == QStringLiteral("orukeet-v0.1"));
    CHECK(changed.count() == 0);

    // A model the list does not have falls back to its first.
    picker.setModels(two(), QStringLiteral("whisper-large"));
    CHECK(picker.currentModel() == QStringLiteral("parakeet-v3"));
    CHECK(changed.count() == 0);
}

TEST_CASE("the picker announces a real change once", "[widget]") {
    fv::LocalModelPicker picker;
    picker.setModels(two(), QStringLiteral("parakeet-v3"));
    QSignalSpy changed(&picker, &fv::LocalModelPicker::modelChanged);

    picker.modelCombo()->setCurrentIndex(1);
    REQUIRE(changed.count() == 1);
    CHECK(changed.front().front().toString() == QStringLiteral("orukeet-v0.1"));
    CHECK(picker.currentModel() == QStringLiteral("orukeet-v0.1"));

    picker.modelCombo()->setCurrentIndex(0);
    CHECK(changed.count() == 2);
    // Showing a model from outside moves the combo without a signal and stages it.
    picker.setCurrent(QStringLiteral("orukeet-v0.1"));
    CHECK(changed.count() == 2);
    CHECK(picker.modelCombo()->currentIndex() == 1);
    CHECK(picker.currentModel() == QStringLiteral("orukeet-v0.1"));
    // An unlisted key is ignored.
    picker.setCurrent(QStringLiteral("unknown"));
    CHECK(picker.currentModel() == QStringLiteral("orukeet-v0.1"));
}

TEST_CASE("the manage link and the caption", "[widget]") {
    fv::LocalModelPicker picker;
    picker.setModels(two(), QStringLiteral("parakeet-v3"));
    QSignalSpy manage(&picker, &fv::LocalModelPicker::manageRequested);
    picker.manageButton()->click();
    CHECK(manage.count() == 1);

    picker.show();
    // Nothing to say: the line is hidden, and a rebuild clears what was said.
    CHECK(picker.captionLabel()->isHidden());
    picker.setCaption(QStringLiteral("Runs on the GPU."));
    CHECK_FALSE(picker.captionLabel()->isHidden());
    CHECK(picker.captionLabel()->text() == QStringLiteral("Runs on the GPU."));
    CHECK(picker.captionLabel()->wordWrap());
    picker.setModels(two(), QStringLiteral("parakeet-v3"));
    CHECK(picker.captionLabel()->isHidden());
    CHECK(picker.captionLabel()->text().isEmpty());
}

TEST_CASE("the catalog's models are the choices", "[widget]") {
    const auto all = fv::LocalModelPicker::catalogModels();
    REQUIRE_FALSE(all.empty());
    CHECK(all.front().key == QStringLiteral("parakeet-v3"));
    for (const auto& model : fv::LocalModelPicker::catalogModels(true)) {
        CHECK_FALSE(model.label.isEmpty());
    }
}
