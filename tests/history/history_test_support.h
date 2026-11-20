#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "history/entry.h"
#include "history/manager.h"
#include "history/record_sync.h"
#include "history/repository.h"
#include "testkit/testkit.h"

namespace fv::history_test {

// A fresh database under a testkit data root, as the per-test `db` of OpenWhisper's conftest.
class TestDatabase {
public:
    TestDatabase();
    ~TestDatabase();
    TestDatabase(const TestDatabase&) = delete;
    TestDatabase& operator=(const TestDatabase&) = delete;

    Database& database() { return *database_; }
    HistoryRepository& repository() { return repository_; }
    const std::filesystem::path& root() const { return root_.path(); }
    std::filesystem::path recordings() const { return root_.path() / "recordings"; }

    // db.add_history_entry(entry_id=..., text=..., timestamp=..., model=..., **fields).
    void add(std::string id, std::string text, std::string timestamp, std::string model = "base",
             ContextFields context = {}, std::optional<std::string> source_name = std::nullopt,
             std::optional<std::string> raw_text = std::nullopt,
             std::optional<double> audio_duration = std::nullopt);
    // session.get(TranscriptionHistory, id).origin_device_id = device
    void set_origin(std::string_view id, std::string_view device);
    // Rows of `query` (first column as text).
    std::vector<std::string> column(std::string_view query);

private:
    testkit::TempDataRoot root_;
    std::unique_ptr<Database> database_;
    HistoryRepository repository_;
};

// The _Sync fakes of the tests: what History asked the paired-host outbox to do.
class RecordingSync final : public RecordSync {
public:
    void record_saved(std::string_view kind, std::string_view id) override {
        saved.emplace_back(kind, id);
    }
    void record_edited(std::string_view kind, std::string_view id) override {
        edited.emplace_back(kind, id);
    }
    void record_deleted(std::string_view kind, std::string_view id) override {
        deleted.emplace_back(kind, id);
    }
    void cleared(std::string_view kind) override { cleared_kinds.emplace_back(kind); }

    std::vector<std::pair<std::string, std::string>> saved;
    std::vector<std::pair<std::string, std::string>> edited;
    std::vector<std::pair<std::string, std::string>> deleted;
    std::vector<std::string> cleared_kinds;
};

// HistoryServices naming a sync fake and, optionally, a clock.
inline HistoryServices services_with(RecordSync* sync, const Clock* clock = nullptr) {
    HistoryServices services;
    services.record_sync = sync;
    services.clock = clock;
    return services;
}

// A file holding `bytes`.
void write_file(const std::filesystem::path& path, std::string_view bytes);
std::string read_file(const std::filesystem::path& path);
// File names in `folder`, sorted.
std::vector<std::string> list_names(const std::filesystem::path& folder);

// A UTC moment, for FakeClock.
std::chrono::system_clock::time_point utc(int year, unsigned month, unsigned day, int hour = 0,
                                          int minute = 0, int second = 0);

} // namespace fv::history_test
