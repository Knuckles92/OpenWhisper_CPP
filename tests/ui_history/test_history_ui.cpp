// History cards and the entry dialog: versions, chips, playback
// (tests/test_personalize_s9_history_ui.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTextEdit>
#include <QWidget>

#include "history_ui_support.h"
#include "ui_history/chip_flow.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_sidebar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/wrapped_label.h"

using fv::HistoryEntry;
using fv::HistoryEntryDialog;
using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeDevice;
using fvtest::FakeStore;
using fvtest::Io;
using fvtest::LocalHistory;

namespace {

// _menu: the card's menu actions by text, built but never shown.
std::map<QString, QAction*> menuOf(HistoryItemWidget& card, std::unique_ptr<QMenu>& holder) {
    holder.reset(card.buildContextMenu());
    std::map<QString, QAction*> actions;
    for (QAction* action : holder->actions()) {
        if (!action->text().isEmpty()) {
            actions[action->text()] = action;
        }
    }
    return actions;
}

// TranscriptionHistory.create with context columns.
HistoryEntry created(std::string text, std::optional<std::string> raw = std::nullopt,
                     fv::ContextFields context = {},
                     std::optional<std::string> sourceName = std::nullopt) {
    fv::NewEntry fields;
    fields.text = std::move(text);
    fields.model = "base";
    fields.raw_text = std::move(raw);
    fields.source_name = std::move(sourceName);
    fields.context = std::move(context);
    return fv::create_entry(fields);
}

// _recorded: an entry with a 0.25 s recording saved through History.
HistoryEntry recorded(LocalHistory& history) {
    const auto source = fvtest::writeRecording(history.root() / "take.wav");
    fv::AddEntry fields;
    fields.text = "played back";
    fields.model = "base";
    fields.source_audio_path = source;
    fields.audio_duration = 0.25;
    return history.add(fields);
}

std::shared_ptr<fv::audio::testing::FakeStream> firstStream(FakeDevice& device) {
    const auto streams = device.backend().streams();
    return streams.empty() ? nullptr : streams.front();
}

bool streamStarted(FakeDevice& device) {
    return fvtest::waitFor([&device] {
        const auto stream = firstStream(device);
        return stream != nullptr && stream->started();
    });
}

// A main window as playback asks it: whether it records, whether a meeting runs.
class BusyWindow : public QWidget {
    Q_OBJECT
public:
    bool recording = true;
    bool meeting = false;
    Q_INVOKABLE bool isRecording() const { return recording; }
    Q_INVOKABLE bool meetingIsActive() const { return meeting; }
};

} // namespace

// from tests/test_personalize_s9_history_ui.py::test_cards_show_the_app_and_cleanup_level
TEST_CASE("cards show the app and cleanup level", "[test_personalize_s9_history_ui][widget]") {
    FakeStore store;
    HistoryEntry entry =
        created("Hi team.", "hi team", {{"cleanup_level", "medium"}, {"app_name", "Slack"}});
    entry.cleanup_provider = "openai";
    entry.cleanup_model = "gpt-4o-mini";
    HistoryItemWidget card(entry, store);
    REQUIRE(card.appChip() != nullptr);
    CHECK(card.appChip()->text() == QStringLiteral("Slack"));
    REQUIRE(card.cleanupChip() != nullptr);
    CHECK(card.cleanupChip()->text() == QStringLiteral("✦ Medium · gpt-4o-mini"));
    CHECK(card.cleanupChip()->toolTip() ==
          QStringLiteral("Transcript cleaned with OpenAI · gpt-4o-mini (Medium)"));

    HistoryEntry legacy = created("Hi.", "hi");
    legacy.cleanup_provider = "openai";
    legacy.cleanup_model = "gpt-4o-mini";
    HistoryItemWidget legacyCard(legacy, store);
    REQUIRE(legacyCard.cleanupChip() != nullptr);
    CHECK(legacyCard.cleanupChip()->text() == QStringLiteral("✦ OpenAI · gpt-4o-mini"));
    CHECK(legacyCard.appChip() == nullptr);
}

