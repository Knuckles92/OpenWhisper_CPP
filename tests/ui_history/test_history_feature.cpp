// The History feature in the main window: the side panel installed through the window's host
// API, the window's panel signals wired, a card click opening the entry dialog, and
// "HistoryFeature connects every signal" (architecture 8.5).
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPushButton>
#include <QTextEdit>

#include "appkit/app_context.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "history_ui_support.h"
#include "settings/testing/memory_store.h"
#include "ui_history/history_edge_tab.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/tabbed_content.h"

using fv::HistoryFeature;
using fv::HistoryPanel;
using fv::HistorySidebar;
using fv::MainWindow;
using fvtest::FakeStore;
using fvtest::Io;

namespace {

// What Bootstrap builds for the shell and History: a data folder of its own, in-memory
// settings, the GUI and io executors, the shell's window, and History over `store`.
class FeatureFixture {
public:
    explicit FeatureFixture(std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>())
        : store_(std::move(store)), paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          ui_(qApp), context_(services()) {
        shell_.provide(context_);
        history_.provide(context_);
        if (store_) {
            context_.provide<fv::HistoryStore>(store_);
        }
        shell_.install(context_);
        history_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
    }

    MainWindow& window() { return context_.service<MainWindow>(); }
    HistoryPanel& panel() { return context_.service<HistoryPanel>(); }
    HistorySidebar* sidebar() { return panel().sidebar(); }
    FakeStore& store() { return *store_; }
    Io& io() { return io_; }
    fv::HistoryPresenter* presenter() { return window().findChild<fv::HistoryPresenter*>(); }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_.executor();
        return made;
    }

    std::shared_ptr<FakeStore> store_;
    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    Io io_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    HistoryFeature history_;
};

// The signals a class declares itself (not its Qt bases').
template <class T> std::vector<QMetaMethod> ownSignals() {
    std::vector<QMetaMethod> found;
    const QMetaObject& meta = T::staticMetaObject;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        if (meta.method(i).methodType() == QMetaMethod::Signal) {
            found.push_back(meta.method(i));
        }
    }
    return found;
}

template <class T>
void checkConnected(const T& object, const std::map<std::string, std::string>& unused) {
    std::set<std::string> declared;
    for (const QMetaMethod& signal : ownSignals<T>()) {
        const std::string name = signal.name().toStdString();
        declared.insert(name);
        INFO(T::staticMetaObject.className() << "::" << name);
        if (unused.contains(name)) {
            CHECK_FALSE(object.hasReceivers(signal));
        } else {
            CHECK(object.hasReceivers(signal));
        }
    }
    for (const auto& [name, owner] : unused) {
        INFO(name << " (" << owner << ")");
        CHECK(declared.contains(name));
    }
}

QMetaMethod signalOf(const QObject& object, const char* signature) {
    const QMetaObject* meta = object.metaObject();
    return meta->method(meta->indexOfSignal(signature));
}

fv::HistoryEntry entryWithId(const char* id, const char* text) {
    fv::HistoryEntry entry = fvtest::makeEntry(text);
    entry.id = id;
    return entry;
}

} // namespace

TEST_CASE("HistoryFeature connects every signal", "[widget]") {
    FeatureFixture fixture;
    REQUIRE(fixture.sidebar() != nullptr);

    checkConnected(*fixture.sidebar(), {});
    // The app's consumers of the panel's requests are the controller (M1.16) and the export
    // and calendar dialogs (M2).
    checkConnected(fixture.panel(), {{"retranscribeRequested", "M1.16"},
                                     {"entryCopied", "M1.16"},
                                     {"entryDeleted", "M1.16"},
                                     {"exportRequested", "M2"},
                                     {"calendarRequested", "M2"},
                                     {"copiedFromDialog", "M1.16"}});
    // The window's host signals for its side panel have their receiver now.
    MainWindow& window = fixture.window();
    CHECK(window.hasReceivers(signalOf(window, "sidePanelRefreshDue()")));
    CHECK(window.hasReceivers(signalOf(window, "sidePanelPageRequested(int)")));
}

