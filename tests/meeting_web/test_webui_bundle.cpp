// The vendored dashboard bundle: tests/test_webui_accessibility.py's static guardrails over
// webui/src (architecture amendment 16), and that webui/dist is the bundle webui/dist.sha256
// records, rebranded.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "base/hash.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "testkit/testkit.h"

namespace {

std::filesystem::path webui() {
    return fv::testkit::source_root() / "webui";
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string source(std::string_view relative) {
    return read_file(webui() / "src" / std::filesystem::path(relative));
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

// from tests/test_webui_accessibility.py::test_keyboard_focus_is_visible_and_motion_can_be_reduced
TEST_CASE("test_keyboard_focus_is_visible_and_motion_can_be_reduced",
          "[test_webui_accessibility]") {
    const std::string styles = source("styles.css");
    CHECK(contains(styles, "button:focus-visible"));
    CHECK(contains(styles, "summary:focus-visible"));
    CHECK(contains(styles, "outline: 3px solid var(--leaf)"));
    CHECK(contains(styles, "@media (prefers-reduced-motion: reduce)"));
}

// from
// tests/test_webui_accessibility.py::test_history_uses_native_buttons_and_report_uses_accessible_timeline
TEST_CASE("test_history_uses_native_buttons_and_report_uses_accessible_timeline",
          "[test_webui_accessibility]") {
    const std::string history = source("components/HistoryPane.tsx");
    const std::string ribbon = source("components/report/RibbonReport.tsx");
    CHECK(contains(history, R"(aria-label="Search meeting transcripts")"));
    // Each meeting row is a native button directly inside its list item.
    CHECK(std::regex_search(history, std::regex(R"(<li key=\{m\.id\}>\s*<button)")));
    CHECK(contains(history, "className={`history-item${"));
    CHECK(contains(ribbon, "<TimelineMinimap"));
}

// from tests/test_webui_accessibility.py::test_errors_dialogs_and_loading_states_are_announced
TEST_CASE("test_errors_dialogs_and_loading_states_are_announced", "[test_webui_accessibility]") {
    const std::string app = source("App.tsx");
    const std::string dialog = source("components/ConfirmDialog.tsx");
    const std::string activity = source("components/ActivityPane.tsx");
    CHECK(contains(app, R"(className="error-screen" role="alert")"));
    CHECK(contains(app, R"(role="status" aria-live="polite")"));
    CHECK(contains(dialog, "role={danger ? 'alertdialog' : 'dialog'}"));
    CHECK(contains(dialog, R"(aria-modal="true")"));
    CHECK(contains(activity, R"(className="activity-error" role="alert")"));
}

TEST_CASE("webui/dist is the bundle webui/dist.sha256 records", "[webui]") {
    // "<hex>  <path relative to dist/>" per served file, sorted.
    std::map<std::string, std::string> recorded;
    std::istringstream manifest(read_file(webui() / "dist.sha256"));
    for (std::string line; std::getline(manifest, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto split = line.find("  ");
        REQUIRE(split == 64);
        recorded.emplace(line.substr(split + 2), line.substr(0, split));
    }
    REQUIRE_FALSE(recorded.empty());
    CHECK(recorded.contains("index.html"));

    const auto dist = webui() / "dist";
    std::map<std::string, std::string> actual;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dist)) {
        if (entry.is_regular_file()) {
            std::string relative = fv::path_to_utf8(entry.path().lexically_relative(dist));
            std::ranges::replace(relative, '\\', '/');
            actual.emplace(relative, fv::sha256_hex(read_file(entry.path())));
        }
    }
    CHECK(actual == recorded);
}

TEST_CASE("the dashboard page names the product, never the old one", "[webui]") {
    std::string index = read_file(webui() / "dist" / "index.html");
    CHECK(contains(index, "<title>" + std::string(fv::identity().product) + " · Meeting</title>"));
    std::ranges::transform(index, index.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // The old product name, assembled so the source carries no brand string of its own.
    const std::string old_name = std::string("open") + "whisper";
    CHECK_FALSE(contains(index, old_name));
}