// from
// tests/test_personalize_s9_history_ui.py::test_rewrites_get_a_kind_chip_instead_of_their_source_title
TEST_CASE("rewrites get a kind chip instead of their source title",
          "[test_personalize_s9_history_ui][widget]") {
    FakeStore store;
    HistoryItemWidget command(
        created("Formal.", std::nullopt, {{"entry_kind", "command"}}, "Quick Record"), store);
    REQUIRE(command.kindChip() != nullptr);
    CHECK(command.kindChip()->text() == QStringLiteral("Command"));
    CHECK(command.titleLabel() == nullptr);

    HistoryItemWidget transform(
        created("Polished.", std::nullopt, {{"entry_kind", "transform"}}, "Polish"), store);
    REQUIRE(transform.kindChip() != nullptr);
    CHECK(transform.kindChip()->text() == QStringLiteral("Transform · Polish"));

    HistoryItemWidget dictation(
        created("Said.", std::nullopt, {{"entry_kind", "dictation"}}, "Quick Record"), store);
    CHECK(dictation.kindChip() == nullptr);
    REQUIRE(dictation.titleLabel() != nullptr);
    CHECK(dictation.titleLabel()->text() == QStringLiteral("Quick Record"));
}

// from tests/test_personalize_s9_history_ui.py::test_the_card_menu_undoes_and_redoes_the_ai_edit
TEST_CASE("the card menu undoes and redoes the ai edit",
          "[test_personalize_s9_history_ui][widget]") {
    LocalHistory history;
    const HistoryEntry entry = history.addCleaned();
    Io io;
    HistorySidebar sidebar(history.store(), io.executor(), nullptr);
    HistoryItemWidget card(*history.get(entry.id), *history.store());
    QObject::connect(&card, &HistoryItemWidget::versionRequested, &sidebar,
                     &HistorySidebar::onVersionRequested);
    std::unique_ptr<QMenu> menu;
    auto actions = menuOf(card, menu);
    CHECK(actions.contains(QStringLiteral("Copy")));
    CHECK(actions.contains(QStringLiteral("Copy Raw")));
    REQUIRE(actions.contains(QStringLiteral("Undo AI edit")));
    CHECK_FALSE(actions.contains(QStringLiteral("Use AI version")));

    actions.at(QStringLiteral("Undo AI edit"))->trigger();
    io.drain();

    const HistoryEntry stored = *history.get(entry.id);
    CHECK(stored.text == "um hello world");
    HistoryItemWidget original(stored, *history.store());
    REQUIRE(original.cleanupChip() != nullptr);
    CHECK(original.cleanupChip()->text() == QStringLiteral("Original"));
    CHECK(original.cleanupChip()->objectName() == QStringLiteral("historyOriginalChip"));
    QObject::connect(&original, &HistoryItemWidget::versionRequested, &sidebar,
                     &HistorySidebar::onVersionRequested);
    std::unique_ptr<QMenu> originalMenu;
    auto originalActions = menuOf(original, originalMenu);
    REQUIRE(originalActions.contains(QStringLiteral("Use AI version")));
    originalActions.at(QStringLiteral("Use AI version"))->trigger();
    io.drain();
    CHECK(history.get(entry.id)->text == "Hello, world.");
}

