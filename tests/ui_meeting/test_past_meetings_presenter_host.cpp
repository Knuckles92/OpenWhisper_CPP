// Past Meetings with a paired host, through MeetingPresenter (ui_qt/widgets/past_meetings_panel.py
// _start_remote_listing, _start_background_load and _fetch_from_host; tests/
// test_remote_records_ui.py): this computer's meetings show first and the host's are merged in
// when it answers, the host does its own search, local copies it also keeps say so, and opening
// a host-kept meeting downloads it first with its progress on the card.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTest>

#include "history/calendar_source.h"
#include "presenter_support.h"
#include "remote/testing/fake_record_sync.h"
#include "ui_history/history_sidebar.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_panel.h"

namespace {

using fv::Json;
using fvtest::PresenterRig;

// Blocks the threads that wait on it until opened (at most 5 s).
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        opened_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; });
    }
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        opened_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable opened_;
    bool open_ = false;
};

// The `records` fixture for meetings: the host "devbox" keeps `listing["meeting"]`; `listGate`
// holds list_remote and `fetchGate` holds check_out_from after its first progress.
class HostMeetings final : public fv::remote::testing::FakeRecordSync,
                           public fv::history::RemoteRecordSource {
public:
    HostMeetings() { wanted = true; }

    Gate* listGate = nullptr;
    Gate* fetchGate = nullptr;
    std::string unreachable;
    std::atomic<int> fetchesDone{0};

    bool listing_wanted(std::string_view kind) override {
        return FakeRecordSync::listing_wanted(kind);
    }
    std::vector<Json> list_remote(std::string_view kind, std::string_view query,
                                  int limit) override {
        if (listGate != nullptr) {
            listGate->wait();
        }
        if (!unreachable.empty()) {
            throw fv::remote::RecordsUnavailable(unreachable);
        }
        return FakeRecordSync::list_remote(kind, query, limit);
    }
    using FakeRecordSync::copies_on_host;
    std::vector<std::string> copies_on_host(std::string_view kind,
                                            const std::vector<std::string>& ids) override {
        const auto found = FakeRecordSync::copies_on_host(kind, std::span<const std::string>(ids));
        return {found.begin(), found.end()};
    }
    std::string host_name() override { return "devbox"; }
    fv::HistoryEntry remote_history_entry(const Json&) override { return {}; }
    void check_out_from(std::string_view kind, std::string_view id, std::string_view pin,
                        std::function<void(std::int64_t, std::int64_t)> progress) override {
        {
            const std::lock_guard lock(mutex);
            checkedOut.push_back(std::string(kind) + "|" + std::string(id) + "|" +
                                 std::string(pin));
        }
        if (progress) {
            progress(0, 0); // no size yet: no percentage
            progress(50, 200);
        }
        if (fetchGate != nullptr) {
            fetchGate->wait();
        }
        if (progress) {
            progress(200, 200);
        }
        ++fetchesDone;
    }

    std::mutex mutex;
    std::vector<std::string> checkedOut;
};

Json localMeeting(const std::string& id, const std::string& started) {
    return {{"id", id},
            {"title", id},
            {"status", "ended"},
            {"started_at", started},
            {"host_token", "host-" + id},
            {"guest_token", "guest-" + id},
            {"cloud_enabled", false},
            {"spool_dir", ""}};
}

Json hostMeeting(const std::string& id, const std::string& started) {
    return {{"id", id},
            {"title", "Planning"},
            {"status", "ended"},
            {"started_at", started},
            {"stored_on", "devbox"},
            {"_stored_on_fingerprint", "fp-devbox"},
            {"revision", "rev-2"}};
}

// The presenter with the host's records provided, two local meetings saved, the Meeting tab
// open and Past Meetings expanded.
struct HostRig {
    HostRig(std::shared_ptr<HostMeetings> host_)
        : host(std::move(host_)),
          rig(Json::object(), {}, true, true, [this](fv::AppContext& context) {
              context.provide<fv::history::RemoteRecordSource>(host);
              context.provide<fv::remote::RecordSyncControl>(host);
          }) {
        fvtest::offGui([&] {
            rig.repository().create_meeting(localMeeting("m-local", "2026-11-10T09:00:00Z"));
            rig.repository().create_meeting(localMeeting("m-both", "2026-11-12T09:00:00Z"));
        });
        rig.window().show();
        REQUIRE(rig.openMeetingTab() != nullptr);
        panel = rig.window().findChild<fv::PastMeetingsPanel*>();
        REQUIRE(panel != nullptr);
        rig.context().service<fv::HistoryPanel>().sidebar()->expand();
    }

    std::vector<std::string> ids() const {
        std::vector<std::string> found;
        for (const auto& meeting : panel->meetings()) {
            found.push_back(meeting.value("id", ""));
        }
        return found;
    }
    const Json* meeting(const std::string& id) const {
        for (const auto& row : panel->meetings()) {
            if (row.value("id", "") == id) {
                return &row;
            }
        }
        return nullptr;
    }
    fv::PastMeetingItem* card(const QString& id) const {
        for (auto* item : panel->cards()) {
            if (item->meetingId() == id) {
                return item;
            }
        }
        return nullptr;
    }

    std::shared_ptr<HostMeetings> host;
    PresenterRig rig;
    QPointer<fv::PastMeetingsPanel> panel;
};

