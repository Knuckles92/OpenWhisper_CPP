// Switching the dictation language from the tray, the shortcut and the overlay chip
// (tests/test_personalize_s8_language_menu.py, and the menu's tests in
// test_personalize_fix_f4_leftovers.py, test_personalize_fix3_h2_voice_languages.py and
// test_personalize_foundation_ui_shell.py). The app's side is a fake LanguageMenuUi over an
// in-memory settings file and a real overlay on a pinned clock.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <QAction>
#include <QMenu>
#include <QStringList>

#include "base/log.h"
#include "overlay_test_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "speech/dictation_language.h"
#include "ui_overlay/language_menu.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// _save: the languages dictated in. Parakeet takes English, Spanish and French.
fv::Json saved(std::vector<std::string> chosen = {"en", "es", "fr"}, std::string active = "en") {
    fv::Json languages = fv::Json::array();
    for (const std::string& code : chosen) {
        languages.push_back(code);
    }
    return fv::Json{{"selected_model", "parakeet"},
                    {"dictation_languages", languages},
                    {"dictation_active_language", active}};
}

// The ui of the Python tests: the overlay, set_status and open_settings_destination.
class FakeUi final : public fv::LanguageMenuUi {
public:
    explicit FakeUi(const fv::Json& settings = fv::Json::object()) : store(settings) {}

    WaveformOverlay& overlay() override { return *overlayFixture; }
    void setStatus(const QString& text) override { statuses.push_back(text); }
    void openVoiceModelSettings() override { ++voiceModelOpened; }
    fv::SettingsStore& settings() override {
        if (broken) {
            // A language list that cannot be read, as the broken module was.
            throw std::runtime_error("bad list");
        }
        return store;
    }

    std::string active() const {
        return fv::keys::dictation_active_language.read(*store.snapshot());
    }

    fv::MemorySettingsStore store;
    Overlay overlayFixture{&store};
    QStringList statuses;
    int voiceModelOpened = 0;
    bool broken = false;
};

QStringList texts(const QMenu& menu) {
    QStringList found;
    for (const QAction* action : menu.actions()) {
        if (!action->isSeparator()) {
            found.append(action->text());
        }
    }
    return found;
}

QAction* actionNamed(const QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

// The records of one logger written while the capture lived.
class LogCapture {
public:
    explicit LogCapture(std::string logger) : sink_(std::make_shared<Sink>(std::move(logger))) {
        fv::log::add_sink(sink_);
    }
    ~LogCapture() { fv::log::remove_sink(sink_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::string> errors() const {
        const std::lock_guard lock(sink_->mutex);
        return sink_->errors;
    }

private:
    struct Sink final : fv::log::Sink {
        explicit Sink(std::string name) : logger(std::move(name)) {}
        void write(const fv::log::Record& record) override {
            if (record.logger == logger && record.level == fv::log::Level::Error) {
                const std::lock_guard lock(mutex);
                errors.emplace_back(record.message);
            }
        }
        std::string logger;
        mutable std::mutex mutex;
        std::vector<std::string> errors;
    };
    std::shared_ptr<Sink> sink_;
};

const QString kChoose = QStringLiteral(u"Choose languages…");

} // namespace

// from
// tests/test_personalize_s8_language_menu.py::TestMenu::test_lists_the_choices_with_the_active_one_checked
TEST_CASE("lists the choices with the active one checked",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved({"en", "es", "fr"}, "es"));
    QMenu menu;
    fv::populateLanguageMenu(&menu, ui);
    CHECK(texts(menu) == QStringList{QStringLiteral("English"), QStringLiteral("Spanish"),
                                     QStringLiteral("French"), kChoose});
    QStringList checked;
    for (const QAction* action : menu.actions()) {
        if (action->isChecked()) {
            checked.append(action->text());
        }
    }
    CHECK(checked == QStringList{QStringLiteral("Spanish")});
    // "Choose languages…" is always last, after a separator.
    REQUIRE(menu.actions().size() == 5);
    CHECK(menu.actions()[3]->isSeparator());
}

// from
// tests/test_personalize_s8_language_menu.py::TestMenu::test_choosing_one_makes_it_active_and_says_so
TEST_CASE("choosing one makes it active and says so",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved());
    QMenu menu;
    fv::populateLanguageMenu(&menu, ui);
    QAction* french = actionNamed(menu, QStringLiteral("French"));
    REQUIRE(french != nullptr);
    french->trigger();
    CHECK(ui.active() == "fr");
    WaveformOverlay& overlay = ui.overlay();
    CHECK(overlay.language() == QStringLiteral("fr"));
    CHECK(overlay.languageChoices() ==
          QStringList{QStringLiteral("en"), QStringLiteral("es"), QStringLiteral("fr")});
    // A tray choice shows no notice.
    CHECK_FALSE(overlay.isVisible());
    CHECK(ui.statuses == QStringList{QStringLiteral("Dictation language: French")});
}