// from tests/test_personalize_s9_history_ui.py::test_entries_from_elsewhere_offer_no_undo
// The host-kept entry is built as remote_history_entry builds it (M9 ports that conversion).
TEST_CASE("entries from elsewhere offer no undo", "[test_personalize_s9_history_ui][widget]") {
    FakeStore store;
    HistoryEntry remote;
    remote.id = "h1";
    remote.text = "Cleaned.";
    remote.raw_text = "cleaned";
    remote.timestamp = "2026-10-06T10:00:00+00:00";
    remote.model = "base";
    remote.stored_on = "devbox";
    remote.entry_kind = "command";
    HistoryItemWidget remoteCard(remote, store);
    std::unique_ptr<QMenu> remoteMenu;
    CHECK_FALSE(menuOf(remoteCard, remoteMenu).contains(QStringLiteral("Undo AI edit")));

    HistoryEntry theirs = created("Cleaned.", "cleaned");
    theirs.origin_device_id = "laptop";
    HistoryItemWidget theirCard(theirs, store);
    std::unique_ptr<QMenu> theirMenu;
    CHECK_FALSE(menuOf(theirCard, theirMenu).contains(QStringLiteral("Undo AI edit")));
}

// from
// tests/test_personalize_s9_history_ui.py::test_the_dialog_toggles_original_and_ai_and_uses_the_shown_version
TEST_CASE("the dialog toggles original and ai and uses the shown version",
          "[test_personalize_s9_history_ui][widget]") {
    LocalHistory history;
    fv::AddEntry fields;
    fields.cleanup_model = "gpt-4o-mini";
    fields.cleanup_provider = "openai";
    const HistoryEntry entry = history.addCleaned(fields);
    Io io;
    HistoryEntryDialog dialog(*history.get(entry.id), history.store(), io.executor());
    std::vector<QString> changed;
    QObject::connect(&dialog, &HistoryEntryDialog::versionChanged,
                     [&changed](const QString& id) { changed.push_back(id); });

    CHECK(dialog.rawButton()->text() == QStringLiteral("Original"));
    CHECK(dialog.fixedButton()->text() == QStringLiteral("AI"));
    CHECK(dialog.fixedButton()->isChecked());
    CHECK(dialog.useVersionButton()->isHidden());

    dialog.rawButton()->click();
    CHECK(dialog.transcriptText()->toPlainText() == QStringLiteral("um hello world"));
    CHECK_FALSE(dialog.useVersionButton()->isHidden());
    dialog.useVersionButton()->click();
    io.drain();

    CHECK(changed == std::vector<QString>{fvtest::q(entry.id)});
    CHECK(history.get(entry.id)->text == "um hello world");
    CHECK(dialog.useVersionButton()->isHidden());
    CHECK(dialog.cleanupChip()->text() == QStringLiteral("Original"));

    HistoryEntryDialog reopened(*history.get(entry.id), history.store(), io.executor());
    CHECK(reopened.rawButton()->isChecked());
    CHECK(reopened.transcriptText()->toPlainText() == QStringLiteral("um hello world"));
    reopened.fixedButton()->click();
    CHECK(reopened.transcriptText()->toPlainText() == QStringLiteral("Hello, world."));
    reopened.useVersionButton()->click();
    io.drain();
    CHECK(history.get(entry.id)->text == "Hello, world.");
}

// from tests/test_personalize_s9_history_ui.py::test_a_legacy_entry_can_be_undone_from_the_dialog
TEST_CASE("a legacy entry can be undone from the dialog",
          "[test_personalize_s9_history_ui][widget]") {
    LocalHistory history;
    fv::HistoryEntry legacy;
    legacy.id = "legacy";
    legacy.text = "Clean.";
    legacy.raw_text = "clean um";
    legacy.timestamp = "2026-01-01T09:00:00";
    legacy.model = "base";
    legacy.source_name = "Quick Record";
    fvtest::offGui([&] { history.manager().repository().add_history_entry(legacy, {}); });
    Io io;
    HistoryEntryDialog dialog(*history.get("legacy"), history.store(), io.executor());

    dialog.rawButton()->click();
    dialog.useVersionButton()->click();
    io.drain();

    const HistoryEntry stored = *history.get("legacy");
    CHECK(stored.text == "clean um");
    CHECK(stored.cleaned_text == "Clean.");
}

