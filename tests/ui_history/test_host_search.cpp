// History's search over host-kept entries (docs/port/audit/a32-checklist-shell.json HI-01): the
// host searches the same columns as this computer (tests/remote_records/test_host_search.cpp),
// and the sidebar shows what it answers without filtering it again by the card's text or title,
// so an entry the host matched by its raw text, time, file name or owner is listed.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QLineEdit>
#include <QVBoxLayout>

#include "base/executor.h"
#include "history_ui_support.h"
#include "host_records_support.h"
#include "ui_history/history_item.h"
#include "ui_history/history_sidebar.h"

using fv::HistoryItemWidget;
using fv::HistorySidebar;
using fvtest::FakeStore;
using fvtest::HostRecords;
using fvtest::Io;
using fvtest::remoteItem;

namespace {

std::vector<std::string> ids(const HistorySidebar& sidebar) {
    std::vector<std::string> found;
    const QVBoxLayout* list = sidebar.historyListLayout();
    for (int index = 0; index < list->count(); ++index) {
        if (auto* card = qobject_cast<HistoryItemWidget*>(list->itemAt(index)->widget())) {
            found.push_back(card->entry().id);
        }
    }
    return found;
}

} // namespace

TEST_CASE("a host-kept entry the host matched by another column than its text is listed",
          "[a32][HI-01][widget]") {
    Io io;
    HostRecords records;
    auto store = std::make_shared<FakeStore>();
    HistorySidebar sidebar(store, io.executor(), nullptr);
    sidebar.setRemoteRecords(&records, &records);
    sidebar.resize(HistorySidebar::kExpandedWidth, 600);
    sidebar.setSidebarWidth(HistorySidebar::kExpandedWidth);
    sidebar.show();
    sidebar.setRemoteMergeWait(std::chrono::milliseconds(1));
    // What the host answers for "UMM": the entry whose raw text holds it; neither its text nor
    // its title does.
    records.listing["dictation"] = {remoteItem("r1", "Ship it Friday")};
    sidebar.expand();
    io.drain();
    sidebar.searchInput()->setText(QStringLiteral("UMM"));
    sidebar.loadHistory();
    io.drain();
    CHECK(records.queries.back() == "umm");
    CHECK(ids(sidebar) == std::vector<std::string>{"r1"});
}