TEST_CASE("the feature id and its requirement", "[unit]") {
    HistoryFeature feature;
    CHECK(feature.id() == "history");
    CHECK(feature.required_features() == std::vector<std::string_view>{"shell"});
}

TEST_CASE("the panel and its edge tab sit beside the tabs", "[widget]") {
    FeatureFixture fixture;
    MainWindow& window = fixture.window();
    CHECK(window.sidePanel() == fixture.sidebar());
    auto* edgeTab = qobject_cast<fv::HistoryEdgeTab*>(window.sidePanelEdgeTab());
    REQUIRE(edgeTab != nullptr);
    CHECK(edgeTab->toolTip() == QStringLiteral("Open History (Ctrl+H)"));

    window.toggleSidePanel();

    CHECK(fixture.sidebar()->isExpanded());
    CHECK(fixture.panel().isExpanded());
    CHECK(edgeTab->isExpanded());
    CHECK(edgeTab->text() == QStringLiteral("›"));
    CHECK(edgeTab->toolTip() == QStringLiteral("Close History (Ctrl+H)"));
}

TEST_CASE("the edge tab and the panel toggle the window's panel", "[widget]") {
    FeatureFixture fixture;
    auto* edgeTab = qobject_cast<QAbstractButton*>(fixture.window().sidePanelEdgeTab());
    REQUIRE(edgeTab != nullptr);
    edgeTab->click();
    CHECK(fixture.sidebar()->isExpanded());
    fixture.panel().toggle();
    CHECK_FALSE(fixture.sidebar()->isExpanded());
}

TEST_CASE("each tab family gets its own page", "[widget]") {
    FeatureFixture fixture;
    CHECK(fixture.sidebar()->page() == HistorySidebar::Page::History);
    Q_EMIT fixture.window().sidePanelPageRequested(fv::TabbedContentWidget::TabMeetingMode);
    CHECK(fixture.sidebar()->page() == HistorySidebar::Page::Meetings);
    CHECK_FALSE(fixture.sidebar()->contentWidget()->isVisibleTo(fixture.sidebar()));
    Q_EMIT fixture.window().sidePanelPageRequested(fv::TabbedContentWidget::TabWalkthrough);
    CHECK(fixture.sidebar()->page() == HistorySidebar::Page::Walkthroughs);
    Q_EMIT fixture.window().sidePanelPageRequested(fv::TabbedContentWidget::TabQuickRecord);
    CHECK(fixture.sidebar()->page() == HistorySidebar::Page::History);
    CHECK(fixture.sidebar()->contentWidget()->isVisibleTo(fixture.sidebar()));
}

TEST_CASE("a card click opens the entry dialog over the main window", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    const fv::HistoryEntry entry = entryWithId("entry-1", "Opened from the list");
    store->history = {entry};
    store->entries[entry.id] = entry;
    FeatureFixture fixture(store);
    fixture.panel().toggle();
    fixture.io().drain();
    REQUIRE(fixture.sidebar()->renderedCardCount() == 1);
    QWidget* parent = nullptr;
    QString shown;
    fixture.presenter()->runDialog = [&](fv::HistoryEntryDialog& dialog) {
        parent = dialog.parentWidget();
        shown = dialog.transcriptText()->toPlainText();
        dialog.copyText = [](const QString&) {
            return true;
        };
        dialog.copyButton()->click();
    };
    int copiedFromDialog = 0;
    QObject::connect(&fixture.panel(), &HistoryPanel::copiedFromDialog,
                     [&copiedFromDialog] { ++copiedFromDialog; });

    auto* card = qobject_cast<fv::HistoryItemWidget*>(
        fixture.sidebar()->historyListLayout()->itemAt(0)->widget());
    REQUIRE(card != nullptr);
    Q_EMIT card->clicked(QStringLiteral("entry-1"));
    fixture.io().drain();

    CHECK(parent == &fixture.window());
    CHECK(shown == QStringLiteral("Opened from the list"));
    CHECK(fixture.window().status() == QStringLiteral("Copied to clipboard"));
    CHECK(copiedFromDialog == 1);
}

