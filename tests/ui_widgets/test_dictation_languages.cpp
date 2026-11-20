// "Languages I dictate in": the chips, the Add menu, what they save and how they sit beside the
// engine card's own Language field. OpenWhisper's tests reach the field through the Settings
// dialog's Voice model page; here a stand-in page wires the field and the engine fields the way
// that page does, and the engine switch goes between the engines OpenWhisper_CPP has.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QSignalSpy>
#include <QToolButton>
#include <QVBoxLayout>

#include "app_theme.h"
#include "settings/testing/memory_store.h"
#include "speech/dictation_language.h"
#include "speech_engine_catalog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/dictation_languages_field.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;

QStringList chipNames(const fv::DictationLanguagesField& field) {
    QStringList names;
    for (const fv::LanguageChip* chip : field.chips()) {
        names << chip->label()->text();
    }
    return names;
}

std::vector<bool> chipFlags(const fv::DictationLanguagesField& field, const char* property) {
    std::vector<bool> flags;
    for (const fv::LanguageChip* chip : field.chips()) {
        flags.push_back(chip->property(property).toBool());
    }
    return flags;
}

QStringList strings(std::initializer_list<const char*> values) {
    QStringList made;
    for (const char* value : values) {
        made << QString::fromUtf8(value);
    }
    return made;
}

// Stands for the Voice model page: the engine card's fields (dictation surface) and the
// languages field, with the field's changes reaching the engine fields as the page connects them
// and an engine switch reaching both.
struct VoicePage {
    explicit VoicePage(fv::MemorySettingsStore& settings, const QString& engine)
        : controls(settings, fvtest::speechCatalog(), engine, true), field(settings) {
        field.setBackend(engine);
        QObject::connect(&field, &fv::DictationLanguagesField::changed, &controls,
                         &fv::LocalEngineControls::syncLanguageField);
    }

    void switchEngine(const QString& engine) {
        controls.setBackend(engine);
        field.setBackend(engine);
    }