// from
// tests/test_personalize_s8_language_menu.py::TestMenu::test_choose_languages_opens_the_voice_model_page
TEST_CASE("choose languages opens the voice model page",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui;
    QMenu menu;
    fv::populateLanguageMenu(&menu, ui);
    menu.actions().last()->trigger();
    CHECK(ui.voiceModelOpened == 1);
}

// from
// tests/test_personalize_s8_language_menu.py::TestMenu::test_a_broken_language_list_still_leaves_a_usable_menu
TEST_CASE("a broken language list still leaves a usable menu",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved());
    ui.broken = true;
    QMenu menu;
    fv::populateLanguageMenu(&menu, ui);
    CHECK(texts(menu) == QStringList{QStringLiteral("No other languages"), kChoose});
    CHECK_FALSE(menu.actions().front()->isEnabled());
}

// from
// tests/test_personalize_s8_language_menu.py::TestCycle::test_cycle_moves_on_and_confirms_near_the_pointer
TEST_CASE("cycle moves on and confirms near the pointer",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved());
    WaveformOverlay& overlay = ui.overlay();
    REQUIRE_FALSE(overlay.isVisible());
    fv::cycleLanguage(ui);
    CHECK(ui.active() == "es");
    CHECK(overlay.language() == QStringLiteral("es"));
    CHECK(overlay.languageChoices() ==
          QStringList{QStringLiteral("en"), QStringLiteral("es"), QStringLiteral("fr")});
    // The notice shows near the pointer.
    CHECK(overlay.isVisible());
    CHECK(overlay.currentState() == WaveformOverlay::kStateLanguage);
    CHECK(ui.statuses == QStringList{QStringLiteral("Dictation language: Spanish")});
}

// from
// tests/test_personalize_s8_language_menu.py::TestCycle::test_while_recording_the_chip_updates_in_place
TEST_CASE("while recording the chip updates in place",
          "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved());
    WaveformOverlay& overlay = ui.overlay();
    overlay.showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay.language() == QStringLiteral("en"));
    fv::cycleLanguage(ui);
    // No notice: the recording keeps the overlay and its chip changes.
    CHECK(overlay.currentState() == WaveformOverlay::kStateRecording);
    CHECK(overlay.language() == QStringLiteral("es"));
}

// from tests/test_personalize_s8_language_menu.py::TestCycle::test_nothing_to_switch_to_says_why
TEST_CASE("nothing to switch to says why", "[widget][test_personalize_s8_language_menu]") {
    // Python's Moonshine case ("Moonshine understands English only.") has no engine here.
    FakeUi ui(saved({"es"}, "es"));
    fv::cycleLanguage(ui);
    CHECK(ui.statuses ==
          QStringList{QStringLiteral(u"Add another language in Settings → Voice model to switch")});
    CHECK(ui.overlay().language().isEmpty());
    CHECK_FALSE(ui.overlay().isVisible());
    CHECK(ui.active() == "es");
}

// from
// tests/test_personalize_s8_language_menu.py::TestCycle::test_language_notice_on_a_real_overlay
TEST_CASE("language notice on a real overlay", "[widget][test_personalize_s8_language_menu]") {
    FakeUi ui(saved({"en", "es", "fr"}, "fr"));
    WaveformOverlay& overlay = ui.overlay();
    fv::WaveformOverlayTestAccess& access = ui.overlayFixture.access;
    fv::cycleLanguage(ui);
    CHECK(overlay.isVisible());
    CHECK(overlay.currentState() == WaveformOverlay::kStateLanguage);
    CHECK(overlay.language() == QStringLiteral("en"));

    access.animationTime() = 0.5;
    fv::cycleLanguage(ui);
    CHECK(overlay.currentState() == WaveformOverlay::kStateLanguage);
    CHECK(overlay.language() == QStringLiteral("es"));
    CHECK(access.animationTime() == 0.0);
    CHECK(access.hiddenTimer().isActive());
}

