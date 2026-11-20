// The engine picker's names and order. The Apple-silicon annotations of OpenWhisper's picker
// describe engines FioriVoce does not have, so only the names, the order and the identities
// remain.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include <QComboBox>

#include "settings/model_selection.h"
#include "ui_widgets/speech_backend_picker.h"

// from
// tests/test_speech_backend_picker.py::test_engine_picker_orders_names_and_recommends_only_supported_macs
TEST_CASE("engine picker orders names and recommends only supported macs",
          "[test_speech_backend_picker][widget]") {
    QComboBox combo;
    fv::populateBackendCombo(&combo, fv::model_values());
    // Alphabetical by name without regard to case, the runtime id as item data.
    REQUIRE(combo.count() == 3);
    CHECK(combo.itemText(0) == QStringLiteral("API"));
    CHECK(combo.itemData(0).toString() == QStringLiteral("api"));
    CHECK(combo.itemText(1) == QStringLiteral("Parakeet"));
    CHECK(combo.itemData(1).toString() == QStringLiteral("parakeet"));
    CHECK(combo.itemText(2) == QStringLiteral("Remote computer"));
    CHECK(combo.itemData(2).toString() == QStringLiteral("remote"));
    // No Mac recommendation: Parakeet reads as itself, and a tooltip is the name.
    for (int index = 0; index < combo.count(); ++index) {
        CHECK(combo.itemData(index, Qt::ToolTipRole).toString() == combo.itemText(index));
    }
}

TEST_CASE("engine picker lists only the ids it is given, and none it cannot name", "[widget]") {
    QComboBox combo;
    constexpr std::array<std::string_view, 3> ids{"remote", "moonshine", "api"};
    fv::populateBackendCombo(&combo, ids);
    REQUIRE(combo.count() == 2);
    CHECK(combo.itemData(0).toString() == QStringLiteral("api"));
    CHECK(combo.itemData(1).toString() == QStringLiteral("remote"));
}

// from
// tests/test_speech_backend_picker.py::test_annotated_selection_emits_canonical_name_and_syncs_silently
TEST_CASE("annotated selection emits canonical name and syncs silently",
          "[test_speech_backend_picker][widget]") {
    // OpenWhisper annotates a Mac's picker labels while signals keep the canonical name. Here the
    // labels are the names, so the canonical name is what the picker shows and what an id maps to.
    QComboBox combo;
    fv::populateBackendCombo(&combo, fv::model_values());
    QStringList emitted;
    QObject::connect(&combo, &QComboBox::currentIndexChanged, &combo, [&] {
        emitted << fv::backendDisplayName(combo.currentData().toString().toStdString());
    });
    combo.setCurrentIndex(combo.findData(QStringLiteral("remote")));
    CHECK(emitted == QStringList{QStringLiteral("Remote computer")});
    CHECK(combo.currentText() == QStringLiteral("Remote computer"));

    // Showing a saved engine without a signal leaves the canonical name unchanged.
    combo.blockSignals(true);
    combo.setCurrentIndex(combo.findData(QStringLiteral("parakeet")));
    combo.blockSignals(false);
    CHECK(emitted.size() == 1);
    CHECK(combo.currentText() == QStringLiteral("Parakeet"));
    CHECK(fv::backendDisplayName("parakeet") == QStringLiteral("Parakeet"));
    CHECK(fv::backendDisplayName("api") == QStringLiteral("API"));
    CHECK(fv::backendDisplayName("remote") == QStringLiteral("Remote computer"));
    // A value no engine has reads as the default engine.
    CHECK(fv::backendDisplayName("moonshine") == QStringLiteral("Parakeet"));
    CHECK(fv::backendDisplayName("") == QStringLiteral("Parakeet"));
}
