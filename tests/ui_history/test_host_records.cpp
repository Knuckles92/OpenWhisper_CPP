// History's entries kept on the paired host (tests/test_remote_records_ui.py and the sidebar's
// host prompts, A09-42): merged by time without holding up this computer's own, deleted there,
// fetched before they are transcribed again. Workers never hold a closed sidebar or card.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>

#include "base/executor.h"
#include "history_ui_support.h"
#include "host_records_support.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_sidebar.h"
#include "ui_widgets/eliding_label.h"

using fv::HistoryEntry;
using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::HostRecords;
using fvtest::Io;
using fvtest::remoteItem;

namespace {

std::vector<HistoryItemWidget*> cards(const HistorySidebar& sidebar) {
    std::vector<HistoryItemWidget*> found;
    const QVBoxLayout* list = sidebar.historyListLayout();
    for (int index = 0; index < list->count(); ++index) {
        if (auto* card = qobject_cast<HistoryItemWidget*>(list->itemAt(index)->widget())) {
            found.push_back(card);
        }
    }
    return found;
}

std::vector<std::string> ids(const HistorySidebar& sidebar) {
    std::vector<std::string> found;
    for (const auto* card : cards(sidebar)) {
        found.push_back(card->entry().id);
    }
    return found;
}

// A History sidebar over a fake store and the fake host's records, open and shown.
struct HostSidebar {
    explicit HostSidebar(fv::Executor& io, fv::SettingsStore* settings = nullptr)
        : sidebar(store, io, settings) {
        sidebar.setRemoteRecords(&records, &records);
        sidebar.resize(HistorySidebar::kExpandedWidth, 600);
        sidebar.setSidebarWidth(HistorySidebar::kExpandedWidth);
        sidebar.show();
    }

    HostRecords records;
    std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
    HistorySidebar sidebar;
};

bool calledWith(const HostRecords& records, const std::string& call) {
    const auto calls = records.calls();
    return std::find(calls.begin(), calls.end(), call) != calls.end();
}

HistoryEntry local(const std::string& text, const std::string& when) {
    auto entry = fvtest::makeEntry(text, "parakeet");
    entry.timestamp = when;
    return entry;
}

} // namespace

TEST_CASE("test_history_merges_the_hosts_entries_by_time", "[test_remote_records_ui][widget]") {
    fv::playback::forgetHostRecordings();
    Io io;
    HostSidebar host(io.executor());
    const auto here = local("kept here", "2026-09-27T10:00:00+00:00");
    host.store->history = {here};
    host.records.listing["dictation"] = {
        remoteItem("remote-old", "old one", "2000-01-01T00:00:00+00:00"),
        remoteItem("remote-new", "new one", "2999-01-01T00:00:00+00:00")};
    host.sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    host.sidebar.expand();
    io.drain();

    CHECK(ids(host.sidebar) == std::vector<std::string>{"remote-new", here.id, "remote-old"});
    HistoryItemWidget* remote = cards(host.sidebar).front();
    CHECK(remote->locationChip()->text() == QStringLiteral("On devbox"));
    REQUIRE(remote->retranscribeButton() != nullptr);
    CHECK(remote->retranscribeButton()->isVisibleTo(&host.sidebar));
    std::optional<HistoryEntry> opened;
    host.sidebar.entryFor(QStringLiteral("remote-new"),
                          [&opened](std::optional<HistoryEntry> entry) { opened = entry; });
    io.drain();
    REQUIRE(opened);
    CHECK(opened->stored_on == "devbox");

    host.sidebar.runMessageBox = [](QMessageBox&) {
        return int(QMessageBox::Yes);
    };
    host.sidebar.onDeleteRequested(QStringLiteral("remote-new"));
    host.records.listing["dictation"].pop_back();
    io.drain();
    CHECK(calledWith(host.records, "delete_remote(dictation,remote-new)"));
    CHECK(host.store->forgotten() == std::vector<std::string>{"remote-new"});
    CHECK(ids(host.sidebar) == std::vector<std::string>{here.id, "remote-old"});
    CHECK(host.store->deletes().empty());
}

TEST_CASE("test_a_slow_host_doesnt_hold_up_this_computers_history",
          "[test_remote_records_ui][widget]") {
    fv::ThreadPool pool("history-test", 2);
    fvtest::Gate gate;
    HostSidebar host(pool);
    host.store->history = {local("kept here", "2026-09-27T10:00:00+00:00")};
    host.records.listing["dictation"] = {
        remoteItem("remote-new", "new one", "2999-01-01T00:00:00+00:00")};
    host.records.slow = &gate;
    host.sidebar.expand();
    CHECK(fvtest::waitFor([&] { return cards(host.sidebar).size() == 1; }));
    CHECK(host.records.finished == 0);
    gate.open();
    CHECK(fvtest::waitFor([&] { return cards(host.sidebar).size() == 2; }));
    CHECK(ids(host.sidebar).front() == "remote-new");
}