// from
// tests/test_personalize_fix_f4_leftovers.py::test_a_broken_language_module_is_logged_and_reported
TEST_CASE("a broken language module is logged and reported",
          "[widget][test_personalize_fix_f4_leftovers]") {
    FakeUi ui(saved());
    ui.broken = true;
    const LogCapture log("ui_qt.widgets.language_menu");
    fv::cycleLanguage(ui);
    const QString failed = QString::fromUtf8(
        fv::kLanguageSwitchFailed.data(), static_cast<qsizetype>(fv::kLanguageSwitchFailed.size()));
    CHECK(ui.statuses == QStringList{failed});
    CHECK(ui.overlay().language().isEmpty());
    CHECK(log.errors() == std::vector<std::string>{std::string(fv::kLanguageSwitchFailed)});
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestTrayShowsWhatIsInEffect::test_one_language_is_shown_checked
TEST_CASE("one language is shown checked", "[widget][test_personalize_fix3_h2_voice_languages]") {
    struct Case {
        fv::Json settings;
        const char* name;
    };
    // Python's first and third cases are on Whisper; Parakeet gives the same rows.
    const Case shown = GENERATE_COPY(
        Case{saved({"es"}, "es"), "Spanish"},
        Case{fv::Json{{"selected_model", "parakeet"}, {"local_asr_language", "en"}}, "English"},
        Case{fv::Json{{"selected_model", "parakeet"}, {"local_asr_language", "auto"}},
             "Detect automatically"});
    CAPTURE(shown.name);
    FakeUi ui(shown.settings);
    QMenu menu;
    fv::populateLanguageMenu(&menu, ui);
    CHECK(texts(menu) == QStringList{QString::fromUtf8(shown.name), kChoose});
    const QAction* first = menu.actions().front();
    CHECK(first->isChecked());
    CHECK_FALSE(first->isEnabled());
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestLanguageCycle::test_cycle_saves_the_next_language_and_shows_it
TEST_CASE("cycle saves the next language and shows it",
          "[widget][test_personalize_foundation_ui_shell]") {
    // Python's pair is English and German; Parakeet takes French, not German.
    FakeUi ui(saved({"en", "fr"}, "en"));
    fv::cycleLanguage(ui);
    CHECK(ui.active() == "fr");
    CHECK(ui.overlay().language() == QStringLiteral("fr"));
    CHECK(ui.overlay().languageChoices() ==
          QStringList{QStringLiteral("en"), QStringLiteral("fr")});
    CHECK(ui.statuses == QStringList{QStringLiteral("Dictation language: French")});
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestLanguageCycle::test_cycle_failure_is_reported_and_leaves_the_overlay_alone
TEST_CASE("cycle failure is reported and leaves the overlay alone",
          "[widget][test_personalize_foundation_ui_shell]") {
    FakeUi ui(saved({"en", "fr"}, "en"));
    ui.broken = true;
    fv::cycleLanguage(ui);
    CHECK(ui.overlay().language().isEmpty());
    CHECK_FALSE(ui.overlay().isVisible());
    CHECK(ui.statuses == QStringList{QStringLiteral("Couldn't switch the dictation language")});
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestEmptyStates::test_language_menu_replaces_old_entries
TEST_CASE("language menu replaces old entries", "[widget][test_personalize_foundation_ui_shell]") {
    FakeUi ui;
    QMenu menu;
    menu.addAction(QStringLiteral("Stale"));
    fv::populateLanguageMenu(&menu, ui);
    fv::populateLanguageMenu(&menu, ui);
    // With no languages chosen, the language in effect: the engine's own.
    const std::string inEffect =
        fv::speech::label(fv::speech::current_language(*ui.store.snapshot()));
    CHECK(inEffect == "English");
    CHECK(texts(menu) == QStringList{QString::fromStdString(inEffect), kChoose});
    CHECK_FALSE(menu.actions().front()->isEnabled());
}