    fv::LocalEngineControls controls;
    fv::DictationLanguagesField field;
};

} // namespace

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_adding_languages_saves_them_and_makes_the_first_active
TEST_CASE("TestField::test_adding_languages_saves_them_and_makes_the_first_active",
          "[test_personalize_s8_voice_languages][widget]") {
    // An engine whose own language is "auto" detects it: Parakeet saved that way.
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "auto"}});
    fv::DictationLanguagesField field(settings);
    QSignalSpy changed(&field, &fv::DictationLanguagesField::changed);
    CHECK(field.chips().empty());
    CHECK(field.captionLabel()->text().startsWith(
        QStringLiteral("Dictation detects the language. Add languages")));

    field.add(QStringLiteral("en"));
    Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == Json::array({"en"}));
    CHECK(saved.at("dictation_active_language") == "en");
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Dictation uses English. Add another to switch between them."));

    field.add(QStringLiteral("es"));
    CHECK(chipNames(field) == strings({"English", "Spanish"}));
    CHECK(field.captionLabel()->text().startsWith(QStringLiteral("Now dictating in English.")));
    CHECK(chipFlags(field, "active") == std::vector<bool>{true, false});
    CHECK(changed.count() == 2);
}

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_removing_the_active_language_moves_to_the_next
TEST_CASE("TestField::test_removing_the_active_language_moves_to_the_next",
          "[test_personalize_s8_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en", "es", "fr"}},
                                          {"dictation_active_language", "en"}});
    fv::DictationLanguagesField field(settings);
    REQUIRE(field.chips().size() == 3);
    field.chips().front()->removeButton()->click();
    const Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == Json::array({"es", "fr"}));
    CHECK(saved.at("dictation_active_language") == "es");
    CHECK(chipNames(field) == strings({"Spanish", "French"}));
}

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_add_menu_offers_only_what_the_engine_takes
TEST_CASE("TestField::test_add_menu_offers_only_what_the_engine_takes",
          "[test_personalize_s8_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"dictation_languages", {"en"}}});
    fv::DictationLanguagesField field(settings);
    field.openAddMenu();
    QStringList offered;
    for (const QAction* action : field.addMenu()->actions()) {
        offered << action->text();
    }
    field.addMenu()->close();
    CHECK(offered == strings({"Russian", "Spanish", "French", "Portuguese"}));

    // Choosing one saves it, as the menu action does.
    field.openAddMenu();
    field.addMenu()->actions().at(1)->trigger();
    field.addMenu()->close();
    CHECK(settings.contents().at("dictation_languages") == Json::array({"en", "es"}));
}

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_languages_the_engine_cannot_use_stay_saved_but_muted
TEST_CASE("TestField::test_languages_the_engine_cannot_use_stay_saved_but_muted",
          "[test_personalize_s8_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en", "de"}},
                                          {"dictation_active_language", "en"}});
    fv::DictationLanguagesField field(settings);
    CHECK(chipFlags(field, "available") == std::vector<bool>{true, false});
    CHECK(field.chips().at(1)->toolTip() == QStringLiteral("Parakeet can't use this language."));
    CHECK(field.chips().at(0)->toolTip().isEmpty());
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Dictation uses English. Add another to switch between them."));
}

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_engine_switch_on_the_page_updates_the_field
TEST_CASE("TestField::test_engine_switch_on_the_page_updates_the_field",
          "[test_personalize_s8_voice_languages][widget]") {
    // OpenWhisper switches to Moonshine, which takes English only, and the chips give way to the
    // reason. The engines OpenWhisper_CPP has all take several languages, so what the switch
    // changes is which saved languages the engine can use.
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en", "de"}},
                                          {"dictation_active_language", "en"}});
    VoicePage page(settings, QStringLiteral("parakeet"));
    CHECK_FALSE(page.field.chipArea()->isHidden());
    CHECK(chipFlags(page.field, "available") == std::vector<bool>{true, false});

    page.switchEngine(QStringLiteral("api"));
    CHECK_FALSE(page.field.chipArea()->isHidden());
    CHECK(chipFlags(page.field, "available") == std::vector<bool>{true, true});
    // The API has no local fields, so the engine card has no Language field either.
    CHECK(page.controls.languageField()->isHidden());

    page.switchEngine(QStringLiteral("parakeet"));
    CHECK(chipFlags(page.field, "available") == std::vector<bool>{true, false});
    CHECK(page.field.chips().at(1)->toolTip() ==
          QStringLiteral("Parakeet can't use this language."));
}

// from
// tests/test_personalize_s8_voice_languages.py::TestField::test_never_writes_the_engine_language
TEST_CASE("TestField::test_never_writes_the_engine_language",
          "[test_personalize_s8_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "ru"}});
    fv::DictationLanguagesField field(settings);
    field.add(QStringLiteral("es"));
    field.add(QStringLiteral("fr"));
    CHECK(settings.contents().at("local_asr_language") == "ru");
}

