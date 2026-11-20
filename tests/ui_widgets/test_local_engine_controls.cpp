// The Model / Device / Language columns of the engine card for the Parakeet engine.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>

#include <QApplication>
#include <QComboBox>
#include <QSignalSpy>

#include "base/json.h"
#include "settings/testing/memory_store.h"
#include "speech/languages.h"
#include "speech_engine_catalog.h"
#include "ui_widgets/local_engine_controls.h"

namespace {

using fv::Json;

// What the speech catalog gives the Parakeet engine, as the feature that builds the controls
// passes it: two models, and the languages of the runtime.
fv::LocalEngineCatalog catalog() {
    fv::LocalEngineCatalog made;
    made.knows = [](const QString& backend) {
        return backend == QStringLiteral("parakeet");
    };
    made.models = [](const QString&) {
        return std::vector<fv::LocalEngineCatalog::Model>{
            {QStringLiteral("parakeet-v3"), QStringLiteral("Parakeet TDT 0.6B v3")},
            {QStringLiteral("orukeet-v0.1"), QStringLiteral("Orukeet TDT 0.6B")}};
    };
    made.selectedModel = [](const Json& settings, const QString& backend) {
        const auto saved = settings.value("local_asr_models", Json::object());
        const std::string key = backend.toStdString();
        const std::string model = saved.is_object() && saved.contains(key) && saved[key].is_string()
                                      ? saved[key].get<std::string>()
                                      : "";
        return QString::fromStdString(model == "orukeet-v0.1" ? model : "parakeet-v3");
    };
    made.selectedDevice = [](const Json& settings, const QString& backend) {
        const auto saved = settings.value("local_asr_devices", Json::object());
        const std::string key = backend.toStdString();
        const std::string device =
            saved.is_object() && saved.contains(key) && saved[key].is_string()
                ? saved[key].get<std::string>()
                : "";
        return QString::fromStdString(device == "cpu" || device == "cuda" ? device : "auto");
    };
    made.languages = [](const QString&) {
        return std::vector<std::pair<QString, QString>>{
            {QStringLiteral("en"), QStringLiteral("English")},
            {QStringLiteral("ru"), QStringLiteral("Russian")},
            {QStringLiteral("auto"), QStringLiteral("Auto")}};
    };
    made.selectedLanguage = [](const QString&, const QString& stored) {
        return stored == QStringLiteral("en") || stored == QStringLiteral("ru")
                   ? stored
                   : QStringLiteral("auto");
    };
    return made;
}

} // namespace

// from tests/test_local_asr.py::test_a_tab_keeps_local_controls_visible
TEST_CASE("a tab keeps local controls visible", "[test_local_asr][widget]") {
    fv::MemorySettingsStore settings;
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    controls.resize(400, 80);
    controls.show();
    QApplication::processEvents();
    CHECK_FALSE(controls.isHidden());
    CHECK_FALSE(controls.modelCombo()->isHidden());
    CHECK_FALSE(controls.deviceCombo()->isHidden());
    CHECK_FALSE(controls.languageField()->isHidden());
    CHECK(controls.backend() == QStringLiteral("parakeet"));

    // An engine the catalog has no local fields for has none: no models, and no language.
    controls.setBackend(QStringLiteral("api"));
    CHECK(controls.modelCombo()->count() == 0);
    CHECK(controls.languageField()->isHidden());
    controls.setBackend(QStringLiteral("parakeet"));
    CHECK(controls.modelCombo()->count() == 2);
    CHECK_FALSE(controls.languageField()->isHidden());
}

