// The Quick Record tab and the scaffolding it shares with the Upload tab: transcript and Fixed /
// Raw switch, collapse, copy actions, recording chrome, live partial text, the engine readout and
// the Command Mode hint. New tests: the behaviours the OpenWhisper tests of the tab do not pin
// (the traps of the tab's source).
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTextEdit>
#include <QVBoxLayout>

#include "record_fixture.h"
#include "ui_record/stats_display.h"
#include "ui_record/transcript_pane.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::EngineStatus;
using fv::Json;
using fv::appcore::OverlayState;

QString label(const fvtest::ShownTab& tab) {
    return tab->resolvedLabel()->text();
}

} // namespace

TEST_CASE("the tab starts collapsed with the quick record knobs", "[widget]") {
    fvtest::ShownTab tab;
    CHECK(tab->isTranscriptionCollapsed());
    CHECK_FALSE(tab->fillsHeight());
    CHECK(tab->findChild<QWidget*>(QStringLiteral("quickRecordContent")) != nullptr);
    CHECK(tab->transcriptText()->placeholderText() ==
          QStringLiteral("Transcription will appear here...\nStart recording to begin."));
    CHECK(tab->transcriptText()->minimumHeight() == 0);
    CHECK(label(tab) == QStringLiteral("Ready to record"));
    CHECK(tab->currentBackend() == QStringLiteral("Parakeet"));
    CHECK(tab->recordButton()->text() == QStringLiteral("Start Recording"));
    CHECK_FALSE(tab->stopButton()->isActive());
    CHECK_FALSE(tab->cancelButton()->isActive());
}

TEST_CASE("quick record's language field steps aside for the dictation languages", "[widget]") {
    // Two dictation languages Parakeet takes, Spanish active: dictation uses it, not the field.
    fvtest::ShownTab tab(Json{{"selected_model", "parakeet"},
                              {"dictation_languages", Json::array({"en", "es"})},
                              {"dictation_active_language", "es"}});
    CHECK(tab->knobs().dictationLanguages);
    CHECK(tab->localEngine()->dictation());
    CHECK(tab->localEngine()->languageField()->isHidden());

    // Without an active choice the engine's own language applies again.
    tab.settings.set_contents(Json{{"selected_model", "parakeet"}});
    tab->syncEngineSelection();
    CHECK_FALSE(tab->localEngine()->languageField()->isHidden());
}

TEST_CASE("a tab keeps local controls visible", "[widget]") {
    fvtest::ShownTab tab;
    tab->setBackend(QStringLiteral("Parakeet"));
    CHECK_FALSE(tab->localEngine()->isHidden());
    CHECK(tab->modelCombo()->dot()->isVisible());
    // The API backend has no local fields and no engine here for the dot to report on.
    tab->setBackend(QStringLiteral("API"));
    CHECK(tab->currentBackend() == QStringLiteral("API"));
    CHECK(tab->localEngine()->isHidden());
    CHECK_FALSE(tab->modelCombo()->dot()->isVisible());
    // A backend no tab offers changes nothing.
    tab->setBackend(QStringLiteral("Local Whisper"));
    CHECK(tab->currentBackend() == QStringLiteral("API"));
    tab->setBackend(QStringLiteral("Parakeet"));
    CHECK_FALSE(tab->localEngine()->isHidden());
    CHECK(tab->modelCombo()->dot()->isVisible());
}

TEST_CASE("set engine message swallows the generic ready lines", "[widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | CPU"), true);
    for (const char* generic : {"Ready", "Ready to record", "Whisper engine ready"}) {
        tab->setStatus(QStringLiteral("Something else"));
        tab->setStatus(QString::fromLatin1(generic));
        CHECK(label(tab) == QStringLiteral("Parakeet TDT 0.6B v3 | CPU"));
    }
    // A message equal to the device info is the idle readout already.
    tab->setStatus(QStringLiteral("Parakeet TDT 0.6B v3 | CPU"));
    CHECK(label(tab) == QStringLiteral("Parakeet TDT 0.6B v3 | CPU"));
}

TEST_CASE("the readout falls back to the initial status", "[widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QString(), std::nullopt);
    CHECK(label(tab) == QStringLiteral("Ready to record"));
    CHECK(tab->resolvedLabel()->accessibleName() == QStringLiteral("Ready to record"));
}

