// Past Meetings with the paired host's meetings merged in (tests/test_remote_records_ui.py): a
// panel closed while the host is still being asked is not held by the worker asking it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QPointer>

#include "base/executor.h"
#include "ui_meeting/past_meetings_panel.h"

using fv::Json;
using fv::PastMeetingsPanel;

TEST_CASE("test_past_meetings_closed_mid_host_request_are_not_held_by_their_workers",
          "[test_remote_records_ui][widget]") {
    std::mutex mutex;
    std::condition_variable opened;
    bool open = false;
    std::atomic<bool> asked{false};
    std::atomic<bool> answered{false};
    fv::ThreadPool pool("past-meetings-test", 2);
    // The provider MeetingPresenter gives the panel: this computer's meetings, then the host's
    // listing, which waits here as a slow host does.
    auto* panel = new PastMeetingsPanel(
        [&](int, const QString&) {
            asked = true;
            std::unique_lock lock(mutex);
            opened.wait_for(lock, std::chrono::seconds(5), [&] { return open; });
            answered = true;
            return std::vector<Json>{{{"id", "m_remote000001"},
                                      {"status", "ended"},
                                      {"started_at", "2026-09-01T00:00:00+00:00"},
                                      {"stored_on", "devbox"}}};
        },
        pool);
    panel->refresh();
    for (int round = 0; round < 300 && !asked; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(asked);

    QPointer<PastMeetingsPanel> ref(panel);
    panel->deleteLater();
    panel = nullptr;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(ref.isNull());
    {
        std::lock_guard lock(mutex);
        open = true;
    }
    opened.notify_all();
    pool.shutdown(std::chrono::milliseconds(5000));
    CHECK(answered);
    // What the worker relayed reaches no panel.
    QCoreApplication::processEvents();
    CHECK(ref.isNull());
}