TEST_CASE("the controls show the models, device and language that are saved", "[widget]") {
    fv::MemorySettingsStore settings(Json{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}},
                                          {"local_asr_devices", {{"parakeet", "cuda"}}},
                                          {"local_asr_language", "ru"}});
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    CHECK(controls.modelCombo()->itemText(0) == QStringLiteral("Parakeet TDT 0.6B v3"));
    CHECK(controls.modelCombo()->currentData().toString() == QStringLiteral("orukeet-v0.1"));
    CHECK(controls.deviceCombo()->currentText() == QStringLiteral("GPU"));
    CHECK(controls.deviceCombo()->currentData().toString() == QStringLiteral("cuda"));
    CHECK(controls.languageCombo()->currentText() == QStringLiteral("Russian"));
    CHECK(controls.languageCombo()->isEnabled());
}

TEST_CASE("nothing saved shows the defaults, and an unknown device shows Auto", "[widget]") {
    fv::MemorySettingsStore settings(Json{{"local_asr_devices", {{"parakeet", "tpu"}}}});
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    CHECK(controls.modelCombo()->currentData().toString() == QStringLiteral("parakeet-v3"));
    CHECK(controls.deviceCombo()->currentData().toString() == QStringLiteral("auto"));
    // The saved language defaults to English.
    CHECK(controls.languageCombo()->currentData().toString() == QStringLiteral("en"));
}

TEST_CASE("a change is saved with the other engines' choices and announced once", "[widget]") {
    fv::MemorySettingsStore settings(
        Json{{"local_asr_models", {{"other", "kept"}}}, {"local_asr_devices", {{"other", "cpu"}}}});
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    QSignalSpy changed(&controls, &fv::LocalEngineControls::engineSettingsChanged);
    CHECK(settings.file().writes() == 0);

    controls.modelCombo()->setCurrentIndex(
        controls.modelCombo()->findData(QStringLiteral("orukeet-v0.1")));
    REQUIRE(changed.count() == 1);
    Json saved = settings.contents();
    CHECK(saved.at("local_asr_models") == Json{{"other", "kept"}, {"parakeet", "orukeet-v0.1"}});
    CHECK(saved.at("local_asr_devices") == Json{{"other", "cpu"}, {"parakeet", "auto"}});
    CHECK(saved.at("local_asr_language") == "en");

    controls.deviceCombo()->setCurrentIndex(
        controls.deviceCombo()->findData(QStringLiteral("cuda")));
    controls.languageCombo()->setCurrentIndex(
        controls.languageCombo()->findData(QStringLiteral("ru")));
    CHECK(changed.count() == 3);
    saved = settings.contents();
    CHECK(saved.at("local_asr_devices").at("parakeet") == "cuda");
    CHECK(saved.at("local_asr_language") == "ru");
}

TEST_CASE("showing the saved engine again neither writes nor announces", "[widget]") {
    fv::MemorySettingsStore settings;
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    QSignalSpy changed(&controls, &fv::LocalEngineControls::engineSettingsChanged);
    settings.set_contents(Json{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}});
    controls.loadFromSettings();
    CHECK(controls.modelCombo()->currentData().toString() == QStringLiteral("orukeet-v0.1"));
    controls.setBackend(QStringLiteral("parakeet"));
    CHECK(changed.count() == 0);
    CHECK(settings.file().writes() == 0);
}

TEST_CASE("busy disables the fields and idle enables them", "[widget]") {
    fv::MemorySettingsStore settings;
    fv::LocalEngineControls controls(settings, catalog(), QStringLiteral("parakeet"));
    controls.setBusy(true);
    CHECK_FALSE(controls.modelCombo()->isEnabled());
    CHECK_FALSE(controls.deviceCombo()->isEnabled());
    CHECK_FALSE(controls.languageCombo()->isEnabled());
    controls.setBusy(false);
    CHECK(controls.modelCombo()->isEnabled());
    CHECK(controls.deviceCombo()->isEnabled());
    CHECK(controls.languageCombo()->isEnabled());
}

