// The History card's "Show in Folder" and the reveal it uses
// (tests/test_history_sidebar_show_in_folder.py). Launches go through FileRevealHooks, which
// here record what would have been started.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include <QAction>
#include <QDir>
#include <QMenu>

#include "history_ui_support.h"
#include "ui_history/history_item.h"
#include "ui_widgets/file_reveal.h"

using fv::HistoryItemWidget;
using fvtest::FakeStore;

namespace {

// _entry.
fv::HistoryEntry entryWith(std::optional<std::string> audioFile) {
    fv::HistoryEntry entry = fvtest::makeEntry("hello there", "base.en");
    entry.audio_file = std::move(audioFile);
    return entry;
}

// _menu_actions and _find: the card's menu, built but never shown.
QAction* findAction(HistoryItemWidget& card, std::unique_ptr<QMenu>& holder, const QString& text) {
    holder.reset(card.buildContextMenu());
    for (QAction* action : holder->actions()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

QString native(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

class RecordedLaunches {
public:
    RecordedLaunches() {
        fv::FileRevealHooks hooks;
        hooks.openUrl = [this](const QUrl& url) {
            urls.push_back(url);
            return true;
        };
        hooks.startDetached = [this](const QString& commandLine) {
            commands.push_back(commandLine);
            return startResult;
        };
        previous_ = fv::setFileRevealHooks(std::move(hooks));
    }
    ~RecordedLaunches() { fv::setFileRevealHooks(std::move(previous_)); }
    RecordedLaunches(const RecordedLaunches&) = delete;
    RecordedLaunches& operator=(const RecordedLaunches&) = delete;

    std::vector<QUrl> urls;
    std::vector<QString> commands;
    bool startResult = true;

private:
    fv::FileRevealHooks previous_;
};

struct Recording {
    Recording() : path(root.path() / "rec.wav") { std::ofstream(path, std::ios::binary) << "RIFF"; }
    fv::testkit::TempDataRoot root;
    std::filesystem::path path;
};

} // namespace

// from
// tests/test_history_sidebar_show_in_folder.py::TestShowInFolderAction::test_action_is_enabled_when_the_recording_exists
TEST_CASE("action is enabled when the recording exists",
          "[test_history_sidebar_show_in_folder][widget]") {
    const fvtest::FakeDevice device;
    const Recording recording;
    FakeStore store;
    store.recordings["rec.wav"] = recording.path;
    HistoryItemWidget card(entryWith("rec.wav"), store);

    std::unique_ptr<QMenu> menu;
    QAction* action = findAction(card, menu, QStringLiteral("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK(action->isEnabled());
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestShowInFolderAction::test_action_is_listed_but_disabled_without_a_recording
TEST_CASE("action is listed but disabled without a recording",
          "[test_history_sidebar_show_in_folder][widget]") {
    FakeStore store;
    HistoryItemWidget card(entryWith(std::nullopt), store);

    std::unique_ptr<QMenu> menu;
    QAction* action = findAction(card, menu, QStringLiteral("Show in Folder"));
    REQUIRE(action != nullptr);
    CHECK_FALSE(action->isEnabled());
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestShowInFolderAction::test_triggering_reveals_the_recording_path
TEST_CASE("triggering reveals the recording path",
          "[test_history_sidebar_show_in_folder][widget]") {
    const fvtest::FakeDevice device;
    const Recording recording;
    FakeStore store;
    store.recordings["rec.wav"] = recording.path;
    HistoryItemWidget card(entryWith("rec.wav"), store);
    std::vector<QString> revealed;
    card.reveal = [&revealed](const QString& path) {
        revealed.push_back(path);
        return true;
    };

    std::unique_ptr<QMenu> menu;
    QAction* action = findAction(card, menu, QStringLiteral("Show in Folder"));
    REQUIRE(action != nullptr);
    action->trigger();

    CHECK(revealed == std::vector<QString>{native(recording.path)});
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestShowInFolderAction::test_nothing_is_revealed_when_the_card_has_no_recording
TEST_CASE("nothing is revealed when the card has no recording",
          "[test_history_sidebar_show_in_folder][widget]") {
    FakeStore store;
    HistoryItemWidget card(entryWith(std::nullopt), store);
    int reveals = 0;
    card.reveal = [&reveals](const QString&) {
        ++reveals;
        return true;
    };

    card.showInFolder();

    CHECK(reveals == 0);
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestRevealInFileManager::test_empty_path_is_refused
TEST_CASE("reveal refuses an empty path", "[test_history_sidebar_show_in_folder][platform]") {
    const RecordedLaunches launches;
    CHECK_FALSE(fv::revealInFileManager(QString()));
    CHECK(launches.commands.empty());
    CHECK(launches.urls.empty());
}

#ifdef Q_OS_WIN
// from
// tests/test_history_sidebar_show_in_folder.py::TestRevealInFileManager::test_windows_selects_the_file_with_a_quoted_command_line
TEST_CASE("windows selects the file with a quoted command line",
          "[test_history_sidebar_show_in_folder][platform]") {
    const Recording recording;
    RecordedLaunches launches;

    CHECK(fv::revealInFileManager(native(recording.path)));

    // Explorer honours the selection only with the path quoted after the comma, so the command
    // line stays one raw string.
    REQUIRE(launches.commands.size() == 1);
    CHECK(launches.commands[0] == QStringLiteral("explorer /select,\"%1\"")
                                      .arg(native(std::filesystem::absolute(recording.path))));
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestRevealInFileManager::test_a_failed_selection_still_falls_back_to_the_folder
TEST_CASE("a failed selection still falls back to the folder",
          "[test_history_sidebar_show_in_folder][platform]") {
    const Recording recording;
    RecordedLaunches launches;
    launches.startResult = false;

    CHECK(fv::revealInFileManager(native(recording.path)));

    CHECK(launches.urls.size() == 1);
}
#endif

// from
// tests/test_history_sidebar_show_in_folder.py::TestRevealInFileManager::test_a_deleted_recording_falls_back_to_its_folder
TEST_CASE("a deleted recording falls back to its folder",
          "[test_history_sidebar_show_in_folder][platform]") {
    const fv::testkit::TempDataRoot root;
    RecordedLaunches launches;

    CHECK(fv::revealInFileManager(native(root.path() / "gone.wav")));

    CHECK(launches.commands.empty());
    REQUIRE(launches.urls.size() == 1);
    CHECK(QDir::cleanPath(launches.urls[0].toLocalFile()) == QDir::cleanPath(native(root.path())));
}

// from
// tests/test_history_sidebar_show_in_folder.py::TestRevealInFileManager::test_a_missing_folder_reports_failure
TEST_CASE("a missing folder reports failure", "[test_history_sidebar_show_in_folder][platform]") {
    const fv::testkit::TempDataRoot root;
    RecordedLaunches launches;

    CHECK_FALSE(fv::revealInFileManager(native(root.path() / "vanished" / "rec.wav")));

    CHECK(launches.urls.empty());
}
