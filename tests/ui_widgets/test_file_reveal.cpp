// Open the desktop file manager on a file or a folder. The launches go through
// FileRevealHooks, which here record what would have been started.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "testkit/testkit.h"
#include "ui_widgets/file_reveal.h"

namespace {

// Replaces the hooks with recorders for one test case.
class RecordedLaunches {
public:
    RecordedLaunches() {
        fv::FileRevealHooks hooks;
        hooks.openUrl = [this](const QUrl& url) {
            urls.push_back(url);
            return openResult;
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
    bool openResult = true;
    bool startResult = true;

private:
    fv::FileRevealHooks previous_;
};

// A fresh folder of the test's own.
struct Scratch {
    Scratch() : dir(root.path()) {}
    fv::testkit::TempDataRoot root;
    std::filesystem::path dir;

    QString folder(const char* name) const {
        const auto path = dir / name;
        std::filesystem::create_directories(path);
        return QString::fromStdWString(path.wstring());
    }
    QString file(const char* name) const {
        const auto path = dir / name;
        std::ofstream(path) << "x";
        return QString::fromStdWString(path.wstring());
    }
};

QString normalised(const QString& path) {
    return QDir::cleanPath(path);
}

} // namespace

// from
// tests/test_past_meetings_show_in_folder.py::TestOpenFolderInFileManager::test_empty_path_is_refused
TEST_CASE("empty path is refused", "[test_past_meetings_show_in_folder][platform]") {
    const RecordedLaunches launches;
    CHECK_FALSE(fv::openFolderInFileManager(QString()));
    CHECK_FALSE(fv::revealInFileManager(QString()));
    CHECK(launches.urls.empty());
    CHECK(launches.commands.empty());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestOpenFolderInFileManager::test_the_folder_itself_is_opened
TEST_CASE("the folder itself is opened", "[test_past_meetings_show_in_folder][platform]") {
    const Scratch scratch;
    const QString spool = scratch.folder("m_abc1234567");
    RecordedLaunches launches;

    CHECK(fv::openFolderInFileManager(spool));

    REQUIRE(launches.urls.size() == 1);
    CHECK(normalised(launches.urls[0].toLocalFile()) == normalised(spool));
    CHECK(launches.commands.empty());
}

// from
// tests/test_past_meetings_show_in_folder.py::TestOpenFolderInFileManager::test_a_missing_folder_reports_failure
TEST_CASE("a missing folder reports failure", "[test_past_meetings_show_in_folder][platform]") {
    const Scratch scratch;
    RecordedLaunches launches;

    CHECK_FALSE(
        fv::openFolderInFileManager(QString::fromStdWString((scratch.dir / "gone").wstring())));

    CHECK(launches.urls.empty());
}

TEST_CASE("a file path is not a folder to open", "[platform]") {
    const Scratch scratch;
    RecordedLaunches launches;
    CHECK_FALSE(fv::openFolderInFileManager(scratch.file("a.wav")));
    CHECK(launches.urls.empty());
}

TEST_CASE("the folder result is the opener's", "[platform]") {
    const Scratch scratch;
    RecordedLaunches launches;
    launches.openResult = false;
    CHECK_FALSE(fv::openFolderInFileManager(scratch.folder("m")));
    CHECK(launches.urls.size() == 1);
}

TEST_CASE("a relative folder resolves against the working directory", "[platform]") {
    const Scratch scratch;
    scratch.folder("relative");
    const QString previous = QDir::currentPath();
    REQUIRE(QDir::setCurrent(QString::fromStdWString(scratch.dir.wstring())));
    RecordedLaunches launches;
    CHECK(fv::openFolderInFileManager(QStringLiteral("./relative/../relative")));
    QDir::setCurrent(previous);
    REQUIRE(launches.urls.size() == 1);
    CHECK(normalised(launches.urls[0].toLocalFile()) ==
          normalised(QString::fromStdWString((scratch.dir / "relative").wstring())));
}

#ifdef Q_OS_WIN
TEST_CASE("revealing a file runs explorer with the path quoted after /select,", "[platform]") {
    const Scratch scratch;
    const QString file = scratch.file("meeting.wav");
    RecordedLaunches launches;

    CHECK(fv::revealInFileManager(file));

    REQUIRE(launches.commands.size() == 1);
    CHECK(launches.commands[0] ==
          QStringLiteral("explorer /select,\"%1\"").arg(QDir::toNativeSeparators(file)));
    CHECK(launches.urls.empty());
}

TEST_CASE("the result of a reveal ignores how the file manager exits", "[platform]") {
    // Explorer exits 1 even on success; only whether it started counts.
    const Scratch scratch;
    RecordedLaunches launches;
    launches.startResult = true;
    CHECK(fv::revealInFileManager(scratch.file("a.wav")));
}

TEST_CASE("a file manager that does not start falls back to the containing folder", "[platform]") {
    const Scratch scratch;
    const QString file = scratch.file("meeting.wav");
    RecordedLaunches launches;
    launches.startResult = false;

    CHECK(fv::revealInFileManager(file));

    REQUIRE(launches.urls.size() == 1);
    CHECK(normalised(launches.urls[0].toLocalFile()) ==
          normalised(QString::fromStdWString(scratch.dir.wstring())));
}
#endif

TEST_CASE("a missing file reveals its folder when that exists", "[platform]") {
    const Scratch scratch;
    RecordedLaunches launches;

    CHECK(fv::revealInFileManager(QString::fromStdWString((scratch.dir / "gone.wav").wstring())));

    CHECK(launches.commands.empty());
    REQUIRE(launches.urls.size() == 1);
    CHECK(normalised(launches.urls[0].toLocalFile()) ==
          normalised(QString::fromStdWString(scratch.dir.wstring())));
}

TEST_CASE("revealing a folder opens the folder", "[platform]") {
    const Scratch scratch;
    const QString folder = scratch.folder("spool");
    RecordedLaunches launches;
#ifdef Q_OS_WIN
    launches.startResult = false;
#endif
    CHECK(fv::revealInFileManager(folder));
    REQUIRE(launches.urls.size() == 1);
    CHECK(normalised(launches.urls[0].toLocalFile()) == normalised(folder));
}

TEST_CASE("nothing to open when neither the path nor its folder exists", "[platform]") {
    const Scratch scratch;
    RecordedLaunches launches;
    CHECK_FALSE(fv::revealInFileManager(
        QString::fromStdWString((scratch.dir / "no" / "such" / "file.wav").wstring())));
    CHECK(launches.urls.empty());
    CHECK(launches.commands.empty());
}