TEST_CASE("the language field steps aside while the dictation languages decide", "[widget]") {
    fv::MemorySettingsStore settings(Json{{"dictation_active_language", "ru"}});
    fv::LocalEngineCatalog deciding = catalog();
    // Stands for dictation_language.job_language: an active dictation language decides.
    QString seenBackend;
    deciding.dictationDecidesLanguage = [&seenBackend](const Json& saved, const QString& backend) {
        seenBackend = backend;
        return saved.value("dictation_active_language", std::string()) == "ru";
    };

    fv::LocalEngineControls dictation(settings, deciding, QStringLiteral("parakeet"), true);
    CHECK(dictation.dictation());
    CHECK(dictation.languageField()->isHidden());
    CHECK(seenBackend == QStringLiteral("parakeet"));
    // Files always use the engine's language: without dictation the field stays.
    fv::LocalEngineControls files(settings, deciding, QStringLiteral("parakeet"));
    CHECK_FALSE(files.languageField()->isHidden());

    // The languages are no longer in charge: the field is back on the next sync.
    settings.update(Json{{"dictation_active_language", ""}});
    dictation.syncLanguageField();
    CHECK_FALSE(dictation.languageField()->isHidden());
    settings.update(Json{{"dictation_active_language", "ru"}});
    dictation.setBackend(QStringLiteral("parakeet"));
    CHECK(dictation.languageField()->isHidden());
    // An engine without local fields has no Language field either way.
    dictation.setBackend(QStringLiteral("api"));
    CHECK(dictation.languageField()->isHidden());

    // A catalog that cannot tell leaves the field to the engine.
    fv::LocalEngineControls unknown(settings, catalog(), QStringLiteral("parakeet"), true);
    CHECK_FALSE(unknown.languageField()->isHidden());
}

TEST_CASE("a language field with one choice is disabled", "[widget]") {
    fv::MemorySettingsStore settings;
    fv::LocalEngineCatalog one = catalog();
    one.languages = [](const QString&) {
        return std::vector<std::pair<QString, QString>>{
            {QStringLiteral("en"), QStringLiteral("English")}};
    };
    fv::LocalEngineControls controls(settings, one, QStringLiteral("parakeet"));
    CHECK_FALSE(controls.languageCombo()->isEnabled());
}

// from tests/test_local_asr.py::test_model_menu_tells_each_download_size_without_renaming_models
TEST_CASE("model menu tells each download size without renaming models",
          "[test_local_asr][widget]") {
    fv::MemorySettingsStore settings(Json{{"local_asr_models", {{"parakeet", "parakeet-v3"}}}});
    fv::LocalEngineControls controls(settings, fvtest::speechCatalog(), QStringLiteral("parakeet"));
    QComboBox* combo = controls.modelCombo();
    const auto tip = [combo](const QString& key) {
        return combo->itemData(combo->findData(key), Qt::ToolTipRole);
    };

    // The model key is still the item's data, so settings find it again.
    CHECK(combo->currentData().toString() == QStringLiteral("parakeet-v3"));
    CHECK(combo->currentText() == QStringLiteral("Parakeet TDT 0.6B v3"));
    CHECK(tip(QStringLiteral("parakeet-v3")).toString() == QStringLiteral("About 714 MB download"));
    REQUIRE(combo->findData(QStringLiteral("orukeet-v0.1")) >= 0);
    CHECK(tip(QStringLiteral("orukeet-v0.1")).toString().startsWith(QStringLiteral("About ")));

    // An item the profiles do not know has no tooltip, and the known ones keep theirs.
    combo->addItem(QStringLiteral("someone/custom"));
    fv::showDownloadSizes(combo);
    CHECK_FALSE(combo->itemData(combo->findText(QStringLiteral("someone/custom")), Qt::ToolTipRole)
                    .isValid());
    CHECK(tip(QStringLiteral("parakeet-v3")).toString() == QStringLiteral("About 714 MB download"));
    // An item with empty data is looked up by its text, as `itemData(i) or itemText(i)` does.
    combo->addItem(QStringLiteral("parakeet-v3"), QString());
    fv::showDownloadSizes(combo);
    CHECK(combo->itemData(combo->count() - 1, Qt::ToolTipRole).toString() ==
          QStringLiteral("About 714 MB download"));
    // Showing the engine again rebuilds the menu with the sizes.
    controls.setBackend(QStringLiteral("parakeet"));
    CHECK(tip(QStringLiteral("parakeet-v3")).toString() == QStringLiteral("About 714 MB download"));
}