// from
// tests/test_personalize_s8_voice_languages.py::test_chips_wrap_and_fit_narrow_windows_at_large_fonts
TEST_CASE("test_chips_wrap_and_fit_narrow_windows_at_large_fonts",
          "[test_personalize_s8_voice_languages][widget]") {
    // classic 720 px, and the 460 px tiled window
    const int width = GENERATE(720, 460);
    fvtest::AppTheme theme(QStringLiteral("dark"), 130);
    // Seven chips need an engine that takes all of them.
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "api"},
             {"dictation_languages", {"en", "es", "fr", "de", "pt", "ja", "uk"}},
             {"dictation_active_language", "fr"}});
    QWidget page;
    auto* layout = new QVBoxLayout(&page);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* field = new fv::DictationLanguagesField(settings);
    layout->addWidget(field);
    layout->addStretch(1);
    page.resize(width, 600);
    page.show();
    for (int pass = 0; pass < 10; ++pass) {
        QApplication::processEvents();
    }
    REQUIRE(field->chips().size() == 7);
    CHECK(field->chipArea()->height() > field->chips().front()->height() * 2);
    for (const QAbstractButton* control : field->findChildren<QAbstractButton*>()) {
        if (control->isVisible()) {
            CHECK(control->mapTo(&page, control->rect().topLeft()).x() >= 0);
            CHECK(control->mapTo(&page, control->rect().bottomRight()).x() < page.width());
        }
    }
    for (const fv::LanguageChip* chip : field->chips()) {
        CHECK(chip->geometry().bottom() < field->chipArea()->height());
    }
    CHECK(field->captionLabel()->height() >=
          field->captionLabel()->heightForWidth(field->captionLabel()->width()));
}

// from tests/test_personalize_s8_voice_languages.py::test_field_works_outside_settings
TEST_CASE("test_field_works_outside_settings", "[test_personalize_s8_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "api"},
                                          {"dictation_languages", {"de", "it"}},
                                          {"dictation_active_language", "it"}});
    fv::DictationLanguagesField field(settings);
    CHECK(chipNames(field) == strings({"German", "Italian"}));
    CHECK(field.captionLabel()->text().startsWith(QStringLiteral("Now dictating in Italian.")));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_first_language_joins_the_engines_own
TEST_CASE("TestAddingKeepsTheCurrentLanguage::test_first_language_joins_the_engines_own",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "en"}});
    fv::DictationLanguagesField field(settings);
    field.add(QStringLiteral("es"));
    const Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == Json::array({"en", "es"}));
    CHECK(saved.at("dictation_active_language") == "en");
    CHECK(saved.at("local_asr_language") == "en");
    // dictation_pipeline.recognition_for(None, settings).language is job_language.
    CHECK(fv::speech::job_language(saved) == "en");
    CHECK(field.captionLabel()->text().startsWith(QStringLiteral("Now dictating in English.")));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_adding_the_engines_own_language_adds_it_once
TEST_CASE("TestAddingKeepsTheCurrentLanguage::test_adding_the_engines_own_language_adds_it_once",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "en"}});
    fv::DictationLanguagesField field(settings);
    field.add(QStringLiteral("en"));
    CHECK(settings.contents().at("dictation_languages") == Json::array({"en"}));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_the_language_in_effect_stays_in_effect
