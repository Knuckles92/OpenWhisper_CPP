// A paired OpenWhisper host serving Qwen lists Mandarin among the host's languages, and a pick
// sends the language code, not the label (tests/test_local_asr_languages.py, RemoteEngineControls).
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QSignalSpy>

#include "base/json.h"
#include "speech/languages.h"
#include "ui_widgets/remote_engine_controls.h"

// from tests/test_local_asr_languages.py::test_remote_mandarin_uses_label_and_transmits_code
TEST_CASE("test_remote_mandarin_uses_label_and_transmits_code",
          "[remote_engine_controls][widget]") {
    // Qwen takes every LANGUAGE_LABELS code (language_choices("qwen_asr") =
    // tuple(LANGUAGE_LABELS)).
    fv::Json languages = fv::Json::array();
    for (const auto& [code, label] : fv::speech::language_labels()) {
        languages.push_back(std::string(code));
    }
    fv::remote::RemoteModels state;
    state.host = "test";
    state.engine = fv::Json{{"family", "qwen_asr"}, {"model", "qwen-0.6b"}};
    state.runtime = fv::Json{{"family", "qwen_asr"},
                             {"model", "qwen-0.6b"},
                             {"can_configure", true},
                             {"selected", fv::Json{{"device", "cpu"}, {"language", "en"}}},
                             {"devices", fv::Json::array({"cpu"})},
                             {"languages", languages}};
    fv::RemoteEngineControls field;
    field.show();
    QApplication::processEvents();
    QSignalSpy changed(&field, &fv::RemoteEngineControls::runtimeSelected);
    field.setState(state);

    const int index = field.languageCombo()->findData(QStringLiteral("zh"));
    REQUIRE(index >= 0);
    CHECK(field.languageCombo()->itemText(index) == QStringLiteral("Mandarin"));
    Q_EMIT field.languageCombo()->activated(index);

    REQUIRE(changed.count() == 1);
    CHECK(changed[0][0].toString() == QStringLiteral("qwen_asr"));
    CHECK(changed[0][1].toString() == QStringLiteral("qwen-0.6b"));
    CHECK(changed[0][2].toMap() == QVariantMap{{QStringLiteral("language"), QStringLiteral("zh")}});
}