// from tests/test_local_asr_languages.py::test_selector_limits_languages_and_fits_narrow_windows
TEST_CASE("selector limits languages and fits narrow windows",
          "[test_local_asr_languages][widget]") {
    // Parakeet's languages; the other engines of the Python table are dropped.
    fv::MemorySettingsStore settings(Json{{"local_asr_language", "ru"}});
    fv::LocalEngineControls controls(settings, fvtest::speechCatalog(), QStringLiteral("parakeet"));
    QSignalSpy changed(&controls, &fv::LocalEngineControls::engineSettingsChanged);
    const Json before = settings.contents();
    controls.setBackend(QStringLiteral("parakeet"));
    std::set<QString> codes;
    for (int index = 0; index < controls.languageCombo()->count(); ++index) {
        codes.insert(controls.languageCombo()->itemData(index).toString());
    }
    const std::set<QString> expected{QStringLiteral("en"), QStringLiteral("ru"),
                                     QStringLiteral("es"), QStringLiteral("fr"),
                                     QStringLiteral("pt"), QStringLiteral("auto")};
    CHECK(codes == expected);
    CHECK(controls.languageCombo()->isEnabled() == (expected.size() > 1));
    CHECK(settings.contents() == before);
    CHECK(changed.count() == 0);
    controls.resize(520, 90);
    controls.show();
    QApplication::processEvents();
    CHECK(controls.languageCombo()->width() >= 96);
    CHECK(controls.languageCombo()->geometry().right() <=
          controls.languageCombo()->parentWidget()->width());
}

// from tests/test_local_asr_languages.py::test_language_round_trip_uses_codes_and_emits_one_reload
TEST_CASE("language round trip uses codes and emits one reload",
          "[test_local_asr_languages][widget]") {
    // Mandarin is Qwen3-ASR's and Nemotron's; Parakeet's codes are the others.
    const QString code =
        GENERATE(QStringLiteral("en"), QStringLiteral("ru"), QStringLiteral("es"),
                 QStringLiteral("fr"), QStringLiteral("pt"), QStringLiteral("auto"));
    fv::MemorySettingsStore settings(
        Json{{"local_asr_language", code == QStringLiteral("auto") ? "en" : "auto"}});
    fv::LocalEngineControls controls(settings, fvtest::speechCatalog(), QStringLiteral("parakeet"));
    QSignalSpy changed(&controls, &fv::LocalEngineControls::engineSettingsChanged);
    controls.languageCombo()->setCurrentIndex(controls.languageCombo()->findData(code));
    CHECK(settings.contents().at("local_asr_language") == code.toStdString());
    CHECK(changed.count() == 1);
    controls.loadFromSettings();
    CHECK(controls.languageCombo()->currentData().toString() == code);
    QString label;
    for (const auto& [known, name] : fv::speech::language_labels()) {
        if (QString::fromUtf8(known.data(), static_cast<qsizetype>(known.size())) == code) {
            label = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
        }
    }
    CHECK(controls.languageCombo()->currentText() == label);
    CHECK(changed.count() == 1);
}

TEST_CASE("switching the surface shows or hides the language field to match", "[widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en", "es"}},
                                          {"dictation_active_language", "en"}});
    fv::LocalEngineControls controls(settings, fvtest::speechCatalog(), QStringLiteral("parakeet"));
    // Files use the engine's language whatever the chips say.
    CHECK_FALSE(controls.dictation());
    CHECK_FALSE(controls.languageField()->isHidden());
    controls.setDictation(true);
    CHECK(controls.dictation());
    CHECK(controls.languageField()->isHidden());
    controls.setDictation(false);
    CHECK_FALSE(controls.languageField()->isHidden());
}
