// The Settings search ranking of ui_qt/dialogs/settings_search.py: OpenWhisper's matching tests
// and the golden vectors of ui_qt.dialogs.settings_search.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "appcore/settings_search.h"
#include "base/json.h"
#include "golden/golden.h"

using fv::Json;
using fv::appcore::highlight;
using fv::appcore::match_entries;
using fv::appcore::SearchEntry;
using fv::appcore::SearchKind;

namespace {

SearchEntry entry(SearchKind kind, std::string title, std::string detail, std::string destination,
                  std::string keywords = {}) {
    SearchEntry made;
    made.kind = kind;
    made.title = std::move(title);
    made.detail = std::move(detail);
    made.destination = std::move(destination);
    made.keywords = std::move(keywords);
    return made;
}

SearchKind kind_of(const std::string& kind) {
    if (kind == "setting") {
        return SearchKind::Setting;
    }
    if (kind == "model") {
        return SearchKind::Model;
    }
    REQUIRE(kind == "help");
    return SearchKind::Help;
}

std::vector<SearchEntry> golden_entries() {
    std::vector<SearchEntry> entries;
    for (const auto& row :
         fv::golden::load("ui_qt.dialogs.settings_search").at("pins").at("entries")) {
        entries.push_back(entry(kind_of(row[0].get<std::string>()), row[1].get<std::string>(),
                                row[2].get<std::string>(), row[3].get<std::string>(),
                                row[4].get<std::string>()));
    }
    return entries;
}

// TestSearchMatching._entries of tests/test_settings_unified.py.
std::vector<SearchEntry> matching_entries() {
    std::vector<SearchEntry> entries{
        entry(SearchKind::Setting, "Recording engine: Parakeet", "Dictation › Voice model",
              "voice_model"),
        entry(SearchKind::Setting, "Microphone", "Dictation › Recording", "recording",
              "audio input device parakeet-free"),
        entry(SearchKind::Model, "Parakeet TDT 0.6B v3", "Downloads · 714 MB", "downloads"),
        entry(SearchKind::Help, "Voice model", "…Parakeet transcribes short chunks…",
              "voice_model"),
    };
    entries[2].model_name = "parakeet-v3";
    return entries;
}

std::size_t count(const std::string& text, const std::string& part) {
    std::size_t found = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++found;
    }
    return found;
}

} // namespace

// from
// tests/test_settings_unified.py::TestSearchMatching::test_every_word_must_match_and_sections_keep_their_order
TEST_CASE("every word must match and sections keep their order", "[test_settings_unified]") {
    const auto entries = matching_entries();
    const auto results = match_entries(entries, "parakeet");
    REQUIRE(results.size() == 4);
    CHECK(results[0].kind == SearchKind::Setting);
    CHECK(results[1].kind == SearchKind::Setting);
    CHECK(results[2].kind == SearchKind::Model);
    CHECK(results[3].kind == SearchKind::Help);
    // A title hit outranks a keyword-only hit.
    CHECK(results[0].title == "Recording engine: Parakeet");
    CHECK(match_entries(entries, "parakeet engine")[0].destination == "voice_model");
    CHECK(match_entries(entries, "nothing like this").empty());
    CHECK(match_entries(entries, "   ").empty());
}

// from tests/test_settings_unified.py::TestSearchMatching::test_each_section_is_capped
TEST_CASE("each section is capped", "[test_settings_unified]") {
    std::vector<SearchEntry> entries;
    for (int index = 0; index < 20; ++index) {
        entries.push_back(
            entry(SearchKind::Model, std::format("model {}", index), "Downloads", "downloads"));
    }
    CHECK(match_entries(entries, "model").size() == 5);
}

// from
// tests/test_settings_unified.py::TestSearchMatching::test_highlight_escapes_and_tints_every_match
TEST_CASE("highlight escapes and tints every match", "[test_settings_unified]") {
    const std::vector<std::string> tokens{"parakeet"};
    const std::string html = highlight("Tag <b> parakeet Parakeet", tokens, "#123456");
    CHECK(html.find("&lt;b&gt;") != std::string::npos);
    CHECK(count(html, "color:#123456") == 2);
}

TEST_CASE("the section titles and caps are the palette's", "[settings_search]") {
    CHECK(fv::appcore::section_title(SearchKind::Setting) == "Settings");
    CHECK(fv::appcore::section_title(SearchKind::Model) == "Models & downloads");
    CHECK(fv::appcore::section_title(SearchKind::Help) == "Help text");
    CHECK(fv::appcore::kSectionLimits == std::array<std::size_t, 3>{6, 5, 3});
    CHECK(SearchEntry{}.icon == "box-blue.svg");
}

TEST_CASE("settings search replays the golden vectors", "[golden][settings_search]") {
    const auto entries = golden_entries();
    fv::golden::for_each(
        "ui_qt.dialogs.settings_search", "match_entries", [&](const fv::golden::Case& c) {
            const auto found = match_entries(entries, c.arg<std::string>(0));
            std::vector<std::size_t> indexes;
            for (const auto& hit : found) {
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (entries[i].title == hit.title && entries[i].kind == hit.kind) {
                        indexes.push_back(i);
                        break;
                    }
                }
            }
            CHECK(indexes == c.result<std::vector<std::size_t>>());
        });
    fv::golden::for_each(
        "ui_qt.dialogs.settings_search", "SearchEntry.haystack", [](const fv::golden::Case& c) {
            const auto made = entry(SearchKind::Setting, c.arg<std::string>(0),
                                    c.arg<std::string>(1), "", c.arg<std::string>(2));
            CHECK(made.haystack() == c.result<std::string>());
        });
    fv::golden::for_each("ui_qt.dialogs.settings_search", "_score", [](const fv::golden::Case& c) {
        const std::string query = c.arg<std::string>(1);
        const auto tokens = fv::appcore::search_tokens(query);
        std::string joined;
        for (const auto& token : tokens) {
            joined += (joined.empty() ? "" : " ") + token;
        }
        const auto made = entry(SearchKind::Setting, c.arg<std::string>(0), "", "");
        CHECK(fv::appcore::search_score(made, joined, tokens) == c.result<int>());
    });
    fv::golden::for_each("ui_qt.dialogs.settings_search", "_tokens", [](const fv::golden::Case& c) {
        CHECK(fv::appcore::search_tokens(c.arg<std::string>(0)) ==
              c.result<std::vector<std::string>>());
    });
    fv::golden::for_each(
        "ui_qt.dialogs.settings_search", "_snippet", [](const fv::golden::Case& c) {
            const auto tokens = c.arg<std::vector<std::string>>(1);
            const std::string text = c.arg<std::string>(0);
            const std::string cut = c.arg_count() > 2
                                        ? fv::appcore::snippet(text, tokens, c.arg<std::size_t>(2))
                                        : fv::appcore::snippet(text, tokens);
            CHECK(cut == c.result<std::string>());
        });
    fv::golden::for_each(
        "ui_qt.dialogs.settings_search", "highlight", [](const fv::golden::Case& c) {
            CHECK(highlight(c.arg<std::string>(0), c.arg<std::vector<std::string>>(1),
                            c.arg<std::string>(2)) == c.result<std::string>());
        });
}
