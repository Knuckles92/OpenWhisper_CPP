// "Export history…" from the History panel (history_sidebar._on_export_history): the modal
// export dialog over the sidebar, with every entry read off the GUI thread first.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QRect>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "history_ui_support.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_history/history_export_dialog.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"

namespace {

fv::HistoryEntry entryOf(const char* id, const char* text, const char* timestamp) {
    fv::HistoryEntry entry;
    entry.id = id;
    entry.text = text;
    entry.timestamp = timestamp;
    entry.model = "base";
    return entry;
}

// The shell and History as Bootstrap builds them, over a History store the test fills.
class ExportWiring {
public:
    ExportWiring()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          context_(services()) {
        store_->folder = root_.path() / "recordings";
        shell_.provide(context_);
        history_.provide(context_);
        context_.provide<fv::HistoryStore>(store_);
        shell_.install(context_);
        history_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::HistoryPresenter* presenter() { return window().findChild<fv::HistoryPresenter*>(); }
    fv::HistorySidebar* sidebar() { return context_.service<fv::HistoryPanel>().sidebar(); }
    fvtest::FakeStore& store() { return *store_; }
    fvtest::Io& io() { return io_; }
    const std::filesystem::path& root() const { return root_.path(); }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.clock = &clock_;
        made.ui = &ui_;
        made.io = &io_.executor();
        return made;
    }

    std::shared_ptr<fvtest::FakeStore> store_ = std::make_shared<fvtest::FakeStore>();
    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::FakeClock clock_{std::chrono::sys_days{std::chrono::year{2026} / 3 / 15}, "UTC"};
    fvtest::Io io_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::HistoryFeature history_;
};

} // namespace

TEST_CASE("export history opens the export dialog over every entry", "[widget]") {
    ExportWiring wiring;
    wiring.store().history = {
        entryOf("b", "The second thing I said.", "2026-03-15T10:00:00+00:00"),
        entryOf("a", "The first thing I said.", "2026-03-14T09:00:00+00:00"),
    };
    fv::HistoryPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    int runs = 0;
    std::vector<std::string> ids;
    QString defaultPath;
    QWidget* parent = nullptr;
    presenter->runExportDialog = [&](fv::HistoryExportDialog& dialog) {
        ++runs;
        for (const fv::Json& item : dialog.items()) {
            ids.push_back(item.at("id").get<std::string>());
        }
        defaultPath = dialog.defaultPath();
        parent = dialog.parentWidget();
    };
    Q_EMIT wiring.sidebar()->exportRequested();
    // A second request while the first is being read opens nothing more.
    Q_EMIT wiring.sidebar()->exportRequested();
    CHECK(runs == 0);
    wiring.io().drain();
    CHECK(runs == 1);
    CHECK(ids == std::vector<std::string>{"b", "a"});
    CHECK(parent == wiring.sidebar());
    // Beside the recordings folder, under FioriVoce's name.
    CHECK(fv::path_from_utf8(fv::qt::toUtf8(defaultPath)) ==
          wiring.root() / "fiorivoce_history.md");

    Q_EMIT wiring.sidebar()->exportRequested();
    wiring.io().drain();
    CHECK(runs == 2);
}

TEST_CASE("a history that cannot be read opens the export dialog on its error", "[widget]") {
    ExportWiring wiring;
    wiring.store().onGetHistory = [](std::optional<std::int64_t>) -> std::vector<fv::HistoryEntry> {
        throw std::runtime_error("database is locked");
    };
    fv::HistoryPresenter* presenter = wiring.presenter();
    REQUIRE(presenter != nullptr);
    QString error;
    bool controls = true;
    presenter->runExportDialog = [&](fv::HistoryExportDialog& dialog) {
        error = dialog.loadError();
        controls = dialog.hasControls();
    };
    Q_EMIT wiring.sidebar()->exportRequested();
    wiring.io().drain();
    CHECK(error == QStringLiteral("database is locked"));
    CHECK_FALSE(controls);
}