TEST_CASE("device info tells the dot what the engine is", "[widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("a"), std::nullopt);
    CHECK(tab->statusDot()->status() == EngineStatus::Unknown);
    tab->setDeviceInfo(QStringLiteral("a"), true);
    CHECK(tab->statusDot()->status() == EngineStatus::Ready);
    tab->setDeviceInfo(QStringLiteral("a"), false);
    CHECK(tab->statusDot()->status() == EngineStatus::Attention);
    // The Backend field's dot follows, and reads unknown while the engine loads.
    CHECK(tab->modelCombo()->dot()->status() == EngineStatus::Attention);
    tab->setEngineBusy(true);
    CHECK(tab->modelCombo()->dot()->status() == EngineStatus::Unknown);
}

TEST_CASE("the activity states put their words in the readout", "[widget]") {
    fvtest::ShownTab tab;
    const std::pair<OverlayState, QString> messages[] = {
        {OverlayState::Recording, QStringLiteral("Recording...")},
        {OverlayState::CommandListening, QStringLiteral(u"Listening for an edit…")},
        {OverlayState::Processing, QStringLiteral("Processing...")},
        {OverlayState::Transcribing, QStringLiteral("Transcribing...")},
        {OverlayState::Cleaning, QStringLiteral("Cleaning up...")},
        {OverlayState::Rewriting, QStringLiteral(u"Rewriting…")},
        {OverlayState::Canceling, QStringLiteral("Canceling...")},
    };
    for (const auto& [state, message] : messages) {
        tab->setActivityState(state);
        CHECK(label(tab) == message);
        tab->setActivityState(OverlayState::None);
        // The state ended and the message was still its own: the idle readout is back.
        CHECK(label(tab) == QStringLiteral("Ready to record"));
    }
    // Only the working states spin the dot.
    tab->setActivityState(OverlayState::Recording);
    CHECK_FALSE(tab->statusDot()->isAnimating());
    tab->setActivityState(OverlayState::Transcribing);
    CHECK(tab->statusDot()->isAnimating());
}

TEST_CASE("a message that is not the activity's own survives the end of the activity", "[widget]") {
    fvtest::ShownTab tab;
    tab->setActivityState(OverlayState::Processing);
    tab->setStatus(QStringLiteral("Error: no audio"));
    tab->setActivityState(OverlayState::None);
    CHECK(label(tab) == QStringLiteral("Error: no audio"));
}

TEST_CASE("model downloads show their name and end independently", "[widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("parakeet-v3 | cpu"), true);
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), true);
    CHECK(label(tab) == QStringLiteral("Downloading model 'parakeet-v3'..."));
    CHECK(tab->engineLoading());
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), false);
    CHECK_FALSE(tab->engineLoading());
    CHECK(label(tab) == QStringLiteral("parakeet-v3 | cpu"));
}

TEST_CASE("the transcript shows the fixed text and a distinct raw one on a switch", "[widget]") {
    fvtest::ShownTab tab;
    tab->setTranscript(QStringLiteral("Hello from FioriVoce."),
                       QStringLiteral("hello from fiorivoce"));
    CHECK_FALSE(tab->versionToggle()->isHidden());
    CHECK(tab->transcriptText()->property("headed").toBool());
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("Hello from FioriVoce."));

    tab->rawButton()->click();
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("hello from fiorivoce"));
    CHECK(tab->shownTranscript() == QStringLiteral("hello from fiorivoce"));
    tab->fixedButton()->click();
    CHECK(tab->shownTranscript() == QStringLiteral("Hello from FioriVoce."));

    // A new transcript starts on Fixed again.
    tab->rawButton()->click();
    tab->setTranscript(QStringLiteral("Next."), QStringLiteral("next"));
    CHECK(tab->fixedButton()->isChecked());
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("Next."));
}

TEST_CASE("a raw text that is empty or the same keeps the switch away", "[widget]") {
    fvtest::ShownTab tab;
    tab->setTranscript(QStringLiteral("Same."), QStringLiteral("Same."));
    CHECK(tab->versionToggle()->isHidden());
    tab->setTranscript(QStringLiteral("Other."), QStringLiteral(""));
    CHECK(tab->versionToggle()->isHidden());
    CHECK_FALSE(tab->transcriptText()->property("headed").toBool());
    tab->setTranscript(QStringLiteral("Raw."), QStringLiteral("raw"));
    CHECK_FALSE(tab->versionToggle()->isHidden());
    tab->clearTranscription();
    CHECK(tab->versionToggle()->isHidden());
    CHECK(tab->transcriptText()->toPlainText().isEmpty());
    CHECK(tab->shownTranscript().isEmpty());
}