TEST_CASE("a double click on a card opens one dialog", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    const fv::HistoryEntry entry = entryWithId("entry-1", "Opened once");
    store->history = {entry};
    store->entries[entry.id] = entry;
    FeatureFixture fixture(store);
    fixture.panel().toggle();
    fixture.io().drain();
    int dialogs = 0;
    fixture.presenter()->runDialog = [&dialogs](fv::HistoryEntryDialog&) {
        ++dialogs;
    };
    auto* card = qobject_cast<fv::HistoryItemWidget*>(
        fixture.sidebar()->historyListLayout()->itemAt(0)->widget());
    REQUIRE(card != nullptr);

    // Both presses land before the entry is looked up, as OpenWhisper's modal dialog met the
    // second one.
    Q_EMIT card->clicked(QStringLiteral("entry-1"));
    Q_EMIT card->clicked(QStringLiteral("entry-1"));
    fixture.io().drain();
    CHECK(dialogs == 1);

    // Once it is closed, the next click opens it again.
    Q_EMIT card->clicked(QStringLiteral("entry-1"));
    fixture.io().drain();
    CHECK(dialogs == 2);
}

TEST_CASE("deleting from the dialog keeps the recording and says so", "[widget]") {
    auto store = std::make_shared<FakeStore>();
    const fv::HistoryEntry entry = entryWithId("entry-1", "To be deleted");
    store->entries[entry.id] = entry;
    FeatureFixture fixture(store);
    std::vector<QString> deleted;
    QObject::connect(&fixture.panel(), &HistoryPanel::entryDeleted,
                     [&deleted](const QString& id) { deleted.push_back(id); });
    fixture.presenter()->runDialog = [](fv::HistoryEntryDialog& dialog) {
        dialog.runMessageBox = [](QMessageBox&) {
            return int(QMessageBox::Yes);
        };
        dialog.deleteButton()->click();
    };

    fixture.presenter()->openEntry(entry);
    fixture.io().drain();

    CHECK(store->deletes() == std::vector<std::pair<std::string, bool>>{{"entry-1", false}});
    CHECK(deleted == std::vector<QString>{QStringLiteral("entry-1")});
    CHECK(fixture.window().status() == QStringLiteral("Entry deleted"));
}

TEST_CASE("transcribe again reaches the panel", "[widget]") {
    FeatureFixture fixture;
    std::vector<QString> asked;
    QObject::connect(&fixture.panel(), &HistoryPanel::retranscribeRequested,
                     [&asked](const QString& path) { asked.push_back(path); });
    Q_EMIT fixture.sidebar()->retranscribeRequested(QStringLiteral(R"(C:\rec\a.wav)"));
    CHECK(asked == std::vector<QString>{QStringLiteral(R"(C:\rec\a.wav)")});
}

TEST_CASE("a copy from the list says so in the status", "[widget]") {
    FeatureFixture fixture;
    Q_EMIT fixture.sidebar()->entryCopied(QStringLiteral("entry-1"));
    CHECK(fixture.window().status() == QStringLiteral("Copied to clipboard"));
}

TEST_CASE("without a History store no panel is installed", "[widget]") {
    FeatureFixture fixture(nullptr);
    CHECK(fixture.sidebar() == nullptr);
    CHECK(fixture.window().sidePanel() == nullptr);
    // The panel's calls do nothing until then.
    fixture.panel().refresh();
    fixture.panel().toggle();
    CHECK_FALSE(fixture.panel().isExpanded());
}