std::shared_ptr<HostMeetings> devbox() {
    auto host = std::make_shared<HostMeetings>();
    host->listing["meeting"] = {hostMeeting("m-host", "2026-11-11T09:00:00Z"),
                                hostMeeting("m-both", "2026-11-12T09:00:00Z")};
    host->on_host = {"m-both"};
    return host;
}

} // namespace

TEST_CASE("Past Meetings shows this computer's meetings first and the host's when it answers",
          "[A31-31][widget]") {
    Gate slow;
    auto host = devbox();
    host->listGate = &slow;
    HostRig hosted(host);
    // The host is slow: after REMOTE_MERGE_WAIT_S the local meetings show alone.
    REQUIRE(QTest::qWaitFor([&] { return hosted.ids().size() == 2; }, 3000));
    CHECK(hosted.ids() == std::vector<std::string>{"m-both", "m-local"});

    slow.open();
    REQUIRE(QTest::qWaitFor([&] { return hosted.ids().size() == 3; }, 3000));
    CHECK(hosted.ids() == std::vector<std::string>{"m-both", "m-host", "m-local"});
    // copies_on_host marks the local meeting the host also keeps.
    const Json* both = hosted.meeting("m-both");
    REQUIRE(both != nullptr);
    CHECK(both->value("also_on", "") == "devbox");
    CHECK(both->value("_host_copy_revision", "") == "rev-2");
    CHECK(both->value("stored_on", "") == "");
    CHECK(hosted.meeting("m-host")->value("stored_on", "") == "devbox");
    CHECK(hosted.meeting("m-local")->value("also_on", "") == "");
}

TEST_CASE("Past Meetings asks the host with the search, and an unreachable host hides nothing",
          "[A31-31][widget]") {
    auto host = devbox();
    HostRig hosted(host);
    REQUIRE(QTest::qWaitFor([&] { return hosted.ids().size() == 3; }, 3000));

    // The host does its own search; nothing is filtered here.
    hosted.panel->searchInput()->setText(QStringLiteral(" Plan "));
    REQUIRE(QTest::qWaitFor(
        [&] {
            const auto calls = host->calls();
            return std::ranges::find(calls, "list_remote(meeting,plan,101)") != calls.end();
        },
        3000));
    REQUIRE(QTest::qWaitFor([&] { return hosted.meeting("m-host") != nullptr; }, 3000));

    host->unreachable = "devbox isn't reachable right now.";
    hosted.panel->searchInput()->clear();
    hosted.panel->refresh();
    const std::vector<std::string> local{"m-both", "m-local"};
    REQUIRE(QTest::qWaitFor([&] { return hosted.ids() == local; }, 3000));
    // Past the search delay and the merge wait, the list stays this computer's.
    QTest::qWait(800);
    CHECK(hosted.ids() == local);
}

TEST_CASE("test_opening_a_host_meeting_downloads_it_first (through the presenter)",
          "[A31-56][test_remote_records_ui][widget]") {
    Gate fetch;
    auto host = devbox();
    host->fetchGate = &fetch;
    HostRig hosted(host);
    REQUIRE(
        QTest::qWaitFor([&] { return hosted.card(QStringLiteral("m-host")) != nullptr; }, 3000));
    QTest::mouseClick(hosted.card(QStringLiteral("m-host")), Qt::LeftButton);
    // check_out_from names the host the row came from; its progress shows on the card.
    REQUIRE(QTest::qWaitFor(
        [&] {
            auto* card = hosted.card(QStringLiteral("m-host"));
            return card != nullptr &&
                   card->contentLabel()->text() == QStringLiteral("Getting it from devbox… 25%");
        },
        3000));
    {
        const std::lock_guard lock(host->mutex);
        CHECK(host->checkedOut == std::vector<std::string>{"meeting|m-host|fp-devbox"});
    }
    CHECK(hosted.rig.controls().named("show_past_meeting").empty());

    fetch.open();
    REQUIRE(QTest::qWaitFor(
        [&] { return !hosted.rig.controls().named("show_past_meeting").empty(); }, 3000));
    CHECK(hosted.rig.controls().named("show_past_meeting") ==
          std::vector<std::string>{"show_past_meeting m-host"});
}

TEST_CASE("test_past_meetings_closed_mid_host_request_are_not_held_by_their_workers (the "
          "presenter's fetch)",
          "[A31-56][test_remote_records_ui][widget]") {
    Gate fetch;
    auto host = devbox();
    host->fetchGate = &fetch;
    HostRig hosted(host);
    REQUIRE(
        QTest::qWaitFor([&] { return hosted.card(QStringLiteral("m-host")) != nullptr; }, 3000));
    QTest::mouseClick(hosted.card(QStringLiteral("m-host")), Qt::LeftButton);
    REQUIRE(QTest::qWaitFor(
        [&] {
            const std::lock_guard lock(host->mutex);
            return !host->checkedOut.empty();
        },
        3000));

    // The panel goes while its download is still running.
    delete hosted.panel.data();
    CHECK(hosted.panel.isNull());
    fetch.open();
    REQUIRE(QTest::qWaitFor([&] { return host->fetchesDone == 1; }, 3000));
    // What the worker relays (progress, done) reaches no panel, and opens nothing.
    QTest::qWait(100);
    CHECK(hosted.rig.controls().named("show_past_meeting").empty());
}