TEST_CASE("the transcript editor is two lines tall empty and grows to twelve", "[widget]") {
    fvtest::ShownTab tab;
    tab->setTranscriptionCollapsed(false);
    QApplication::processEvents();
    QTextEdit* edit = tab->transcriptText();
    const int line = edit->fontMetrics().lineSpacing();
    const int empty = edit->minimumSizeHint().height();
    tab->setTranscript(QStringLiteral("One line."));
    QApplication::processEvents();
    const int one = edit->minimumSizeHint().height();
    CHECK(one < empty);
    CHECK(empty - one >= line - 1);
    CHECK(empty - one <= line + 1);

    QString lines;
    for (int i = 0; i < 40; ++i) {
        lines += QStringLiteral("line %1\n").arg(i);
    }
    tab->setTranscript(lines);
    QApplication::processEvents();
    const int capped = edit->sizeHint().height();
    CHECK(capped > one);
    CHECK(capped - one >= 10 * line);
    CHECK(capped - one <= 12 * line);
}

TEST_CASE("partial text: a final text replaces, a draft is appended with an ellipsis", "[widget]") {
    fvtest::ShownTab tab;
    tab->setPartialTranscription(QStringLiteral("hello"), false);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("hello ..."));
    tab->setPartialTranscription(QStringLiteral("hello there"), true);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("hello there"));
    tab->setPartialTranscription(QStringLiteral("how are"), false);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("hello there how are ..."));
    // A final text replaces what the buffer held, not appends to it.
    tab->setPartialTranscription(QStringLiteral("hello there how are you"), true);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("hello there how are you"));
    // An empty final text empties the buffer.
    tab->setPartialTranscription(QString(), true);
    CHECK(tab->transcriptText()->toPlainText().isEmpty());
    tab->setPartialTranscription(QStringLiteral("again"), true);
    tab->clearPartialTranscription();
    tab->setPartialTranscription(QStringLiteral("fresh"), false);
    CHECK(tab->transcriptText()->toPlainText() == QStringLiteral("fresh ..."));
}

TEST_CASE("every partial update resets and hides the fixed raw switch", "[widget]") {
    fvtest::ShownTab tab;
    tab->setTranscript(QStringLiteral("Done."), QStringLiteral("done"));
    CHECK_FALSE(tab->versionToggle()->isHidden());
    tab->setPartialTranscription(QStringLiteral("next"), false);
    CHECK(tab->versionToggle()->isHidden());
}

