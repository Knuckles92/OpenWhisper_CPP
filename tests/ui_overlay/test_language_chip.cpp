// The overlay's dictation language chip and the language notice
// (tests/test_personalize_s8_overlay.py), and the chip's text
// (tests/test_personalize_fix_f4_leftovers.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <vector>

#include <QApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QTest>

#include "overlay_test_support.h"
#include "settings/testing/memory_store.h"
#include "speech/dictation_language.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// two_languages: English and Spanish, Spanish active. Parakeet takes both.
fv::Json twoLanguages() {
    return fv::Json{{"selected_model", "parakeet"},
                    {"dictation_languages", fv::Json::array({"en", "es"})},
                    {"dictation_active_language", "es"}};
}

QStringList enEs() {
    return {QStringLiteral("en"), QStringLiteral("es")};
}

QPoint chipCenter(Overlay& overlay) {
    fvtest::paint(overlay);
    return overlay.access.chipRect().center().toPoint();
}

void click(QWidget& widget, const QPoint& at) {
    fvtest::sendMouse(widget, QEvent::MouseButtonPress, at);
    fvtest::sendMouse(widget, QEvent::MouseButtonRelease, at);
}

// A pointer move with no button held, as hovering sends it.
void hoverAt(QWidget& widget, const QPoint& local) {
    QMouseEvent event(QEvent::MouseMove, QPointF(local), QPointF(widget.mapToGlobal(local)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

} // namespace

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_chip_shows_the_saved_language_when_recording_starts
TEST_CASE("chip shows the saved language when recording starts",
          "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay.access.chipVisible());
    CHECK(overlay->language() == QStringLiteral("es"));
    CHECK(overlay->languageChoices() == enEs());
    fvtest::paint(overlay);
    CHECK_FALSE(overlay.access.chipRect().isEmpty());
    CHECK(overlay.access.chipRect().right() > overlay->width() / 2.0);
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_no_chip_with_one_or_no_language_the_engine_takes
TEST_CASE("no chip with one or no language the engine takes",
          "[widget][test_personalize_s8_overlay]") {
    // Python's engine is English-only Moonshine; Parakeet does not take German either.
    const std::vector<const char*> languages =
        GENERATE(std::vector<const char*>{}, std::vector<const char*>{"en"},
                 std::vector<const char*>{"en", "de"});
    fv::Json chosen = fv::Json::array();
    for (const char* code : languages) {
        chosen.push_back(code);
    }
    CAPTURE(chosen.dump());
    fv::MemorySettingsStore settings(
        fv::Json{{"selected_model", "parakeet"}, {"dictation_languages", chosen}});
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::paint(overlay);
    CHECK_FALSE(overlay.access.chipVisible());
    CHECK(overlay.access.chipRect().isEmpty());
}

// from tests/test_personalize_s8_overlay.py::TestLanguageChip::test_chip_only_while_listening
TEST_CASE("chip only while listening", "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    CHECK(overlay.access.chipVisible());
    overlay->setState(WaveformOverlay::kStateProcessing);
    fvtest::paint(overlay);
    CHECK(overlay.access.chipRect().isEmpty());
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_clicking_the_chip_asks_to_switch
TEST_CASE("clicking the chip asks to switch", "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay.access.setTakesClicks(true);
    int asked = 0;
    QObject::connect(&*overlay, &WaveformOverlay::languageCycleRequested, [&asked] { ++asked; });
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    click(*overlay, QPoint(20, 60));
    CHECK(asked == 0);
    click(*overlay, chipCenter(overlay));
    CHECK(asked == 1);
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_chip_is_display_only_where_clicks_could_take_focus
TEST_CASE("chip is display only where clicks could take focus",
          "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay.access.setTakesClicks(false);
    int asked = 0;
    QObject::connect(&*overlay, &WaveformOverlay::languageCycleRequested, [&asked] { ++asked; });
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    click(*overlay, chipCenter(overlay));
    CHECK(asked == 0);
    CHECK_FALSE(overlay.access.chipClickable());
}

// The offscreen platform the tests run on is not one that takes overlay clicks.
TEST_CASE("only windows x11 and macos take overlay clicks", "[widget]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay.access.chipVisible());
    CHECK(QGuiApplication::platformName() == QStringLiteral("offscreen"));
    CHECK_FALSE(overlay.access.chipClickable());
}

// from tests/test_personalize_s8_overlay.py::TestLanguageChip::test_hover_shows_a_pointing_hand
TEST_CASE("hover shows a pointing hand", "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay.access.setTakesClicks(true);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    hoverAt(*overlay, chipCenter(overlay));
    CHECK(overlay.access.hover() == QStringLiteral("language"));
    CHECK(overlay->cursor().shape() == Qt::PointingHandCursor);
    overlay->setState(WaveformOverlay::kStateProcessing);
    CHECK(overlay.access.hover().isEmpty());
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_switching_while_shown_flashes_the_chip
TEST_CASE("switching while shown flashes the chip", "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay.access.languageFlash() == 0.0);
    overlay->setLanguage(QStringLiteral("en"), enEs());
    CHECK(overlay->language() == QStringLiteral("en"));
    CHECK(overlay.access.languageFlash() > 0.5);
    fvtest::paint(overlay);
}

// The flash lasts 0.6 s on the clock (waveform_overlay.py:939-942).
TEST_CASE("the chip's flash fades over 0.6 s", "[widget]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setLanguage(QStringLiteral("en"), enEs());
    overlay.clock->advance(std::chrono::milliseconds(300));
    CHECK(overlay.access.languageFlash() == Catch::Approx(0.5).margin(1e-9));
    fvtest::paint(overlay);
    overlay.clock->advance(std::chrono::milliseconds(301));
    CHECK(overlay.access.languageFlash() == 0.0);
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_hidden_overlay_takes_a_new_language_quietly
TEST_CASE("hidden overlay takes a new language quietly", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->setLanguage(QStringLiteral("de"), {QStringLiteral("en"), QStringLiteral("de")});
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay.access.languageFlash() == 0.0);
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_language_notice_shows_then_hides_itself
TEST_CASE("language notice shows then hides itself", "[widget][test_personalize_s8_overlay]") {
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    overlay->setLanguage(QStringLiteral("es"), enEs());
    overlay->showAtCursor(WaveformOverlay::kStateLanguage);
    CHECK(overlay.access.hiddenTimer().isActive());
    fvtest::paint(overlay);
    fvtest::fireTimeout(overlay.access.hiddenTimer());
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->language().isEmpty());
}

// from
// tests/test_personalize_s8_overlay.py::TestCommandStates::test_every_new_look_paints_in_both_themes
TEST_CASE("every new look paints in both themes", "[widget][test_personalize_s8_overlay]") {
    const bool dark = GENERATE(true, false);
    CAPTURE(dark);
    fvtest::ScopedPalette palette(dark ? fv::ui::darkPalette() : fv::ui::lightPalette());
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    for (const QString& state :
         {WaveformOverlay::kStateRecording, WaveformOverlay::kStateStreaming,
          WaveformOverlay::kStateCommandListening, WaveformOverlay::kStateRewriting,
          WaveformOverlay::kStateLanguage}) {
        CAPTURE(state.toStdString());
        overlay->showAtCursor(state);
        overlay->setHandsFree(true);
        overlay->showCaption(QStringLiteral("Switched to Built-in microphone"));
        fvtest::paint(overlay);
        overlay->hide();
    }
}

// from
// tests/test_personalize_s8_overlay.py::test_overlay_uses_the_language_module_for_its_chip_text
TEST_CASE("overlay uses the language module for its chip text",
          "[widget][test_personalize_s8_overlay]") {
    // Python swaps in a fake short_label; here the chip's text is the module's for any code.
    Overlay overlay;
    for (const char* code : {"de", "auto", "pt"}) {
        overlay->setLanguage(QString::fromLatin1(code),
                             {QStringLiteral("en"), QStringLiteral("de")});
        CHECK(overlay.access.languageText() ==
              QString::fromStdString(fv::speech::short_label(code)));
    }
}

// from
// tests/test_personalize_fix_f4_leftovers.py::test_the_overlay_chip_never_falls_back_to_raw_codes
TEST_CASE("the overlay chip never falls back to raw codes",
          "[widget][test_personalize_fix_f4_leftovers]") {
    Overlay overlay;
    overlay->setLanguage(QStringLiteral("de"), {});
    CHECK(overlay.access.languageText() == QStringLiteral("DE"));
    CHECK(overlay.access.languageName() == QStringLiteral("German"));
}

// The chip clicked on the real window (Windows' platform, not offscreen): the click leaves the
// app being dictated into focused and asks to switch. It shows a window and moves the pointer.
TEST_CASE("clicking the chip on the real window asks to switch", "[interactive]") {
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        SKIP("needs Qt's windows platform and a desktop session");
    }
    fv::MemorySettingsStore settings(twoLanguages());
    Overlay overlay(&settings);
    int asked = 0;
    QObject::connect(&*overlay, &WaveformOverlay::languageCycleRequested, [&asked] { ++asked; });
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    REQUIRE(QTest::qWaitForWindowExposed(&*overlay));
    const QPoint at = chipCenter(overlay);
    QTest::mouseClick(&*overlay, Qt::LeftButton, Qt::NoModifier, at);
    CHECK(asked == 1);
    CHECK_FALSE(overlay->isActiveWindow());
}