TEST_CASE("TestAddingKeepsTheCurrentLanguage::test_the_language_in_effect_stays_in_effect",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    struct Row {
        Json chosen;
        Json expected;
    };
    // Chosen on another engine, Parakeet can use neither; and French chosen but not active, so
    // the engine's French is in effect.
    const Row row = GENERATE(Row{{"de", "it"}, {"de", "it", "fr", "es"}},
                             Row{{"pt", "fr"}, {"pt", "fr", "es"}});
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"local_asr_language", "fr"},
                                          {"dictation_languages", row.chosen},
                                          {"dictation_active_language", "de"}});
    const std::string before = fv::speech::current_language(settings.contents());
    fv::DictationLanguagesField field(settings);
    field.add(QStringLiteral("es"));
    const Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == row.expected);
    CHECK(before == "fr");
    CHECK(fv::speech::current_language(saved) == "fr");
    CHECK(fv::speech::job_language(saved) == "fr");
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_once_the_chips_decide_adding_changes_nothing_else
TEST_CASE(
    "TestAddingKeepsTheCurrentLanguage::test_once_the_chips_decide_adding_changes_nothing_else",
    "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"local_asr_language", "en"},
                                          {"dictation_languages", {"es", "fr"}},
                                          {"dictation_active_language", "fr"}});
    fv::DictationLanguagesField field(settings);
    field.add(QStringLiteral("pt"));
    const Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == Json::array({"es", "fr", "pt"}));
    CHECK(saved.at("dictation_active_language") == "fr");
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_an_engine_that_detects_the_language_says_what_changes
TEST_CASE(
    "TestAddingKeepsTheCurrentLanguage::test_an_engine_that_detects_the_language_says_what_changes",
    "[test_personalize_fix3_h2_voice_languages][widget]") {
    // Parakeet set to Auto, and the API (which detects unless told): the second takes the place
    // of OpenWhisper's Local Whisper.
    const Json values =
        GENERATE(Json{{"selected_model", "parakeet"}, {"local_asr_language", "auto"}},
                 Json{{"selected_model", "api"}});
    fv::MemorySettingsStore settings(values);
    fv::DictationLanguagesField field(settings);
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Dictation detects the language. Add languages to switch between them "
                         "from the overlay, the tray or a shortcut."));
    field.add(QStringLiteral("es"));
    const Json saved = settings.contents();
    CHECK(saved.at("dictation_languages") == Json::array({"es"}));
    CHECK(saved.at("dictation_active_language") == "es");
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Dictation uses Spanish. Add another to switch between them."));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_empty_list_names_the_language_dictation_uses
TEST_CASE("TestAddingKeepsTheCurrentLanguage::test_empty_list_names_the_language_dictation_uses",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "ru"}});
    fv::DictationLanguagesField field(settings);
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Dictation uses Russian. Add languages to switch between them "
                         "from the overlay, the tray or a shortcut."));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestAddingKeepsTheCurrentLanguage::test_choices_the_engine_cannot_use_name_the_fallback
TEST_CASE("TestAddingKeepsTheCurrentLanguage::test_choices_the_engine_cannot_use_name_the_fallback",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"local_asr_language", "fr"},
                                          {"dictation_languages", {"de", "it"}}});
    fv::DictationLanguagesField field(settings);
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Parakeet can't use these, so dictation uses French."));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestOneLanguageControl::test_engine_language_steps_aside_while_the_chips_decide
TEST_CASE("TestOneLanguageControl::test_engine_language_steps_aside_while_the_chips_decide",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"local_asr_language", "en"}});
    VoicePage page(settings, QStringLiteral("parakeet"));
    CHECK_FALSE(page.controls.languageField()->isHidden());

    page.field.add(QStringLiteral("es"));
    CHECK(page.controls.languageField()->isHidden());

    for (const char* code : {"en", "es"}) {
        page.field.remove(QString::fromLatin1(code));
    }
    CHECK_FALSE(page.controls.languageField()->isHidden());
    CHECK(page.controls.languageCombo()->currentData().toString() == QStringLiteral("en"));
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestOneLanguageControl::test_saved_choices_hide_it_when_the_page_opens
TEST_CASE("TestOneLanguageControl::test_saved_choices_hide_it_when_the_page_opens",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en", "es"}},
                                          {"dictation_active_language", "en"}});
    VoicePage page(settings, QStringLiteral("parakeet"));
    CHECK(page.controls.languageField()->isHidden());
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestOneLanguageControl::test_an_active_language_this_engine_lacks_leaves_the_engine_in_charge
TEST_CASE(
    "TestOneLanguageControl::test_an_active_language_this_engine_lacks_leaves_the_engine_in_charge",
    "[test_personalize_fix3_h2_voice_languages][widget]") {
    // German was active elsewhere; Parakeet cannot use it, so the engine's Russian is what
    // dictation uses, and the page says so.
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"local_asr_language", "ru"},
                                          {"dictation_languages", {"en", "de", "fr"}},
                                          {"dictation_active_language", "de"}});
    VoicePage page(settings, QStringLiteral("parakeet"));
    CHECK_FALSE(page.controls.languageField()->isHidden());
    CHECK(
        page.field.captionLabel()->text().startsWith(QStringLiteral("Now dictating in Russian.")));
    CHECK(fv::speech::job_language(settings.contents()).empty());
}

