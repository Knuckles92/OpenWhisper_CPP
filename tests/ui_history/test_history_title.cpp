// A History card names an entry by the title an agent gave it (tests/test_mcp_desktop_changes.py).
#include <catch2/catch_test_macros.hpp>

#include "history_ui_support.h"
#include "ui_history/history_item.h"
#include "ui_widgets/wrapped_label.h"

TEST_CASE("history uses title without replacing source filename",
          "[test_mcp_desktop_changes][widget]") {
    // from
    // tests/test_mcp_desktop_changes.py::test_history_uses_title_without_replacing_source_filename
    auto entry = fvtest::makeEntry("Body", "test");
    entry.source_name = "original.wav";
    entry.title = "Project kickoff";
    fvtest::FakeStore store;
    const fv::HistoryItemWidget widget(entry, store);
    REQUIRE(widget.titleLabel() != nullptr);
    CHECK(widget.titleLabel()->text() == QStringLiteral("Project kickoff"));
    CHECK(entry.source_name == "original.wav");
    CHECK(widget.entry().source_name == "original.wav");
}