TEST_CASE("expanding the card tells the window and applies the stretch after the animation",
          "[widget]") {
    fvtest::ShownTab tab;
    QSignalSpy collapsed(tab.tab.get(), &fv::QuickRecordTab::transcriptionCollapsed);
    // Measure from the action that schedules the timer. CI can pause between assertions.
    QElapsedTimer waited;
    waited.start();
    tab->expandTranscription();
    REQUIRE(collapsed.count() == 1);
    CHECK(collapsed.at(0).at(0).toBool() == false);
    CHECK_FALSE(tab->isTranscriptionCollapsed());
    // The stretch follows the animation: the card is not elastic until the timer fires.
    const int index = tab->contentLayout()->indexOf(tab->transcriptionCard());
    CHECK(tab->contentLayout()->stretch(index) == 0);
    // A loaded machine can run the single shot late, so wait for it rather than for a fixed time.
    while (tab->contentLayout()->stretch(index) != 1 && waited.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    CHECK(waited.elapsed() >= fv::TranscriptionTabBase::kCollapseStretchDelayMs - 20);
    CHECK(tab->contentLayout()->stretch(index) == 1);
    CHECK(tab->fillsHeight());

    // Restoring a saved state is silent.
    tab->setTranscriptionCollapsed(true);
    CHECK(collapsed.count() == 1);
    CHECK(tab->contentLayout()->stretch(index) == 0);
}

TEST_CASE("the copy buttons copy what is shown and say how it went", "[widget]") {
    fvtest::ShownTab tab;
    CHECK_FALSE(tab->copyButton()->isEnabled());
    CHECK_FALSE(tab->collapsedCopyButton()->isVisible());
    QSignalSpy copied(tab.tab.get(), &fv::QuickRecordTab::copyRequested);

    tab->setTranscript(QStringLiteral("Fixed text."), QStringLiteral("raw text"));
    CHECK(tab->copyButton()->isEnabled());
    // While the card is collapsed the header carries the Copy button.
    CHECK(tab->collapsedCopyButton()->isVisible());
    tab->collapsedCopyButton()->click();
    REQUIRE(copied.count() == 1);
    CHECK(copied.at(0).at(0).toString() == QStringLiteral("Fixed text."));

    tab->setTranscriptionCollapsed(false);
    QApplication::processEvents();
    CHECK_FALSE(tab->collapsedCopyButton()->isVisible());
    tab->rawButton()->click();
    tab->copyButton()->click();
    REQUIRE(copied.count() == 2);
    CHECK(copied.at(1).at(0).toString() == QStringLiteral("raw text"));

    CHECK(tab->copyButton()->accessibleName() == QStringLiteral("Copy transcript"));
    CHECK(tab->copyButton()->toolTip() == QStringLiteral("Copy transcript"));
    tab->showCopyResult(true);
    CHECK(tab->copyButton()->toolTip() == QStringLiteral("Copied to clipboard"));
    CHECK(tab->collapsedCopyButton()->text() == QStringLiteral("Copied"));
    // Timed from just before the call that restarts the reset timer.
    QElapsedTimer waited;
    waited.start();
    tab->showCopyResult(false);
    CHECK(tab->copyButton()->toolTip() == QStringLiteral("Copy failed. Try again."));
    CHECK(tab->collapsedCopyButton()->text() == QStringLiteral("Retry copy"));
    while (tab->collapsedCopyButton()->text() != QStringLiteral("Copy") &&
           waited.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // A Qt::CoarseTimer (QTimer's default, as in OpenWhisper) may fire up to 5 % early, and
    // both clocks count whole milliseconds.
    CHECK(waited.elapsed() >= fv::QuickRecordTab::kCopyFeedbackMs * 95 / 100 - 1);
    CHECK(tab->copyButton()->toolTip() == QStringLiteral("Copy transcript"));
    CHECK(tab->collapsedCopyButton()->text() == QStringLiteral("Copy"));
}

TEST_CASE("a blank transcript has nothing to copy", "[widget]") {
    fvtest::ShownTab tab;
    QSignalSpy copied(tab.tab.get(), &fv::QuickRecordTab::copyRequested);
    tab->setTranscript(QStringLiteral("  \n "));
    CHECK_FALSE(tab->copyButton()->isEnabled());
    tab->copyButton()->click();
    CHECK(copied.count() == 0);
}

TEST_CASE("the corner widget sits in the pane's top right corner", "[widget]") {
    fvtest::ShownTab tab;
    tab->setTranscriptionCollapsed(false);
    QApplication::processEvents();
    const QWidget* pane = tab->transcriptPane();
    const QWidget* corner = tab->copyButton();
    CHECK(corner->parentWidget() == pane);
    CHECK(corner->x() == pane->width() - corner->width() - fv::TranscriptPane::kCornerInsetX);
    CHECK(corner->y() == fv::TranscriptPane::kCornerInsetY);
}

TEST_CASE("record, stop and cancel ask and wait", "[widget]") {
    fvtest::ShownTab tab;
    QSignalSpy toggled(tab.tab.get(), &fv::QuickRecordTab::recordToggled);
    QSignalSpy canceled(tab.tab.get(), &fv::QuickRecordTab::recordCanceled);

    QTest::mouseClick(tab->recordButton(), Qt::LeftButton);
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.at(0).at(0).toBool() == true);
    // The chrome waits for the answer, but the button no longer takes a second click.
    CHECK_FALSE(tab->recordButton()->isActive());
    QTest::mouseClick(tab->recordButton(), Qt::LeftButton);
    CHECK(toggled.count() == 1);

    // Stop and Cancel are inactive until recording and swallow a click.
    QTest::mouseClick(tab->stopButton(), Qt::LeftButton);
    QTest::mouseClick(tab->cancelButton(), Qt::LeftButton);
    CHECK(toggled.count() == 1);
    CHECK(canceled.count() == 0);
    tab->setRecordingActive(true);
    QTest::mouseClick(tab->stopButton(), Qt::LeftButton);
    REQUIRE(toggled.count() == 2);
    CHECK(toggled.at(1).at(0).toBool() == false);
    QTest::mouseClick(tab->cancelButton(), Qt::LeftButton);
    CHECK(canceled.count() == 1);
}

TEST_CASE("the recording chrome locks the engine and unlocks after", "[widget]") {
    fvtest::ShownTab tab;
    tab->setRecordingActive(true);
    CHECK(tab->isRecording());
    CHECK(tab->recordButton()->text() == QStringLiteral("Recording..."));
    CHECK_FALSE(tab->recordButton()->isActive());
    CHECK(tab->stopButton()->isActive());
    CHECK(tab->cancelButton()->isActive());
    CHECK_FALSE(tab->modelCombo()->isEnabled());
    CHECK_FALSE(tab->localEngine()->modelCombo()->isEnabled());
    CHECK_FALSE(tab->localEngine()->deviceCombo()->isEnabled());
    CHECK_FALSE(tab->livePreviewCheck()->isEnabled());
    CHECK(label(tab) == QStringLiteral("Recording in progress..."));

    tab->setRecordingActive(false);
    CHECK_FALSE(tab->isRecording());
    CHECK(tab->recordButton()->text() == QStringLiteral("Start Recording"));
    CHECK(tab->recordButton()->isActive());
    CHECK_FALSE(tab->stopButton()->isActive());
    CHECK_FALSE(tab->cancelButton()->isActive());
    CHECK(tab->modelCombo()->isEnabled());
    CHECK(tab->localEngine()->modelCombo()->isEnabled());
    CHECK(tab->livePreviewCheck()->isEnabled());
    CHECK(label(tab) == QStringLiteral("Ready to record"));
}

TEST_CASE("the AI cleanup checkbox persists and announces; loading is silent", "[widget]") {
    fvtest::ShownTab tab;
    QSignalSpy toggled(tab.tab.get(), &fv::QuickRecordTab::cleanupToggled);
    tab->cleanupCheck()->setChecked(true);
    CHECK(toggled.count() == 1);
    CHECK(tab.settings.contents().at("transcript_cleanup_enabled") == true);

    tab.settings.set_contents(Json{{"transcript_cleanup_enabled", false}});
    tab->loadCleanupSetting();
    CHECK_FALSE(tab->cleanupCheck()->isChecked());
    CHECK(toggled.count() == 1);
}

TEST_CASE("the command mode hint and link follow the shortcut and the provider", "[widget]") {
    SECTION("no shortcut and a provider that is not ready: set it up") {
        fvtest::ShownTab tab(Json{{"hotkeys", {{"command_mode", ""}}}});
        CHECK_FALSE(tab->commandHint()->isVisible());
        CHECK(tab->commandLink()->isVisible());
        CHECK(tab->commandLink()->text() == QStringLiteral(u"Set up Command Mode  →"));
        CHECK(tab->commandLink()->toolTip() ==
              QStringLiteral("Command Mode needs an AI cleanup model, even while cleanup is off"));
        QSignalSpy requested(tab.tab.get(), &fv::QuickRecordTab::settingsRequested);
        tab->commandLink()->click();
        REQUIRE(requested.count() == 1);
        CHECK(requested.at(0).at(0).toString() == QStringLiteral("commands"));
    }
    SECTION("no shortcut and a ready provider: set a shortcut") {
        fvtest::ShownTab tab(Json{{"hotkeys", {{"command_mode", ""}}}});
        tab->setCommandModeReadiness([](const Json&) { return std::optional<bool>(true); });
        CHECK(tab->commandLink()->text() == QStringLiteral(u"Set a Command Mode shortcut  →"));
        CHECK(tab->commandLink()->toolTip() == QStringLiteral("Rewrite selected text by voice"));
        CHECK(tab->commandLink()->isVisible());
    }
    SECTION("a shortcut and a provider not known to be broken: the hint") {
        fvtest::ShownTab tab(Json{{"hotkeys", {{"command_mode", "ctrl+alt+m"}}}});
        tab->setCommandModeReadiness([](const Json&) { return std::nullopt; });
        CHECK(tab->commandHint()->isVisible());
        CHECK(
            tab->commandHint()->text() ==
            QStringLiteral(u"Command Mode · Ctrl+Alt+M · Select text, then say how to change it."));
        CHECK_FALSE(tab->commandLink()->isVisible());
    }
    SECTION("a shortcut and a provider known not to work: the link, not the hint") {
        fvtest::ShownTab tab(Json{{"hotkeys", {{"command_mode", "ctrl+alt+m"}}}});
        CHECK_FALSE(tab->commandHint()->isVisible());
        CHECK(tab->commandLink()->isVisible());
    }
}

TEST_CASE("window activation refreshes the command mode hint", "[widget]") {
    fvtest::ShownTab tab;
    CHECK_FALSE(tab->commandHint()->isVisible());
    tab.settings.save_setting("hotkeys", Json{{"command_mode", "ctrl+alt+m"}});
    tab->setCommandModeReadiness([](const Json&) { return std::optional<bool>(true); });
    // The setter refreshed it already; a second save shows only after the window is activated.
    tab.settings.save_setting("hotkeys", Json{{"command_mode", ""}});
    CHECK(tab->commandHint()->isVisible());
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(tab.tab.get(), &activate);
    CHECK_FALSE(tab->commandHint()->isVisible());
}

TEST_CASE("the live preview of an engine that cannot preview shows why", "[widget]") {
    CHECK(fv::livePreviewUnavailableReason(QStringLiteral("parakeet")).isEmpty());
    CHECK(fv::livePreviewUnavailableReason(QStringLiteral("api")) ==
          QStringLiteral("Live preview needs Parakeet"));
    CHECK(fv::livePreviewUnavailableReason(QStringLiteral("remote")) ==
          QStringLiteral("Live preview needs Parakeet"));
    fvtest::ShownTab tab;
    CHECK(tab->livePreviewUnavailableReason().isEmpty());
    CHECK(tab->livePreviewCheck()->toolTip().startsWith(QStringLiteral("Show draft text")));
}

TEST_CASE("stats appear with the transcript and clear with it", "[widget]") {
    fvtest::ShownTab tab;
    CHECK_FALSE(tab->statsWidget()->isVisible());
    QSignalSpy visibility(tab->statsWidget(), &fv::TranscriptionStatsWidget::visibilityChanged);
    tab->setTranscriptionStats(2.4, 12.7, 403200);
    CHECK(tab->statsWidget()->isVisible());
    CHECK(tab->statsWidget()->transcriptionTimeLabel()->text() == QStringLiteral("2.4s"));
    CHECK(tab->statsWidget()->audioDurationLabel()->text() == QStringLiteral("12.7s"));
    CHECK(tab->statsWidget()->fileSizeLabel()->text() == QStringLiteral("393.8 KB"));
    tab->clearTranscriptionStats();
    CHECK_FALSE(tab->statsWidget()->isVisible());
    CHECK(tab->statsWidget()->fileSizeLabel()->text() == QStringLiteral("--"));
    REQUIRE(visibility.count() == 2);
    CHECK(visibility.at(0).at(0).toBool());
    CHECK_FALSE(visibility.at(1).at(0).toBool());
}

TEST_CASE("the hotkey hints follow update hotkeys", "[widget]") {
    fvtest::ShownTab tab;
    // Without a shortcut a button shows no hint.
    tab->recordButton()->hotkeyHintFilter()->showHint();
    CHECK(tab->recordButton()->hotkeyHintFilter()->hint() == nullptr);

    tab->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"),
                       QStringLiteral("ignored"));
    const auto hintOf = [](fv::Button* button) {
        fv::HotkeyHintFilter* filter = button->hotkeyHintFilter();
        filter->showHint();
        REQUIRE(filter->hint() != nullptr);
        const QString shown = filter->hint()->findChild<QLabel*>()->text();
        filter->hideHint();
        return shown;
    };
    // Record and Stop share the record key; Cancel has its own.
    CHECK(hintOf(tab->recordButton()) == QStringLiteral("Ctrl+Alt+R"));
    CHECK(hintOf(tab->stopButton()) == QStringLiteral("Ctrl+Alt+R"));
    CHECK(hintOf(tab->cancelButton()) == QStringLiteral("Esc"));
}
