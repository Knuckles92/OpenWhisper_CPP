#pragma once

// What the History UI tests share: a real History on a fresh database (OpenWhisper's conftest
// gives every test its own), a History store whose answers a test sets (the methods
// OpenWhisper's tests patch on history_manager), an io executor the test drains, a player on a
// fake output device, and the application styled as OpenWhisper styles it.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QString>

#include "audio/player.h"
#include "audio/testing/fake_backend.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "history/entry.h"
#include "history/manager.h"
#include "testkit/testkit.h"
#include "ui_history/history_store.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// Runs fn on a thread of its own and returns what it returned: History blocks on its database,
// which a debug build refuses on the GUI thread.
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

// The application styled as OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// The io executor of the widgets under test: drain() runs what they posted on another thread,
// then delivers the results on this one, until nothing is left.
class Io {
public:
    fv::ManualExecutor& executor() { return executor_; }
    void drain();

private:
    fv::ManualExecutor executor_;
};

// A fresh History: a database and a recordings folder under a data root of the test's own.
class LocalHistory {
public:
    LocalHistory();
    ~LocalHistory();
    LocalHistory(const LocalHistory&) = delete;
    LocalHistory& operator=(const LocalHistory&) = delete;

    fv::HistoryManager& manager() { return *manager_; }
    const std::shared_ptr<fv::HistoryStore>& store() const { return store_; }
    const std::filesystem::path& root() const { return root_.path(); }

    // history_manager.add_entry, off the GUI thread.
    fv::HistoryEntry add(const fv::AddEntry& fields);
    // A cleaned dictation: text "Hello, world.", raw "um hello world", model "base".
    fv::HistoryEntry addCleaned(fv::AddEntry fields = {});
    // history_manager.get_entry_by_id, off the GUI thread.
    std::optional<fv::HistoryEntry> get(const std::string& id);

private:
    fv::testkit::TempDataRoot root_;
    std::unique_ptr<fv::Database> database_;
    std::unique_ptr<fv::HistoryManager> manager_;
    std::shared_ptr<fv::HistoryStore> store_;
};

// A History store answering what the test set. Every call is recorded.
// Affinity: thread-safe.
class FakeStore final : public fv::HistoryStore {
public:
    std::vector<fv::HistoryEntry> get_history(std::optional<std::int64_t> limit) override;
    std::vector<fv::HistoryEntry> search(std::string_view query,
                                         std::optional<std::int64_t> limit) override;
    std::optional<fv::HistoryEntry> get_entry(std::string_view entry_id) override;
    std::optional<fv::HistoryEntry> last_dictation() override;
    void forget_dictation(std::string_view entry_id) override;
    std::optional<fv::HistoryEntry> use_version(std::string_view entry_id,
                                                std::string_view version) override;
    bool delete_entry(std::string_view entry_id, bool delete_audio_file) override;
    void clear_history() override;
    void clear_history_and_recordings() override;
    std::optional<std::filesystem::path> get_recording_path(std::string_view filename) override;
    std::filesystem::path recordings_folder() const override;

    // get_history's answer: `history`, unless onGetHistory is set.
    std::vector<fv::HistoryEntry> history;
    std::function<std::vector<fv::HistoryEntry>(std::optional<std::int64_t>)> onGetHistory;
    // get_entry's answers by id.
    std::map<std::string, fv::HistoryEntry> entries;
    // get_recording_path's answers by file name; anything else is not on disk.
    std::map<std::string, std::filesystem::path> recordings;
    std::filesystem::path folder;
    bool deleteResult = true;

    // (entry id, delete_audio_file) of each delete_entry call.
    std::vector<std::pair<std::string, bool>> deletes() const;
    // The entry ids forget_dictation was told, in order.
    std::vector<std::string> forgotten() const;
    int clears() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, bool>> deletes_;
    std::vector<std::string> forgotten_;
    int clears_ = 0;
};

// A process-wide player on a fake output device, for one test case (the `device` fixture).
class FakeDevice {
public:
    FakeDevice();
    ~FakeDevice();
    FakeDevice(const FakeDevice&) = delete;
    FakeDevice& operator=(const FakeDevice&) = delete;

    fv::audio::testing::FakeBackend& backend() { return *backend_; }

private:
    std::shared_ptr<fv::audio::testing::FakeBackend> backend_;
    std::shared_ptr<fv::audio::Player> previous_;
};

// player_tests._wav: a recorder-format WAV (44.1 kHz mono 16-bit) of `seconds`.
std::filesystem::path writeRecording(const std::filesystem::path& path, double seconds = 0.25);

// Polls until done() or 3 s pass, turning the event loop; returns done().
bool waitFor(const std::function<bool()>& done);

// TranscriptionHistory.create(text=..., model=...) with a fresh id and the current time.
fv::HistoryEntry makeEntry(std::string text, std::string model = "base");

QString q(const std::string& utf8);

} // namespace fvtest