TEST_CASE("the host is asked with the search and says why it couldn't be", "[A09-42][widget]") {
    Io io;
    HostSidebar host(io.executor());
    host.sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    host.store->history = {local("kept here", "2026-09-27T10:00:00+00:00")};
    host.records.unreachable = "devbox isn't reachable right now.";
    host.sidebar.expand();
    io.drain();
    REQUIRE(cards(host.sidebar).size() == 1);
    const auto* note = qobject_cast<QLabel*>(host.sidebar.historyListLayout()->itemAt(0)->widget());
    REQUIRE(note != nullptr);
    CHECK(note->objectName() == QStringLiteral("historyRemoteNotice"));
    CHECK(note->text() ==
          QStringLiteral("devbox isn't reachable right now. Entries kept there show when it is."));

    host.records.unreachable = "The host is busy.";
    host.sidebar.searchInput()->setText(QStringLiteral("Kept"));
    host.store->history.clear();
    host.sidebar.loadHistory();
    io.drain();
    CHECK(host.records.queries.back() == "kept");
    // Nothing to show: the empty list says so, with no note.
    const QVBoxLayout* list = host.sidebar.historyListLayout();
    REQUIRE(list->count() == 1);
    CHECK(qobject_cast<QLabel*>(list->itemAt(0)->widget())->text() ==
          QStringLiteral("No matching entries"));

    host.store->history = {local("kept here", "2026-09-27T10:00:00+00:00")};
    host.sidebar.loadHistory();
    io.drain();
    CHECK(qobject_cast<QLabel*>(list->itemAt(0)->widget())->text() ==
          QStringLiteral("The host is busy."));
}

TEST_CASE("a host-kept entry is deleted there as the History prompts say", "[A09-42][widget]") {
    Io io;
    fv::testkit::TempDataRoot data;
    fv::SettingsStore settings(data.path() / "settings.json");
    HostSidebar host(io.executor(), &settings);
    host.sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    host.records.listing["dictation"] = {remoteItem("r1", "one"), remoteItem("r2", "two")};
    const auto here = local("kept here", "2026-09-26T10:00:00+00:00");
    host.store->history = {here};
    host.records.on_host = {here.id};
    host.sidebar.expand();
    io.drain();

    // The host-kept entry's box asks too, and offers not to ask again.
    QString title;
    QString question;
    QString detail;
    bool dontAsk = false;
    host.sidebar.runMessageBox = [&](QMessageBox& box) {
        title = box.windowTitle();
        question = box.text();
        detail = box.informativeText();
        dontAsk = box.checkBox() != nullptr;
        box.checkBox()->setChecked(true);
        return int(QMessageBox::Yes);
    };
    host.sidebar.onDeleteRequested(QStringLiteral("r1"));
    io.drain();
    CHECK(title == QStringLiteral("Delete History Entry"));
    CHECK(question == QStringLiteral("Delete this transcription from devbox?"));
    CHECK(detail == QStringLiteral("Its recording there is deleted too. This cannot be undone."));
    CHECK(dontAsk);
    CHECK_FALSE(settings.get(fv::keys::confirm_history_entry_delete));
    CHECK(calledWith(host.records, "delete_remote(dictation,r1)"));

    // Asked no more: the next goes at once.
    int asked = 0;
    host.sidebar.runMessageBox = [&](QMessageBox&) {
        ++asked;
        return int(QMessageBox::Yes);
    };
    host.sidebar.onDeleteRequested(QStringLiteral("r2"));
    io.drain();
    CHECK(asked == 0);
    CHECK(calledWith(host.records, "delete_remote(dictation,r2)"));

    // A copy the host also keeps: what the user shared stays shared (a17-retention).
    const auto box = host.sidebar.makeDeleteConfirmation(nullptr, QStringLiteral("devbox"), false);
    CHECK(box.box->informativeText() ==
          QStringLiteral("This cannot be undone. Its copy on devbox is kept."));
}

