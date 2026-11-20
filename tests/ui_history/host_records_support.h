#pragma once

// OpenWhisper's tests/test_remote_records_ui.py `records` fixture: a record sync that answers
// History from memory, both as the source History lists (history::RemoteRecordSource) and as the
// actions it runs (remote::RecordSyncControl, recorded by FakeRecordSync). `gate` holds the
// calls a test wants slow until it opens.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "history/calendar_source.h"
#include "remote/testing/fake_record_sync.h"

namespace fvtest {

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

// _remote_entry: a host-kept dictation as records_list lists it.
inline fv::Json remoteItem(const std::string& id, const std::string& text,
                           const std::string& when = "2026-09-27T10:00:00+00:00",
                           bool audio = true) {
    return {{"id", id},        {"text", text},          {"timestamp", when},
            {"model", "base"}, {"stored_on", "devbox"}, {"has_audio", audio}};
}

class HostRecords final : public fv::remote::testing::FakeRecordSync,
                          public fv::history::RemoteRecordSource {
public:
    HostRecords() { wanted = true; }

    // When set, list_remote, delete_remote and audio_for wait for it.
    Gate* slow = nullptr;
    // Each slow call that has started, and each that has finished.
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    // Thrown by list_remote, as RecordSync says why the host can't be asked.
    std::string unreachable;
    std::vector<std::string> queries;

    bool listing_wanted(std::string_view kind) override {
        return FakeRecordSync::listing_wanted(kind);
    }
    std::vector<fv::Json> list_remote(std::string_view kind, std::string_view query,
                                      int limit) override {
        {
            std::lock_guard lock(mutex_);
            queries.emplace_back(query);
        }
        Waiting waiting(*this);
        if (!unreachable.empty()) {
            throw fv::remote::RecordsUnavailable(unreachable);
        }
        return FakeRecordSync::list_remote(kind, query, limit);
    }
    bool delete_remote(std::string_view kind, std::string_view id) override {
        Waiting waiting(*this);
        return FakeRecordSync::delete_remote(kind, id);
    }
    std::filesystem::path audio_for(std::string_view id) override {
        Waiting waiting(*this);
        return FakeRecordSync::audio_for(id);
    }
    using FakeRecordSync::copies_on_host;
    std::vector<std::string> copies_on_host(std::string_view kind,
                                            const std::vector<std::string>& ids) override {
        const auto found = FakeRecordSync::copies_on_host(kind, std::span<const std::string>(ids));
        return {found.begin(), found.end()};
    }
    std::string host_name() override { return "devbox"; }
    // history_manager.remote_history_entry, for the fields these tests list.
    fv::HistoryEntry remote_history_entry(const fv::Json& item) override {
        fv::HistoryEntry entry;
        entry.id = item.at("id").get<std::string>();
        entry.text = item.value("text", "");
        entry.timestamp = item.value("timestamp", "");
        entry.model = item.value("model", "");
        entry.stored_on = item.value("stored_on", "the host");
        entry.remote_audio = item.value("has_audio", false);
        return entry;
    }

private:
    struct Waiting {
        explicit Waiting(HostRecords& records) : records_(records) {
            ++records_.started;
            if (records_.slow != nullptr) {
                records_.slow->wait();
            }
        }
        ~Waiting() { ++records_.finished; }
        HostRecords& records_;
    };
    std::mutex mutex_;
};

} // namespace fvtest
