#pragma once

// What the Past Meetings tests share: an io executor the test drains, a panel on a pinned clock
// whose provider records its calls, OpenWhisper's `_meeting` row helpers, a recorder of what the
// file manager would have been asked to open, and a runner for the database calls the GUI thread
// may not make.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QUrl>

#include "base/executor.h"
#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "ui_meeting/past_meeting_item.h"
#include "ui_meeting/past_meetings_panel.h"
#include "ui_widgets/file_reveal.h"

namespace fvtest {

// Runs fn on a thread of its own and returns what it returned: the meeting database blocks, which
// a debug build refuses on the GUI thread.
template <class Fn> auto offGui(Fn fn) {
    using Result = std::invoke_result_t<Fn&>;
    if constexpr (std::is_void_v<Result>) {
        std::thread worker(fn);
        worker.join();
    } else {
        std::optional<Result> result;
        std::thread worker([&] { result.emplace(fn()); });
        worker.join();
        return std::move(*result);
    }
}

// The io executor of the widgets under test: drain() runs what they posted on another thread,
// then delivers the results on this one, until nothing is left.
class Io {
public:
    fv::ManualExecutor& executor() { return executor_; }
    void drain() {
        for (int round = 0; round < 50; ++round) {
            const bool queued = executor_.queued() > 0;
            if (queued) {
                offGui([this] { executor_.run_all(); });
            }
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
            if (!queued && executor_.queued() == 0) {
                return;
            }
        }
    }

private:
    fv::ManualExecutor executor_;
};

// tests' `_meeting`: a finished meeting of 2025-01-02 09:30, 42 minutes with 2 paused, whose
// state_json holds the finalization when one is given (a JSON object, as MeetingState writes it).
inline fv::Json meetingRow(const std::string& id, const std::string& status = "ended",
                           const std::string& title = "Review",
                           const std::optional<fv::Json>& finalization = std::nullopt,
                           bool cloudEnabled = true) {
    fv::Json row = {{"id", id},
                    {"title", title},
                    {"status", status},
                    {"started_at", "2025-01-02T09:30:00"},
                    {"ended_at", "2025-01-02T10:12:00"},
                    {"paused_total_s", 120},
                    {"cloud_enabled", cloudEnabled}};
    if (finalization) {
        fv::Json state = {{"meeting_id", id},
                          {"status", status},
                          {"cloud_enabled", cloudEnabled},
                          {"title", title},
                          {"finalization", *finalization}};
        row["state_json"] = state.dump();
    }
    return row;
}

// A panel over rows the test sets, on a clock pinned to 2026-03-15, with every call of its
// provider recorded.
struct PanelFixture {
    explicit PanelFixture(std::vector<fv::Json> initial = {}, bool sourceFiltered = false,
                          fv::SettingsStore* settings = nullptr)
        : rows(std::move(initial)),
          clock(std::chrono::sys_days{std::chrono::year{2026} / 3 / 15} + std::chrono::hours{15},
                "America/New_York") {
        fv::PastMeetingsOptions options;
        options.clock = &clock;
        options.sourceFiltered = sourceFiltered;
        options.settings = settings;
        panel = std::make_unique<fv::PastMeetingsPanel>(
            [this](int limit, const QString& query) {
                std::lock_guard lock(mutex);
                calls.push_back({limit, query, std::this_thread::get_id()});
                return rows;
            },
            io.executor(), options);
    }

    struct Call {
        int limit;
        QString query;
        std::thread::id thread;
    };

    // Loads and waits for the result.
    void refresh() {
        panel->refresh();
        io.drain();
    }
    std::vector<std::string> cardIds() const {
        std::vector<std::string> ids;
        for (fv::PastMeetingItem* card : panel->cards()) {
            ids.push_back(card->meetingId().toStdString());
        }
        return ids;
    }

    std::vector<fv::Json> rows;
    Io io;
    fv::FakeClock clock;
    std::mutex mutex;
    std::vector<Call> calls;
    std::unique_ptr<fv::PastMeetingsPanel> panel;
};

// What openFolderInFileManager would have launched.
class RecordedLaunches {
public:
    RecordedLaunches() {
        fv::FileRevealHooks hooks;
        hooks.openUrl = [this](const QUrl& url) {
            urls.push_back(url);
            return opened;
        };
        hooks.startDetached = [this](const QString& commandLine) {
            commands.push_back(commandLine);
            return true;
        };
        previous_ = fv::setFileRevealHooks(std::move(hooks));
    }
    ~RecordedLaunches() { fv::setFileRevealHooks(std::move(previous_)); }
    RecordedLaunches(const RecordedLaunches&) = delete;
    RecordedLaunches& operator=(const RecordedLaunches&) = delete;

    std::vector<QUrl> urls;
    std::vector<QString> commands;
    bool opened = true;

private:
    fv::FileRevealHooks previous_;
};

} // namespace fvtest