TEST_CASE("a host delete that fails says so under History's title", "[A09-42][widget]") {
    Io io;
    HostSidebar host(io.executor());
    host.sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    host.records.listing["dictation"] = {remoteItem("r1", "one")};
    host.sidebar.expand();
    io.drain();
    host.records.fail_next(std::make_exception_ptr(std::runtime_error("host offline")));
    host.sidebar.runMessageBox = [](QMessageBox&) {
        return int(QMessageBox::Yes);
    };
    host.sidebar.onDeleteRequested(QStringLiteral("r1"));
    io.drain();
    QMessageBox* notice = nullptr;
    for (auto* box : host.sidebar.findChildren<QMessageBox*>()) {
        notice = box;
    }
    REQUIRE(notice != nullptr);
    CHECK(notice->windowTitle() == QStringLiteral("Delete History Entry"));
    CHECK(notice->text() == QStringLiteral("The entry couldn't be deleted: host offline"));
    CHECK(host.store->forgotten().empty());
    notice->close();
}

TEST_CASE("clearing History has no note about the host", "[A09-42][a17-retention][widget]") {
    // OpenWhisper: "This computer's entries on <host> are deleted too." A clear here leaves what
    // was shared with the host there.
    Io io;
    HostSidebar host(io.executor());
    QStringList texts;
    host.sidebar.runMessageBox = [&texts](QMessageBox& box) {
        texts << box.text();
        return int(QMessageBox::No);
    };
    host.sidebar.onClearHistory();
    host.sidebar.onClearHistoryAndRecordings();
    CHECK(texts == QStringList{QStringLiteral("Delete all history entries?\n\nSaved recordings "
                                              "will be kept."),
                               QStringLiteral("Delete all history entries AND permanently delete "
                                              "all saved recordings from disk?\n\nThis cannot be "
                                              "undone.")});
}

TEST_CASE("transcribing a host-kept entry again fetches its recording first", "[A09-42][widget]") {
    Io io;
    FakeStore store;
    auto entry = fvtest::makeEntry("On the host.");
    entry.stored_on = "devbox";
    entry.remote_audio = true;
    HistoryItemWidget card(entry, store);
    HostRecords records;
    records.audio_path = "/tmp/host-take.wav";
    card.setRemoteAudio(io.executor(), [&records, id = entry.id] { return records.audio_for(id); });
    QSignalSpy asked(&card, &HistoryItemWidget::retranscribeRequested);
    card.requestRetranscribe();
    CHECK(card.retranscribeButton()->text() == QStringLiteral("Getting it from devbox…"));
    CHECK_FALSE(card.retranscribeButton()->isEnabled());
    io.drain();
    REQUIRE(asked.size() == 1);
    CHECK(asked.at(0).at(0).toString() == QStringLiteral("/tmp/host-take.wav"));
    CHECK(card.retranscribeButton()->text() == QStringLiteral("Transcribe again"));
    CHECK(card.retranscribeButton()->isEnabled());

    records.fail_next(std::make_exception_ptr(std::runtime_error("host offline")));
    card.requestRetranscribe();
    io.drain();
    CHECK(asked.size() == 1);
    CHECK(card.retranscribeButton()->toolTip() ==
          QStringLiteral("Couldn't get the recording: host offline"));
}

TEST_CASE("test_a_history_closed_mid_host_request_is_not_held_by_its_workers",
          "[test_remote_records_ui][widget]") {
    fv::playback::forgetHostRecordings();
    fv::ThreadPool pool("history-test", 4);
    fvtest::Gate gate;
    auto records = std::make_unique<HostRecords>();
    records->listing["dictation"] = {remoteItem("remote-new", "new one")};
    records->slow = &gate;
    auto store = std::make_shared<FakeStore>();
    auto* sidebar = new HistorySidebar(store, pool, nullptr);
    sidebar->setRemoteRecords(records.get(), records.get());
    auto entry = fvtest::makeEntry("new one");
    entry.id = "remote-new";
    entry.stored_on = "devbox";
    entry.remote_audio = true;
    auto* card = new HistoryItemWidget(entry, *store);
    card->setRemoteAudio(pool, [&records] { return records->audio_for("remote-new"); });

    sidebar->loadHistory();
    sidebar->deleteEntryFromDialog(entry);
    card->requestRetranscribe();
    card->playback()->button()->click();
    CHECK(fvtest::waitFor([&] { return records->started.load() >= 3; }));

    QPointer<HistorySidebar> sidebarRef(sidebar);
    QPointer<HistoryItemWidget> cardRef(card);
    sidebar->deleteLater();
    card->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(sidebarRef.isNull());
    CHECK(cardRef.isNull());
    gate.open();
    CHECK(fvtest::waitFor([&] { return records->finished.load() == records->started.load(); }));
    pool.shutdown(std::chrono::milliseconds(5000));
    QCoreApplication::processEvents();
    CHECK(sidebarRef.isNull());
}