TEST_CASE("the caption says which language the next dictation uses", "[widget]") {
    using fv::DictationLanguagesField;
    const QString parakeet = QStringLiteral("Parakeet");
    const std::vector<std::string> none;
    const std::vector<std::string> one{"en"};
    const std::vector<std::string> two{"en", "es"};
    CHECK(DictationLanguagesField::captionText(none, none, "auto", parakeet) ==
          QStringLiteral("Dictation detects the language. Add languages to switch between them "
                         "from the overlay, the tray or a shortcut."));
    CHECK(DictationLanguagesField::captionText(two, none, "ru", parakeet) ==
          QStringLiteral("Parakeet can't use these, so dictation uses Russian."));
    CHECK(DictationLanguagesField::captionText(two, one, "en", parakeet) ==
          QStringLiteral("Dictation uses English. Add another to switch between them."));
    CHECK(DictationLanguagesField::captionText(two, two, "auto", parakeet) ==
          QStringLiteral("Dictation detects the language. Switch from the overlay, the tray or a "
                         "shortcut."));
    CHECK(DictationLanguagesField::captionText(two, two, "es", parakeet) ==
          QStringLiteral("Now dictating in Spanish. Switch from the overlay, the tray or a "
                         "shortcut."));
}

TEST_CASE("a failed save says so and announces nothing", "[widget]") {
    fv::MemorySettingsStore settings(
        Json{{"selected_model", "parakeet"}, {"dictation_languages", {"en"}}});
    fv::DictationLanguagesField field(settings);
    QSignalSpy changed(&field, &fv::DictationLanguagesField::changed);
    settings.file().fail_next_replace("disk full");
    field.add(QStringLiteral("es"));
    CHECK(field.captionLabel()->text() ==
          QStringLiteral("Couldn't save your languages. Try again."));
    CHECK(changed.count() == 0);
    CHECK(settings.contents().at("dictation_languages") == Json::array({"en"}));
    // The next edit works again and the caption is the usual one.
    field.add(QStringLiteral("es"));
    CHECK(changed.count() == 1);
    CHECK_FALSE(field.captionLabel()->text().startsWith(QStringLiteral("Couldn't")));
}

TEST_CASE("the chips and the Add button carry the names the theme and screen readers use",
          "[widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", {"en"}},
                                          {"dictation_active_language", "en"}});
    fv::DictationLanguagesField field(settings);
    CHECK(field.objectName() == QStringLiteral("dictationLanguages"));
    CHECK(field.chipArea()->objectName() == QStringLiteral("dictationLanguageChips"));
    CHECK(field.captionLabel()->objectName() == QStringLiteral("infoLabel"));
    REQUIRE(field.chips().size() == 1);
    const fv::LanguageChip* chip = field.chips().front();
    CHECK(chip->objectName() == QStringLiteral("dictationLanguageChip"));
    CHECK(chip->height() == 30);
    CHECK(chip->label()->objectName() == QStringLiteral("dictationLanguageChipLabel"));
    CHECK(chip->removeButton()->objectName() == QStringLiteral("dictationLanguageChipRemove"));
    CHECK(chip->removeButton()->toolTip() == QStringLiteral("Remove English"));
    CHECK(chip->removeButton()->accessibleName() == QStringLiteral("Remove English"));
    CHECK(chip->removeButton()->size() == QSize(22, 22));
    CHECK(field.addButton()->toolTip() == QStringLiteral("Add a language"));
    CHECK(field.addButton()->accessibleName() == QStringLiteral("Add a language"));
    CHECK(field.addButton()->isEnabled());

    // With every language of the engine chosen, there is nothing left to add.
    for (const char* code : {"ru", "es", "fr", "pt"}) {
        field.add(QString::fromLatin1(code));
    }
    CHECK_FALSE(field.addButton()->isEnabled());
}