// from tests/test_personalize_s9_history_ui.py::test_the_dialog_lists_the_app
TEST_CASE("the dialog lists the app", "[test_personalize_s9_history_ui][widget]") {
    auto store = std::make_shared<FakeStore>();
    Io io;
    HistoryEntry entry = created("Hi.", std::nullopt, {{"app_name", "Outlook"}});
    entry.audio_duration = 2.0;
    HistoryEntryDialog dialog(entry, store, io.executor());
    CHECK(dialog.factLabels().at(QStringLiteral("App"))->text() == QStringLiteral("Outlook"));
}

// from tests/test_personalize_s9_history_ui.py::test_a_card_plays_and_stops_its_recording
TEST_CASE("a card plays and stops its recording", "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    LocalHistory history;
    const HistoryEntry entry = recorded(history);
    HistoryItemWidget card(entry, *history.store());
    REQUIRE(card.playback() != nullptr);
    QPushButton* button = card.playback()->button();
    CHECK(button->isEnabled());
    CHECK(button->text() == QStringLiteral("Play"));
    CHECK(card.playback()->progress()->isHidden());

    button->click();
    REQUIRE(streamStarted(device));
    CHECK(card.playback()->isPlaying());
    CHECK(button->text() == QStringLiteral("Stop"));
    CHECK_FALSE(card.playback()->progress()->isHidden());
    firstStream(device)->pull(5512);
    card.playback()->sync();
    CHECK(card.playback()->progress()->fraction() == Catch::Approx(0.5).margin(0.01));
    CHECK(card.playback()->timeLabel()->text() == QStringLiteral("0:00 / 0:00"));

    // A refresh rebuilds the card; the new one shows the same playback.
    HistoryItemWidget rebuilt(*history.get(entry.id), *history.store());
    CHECK(rebuilt.playback()->isPlaying());
    CHECK(rebuilt.playback()->button()->text() == QStringLiteral("Stop"));

    button->click();
    CHECK_FALSE(card.playback()->isPlaying());
    CHECK(button->text() == QStringLiteral("Play"));
    CHECK(firstStream(device)->aborted());
    QCoreApplication::processEvents();
}

// from tests/test_personalize_s9_history_ui.py::test_playing_to_the_end_resets_the_card
TEST_CASE("playing to the end resets the card", "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    LocalHistory history;
    HistoryItemWidget card(recorded(history), *history.store());
    card.playback()->button()->click();
    REQUIRE(streamStarted(device));
    while (firstStream(device)->pull(4096)) {
    }
    card.playback()->sync();
    CHECK(card.playback()->button()->text() == QStringLiteral("Play"));
    CHECK(card.playback()->progress()->isHidden());
}

// from
// tests/test_personalize_s9_history_ui.py::test_playback_is_refused_while_recording_or_in_a_meeting
TEST_CASE("playback is refused while recording or in a meeting",
          "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    LocalHistory history;
    BusyWindow window;
    auto* card = new HistoryItemWidget(recorded(history), *history.store(), &window);

    card->playback()->button()->click();
    CHECK(card->playback()->note() == QString::fromUtf8(fv::playback::kRecordingBusy));
    CHECK(device.backend().streams().empty());
    CHECK_FALSE(card->playback()->isPlaying());

    window.recording = false;
    window.meeting = true;
    card->playback()->button()->click();
    CHECK(card->playback()->note() == QString::fromUtf8(fv::playback::kMeetingBusy));
    CHECK(device.backend().streams().empty());
}

// from
// tests/test_personalize_s9_history_ui.py::test_a_removed_recording_shows_a_disabled_play_with_the_reason
TEST_CASE("a removed recording shows a disabled play with the reason",
          "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    auto store = std::make_shared<FakeStore>();
    Io io;
    HistoryEntry entry = created("old");
    entry.audio_file = "recording_gone.wav";
    HistoryItemWidget card(entry, *store);
    CHECK(card.playback() == nullptr);
    HistoryEntryDialog dialog(entry, store, io.executor());
    CHECK_FALSE(dialog.playButton()->isHidden());
    CHECK_FALSE(dialog.playButton()->isEnabled());
    CHECK(dialog.playButton()->toolTip() == QString::fromUtf8(fv::playback::kRecordingRemoved));
}

// from tests/test_personalize_s9_history_ui.py::test_closing_the_dialog_stops_its_playback
TEST_CASE("closing the dialog stops its playback", "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    LocalHistory history;
    Io io;
    HistoryEntryDialog dialog(recorded(history), history.store(), io.executor());
    dialog.playButton()->click();
    REQUIRE(streamStarted(device));
    CHECK(dialog.playback()->isPlaying());
    dialog.reject();
    CHECK_FALSE(dialog.playback()->isPlaying());
    CHECK(firstStream(device)->aborted());
}

// from tests/test_personalize_s9_history_ui.py::test_collapsing_history_stops_playback
TEST_CASE("collapsing history stops playback", "[test_personalize_s9_history_ui][widget]") {
    FakeDevice device;
    LocalHistory history;
    Io io;
    HistorySidebar sidebar(history.store(), io.executor(), nullptr);
    sidebar.expand();
    HistoryItemWidget card(recorded(history), *history.store());
    card.playback()->button()->click();
    REQUIRE(streamStarted(device));
    sidebar.collapse();
    CHECK_FALSE(card.playback()->isPlaying());
}

// from tests/test_personalize_s9_history_ui.py::test_chips_wrap_instead_of_eliding_to_nothing
TEST_CASE("chips wrap instead of eliding to nothing", "[test_personalize_s9_history_ui][widget]") {
    FakeStore store;
    HistoryEntry entry = created("Polished.", "draft",
                                 {{"entry_kind", "transform"},
                                  {"cleanup_level", "medium"},
                                  {"app_name", "Google Docs — Q3 planning notes"}},
                                 "Polish");
    entry.cleanup_model = "gpt-4o-mini";
    HistoryItemWidget card(entry, store);
    fv::ChipFlow* flow = card.chips();
    REQUIRE(flow != nullptr);
    const int line = flow->minimumSizeHint().height();
    CHECK(flow->heightForWidth(2000) == line);
    CHECK(flow->heightForWidth(200) > line);
    card.setFixedWidth(240);
    card.show();
    QCoreApplication::processEvents();
    const int width = flow->width();
    CHECK(width < 240);
    CHECK(flow->height() > line);
    for (QWidget* chip :
         {static_cast<QWidget*>(card.kindChip()), static_cast<QWidget*>(card.cleanupChip()),
          static_cast<QWidget*>(card.appChip())}) {
        REQUIRE(chip != nullptr);
        CHECK(chip->geometry().right() < width);
        CHECK(chip->width() == std::min(chip->sizeHint().width(), width));
    }
}

TEST_CASE("a card's click opens it unless a button was pressed", "[widget]") {
    FakeDevice device;
    LocalHistory history;
    HistoryItemWidget card(recorded(history), *history.store());
    card.resize(360, 200);
    card.show();
    QCoreApplication::processEvents();
    std::vector<QString> clicks;
    QObject::connect(&card, &HistoryItemWidget::clicked,
                     [&clicks](const QString& id) { clicks.push_back(id); });

    QMouseEvent onCard(QEvent::MouseButtonPress, QPointF(5, 5), card.mapToGlobal(QPointF(5, 5)),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&card, &onCard);
    QPushButton* play = card.playback()->button();
    const QPointF onButton = play->geometry().center();
    QMouseEvent onPlay(QEvent::MouseButtonPress, onButton, card.mapToGlobal(onButton),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&card, &onPlay);

    CHECK(clicks == std::vector<QString>{fvtest::q(card.entry().id)});
}

TEST_CASE("the clock reads minutes and seconds", "[unit]") {
    CHECK(fv::playback::clock(4.9) == "0:04");
    CHECK(fv::playback::clock(750) == "12:30");
    CHECK(fv::playback::clock(-3) == "0:00");
}

#include "test_history_ui.moc"
